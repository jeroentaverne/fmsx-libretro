#!/usr/bin/env python3
"""Check MSX2+ and 2 MiB initial fMSX model before a BIOS is loaded."""
import ctypes
import pathlib
import sys

def main(path):
    core = ctypes.CDLL(str(path.resolve()))
    mode = ctypes.c_int.in_dll(core, "Mode").value
    ram_pages = ctypes.c_int.in_dll(core, "RAMPages").value
    vram_pages = ctypes.c_int.in_dll(core, "VRAMPages").value
    assert (mode & 3) == 2, f"Not MSX2+: mode={mode}"
    assert ram_pages == 128, f"Not 2 MiB RAM: {ram_pages} pages"
    assert vram_pages == 8, f"Not 128 KiB VRAM: {vram_pages} pages"
    print("PASS: MSX2+ V9958, 2 MiB RAM, 128 KiB VRAM")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: smoke_msx2p_profile.py fmsx_libretro.so")
    main(pathlib.Path(sys.argv[1]))
