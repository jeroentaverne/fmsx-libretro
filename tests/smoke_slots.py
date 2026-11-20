#!/usr/bin/env python3
"""Exercise actual fMSX RdZ80/WrZ80 at the FFFFh expanded-slot boundary.

Uses an 8K test buffer for page 3 and needs no copyright ROMs or BIOS.
"""
import ctypes
import pathlib
import sys


def main(path):
    lib = ctypes.CDLL(str(path.resolve()))
    lib.RdZ80.argtypes = [ctypes.c_uint16]
    lib.RdZ80.restype = ctypes.c_uint8
    lib.WrZ80.argtypes = [ctypes.c_uint16, ctypes.c_uint8]
    lib.WrZ80.restype = None
    slots = (ctypes.c_uint8 * 4).in_dll(lib, "PSL")
    subslot_registers = (ctypes.c_uint8 * 4).in_dll(lib, "SSLReg")
    writable_pages = (ctypes.c_uint8 * 4).in_dll(lib, "EnWrite")
    mapped_8kb = (ctypes.c_void_p * 8).in_dll(lib, "RAM")
    machine_model = ctypes.c_int.in_dll(lib, "Mode")
    backing = (ctypes.c_uint8 * 8192)()

    def test_unexpanded(model, primary_slot):
        machine_model.value = model
        slots[3] = primary_slot
        subslot_registers[primary_slot] = 0x17
        writable_pages[3] = 1
        mapped_8kb[7] = ctypes.addressof(backing)
        backing[-1] = 0x65
        assert lib.RdZ80(0xFFFF) == 0x65
        lib.WrZ80(0xFFFF, 0x98)
        assert backing[-1] == 0x98
        assert subslot_registers[primary_slot] == 0x17

    def test_expanded(model, primary_slot):
        machine_model.value = model
        slots[3] = primary_slot
        subslot_registers[primary_slot] = 0x17
        assert lib.RdZ80(0xFFFF) == 0xE8
        lib.WrZ80(0xFFFF, 0xA4)
        assert subslot_registers[primary_slot] == 0xA4
        assert lib.RdZ80(0xFFFF) == 0x5B

    test_unexpanded(0, 0)  # MSX1: slot 0 is NOT expanded
    test_unexpanded(0, 1)
    test_unexpanded(1, 1)  # MSX2: cartridge slots not expanded
    test_unexpanded(1, 2)
    test_expanded(0, 3)    # MSX1: slot 3 is expanded
    test_expanded(1, 0)    # MSX2: slot 0 is expanded
    test_expanded(1, 3)
    test_expanded(2, 0)    # MSX2+: slot 0 is expanded
    print("PASS: actual fMSX FFFFh reads/writes in expanded and unexpanded slots")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: smoke_slots.py fmsx_libretro.so")
    main(pathlib.Path(sys.argv[1]))
