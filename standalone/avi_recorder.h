/* Every fMSX frame is preserved by FFV1 inside an AVI container.
 * No lossy codecs, dropped frames, or SDL framebuffer screenshots.
 * The ffmpeg executable is optional until recording is requested. */
#ifndef MSXPICO_AVI_RECORDER_H
#define MSXPICO_AVI_RECORDER_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

enum { AVI_WIDTH = 640, AVI_HEIGHT = 480 };

typedef struct avi_recorder
{
    int pipe_fd;
    pid_t encoder_pid;
    uint8_t *canvas;
    uint64_t frames_written;
    unsigned fps;
    bool active;
    bool have_frame;
} avi_recorder_t;

bool avi_recorder_start(avi_recorder_t *rec, const char *filename,
                        unsigned frames_per_second);
bool avi_recorder_frame(avi_recorder_t *rec, const void *rgb565,
                        unsigned width, unsigned height, size_t pitch);
bool avi_recorder_stop(avi_recorder_t *rec);
#endif
