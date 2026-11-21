/* Native MP4 recorder: RGB565 -> fixed-size RGB565 -> H.264/MP4.
 *
 * Default output is one FFV1 video image for every two 50/60 Hz MSX
 * frames (25/30 fps), with an opt-in full 50/60 fps mode. A NULL image
 * is a duplicate of the preceding one; audio is never decimated.
 * The fixed 640x480 canvas accommodates all MSX2+ modes including TEXT80,
 * plus high-resolution and PAL modes, using pixel-exact 2x nearest-neighbor
 * replication and centered black margins. No intermediate lossy format.
 *
 * H.264/YUV420p creates a widely playable video directly in an MP4
 * container. The encoder is external ffmpeg to avoid libavcodec headers.
 */
#define _POSIX_C_SOURCE 200809L
#include "mp4_recorder.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool write_all(int fd, const uint8_t *ptr, size_t n)
{
    while (n)
    {
        const ssize_t written = write(fd, ptr, n);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        ptr += written;
        n -= (size_t)written;
    }
    return true;
}

bool mp4_recorder_start(mp4_recorder_t *rec, const char *filename,
                        unsigned source_fps, unsigned output_fps)
{
    if (!rec || rec->active || !filename || !*filename ||
        (source_fps != 50 && source_fps != 60) ||
        (output_fps != source_fps && output_fps != source_fps / 2))
        return false;
    /* Do not silently overwrite users' previous recordings. */
    if (access(filename, F_OK) == 0)
    {
        fprintf(stderr, "MP4 recording already exists: %s\n", filename);
        return false;
    }
    uint8_t *pixels = malloc((size_t)MP4_WIDTH * MP4_HEIGHT * 2);
    if (!pixels) return false;
    memset(pixels, 0, (size_t)MP4_WIDTH * MP4_HEIGHT * 2);
    int pipe_fds[2];
    if (pipe(pipe_fds))
    {
        perror("MP4 pipe");
        free(pixels);
        return false;
    }
    char size[32], rate[16];
    snprintf(size, sizeof size, "%ux%u", MP4_WIDTH, MP4_HEIGHT);
    snprintf(rate, sizeof rate, "%u", output_fps);
    pid_t child = fork();
    if (child < 0)
    {
        perror("MP4 fork");
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        free(pixels);
        return false;
    }
    if (child == 0)
    {
        if (dup2(pipe_fds[0], STDIN_FILENO) < 0) _exit(127);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        /* Raw input is little-endian RGB565. Output H.264 YUV420P is
         * compatible with standard MP4 players. -n avoids overwrites. */
        char *const args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-nostdin", "-n", "-f", "rawvideo", "-pixel_format", "rgb565le",
            "-video_size", size, "-framerate", rate,
            "-i", "pipe:0", "-an", "-c:v", "libx264",
            "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p",
            "-threads", "2", "-movflags", "+faststart", "-f", "mp4",
            (char *)filename, NULL
        };
        execvp(args[0], args);
        perror("ffmpeg (install with: sudo apt install ffmpeg)");
        _exit(127);
    }
    close(pipe_fds[0]);
    /* ffmpeg can close the pipe if it is not installed or its MP4 fails.
     * Treat EPIPE as an error instead of letting SIGPIPE kill the emulator. */
    (void)signal(SIGPIPE, SIG_IGN);
    rec->pipe_fd = pipe_fds[1];
    rec->encoder_pid = child;
    rec->canvas = pixels;
    rec->fps = output_fps;
    rec->source_fps = source_fps;
    rec->decimation = source_fps / output_fps;
    rec->frames_written = 0;
    rec->input_frames = 0;
    rec->have_frame = false;
    rec->active = true;
    fprintf(stderr, "MP4: recording %s (%ux%u %u fps, H.264 video)\n",
            filename, MP4_WIDTH, MP4_HEIGHT, output_fps);
    return true;
}

bool mp4_recorder_frame(mp4_recorder_t *rec, const void *rgb565,
                        unsigned w, unsigned h, size_t pitch)
{
    if (!rec || !rec->active) return false;
    /* Preserve temporal spacing: every second guest frame at 25/30 Hz,
     * while retaining every guest frame for the 50/60 Hz option. */
    if ((rec->input_frames++ % rec->decimation) != 0)
        return true;
    if (rgb565)
    {
        if (!w || !h || w > MP4_WIDTH || h > MP4_HEIGHT ||
            pitch < (size_t)w * 2)
        {
            fprintf(stderr, "MP4: unsupported video frame dimensions %ux%u\n", w, h);
            return false;
        }
        const unsigned sx = w <= MP4_WIDTH / 2 ? 2 : 1;
        const unsigned sy = h <= MP4_HEIGHT / 2 ? 2 : 1;
        const unsigned dst_w = w * sx;
        const unsigned dst_h = h * sy;
        const unsigned offset_x = (MP4_WIDTH - dst_w) / 2;
        const unsigned offset_y = (MP4_HEIGHT - dst_h) / 2;
        memset(rec->canvas, 0, (size_t)MP4_WIDTH * MP4_HEIGHT * 2);
        const uint8_t *src = (const uint8_t *)rgb565;
        for (unsigned y = 0; y < h; ++y)
        {
            uint8_t *dst = rec->canvas +
                ((size_t)(offset_y + y * sy) * MP4_WIDTH + offset_x) * 2;
            const uint8_t *row = src + (size_t)y * pitch;
            if (sx == 1)
                memcpy(dst, row, (size_t)w * 2);
            else
                for (unsigned x = 0; x < w; ++x)
                {
                    dst[4*x+0] = dst[4*x+2] = row[2*x+0];
                    dst[4*x+1] = dst[4*x+3] = row[2*x+1];
                }
            if (sy == 2)
                memcpy(dst + (size_t)MP4_WIDTH * 2, dst,
                       (size_t)MP4_WIDTH * 2 - (size_t)offset_x * 2);
        }
        rec->have_frame = true;
    }
    if (!rec->have_frame) return true; /* Libretro duplicate before first frame. */
    const size_t frame_bytes = (size_t)MP4_WIDTH * MP4_HEIGHT * 2;
    if (!write_all(rec->pipe_fd, rec->canvas, frame_bytes))
    {
        perror("MP4 encoder input");
        return false;
    }
    ++rec->frames_written;
    return true;
}

bool mp4_recorder_stop(mp4_recorder_t *rec)
{
    if (!rec || !rec->active) return false;
    const uint64_t frames = rec->frames_written;
    const int fd = rec->pipe_fd;
    const pid_t child = rec->encoder_pid;
    rec->active = false;
    rec->pipe_fd = -1;
    rec->encoder_pid = 0;
    free(rec->canvas);
    rec->canvas = NULL;
    rec->have_frame = false;
    const bool closed = close(fd) == 0;
    int status = 0;
    pid_t waited;
    do waited = waitpid(child, &status, 0);
    while (waited < 0 && errno == EINTR);
    const bool ok = closed && waited == child &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0;
    fprintf(stderr, "MP4: %s, %llu frames, %u fps\n",
            ok ? "saved" : "FAILED", (unsigned long long)frames, rec->fps);
    return ok;
}


/* Mux the finished H.264 MP4 with AAC audio from a separately captured WAV.
 * Copy video without transcoding. Keep WAV as an exact PCM recording. */
bool mp4_recorder_mux_audio(const char *video_only, const char *wav,
                            const char *output)
{
    if (!video_only || !*video_only || !wav || !*wav ||
        !output || !*output || !strcmp(video_only, output) ||
        !strcmp(wav, output) || access(video_only, R_OK) ||
        access(wav, R_OK) || access(output, F_OK) == 0)
        return false;

    const pid_t child = fork();
    if (child < 0)
    {
        perror("MP4 audio mux fork");
        return false;
    }
    if (child == 0)
    {
        char *const args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
            "-n", "-i", (char *)video_only, "-i", (char *)wav,
            "-map", "0:v:0", "-map", "1:a:0",
            "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
            "-movflags", "+faststart",
            "-f", "mp4", (char *)output, NULL
        };
        execvp(args[0], args);
        perror("ffmpeg MP4 audio mux");
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do waited = waitpid(child, &status, 0);
    while (waited < 0 && errno == EINTR);
    const bool ok = waited == child && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 0;
    if (!ok)
    {
        /* The output did not exist before this call, so only remove
         * the incomplete file created by our ffmpeg child. */
        (void)unlink(output);
        fprintf(stderr, "MP4: FAILED to mux AAC audio; keeping %s and %s\n",
                video_only, wav);
        return false;
    }
    fprintf(stderr, "MP4: video + AAC audio saved: %s\n", output);
    return true;
}
