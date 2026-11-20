/* Slot register behavior: matches openMSX's expanded-slot checks. */
#include "../fMSX/SlotLogic.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
  for (unsigned primarySlot = 0; primarySlot != 4; ++primarySlot)
  {
    for (int isMSX1 = 0; isMSX1 != 2; ++isMSX1)
    {
      const int expectedExpanded = (primarySlot == 3) ||
                                   ((primarySlot == 0) && !isMSX1);
      assert(!!MSXSlotIsExpanded(primarySlot, isMSX1) ==
             !!expectedExpanded);
      assert(!!MSXSecondarySlotRegisterVisible(
                 0xFFFF, primarySlot, isMSX1) == !!expectedExpanded);
      assert(!MSXSecondarySlotRegisterVisible(
                 0xFFFE, primarySlot, isMSX1));
      assert(!MSXSecondarySlotRegisterVisible(
                 0x0000, primarySlot, isMSX1));
    }
  }
  puts("PASS: expanded primary slot and FFFFh register visibility");
  return 0;
}
