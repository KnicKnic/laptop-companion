#!/usr/bin/env python3
"""ASCII preview of packed 1bpp icons, so the generated art can be eyeballed."""
import re
import sys


def main():
    src = open(sys.argv[1], encoding="utf-8").read()
    want = sys.argv[2] if len(sys.argv) > 2 else "28"
    for m in re.finditer(
        r"static const uint8_t (icon_\w+?_(\d+))_bits\[\] = \{([^}]*)\};", src
    ):
        name, px, body = m.group(1), int(m.group(2)), m.group(3)
        if str(px) != want:
            continue
        data = [int(b.strip(), 16) for b in body.split(",")]
        stride = (px + 7) // 8
        print(name)
        for y in range(px):
            row = ""
            for x in range(px):
                byte = data[y * stride + (x >> 3)]
                bit = (byte >> (7 - (x & 7))) & 1
                row += ".." if bit else "##"
            print("  " + row)
        print()


if __name__ == "__main__":
    main()
