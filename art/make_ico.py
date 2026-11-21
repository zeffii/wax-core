"""Builds art/wax.ico (the Windows program icon) from art/icons/wax-icon.svg: every size is rendered from the vector with its own antialiasing
(not a big picture shrunk), then packed as PNG entries. Needs rsvg-convert. Run: python3 art/make_ico.py"""
import os, struct, subprocess, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(HERE, "icons", "wax-icon.svg")
SIZES = [16, 24, 32, 48, 64, 128, 256]

def render(size):
    with tempfile.TemporaryDirectory() as tmp:
        png = os.path.join(tmp, "icon.png")
        subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), SVG, "-o", png], check=True)
        return open(png, "rb").read()

images = [(size, render(size)) for size in SIZES]
header = struct.pack("<HHH", 0, 1, len(images))
offset = 6 + 16 * len(images)
entries, data = b"", b""
for size, png in images:
    entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(png), offset + len(data))   # (256 is stored as 0)
    data += png
open(os.path.join(HERE, "wax.ico"), "wb").write(header + entries + data)
