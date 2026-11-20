"""Builds art/wax.ico (the Windows program icon) from the paths in art/icons/wax-icon.svg. Needs Pillow. Run: python3 art/make_ico.py"""
import re
from PIL import Image, ImageDraw
SVG = open(__file__.replace("make_ico.py", "icons/wax-icon.svg")).read()
BG, FG, S = (22, 27, 31, 255), (79, 209, 184, 255), 1024
def logo_point(x, y, dx):   # path coordinates -> icon square (same transform as the svg)
    return ((x + dx - 70) * 0.3276 + 110) * S / 1000, ((y - 90) * 0.3276 + 366) * S / 1000
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((0, 0, S - 1, S - 1), radius=190 * S / 1000, fill=BG)
for dx, path in re.findall(r'translate\((\d+)\)" d="([^"]+)"', SVG):
    for k, sub in enumerate(p for p in path.split("Z") if p.strip()):
        pts = [logo_point(*map(float, m), int(dx)) for m in re.findall(r"[ML]\s*(-?[\d.]+)\s+(-?[\d.]+)", sub)]
        d.polygon(pts, fill=FG if k == 0 else BG)   # the second subpath is the hole in the A
img.save(__file__.replace("make_ico.py", "wax.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
