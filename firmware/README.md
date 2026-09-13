# Original firmware drop-in folder

Place the Nokia 3210 NSE-8 v6.00 firmware here and the build regenerates
every derived asset (UI strings, fonts, bitmaps, tones, T9 dictionaries)
from it automatically. Any ONE of these works — inputs are verified by
hash, so repackaged archives are fine as long as the flash bytes match:

| File | SHA-256 |
| --- | --- |
| `nokia_3210_6.00.zip` | `a635b2c6675077702283fc24f8a160ce6a9a5f77ba1b1c24848ee2ce2092b673` |
| `3210600c.fls` (2 MiB full flash) | `0ab99ed809232d2c14c6587c9fc904323358f223e978b6215d15475a62362f6c` |
| any zip containing either of the above | (inner hash is what counts) |

The archive.org item `nokia-phone-firmwares` carries a byte-identical
`nokia_3210_6.00.zip` inside its `nokia_firmwares.zip`.

Optional: `nse8_600.exe` (SHA-256
`00550789e3870c2b32cbaed2b8e9ff17f121145394e84fafece94fc7963e2c01`, the
original WinTesla flash installer) placed beside the firmware unlocks the
full regional language inventory (34 LTR languages + extra T9
dictionaries) for `SISU_ASSET_LANGS`/`SISU_T9_LANGS` selections beyond the
shipped set.

Optional T9 donor: `NPE3_527.exe` (SHA-256
`d2a8197580f4194e70af16bffaa20eca806bfcf240561fa1368cad4b6d176f90`, the
Nokia 6210 v5.27 sales package, publicly archived in the archive.org item
`Nokia_DCT3_firmwares`) adds Polish and Turkish T9 dictionaries — same
DCT-3 Tegic generation, verified byte-compatible; Czech comes from the
main archive itself. Only its T9 dictionaries are used: fonts, strings
and every other asset in it belong to different hardware and are ignored.

The generated assets are never committed (they embed Nokia/Tegic-derived
data), so a fresh checkout needs the firmware here once. After that the
build works without it from the previously generated files; with firmware
present they are always regenerated from it. Nothing in this folder is
ever committed either.
