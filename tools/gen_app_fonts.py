#!/usr/bin/env python3
"""Generate the application's LVGL CJK font subsets.

The inventory is the 6763 GB2312 hanzi plus printable ASCII and the
punctuation used by the device UI.  Keeping the inventory in the repository
makes the generated fonts reproducible with a pinned lv_font_conv version.

Usage:
    python3 tools/gen_app_fonts.py [--check]

Requires Node.js/npx and a CJK font.  Set APP_FONT_PATH to override the font.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "assets" / "fonts"
INVENTORY = OUT_DIR / "charset.txt"
DEFAULT_FONT = Path("/usr/share/fonts/opentype/noto-cjk-otf/NotoSansCJKsc-Regular.otf")
LV_FONT_CONV = "lv_font_conv@1.5.3"

# Punctuation and symbols intentionally supported by the UI (in addition to
# printable ASCII and the GB2312 hanzi inventory).  They all exist in
# Noto Sans CJK SC.
EXTRA_SYMBOLS = (
    "，。、；：？！“”‘’（）《》【】—…·～￥％°"
    "０１２３４５６７８９"
)


def gb2312_hanzi() -> str:
    chars: list[str] = []
    for high in range(0xB0, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                char = bytes((high, low)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            chars.append(char)
    return "".join(chars)


def build_inventory() -> str:
    ascii_chars = "".join(chr(cp) for cp in range(0x20, 0x7F))
    chars = ascii_chars + gb2312_hanzi() + EXTRA_SYMBOLS
    # Preserve order while removing duplicates.
    return "".join(dict.fromkeys(chars))


def generate(size: int, font: Path, symbols: str, check: bool) -> None:
    output = OUT_DIR / f"app_font_{size}.c"
    cmd = [
        "npx", "--yes", LV_FONT_CONV,
        "--font", str(font),
        "--symbols", symbols,
        "--size", str(size),
        "--bpp", "4",
        "--format", "lvgl",
        "--no-compress",
        "--lv-font-name", f"app_font_{size}",
        "--lv-include", "lvgl.h",
        "--output", str(output),
    ]
    if check:
        print("would run:", " ".join(cmd[:8]), "...", len(symbols), "symbols")
        return
    print(f"generating {output.name} ({size}px, {len(symbols)} glyphs)", flush=True)
    subprocess.run(cmd, cwd=ROOT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="print the inventory size only")
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    symbols = build_inventory()
    hanzi = gb2312_hanzi()
    print(f"inventory: {len(symbols)} glyphs ({len(hanzi)} GB2312 hanzi)")
    if len(hanzi) != 6763:
        print(f"ERROR: expected 6763 GB2312 hanzi, got {len(hanzi)}", file=sys.stderr)
        return 1

    if args.check:
        return 0

    INVENTORY.write_text(symbols, encoding="utf-8")

    font = Path(os.environ.get("APP_FONT_PATH", DEFAULT_FONT))
    if not font.is_file():
        print(f"ERROR: font not found: {font}", file=sys.stderr)
        print("Set APP_FONT_PATH to a licensed CJK TTF/OTF with GB2312 coverage.", file=sys.stderr)
        return 1
    if not shutil.which("npx"):
        print("ERROR: npx is not available; install Node.js first.", file=sys.stderr)
        return 1

    for size in (16, 20):
        generate(size, font, symbols, args.check)
    print("fonts generated under assets/fonts/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
