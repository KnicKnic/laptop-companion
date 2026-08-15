#!/usr/bin/env python3
"""Render a few Lucide SVGs into freeink::Icon C structs.

Mirrors freeink/libs/assets/Icons/tools/gen_icons.py but rasterises with svglib +
reportlab instead of rsvg-convert, so no native librsvg is needed, and fetches the
SVGs straight from the Lucide repo so the icon submodule does not have to be
checked out. Only emits the icons named on the command line, so the existing
generated header stays untouched -- append the output to src/icons/CompanionIcons.h
and add the aliases to src/icons/companion-icons.txt.

Requires: pip install svglib reportlab rlPyCairo pillow

Usage:
    python scripts/gen_extra_icons.py out.h thumbs_up=thumbs-up heart=heart
    python scripts/preview_icons.py out.h 28      # eyeball the result
"""
import io
import re
import sys
import urllib.request

from PIL import Image
from reportlab.graphics import renderPM
from svglib.svglib import svg2rlg

THRESHOLD = 110
RAW = "https://raw.githubusercontent.com/lucide-icons/lucide/main/icons/{}.svg"


def fetch(name):
    with urllib.request.urlopen(RAW.format(name), timeout=30) as r:
        return r.read().decode("utf-8")


def prepare(svg_text, stroke_width):
    # svglib cannot resolve `currentColor`, and the default 2px stroke thins out badly
    # once the render is downsampled, so both are pinned here.
    svg_text = svg_text.replace('stroke="currentColor"', 'stroke="#000000"')
    svg_text = svg_text.replace('fill="currentColor"', 'fill="#000000"')
    svg_text = re.sub(r'stroke-width="[^"]*"', f'stroke-width="{stroke_width}"', svg_text)
    return svg_text


def rasterize(svg_text, px):
    # Render large and downsample: reportlab's rasteriser aliases badly at 28px.
    supersample = 8
    big = px * supersample
    stroke = max(2.0, 2.0 * (24.0 / px) * 1.15)
    drawing = svg2rlg(io.BytesIO(prepare(svg_text, stroke).encode("utf-8")))
    scale = big / drawing.width
    drawing.width = big
    drawing.height = big
    drawing.scale(scale, scale)
    png = renderPM.drawToString(drawing, fmt="PNG", bg=0xFFFFFF)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    bg.paste(img, mask=img.split()[3])
    return bg.convert("L").resize((px, px), Image.LANCZOS)


def pack(img, px):
    pix = img.load()
    data = []
    sum_y = 0
    count = 0
    for y in range(px):
        for xb in range(0, px, 8):
            byte = 0
            for b in range(8):
                x = xb + b
                white = 1
                if x < px and pix[x, y] < THRESHOLD:
                    white = 0
                    sum_y += y
                    count += 1
                byte |= white << (7 - b)
            data.append(byte)
    center = round(sum_y / count) if count else px // 2
    return data, center, count


def ident(name):
    return re.sub(r"[^A-Za-z0-9]", "_", name)


def main():
    if len(sys.argv) < 3:
        print("usage: gen_extra_icons.py out.h alias=lucide [alias=lucide ...]")
        return 1

    out_path = sys.argv[1]
    entries = []
    for arg in sys.argv[2:]:
        alias, lucide = arg.split("=", 1) if "=" in arg else (arg, arg)
        entries.append((alias, lucide))

    sizes = [28, 36, 48]
    lines = []
    for alias, lucide in entries:
        try:
            svg = fetch(lucide)
        except Exception as exc:  # noqa: BLE001
            print(f"ERROR fetching {lucide}: {exc}", file=sys.stderr)
            return 1

        lines.append(f"// {alias}  (lucide: {lucide})")
        for px in sizes:
            data, center, count = pack(rasterize(svg, px), px)
            if count == 0:
                print(f"ERROR: {alias}@{px} rendered blank", file=sys.stderr)
                return 1
            arr = f"icon_{ident(alias)}_{px}_bits"
            body = ", ".join(f"0x{b:02X}" for b in data)
            lines.append(f"static const uint8_t {arr}[] = {{{body}}};")
            lines.append(
                f"static const freeink::Icon icon_{ident(alias)}_{px} = "
                f"{{{px}, {px}, {center}, {arr}}};"
            )
            print(f"  {alias}@{px}: {count} dark px, center={center}")
        lines.append("")

    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
