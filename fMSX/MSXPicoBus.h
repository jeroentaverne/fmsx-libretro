/* Optional MSX Pico cartridge bridge, provided by the private msxpico_sw
 * host-simulator build. No MSX Pico firmware code is distributed by fMSX.
 * The hooks return false when fMSX must handle the access normally.
 */
#ifndef FMSX_MSXPICOBUS_H
#define FMSX_MSXPICOBUS_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool msxpico_fmsx_mem_read(uint16_t address, uint8_t *value);
bool msxpico_fmsx_mem_write(uint16_t address, uint8_t value);
bool msxpico_fmsx_io_read(uint16_t port, uint8_t *value);
bool msxpico_fmsx_io_write(uint16_t port, uint8_t value);
bool msxpico_fmsx_attach_from_env(void);
void msxpico_fmsx_pace(unsigned frames_per_second);
void msxpico_fmsx_stop(void);
#ifdef __cplusplus
}
#endif
#endif
