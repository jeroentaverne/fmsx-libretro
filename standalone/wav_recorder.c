/* Save the exact audio passed to SDL as 16-bit stereo PCM.
 *
 * Writes a placeholder RIFF/WAVE header, streams each callback verbatim
 * in little-endian sample order, and patches RIFF/data lengths on close.
 * No resampling, no proprietary codec, no ffmpeg dependency for WAV.
 */
#define _POSIX_C_SOURCE 200809L
#include "wav_recorder.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool u16le(FILE *f, uint16_t v)
{
    const uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    return fwrite(b, 1, sizeof b, f) == sizeof b;
}
static bool u32le(FILE *f, uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8),
                           (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return fwrite(b, 1, sizeof b, f) == sizeof b;
}
static bool put_tag(FILE *f, const char *four)
{
    return fwrite(four, 1, 4, f) == 4;
}

bool wav_recorder_start(wav_recorder_t *rec, const char *path, unsigned rate)
{
    if (!rec || rec->active || !path || !*path || rate < 8000 || rate > 192000)
        return false;
    /* xb is atomic exclusive-create; never silently overwrite recordings. */
    FILE *f = fopen(path, "wbx");
    if (!f)
    {
        perror("WAV open");
        return false;
    }
    bool ok = put_tag(f, "RIFF") && u32le(f, 36) &&
              put_tag(f, "WAVE") && put_tag(f, "fmt ") &&
              u32le(f, 16) && u16le(f, 1) &&    /* PCM */
              u16le(f, 2) &&                    /* stereo */
              u32le(f, rate) && u32le(f, rate * 4) &&
              u16le(f, 4) && u16le(f, 16) &&
              put_tag(f, "data") && u32le(f, 0);
    if (!ok)
    {
        fclose(f);
        unlink(path);
        return false;
    }
    rec->file = f;
    rec->sample_rate = rate;
    rec->frames_written = 0;
    rec->active = true;
    fprintf(stderr, "WAV: recording %s (%u Hz, stereo, 16-bit PCM)\n", path, rate);
    return true;
}

bool wav_recorder_samples(wav_recorder_t *rec, const int16_t *samples, size_t frames)
{
    if (!rec || !rec->active) return false;
    if (!frames) return true;
    if (!samples || frames > (UINT32_MAX - 36u) / 4u ||
        rec->frames_written > ((UINT32_MAX - 36u) / 4u) - frames)
        return false; /* An ordinary RIFF chunk cannot exceed 4 GiB. */

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    if (fwrite(samples, sizeof(int16_t) * 2, frames, rec->file) != frames)
        return false;
#else
    for (size_t i = 0; i < frames * 2; ++i)
        if (!u16le(rec->file, (uint16_t)samples[i])) return false;
#endif
    rec->frames_written += frames;
    return true;
}

bool wav_recorder_stop(wav_recorder_t *rec)
{
    if (!rec || !rec->active) return false;
    const uint64_t frames = rec->frames_written;
    const uint32_t bytes = (uint32_t)(frames * 4);
    FILE *f = rec->file;
    rec->file = NULL;
    rec->active = false;
    bool ok = fseek(f, 4, SEEK_SET) == 0 &&
              u32le(f, bytes + 36) &&
              fseek(f, 40, SEEK_SET) == 0 &&
              u32le(f, bytes);
    if (fclose(f) != 0) ok = false;
    fprintf(stderr, "WAV: %s, %" PRIu64 " stereo samples, %u Hz\n",
            ok ? "saved" : "FAILED", frames, rec->sample_rate);
    return ok;
}
