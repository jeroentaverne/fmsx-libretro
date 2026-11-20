/*
 * MSX primary/secondary slot rules shared by memory access and regression
 * tests. Only primary slots 0 and 3 can be expanded in this fMSX machine
 * model. MSX1 has no expanded slot 0; MSX2/MSX2+ does.
 *
 * Compare openMSX: MSXCPUInterface::readMemSlow/writeMemSlow only intercept
 * FFFFh if the current page-3 primary slot is expanded.
 */
#ifndef FMSX_SLOTLOGIC_H
#define FMSX_SLOTLOGIC_H

#include <stdint.h>

static inline int MSXSlotIsExpanded(unsigned primarySlot, int isMSX1)
{
  return (primarySlot == 3) || ((primarySlot == 0) && !isMSX1);
}

static inline int MSXSecondarySlotRegisterVisible(
    uint16_t address, unsigned page3PrimarySlot, int isMSX1)
{
  return (address == 0xFFFF) &&
         MSXSlotIsExpanded(page3PrimarySlot, isMSX1);
}

#endif /* FMSX_SLOTLOGIC_H */
