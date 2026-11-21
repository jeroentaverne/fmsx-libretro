/* Native MP4 H.264 recorder for RGB565 fMSX frames.
 * PAL 50 Hz -> 25 fps and NTSC 60 Hz -> 30 fps by default, with
 * full-rate 50/60 fps selectable. Raw PCM stereo is saved as WAV.
 * The finished MP4 contains AAC audio and H.264 video. */
#ifndef MSXPICO_MP4_RECORDER_H
#define MSXPICO_MP4_RECORDER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { MP4_WIDTH = 640, MP4_HEIGHT = 480 };

typedef struct mp4_recorder
{
    int pipe_fd;
    pid_t encoder_pid;
    uint8_t *canvas;
    uint64_t frames_written;
    uint64_t input_frames;
    unsigned fps;
    unsigned source_fps;
    unsigned decimation;
    bool active;
    bool have_frame;
} mp4_recorder_t;

bool mp4_recorder_start(mp4_recorder_t *rec, const char *filename,
                        unsigned source_fps, unsigned output_fps);
bool mp4_recorder_frame(mp4_recorder_t *rec, const void *rgb565,
                        unsigned width, unsigned height, size_t pitch);
bool mp4_recorder_stop(mp4_recorder_t *rec);
bool mp4_recorder_mux_audio(const char *video_only, const char *wav,
                            const char *output);
#endif
