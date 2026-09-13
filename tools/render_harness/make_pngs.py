#!/usr/bin/env python3
"""Convert the harness PBM dumps to scaled PNGs + a labelled contact sheet."""
import glob
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
SCALE = 5
W, H = 84, 48
PAD = 6
LABEL_H = 14


def read_pbm(path):
    with open(path) as f:
        toks = f.read().split()
    assert toks[0] == "P1"
    w, h = int(toks[1]), int(toks[2])
    bits = [int(t) for t in toks[3:3 + w * h]]
    img = Image.new("L", (w, h), 255)
    px = img.load()
    for i, b in enumerate(bits):
        # 1 = lit pixel -> dark on the LCD
        px[i % w, i // w] = 0 if b else 200  # 200 = greenish-grey LCD background feel
    return img


def scaled(img):
    return img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST)


def main():
    pbms = sorted(glob.glob(os.path.join(OUT, "*.pbm")))
    tiles = []
    for p in pbms:
        name = os.path.splitext(os.path.basename(p))[0]
        img = scaled(read_pbm(p))
        img.save(os.path.join(OUT, name + ".png"))
        tiles.append((name, img))
        print("png:", name)

    # contact sheet, 3 columns
    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    tw, th = W * SCALE, H * SCALE
    cw = tw + PAD * 2
    ch = th + LABEL_H + PAD * 2
    sheet = Image.new("RGB", (cw * cols, ch * rows), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 11)
    except Exception:
        font = ImageFont.load_default()
    for idx, (name, img) in enumerate(tiles):
        r, c = divmod(idx, cols)
        x = c * cw + PAD
        y = r * ch + PAD
        sheet.paste(Image.new("RGB", (tw, th), (255, 255, 255)), (x, y))
        sheet.paste(img.convert("RGB"), (x, y))
        draw.rectangle([x, y, x + tw - 1, y + th - 1], outline=(120, 120, 120))
        draw.text((x, y + th + 2), name, fill=(230, 230, 230), font=font)
    sheet_path = os.path.join(OUT, "_contact_sheet.png")
    sheet.save(sheet_path)
    print("contact sheet:", sheet_path)


if __name__ == "__main__":
    main()
