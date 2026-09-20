"""Optional hash-verified Nokia 3210 v6.00 business-card probes (Unicorn)."""

import argparse
import hashlib
from pathlib import Path

from test_operator_logo_rom import Probe, SHA256
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1


class CardProbe(Probe):
    def __init__(self, rom):
        super().__init__(rom)
        self.cpu.mem_write(0x112510, b"\x01\x01\x01\x01")
        self.received = None

    def hook(self, cpu, address, size, context):
        if address == 0x298D72:
            obj = cpu.reg_read(UC_ARM_REG_R1)
            name = bytes(cpu.mem_read(obj, 17)).split(b"\0", 1)[0]
            number = bytes(cpu.mem_read(obj + 17, 32)).split(b"\0", 1)[0]
            self.received = cpu.reg_read(UC_ARM_REG_R0), name, number
            cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR) | 1)
        else:
            super().hook(cpu, address, size, context)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path)
    rom = parser.parse_args().rom.read_bytes()
    assert hashlib.sha256(rom).hexdigest() == SHA256
    for mode, prefix in enumerate((b"\x04\x04\x02\xe2\x00", b"//SCKE2 ", b"")):
        for name in (b"Alice", b""):
            probe = CardProbe(rom)
            probe.cpu.mem_write(0x111AB0, name + b"\0")
            probe.cpu.mem_write(0x111ADC, b"+123456789\0")
            count = probe.call(0x287BB0, 0x120000, mode, 140)
            body = b"BEGIN:VCARD\r\n" + (b"N:" + name + b"\r\n" if name else b"")
            body += b"TEL:+123456789\r\nEND:VCARD\r\n"
            assert bytes(probe.cpu.mem_read(0x120000, count)) == prefix + body
    for port in (0xE2, 0x23F4, 0xC004, 0x157D, 0x1585):
        probe = CardProbe(rom)
        body = b"BEGIN:VCARD\r\nN:Alice\r\nTEL:+123456789\r\nEND:VCARD\r\n"
        probe.cpu.mem_write(0x120000, body + bytes(16))
        probe.call(0x29E328, port, 0x120000, len(body), 0)
        expected = (2, b"Alice", b"+123456789") if port in (0xE2, 0x23F4, 0xC004) else None
        assert probe.received == expected
    print("PASS: original business-card encodings and vCard port dispatch")


if __name__ == "__main__":
    main()
