/* FFV1 is lossless for each selected image. Default video cadence
 * decimates native 50/60 Hz to 25/30 Hz; full-rate remains selectable.
 * All stereo PCM audio is recorded at the original 48 kHz sample rate. */
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
    uint64_t input_frames;
    unsigned fps;           /* Output AVI frame rate: 25, 30, 50 or 60 */
    unsigned source_fps;    /* fMSX callback cadence: 50 or 60 */
    unsigned decimation;    /* Source frames per encoded frame: 1 or 2 */
    bool active;
    bool have_frame;
} avi_recorder_t;

bool avi_recorder_start(avi_recorder_t *rec, const char *filename,
                        unsigned source_fps, unsigned output_fps);
bool avi_recorder_frame(avi_recorder_t *rec, const void *rgb565,
                        unsigned width, unsigned height, size_t pitch);
bool avi_recorder_stop(avi_recorder_t *rec);
/* Losslessly stream-copy FFV1 video and mux the WAV audio into AVI.
 * video_only and wav are complete files; output must not exist.
 * Never deletes source files, so a failed mux can be recovered. */
bool avi_recorder_mux_audio(const char *video_only, const char *wav,
                            const char *output);
#endif
