/* Direct PNG capture of the unscaled fMSX video callback.
 * RGB565LE -> RGB888, losslessly compressed by zlib/DEFLATE.
 * Uses no SDL window pixels and never changes emulator state.
 */
#define _POSIX_C_SOURCE 200809L
#include "png_screenshot.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

static bool write_bytes(FILE *fp, const void *data, size_t length)
{
    return fwrite(data, 1, length, fp) == length;
}
static bool write_be32(FILE *fp, uint32_t number)
{
    const uint8_t bytes[] = { (uint8_t)(number >> 24),
                              (uint8_t)(number >> 16),
                              (uint8_t)(number >> 8),
                              (uint8_t)number };
    return write_bytes(fp, bytes, sizeof bytes);
}
static bool write_chunk(FILE *fp, const char type[4],
                        const uint8_t *data, uint32_t length)
{
    uLong crc = crc32(0, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (length) crc = crc32(crc, data, length);
    return write_be32(fp, length) && write_bytes(fp, type, 4) &&
           (!length || write_bytes(fp, data, length)) &&
           write_be32(fp, (uint32_t)crc);
}

bool fmsx_write_png(const char *path, const void *rgb565,
                    unsigned width, unsigned height, size_t pitch)
{
    if (!path || !*path || !rgb565 || !width || !height ||
        width > 4096 || height > 4096 || pitch < (size_t)width * 2 ||
        pitch > SIZE_MAX / height) return false;
    const size_t stride = (size_t)width * 3 + 1;
    if (height > SIZE_MAX / stride) return false;
    const size_t raw_size = stride * height;
    if (raw_size > UINT32_MAX) return false;
    uint8_t *raw = malloc(raw_size);
    if (!raw) return false;

    const uint8_t *pixels = (const uint8_t *)rgb565;
    for (unsigned y = 0; y < height; ++y)
    {
        uint8_t *dst = raw + (size_t)y * stride;
        const uint8_t *src = pixels + (size_t)y * pitch;
        dst[0] = 0;  /* PNG filter = None */
        ++dst;
        for (unsigned x = 0; x < width; ++x)
        {
            const uint16_t px = (uint16_t)src[2 * x] |
                                ((uint16_t)src[2 * x + 1] << 8);
            const uint8_t r = (uint8_t)((px >> 11) & 31);
            const uint8_t g = (uint8_t)((px >> 5) & 63);
            const uint8_t b = (uint8_t)(px & 31);
            dst[3 * x]     = (uint8_t)((r << 3) | (r >> 2));
            dst[3 * x + 1] = (uint8_t)((g << 2) | (g >> 4));
            dst[3 * x + 2] = (uint8_t)((b << 3) | (b >> 2));
        }
    }

    uLongf compressed_size = compressBound((uLong)raw_size);
    uint8_t *compressed = malloc(compressed_size);
    if (!compressed)
    {
        free(raw);
        return false;
    }
    const int compressed_ok = compress2(compressed, &compressed_size,
                                        raw, (uLong)raw_size, Z_BEST_COMPRESSION);
    free(raw);
    if (compressed_ok != Z_OK || compressed_size > UINT32_MAX)
    {
        free(compressed);
        return false;
    }

    /* O_EXCL avoids destroying an existing screenshot on name collision. */
    const int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0)
    {
        perror("PNG screenshot open");
        free(compressed);
        return false;
    }
    FILE *fp = fdopen(fd, "wb");
    if (!fp)
    {
        close(fd);
        unlink(path);
        free(compressed);
        return false;
    }
    static const uint8_t signature[8] =
        { 137, 80, 78, 71, 13, 10, 26, 10 };
    const uint8_t ihdr[13] = {
        (uint8_t)(width >> 24), (uint8_t)(width >> 16),
        (uint8_t)(width >> 8), (uint8_t)width,
        (uint8_t)(height >> 24), (uint8_t)(height >> 16),
        (uint8_t)(height >> 8), (uint8_t)height,
        8, 2, 0, 0, 0 /* truecolor RGB, 8-bit, no interlace */
    };
    bool ok = write_bytes(fp, signature, sizeof signature) &&
              write_chunk(fp, "IHDR", ihdr, sizeof ihdr) &&
              write_chunk(fp, "IDAT", compressed, (uint32_t)compressed_size) &&
              write_chunk(fp, "IEND", NULL, 0);
    if (fclose(fp) != 0) ok = false;
    free(compressed);
    if (!ok) unlink(path);
    return ok;
}
