#!/bin/sh
# Build and run the host render harness, then make scaled PNGs.
set -e
cd "$(dirname "$0")/../.."   # repo root
mkdir -p tools/render_harness/out
cc -std=c11 -Wall -I include \
   tools/render_harness/screenshots.c \
   src/ui/framebuffer.c src/ui/assets.c src/generated/assets_data.c \
   src/ui/ui.c src/ui/status_chrome.c \
   -o tools/render_harness/screenshots
./tools/render_harness/screenshots
python3 tools/render_harness/make_pngs.py
echo "PNGs + contact sheet in tools/render_harness/out/"
