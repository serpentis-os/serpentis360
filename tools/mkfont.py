# turns DejaVu Sans Mono into kernel/font.c: one byte of coverage per pixel, 10x20 cells,
# printable ascii, then the pacman sprites. a sprite is a 20x20 square cut into two cells.
# run it again to change anything. needs pillow
import sys
from PIL import Image, ImageDraw, ImageFont

TTF = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else "kernel/font.c"
W, H, SIZE = 10, 20, 16
S = 4 * H

font = ImageFont.truetype(TTF, SIZE)
ascent, descent = font.getmetrics()
top = (H - (ascent + descent)) // 2

glyphs = []
for code in range(32, 127):
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    left = (W - font.getlength(chr(code))) / 2
    d.text((left, top), chr(code), font=font, fill=255)
    glyphs.append(img)

def circle(d, cx, cy, r, fill=255):
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=fill)

def dot(d):
    circle(d, S / 2, S / 2, 9)

def pellet(d):
    circle(d, S / 2, S / 2, 24)

def pac(angle):
    def draw(d):
        if angle is None:
            circle(d, S / 2, S / 2, 36)
        else:
            d.pieslice((4, 4, S - 4, S - 4), angle + 35, angle - 35 + 360, fill=255)
    return draw

def ghost(scared):
    def draw(d):
        d.pieslice((6, 4, S - 6, S - 8), 180, 360, fill=255)
        d.rectangle((6, S / 2 - 6, S - 6, S - 14), fill=255)
        for i in range(4):
            x = 6 + i * (S - 12) / 4
            d.polygon([(x, S - 15), (x + (S - 12) / 8, S - 4), (x + (S - 12) / 4, S - 15)], fill=255)
        if scared:
            circle(d, 28, 32, 5, 0)
            circle(d, 52, 32, 5, 0)
            pts = [(18 + i * 7, 52 + (5 if i % 2 else 0)) for i in range(7)]
            d.line(pts, fill=0, width=4)
        else:
            circle(d, 27, 32, 9, 0)
            circle(d, 53, 32, 9, 0)
    return draw

def wall(d):
    d.rectangle((0, 0, S, S), fill=255)

# the order is the SPR_ numbers in screen.h
SPRITES = [dot, pellet, pac(0), pac(180), pac(270), pac(90), pac(None), ghost(False), ghost(True), wall]

for draw in SPRITES:
    tile = Image.new("L", (S, S), 0)
    draw(ImageDraw.Draw(tile))
    small = tile.resize((2 * W, H), Image.LANCZOS)
    glyphs.append(small.crop((0, 0, W, H)))
    glyphs.append(small.crop((W, 0, 2 * W, H)))

with open(OUT, "w", newline="\n") as f:
    f.write("#include <stdint.h>\n\n")
    f.write("// DejaVu Sans Mono at 16 px plus the pacman sprites, made by tools/mkfont.py.\n")
    f.write("// Bitstream Vera license, see LICENSE.fonts\n\n")
    f.write("const uint8_t font_glyphs[%d][%d][%d] = {\n" % (len(glyphs), H, W))
    for img in glyphs:
        px = img.load()
        f.write("\t{\n")
        for y in range(H):
            f.write("\t\t{" + ", ".join(str(px[x, y]) for x in range(W)) + "},\n")
        f.write("\t},\n")
    f.write("};\n")

sheet = Image.new("L", (W * 32, H * 4), 0)
for i, g in enumerate(glyphs):
    sheet.paste(g, ((i % 32) * W, (i // 32) * H))
sheet.resize((W * 32 * 3, H * 4 * 3), Image.NEAREST).save("build/font_preview.png")
print(len(glyphs))
