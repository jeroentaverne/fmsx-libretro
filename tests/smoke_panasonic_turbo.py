#!/usr/bin/env python3
"""Panasonic switched-I/O turbo regression against the real fMSX callbacks."""
import ctypes
import pathlib
import sys

def check(path):
    core = ctypes.CDLL(str(path.resolve()))
    core.InZ80.argtypes = [ctypes.c_uint16]
    core.InZ80.restype = ctypes.c_uint8
    core.OutZ80.argtypes = [ctypes.c_uint16, ctypes.c_uint8]
    core.OutZ80.restype = None
    core.fmsx_cpu_frequency_hz.restype = ctypes.c_int
    hp = ctypes.c_int.in_dll(core, "HPeriod")
    assert core.fmsx_cpu_frequency_hz() == 3579545
    assert core.InZ80(0x40) == 0xff
    core.OutZ80(0x41, 0)
    assert core.fmsx_cpu_frequency_hz() == 3579545
    core.OutZ80(0x40, 9)
    core.OutZ80(0x41, 0)
    assert core.InZ80(0x40) == 0xff
    core.OutZ80(0x40, 8)
    assert core.InZ80(0x40) == 0xf7
    assert core.InZ80(0x41) == 0xfb
    core.OutZ80(0x41, 0)
    assert core.InZ80(0x41) == 0xfa
    assert core.fmsx_cpu_frequency_hz() == 8000000
    assert hp.value == (228 * 8000000 + 3579545 // 2) // 3579545
    core.OutZ80(0x41, 1)
    assert core.InZ80(0x41) == 0xfb
    assert core.fmsx_cpu_frequency_hz() == 3579545
    assert hp.value == 228
    core.OutZ80(0x40, 0)
    assert core.InZ80(0x40) == 0xff
    print("PASS: Panasonic device 08h switched I/O and 8MHz CPU timing")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: smoke_panasonic_turbo.py fmsx_libretro.so")
    check(pathlib.Path(sys.argv[1]))
