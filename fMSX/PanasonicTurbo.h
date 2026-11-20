/* Panasonic Matsushita switched I/O: openMSX MSXDeviceSwitch/MSXMatsushita.
 * This MSX Pico simulator uses 8 MHz turbo instead of Panasonic's 5.37 MHz.
 */
#ifndef PANASONIC_TURBO_H
#define PANASONIC_TURBO_H
#include <stdint.h>
#include <stdbool.h>

enum { PANASONIC_SWITCH_ID = 0x08, TURBO_FREQUENCY_HZ = 8000000 };
typedef struct panasonic_turbo {
    uint8_t selected_device;
    bool enabled;
} panasonic_turbo_t;

static inline void panasonic_turbo_reset(panasonic_turbo_t *turbo)
{
    turbo->selected_device = 0;
    turbo->enabled = false;
}
static inline uint8_t panasonic_turbo_read(const panasonic_turbo_t *turbo,
                                          uint8_t port)
{
    if (turbo->selected_device != PANASONIC_SWITCH_ID) return 0xff;
    if (port == 0x40) return (uint8_t)~PANASONIC_SWITCH_ID;
    if (port == 0x41) return turbo->enabled ? 0xfa : 0xfb;
    return 0xff;
}
static inline bool panasonic_turbo_write(panasonic_turbo_t *turbo,
                                        uint8_t port, uint8_t value)
{
    if (port == 0x40)
    {
        turbo->selected_device = value;
        return false;
    }
    if (port != 0x41 || turbo->selected_device != PANASONIC_SWITCH_ID)
        return false;
    const bool changed = turbo->enabled != !(value & 1);
    turbo->enabled = !(value & 1);
    return changed;
}
#endif
