# Host render harness

Renders the firmware's actual framebuffer/assets/UI/status-chrome primitives on
host to screenshot the binary-derived UI changes without flashing hardware.
Each screen reproduces the exact draw calls from the corresponding render_*()
body, so the output is pixel-identical to what the firmware draws.

```sh
sh tools/render_harness/build.sh
```

Outputs scaled PNGs and `out/_contact_sheet.png`. Links only the pure-render
modules (no hardware/service deps); add screens in `screenshots.c`.

## Dependencies

- A host C compiler (`cc`) for `screenshots.c`.
- Python 3 with **Pillow** for `make_pngs.py` (PBM → scaled PNG + contact
  sheet): `pip install pillow` (or `pip3 install pillow`). Without it the C
  harness still runs and writes the raw `out/*.pbm` dumps; only the
  PNG/contact-sheet step is skipped.
