#!/usr/bin/env python3
"""Convert test_picture/b1c9c847-3bda4659.jpg to BGR C header for bare-metal tests."""
import os
from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
IMG_PATH = os.path.join(SCRIPT_DIR, '..', 'test_picture', 'b1c9c847-3bda4659.jpg')
OUT_PATH = os.path.join(SCRIPT_DIR, 'shen_test_image_bdd100k.h')

img = Image.open(IMG_PATH)
w, h = img.size
pixels = img.tobytes()  # RGB row-major

COLS = 24

with open(OUT_PATH, 'w') as f:
    f.write('// Auto-generated from test_picture/b1c9c847-3bda4659.jpg (BDD100K)\n')
    f.write('// PIL reads as RGB; stored here as BGR for OpenCV-compatible pipeline\n\n')
    f.write('#ifndef SHEN_TEST_IMAGE_BDD100K_H\n')
    f.write('#define SHEN_TEST_IMAGE_BDD100K_H\n\n')
    f.write('#include <stdint.h>\n\n')
    f.write(f'#define SHEN_IMG_ORIG_W {w}\n')
    f.write(f'#define SHEN_IMG_ORIG_H {h}\n')
    f.write(f'#define SHEN_IMG_ORIG_SIZE ({w} * {h} * 3)\n\n')
    f.write('/* BGR raw pixels, row-major [H][W][3] */\n')
    f.write(f'static const uint8_t shen_raw_image_bgr[{h} * {w} * 3] = {{\n')

    total = w * h
    buf = []
    for i in range(total):
        r = pixels[i * 3 + 0]
        g = pixels[i * 3 + 1]
        b = pixels[i * 3 + 2]
        buf.extend([b, g, r])
        if len(buf) >= COLS:
            line = ', '.join(f'0x{v:02x}' for v in buf[:COLS])
            f.write(f'  {line},\n')
            buf = buf[COLS:]
    if buf:
        line = ', '.join(f'0x{v:02x}' for v in buf)
        f.write(f'  {line}\n')
    f.write('};\n\n')
    f.write('#endif\n')

fsize = os.path.getsize(OUT_PATH)
print(f'Generated {OUT_PATH}: {fsize / 1024 / 1024:.1f} MB, image {w}x{h}')
