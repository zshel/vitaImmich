#!/usr/bin/env python3
"""Generate a Web-Mercator tile pyramid (LODs) from a world map image and
pack it into one file for the Vita map view.

The source image covers a cropped-Mercator region: longitude spans its full
width, latitude band [MAP_IMG_MY0, MAP_IMG_MY1]. The default source carries a
baked-in 10° graticule, against which those bounds were fit (see map.c).
Each LOD level is the source scaled to a width, padded to whole 256px tiles,
and cut up; tiles are JPEG (decoded into pooled textures on device, the safe
path). Everything goes into assets/maptiles.pak:

  magic 'MTIL' (4)  u32 version  u32 tile_size  u32 nlevels
  per level:  u32 imgW imgH cols rows
  u32 ntiles
  per tile:   u16 level  u16 col  u16 row  u32 offset  u32 size
  <concatenated JPEG bytes>
offsets are from the start of the JPEG blob.
"""
import sys, struct, io
from PIL import Image

SRC = sys.argv[1] if len(sys.argv) > 1 else '/home/sads/Desktop/world map cities.jpeg'
OUT = sys.argv[2] if len(sys.argv) > 2 else '/home/sads/Code/vitaImmich/assets/maptiles.pak'
TILE = 256
QUALITY = 94
# level widths (px). The last entry (10496 ~= 1.5x the 7000px source) is a
# LANCZOS supersample: it adds no true detail beyond native, but lets a zoomed-in
# view sample crisp pre-upscaled tiles instead of the GPU bilinear-magnifying the
# native level, so city labels read sharper. heights follow aspect.
WIDTHS = [512, 1024, 2048, 4096, 7000, 10496]

src = Image.open(SRC).convert('RGB')
SW, SH = src.size
print('source', SW, SH, 'aspect', round(SW/SH, 4))

levels = []   # (imgW,imgH,cols,rows,Image-padded)
for w in WIDTHS:
    if w > 2 * SW:      # allow a modest supersample, but cap runaway upscales
        w = 2 * SW
    h = round(w * SH / SW)
    img = src.resize((w, h), Image.LANCZOS)
    cols = (w + TILE - 1) // TILE
    rows = (h + TILE - 1) // TILE
    # pad to whole tiles with the ocean colour so edge tiles aren't black
    bg = src.getpixel((4, SH // 2))  # a sea pixel (top-left corner area is sea)
    padded = Image.new('RGB', (cols * TILE, rows * TILE), bg)
    padded.paste(img, (0, 0))
    levels.append((w, h, cols, rows, padded))
    print('level', len(levels)-1, 'img', w, h, 'tiles', cols, 'x', rows, '=', cols*rows)

# de-dup identical level widths (when native < a planned width)
seen = set(); uniq = []
for L in levels:
    if L[0] in seen:
        continue
    seen.add(L[0]); uniq.append(L)
levels = uniq

blob = io.BytesIO()
index = []   # (level,col,row,offset,size)
for li, (w, h, cols, rows, padded) in enumerate(levels):
    for r in range(rows):
        for c in range(cols):
            tile = padded.crop((c*TILE, r*TILE, c*TILE+TILE, r*TILE+TILE))
            b = io.BytesIO(); tile.save(b, 'JPEG', quality=QUALITY)
            data = b.getvalue()
            off = blob.tell(); blob.write(data)
            index.append((li, c, r, off, len(data)))

with open(OUT, 'wb') as f:
    f.write(b'MTIL')
    f.write(struct.pack('<III', 1, TILE, len(levels)))
    for (w, h, cols, rows, _) in levels:
        f.write(struct.pack('<IIII', w, h, cols, rows))
    f.write(struct.pack('<I', len(index)))
    for (li, c, r, off, sz) in index:
        f.write(struct.pack('<HHHII', li, c, r, off, sz))
    f.write(blob.getvalue())

import os
print('levels', len(levels), 'tiles', len(index),
      'pak', round(os.path.getsize(OUT)/1e6, 2), 'MB')
