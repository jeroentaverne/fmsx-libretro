/* Native AVI recorder: RGB565 -> fixed-size RGB565 -> FFV1/AVI.
 *
 * Default output is one FFV1 video image for every two 50/60 Hz MSX
 * frames (25/30 fps), with an opt-in full 50/60 fps mode. A NULL image
 * is a duplicate of the preceding one; audio is never decimated.
 * The fixed 640x480 canvas accommodates all MSX2+ modes including TEXT80,
 * plus high-resolution and PAL modes, using pixel-exact 2x nearest-neighbor
 * replication and centered black margins. No intermediate lossy format.
 *
 * FFV1 is an open lossless intra-frame codec. Running ffmpeg as a child
 * means ordinary Ubuntu SDL2 builds do NOT require libavcodec headers.
 */
#define _POSIX_C_SOURCE 200809L
#include "avi_recorder.h"
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

bool avi_recorder_start(avi_recorder_t *rec, const char *filename,
                        unsigned source_fps, unsigned output_fps)
{
    if (!rec || rec->active || !filename || !*filename ||
        (source_fps != 50 && source_fps != 60) ||
        (output_fps != source_fps && output_fps != source_fps / 2))
        return false;
    /* Do not silently overwrite users' previous recordings. */
    if (access(filename, F_OK) == 0)
    {
        fprintf(stderr, "AVI recording already exists: %s\n", filename);
        return false;
    }
    uint8_t *pixels = malloc((size_t)AVI_WIDTH * AVI_HEIGHT * 2);
    if (!pixels) return false;
    memset(pixels, 0, (size_t)AVI_WIDTH * AVI_HEIGHT * 2);
    int pipe_fds[2];
    if (pipe(pipe_fds))
    {
        perror("AVI pipe");
        free(pixels);
        return false;
    }
    char size[32], rate[16];
    snprintf(size, sizeof size, "%ux%u", AVI_WIDTH, AVI_HEIGHT);
    snprintf(rate, sizeof rate, "%u", output_fps);
    pid_t child = fork();
    if (child < 0)
    {
        perror("AVI fork");
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
        /* The raw input is RGB565LE; FFV1 uses lossless BGR0 internally.
         * -n prevents ffmpeg from overwriting a concurrent existing AVI.
         * FFmpeg's AVI muxer writes headers/index when stdin is closed. */
        char *const args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-nostdin", "-n", "-f", "rawvideo", "-pixel_format", "rgb565le",
            "-video_size", size, "-framerate", rate,
            "-i", "pipe:0", "-an", "-c:v", "ffv1", "-level", "3",
            "-pix_fmt", "bgr0", "-threads", "2", "-f", "avi",
            (char *)filename, NULL
        };
        execvp(args[0], args);
        perror("ffmpeg (install with: sudo apt install ffmpeg)");
        _exit(127);
    }
    close(pipe_fds[0]);
    /* ffmpeg can close the pipe if it is not installed or its AVI fails.
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
    fprintf(stderr, "AVI: recording %s (%ux%u %u fps, FFV1 lossless)\n",
            filename, AVI_WIDTH, AVI_HEIGHT, output_fps);
    return true;
}

bool avi_recorder_frame(avi_recorder_t *rec, const void *rgb565,
                        unsigned w, unsigned h, size_t pitch)
{
    if (!rec || !rec->active) return false;
    /* Preserve temporal spacing: every second guest frame at 25/30 Hz,
     * while retaining every guest frame for the 50/60 Hz option. */
    if ((rec->input_frames++ % rec->decimation) != 0)
        return true;
    if (rgb565)
    {
        if (!w || !h || w > AVI_WIDTH || h > AVI_HEIGHT ||
            pitch < (size_t)w * 2)
        {
            fprintf(stderr, "AVI: unsupported video frame dimensions %ux%u\n", w, h);
            return false;
        }
        const unsigned sx = w <= AVI_WIDTH / 2 ? 2 : 1;
        const unsigned sy = h <= AVI_HEIGHT / 2 ? 2 : 1;
        const unsigned dst_w = w * sx;
        const unsigned dst_h = h * sy;
        const unsigned offset_x = (AVI_WIDTH - dst_w) / 2;
        const unsigned offset_y = (AVI_HEIGHT - dst_h) / 2;
        memset(rec->canvas, 0, (size_t)AVI_WIDTH * AVI_HEIGHT * 2);
        const uint8_t *src = (const uint8_t *)rgb565;
        for (unsigned y = 0; y < h; ++y)
        {
            uint8_t *dst = rec->canvas +
                ((size_t)(offset_y + y * sy) * AVI_WIDTH + offset_x) * 2;
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
                memcpy(dst + (size_t)AVI_WIDTH * 2, dst,
                       (size_t)AVI_WIDTH * 2 - (size_t)offset_x * 2);
        }
        rec->have_frame = true;
    }
    if (!rec->have_frame) return true; /* Libretro duplicate before first frame. */
    const size_t frame_bytes = (size_t)AVI_WIDTH * AVI_HEIGHT * 2;
    if (!write_all(rec->pipe_fd, rec->canvas, frame_bytes))
    {
        perror("AVI encoder input");
        return false;
    }
    ++rec->frames_written;
    return true;
}

bool avi_recorder_stop(avi_recorder_t *rec)
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
    fprintf(stderr, "AVI: %s, %llu frames, %u fps\n",
            ok ? "saved" : "FAILED", (unsigned long long)frames, rec->fps);
    return ok;
}

/* After capture, stream-copy FFV1 video while muxing the already finalized
 * WAV as uncompressed PCM. This retains every video frame and every audio
 * sample, without risking deadlock between two simultaneous FFmpeg pipes.
 * The standalone WAV is intentionally retained. */
bool avi_recorder_mux_audio(const char *video_only, const char *wav,
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
        perror("AVI audio mux fork");
        return false;
    }
    if (!child)
    {
        /* -c:v copy ensures the FFV1 bitstream is completely unmodified.
         * -c:a pcm_s16le embeds exactly the WAV's PCM samples. Do NOT
         * specify -shortest: it could truncate a video frame or audio tail. */
        char *const args[] = {
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
            "-n", "-i", (char *)video_only, "-i", (char *)wav,
            "-map", "0:v:0", "-map", "1:a:0",
            "-c:v", "copy", "-c:a", "pcm_s16le",
            "-f", "avi", (char *)output, NULL
        };
        execvp(args[0], args);
        perror("ffmpeg AVI audio mux");
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
        /* A failed ffmpeg may leave a partial output. We verified the
         * filename was absent before spawning, so it is safe to remove. */
        (void)unlink(output);
        fprintf(stderr, "AVI: FAILED to mux WAV audio; keeping %s and %s\n",
                video_only, wav);
        return false;
    }
    fprintf(stderr, "AVI: video + WAV audio muxed without re-encoding video: %s\n",
            output);
    return true;
}
