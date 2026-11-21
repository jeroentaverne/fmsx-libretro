/* Lossless PNG screenshot writer for the native fMSX RGB565 framebuffer. */
#ifndef FMSX_PNG_SCREENSHOT_H
#define FMSX_PNG_SCREENSHOT_H
#include <stdbool.h>
#include <stddef.h>
bool fmsx_write_png(const char *path, const void *rgb565,
                    unsigned width, unsigned height, size_t pitch);
#endif
