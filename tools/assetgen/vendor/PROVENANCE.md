# Vendored extractor provenance

All files here run only at asset-generation time on the developer's machine;
none of their code ships in the firmware. Everything below is the repository
author's own work (from the reverse_3210 project) and is released under the
repository license:

- `extract_ppm_chunks.py` — PPM chunk/subchunk parser + TONE resource export
- `extract_ppm_strings.py` — PPM TEXT decoding to per-language JSON
- `extract_system_tones.py` — MCU-embedded system tone export
- `extract_embedded_ftest_font.py` — FS4 ftest/plain font record extractor
- `build_firmware_assets.py` — PBM -> packed-glyph JSON atlases

The system-bitmap and font PBM exporters (`../extract_dct3.py`) are an
original reimplementation written for this project from DCT-3 format
knowledge; the previously vendored third-party C extractors were removed
after the reimplementation was verified PBM-for-PBM identical (274 bitmaps,
1,710 glyphs) and the end-to-end pipeline byte-exact.
