"""Optional v6.00 ROM probes; requires Unicorn and the user's original image."""

import argparse
import hashlib
import struct
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_BIG_ENDIAN, UC_MODE_THUMB
from unicorn.arm_const import (
    UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
    UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_CPU_ARM_TI925T,
)

SHA256 = "0ab99ed809232d2c14c6587c9fc904323358f223e978b6215d15475a62362f6c"


class Probe:
    def __init__(self, rom, network=b"\x13\x01\x84"):
        self.cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_BIG_ENDIAN)
        self.cpu.ctl_set_cpu_model(UC_CPU_ARM_TI925T)
        self.cpu.mem_map(0x100000, 0x100000)
        self.cpu.mem_map(0x200000, (len(rom) + 4095) & ~4095)
        self.cpu.mem_write(0x200000, rom)
        self.cpu.mem_write(0x11FD16, b"\x02")
        self.cpu.mem_write(0x110900, b"\0\1\0\0")
        self.heap = 0x140000
        self.record = network + bytes(133)
        self.draws = []
        self.cpu.hook_add(UC_HOOK_CODE, self.hook)

    def hook(self, cpu, address, size, _):
        if address == 0x26AFE0:  # malloc
            size = (cpu.reg_read(UC_ARM_REG_R0) + 3) & ~3
            cpu.mem_write(self.heap, bytes(size))
            cpu.reg_write(UC_ARM_REG_R0, self.heap)
            self.heap += size
        elif address == 0x2ABBF4:  # EEPROM read, key 0x744
            assert cpu.reg_read(UC_ARM_REG_R1) == 0x744
            cpu.mem_write(cpu.reg_read(UC_ARM_REG_R0), self.record)
        elif address == 0x25F3B6:  # LCD bitmap submission
            data = cpu.mem_read(cpu.reg_read(UC_ARM_REG_R0), 12)
            self.draws.append((data[4], data[5]))
        elif address not in (
            0x26ABF8, 0x289734,  # free / bitmap transpose (not PLMN matching)
            0x2AA8FA, 0x2AA914, 0x2697AA, 0x2695F4,  # locks and timers
            0x25EEC0, 0x25FDE4, 0x25FF1E, 0x29BC70, 0x2ABD7C,  # LCD flush
        ):
            return
        # ARMv4 Thumb MOV lr,pc / BX callbacks need an explicit Thumb return.
        cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR) | 1)

    def call(self, address, *args):
        for register, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1,
                                    UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.cpu.reg_write(register, value)
        self.cpu.reg_write(UC_ARM_REG_SP, 0x1FF000)
        self.cpu.mem_write(0x1FF000, bytes(16))
        self.cpu.reg_write(UC_ARM_REG_LR, 0x1FE001)
        self.cpu.emu_start(address | 1, 0x1FE000, count=1000000)
        assert self.cpu.reg_read(UC_ARM_REG_PC) == 0x1FE000, "probe did not return"
        return self.cpu.reg_read(UC_ARM_REG_R0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    args = parser.parse_args()
    rom = args.rom.read_bytes()
    assert hashlib.sha256(rom).hexdigest() == SHA256, "expected Nokia 3210 v6.00 image"
    cases = [
        ("130184", "130184", True),   # 311-480
        ("130062", "130062", True),   # 310-260
        ("130014", "130014", True),   # 310-410
        ("13f062", "130062", False),  # 310-26 is not 310-260
        ("131184", "130184", False),  # 311-481 is not 311-480
        ("42f450", "42f450", True),
        ("42f4f5", "42f450", True),   # original single-digit compatibility
    ]
    for stored, serving, matches in cases:
        probe = Probe(rom, bytes.fromhex(stored))
        probe.cpu.mem_write(0x110000, bytes.fromhex(serving))
        assert bool(probe.call(0x2897DA, 0x110000)) == matches
    for preview in (False, True):
        probe = Probe(rom)
        probe.cpu.mem_write(0x110000, struct.pack(">IIBBBB", 0x110100, 0, 72, 14, 0, 0))
        probe.cpu.mem_write(0x110100, b"\xff" * 144)
        if preview:
            probe.call(0x243550, 0x4B, 0, 0, 0x110000)
        else:
            probe.call(0x2A4A04, 0x0F, 0x110000, 0, 0)
        probe.call(0x243A2C)
        assert probe.draws and all(xy == (6, 7) for xy in probe.draws), probe.draws
    print("PASS: original ROM PLMN matching and logo preview/standby placement")


if __name__ == "__main__":
    main()
