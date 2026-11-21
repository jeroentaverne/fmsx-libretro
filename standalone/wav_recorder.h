/* Streaming stereo 16-bit PCM WAV capture for native Ubuntu fMSX.
 * Records the exact interleaved samples delivered by the fMSX audio callback.
 * Standard RIFF/WAVE files are limited to 4 GiB; the writer refuses overflow.
 */
#ifndef FMSX_WAV_RECORDER_H
#define FMSX_WAV_RECORDER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct wav_recorder {
    FILE *file;
    uint64_t frames_written;
    unsigned sample_rate;
    bool active;
} wav_recorder_t;

bool wav_recorder_start(wav_recorder_t *rec, const char *path, unsigned rate);
bool wav_recorder_samples(wav_recorder_t *rec, const int16_t *samples, size_t frames);
bool wav_recorder_stop(wav_recorder_t *rec);
#endif
