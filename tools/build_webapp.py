#!/usr/bin/env python3
"""Builds the on-device web app served by the PaperColor's Wi-Fi hotspot.

The frame does no image processing: the phone browser runs the very same
dither pipeline the desktop Photo Lab runs, and uploads a finished 4-bit BMP.
To keep that literally true rather than merely intended, this script lifts the
`#core` and `#shared` script blocks straight out of photo_lab.html at build
time and splices them into the mobile UI. There is no second copy of the
pipeline to keep in parity.

Usage:
    build_webapp.py --ui main/web/app.ui.html \
                    --lab tools/prepare_photo/photo_lab.html \
                    --out build/webapp/app.html

Writes both app.html and app.html.gz; the gzip is what gets embedded in the
firmware image and served with Content-Encoding: gzip.
"""

from __future__ import annotations

import argparse
import gzip
import re
import sys
from pathlib import Path

# Blocks lifted from photo_lab.html, in the order they must appear. `#core` is
# the parity-checked pipeline; `#shared` is the DOM-using but chrome-free
# helper layer (fitBitmap, burnLabel, decodeBmp4, the worker shim).
SHARED_BLOCKS = ("core", "shared")

# The mobile UI marks where each block goes with a placeholder comment.
PLACEHOLDER = "<!--@{}-->"


class BuildError(Exception):
    """A build input was missing or malformed."""


def extract_block(html: str, block_id: str, source: Path) -> str:
    """Returns the full <script id="..."> ... </script> element, tags included.

    Non-greedy up to the first </script>, which is safe because none of these
    blocks contain that string (a JS string holding "</script>" would end the
    element in the browser too, so it can never legally appear).
    """
    pattern = re.compile(
        r'<script\s+id="%s"\s*>.*?</script>' % re.escape(block_id),
        re.DOTALL,
    )
    matches = pattern.findall(html)
    if not matches:
        raise BuildError(
            f'{source}: no <script id="{block_id}"> block found. '
            f"The web app is built from that block; if it was renamed or "
            f"removed, update SHARED_BLOCKS in this script."
        )
    if len(matches) > 1:
        raise BuildError(
            f'{source}: {len(matches)} <script id="{block_id}"> blocks found, '
            f"expected exactly one."
        )
    return matches[0]


def extract_block_body(html: str, block_id: str, source: Path) -> str:
    """Returns just the JavaScript inside a block, without the <script> tags.

    The parity tests feed #core to `jsc`, which wants bare JS.
    """
    element = extract_block(html, block_id, source)
    return re.sub(r"^<script[^>]*>|</script>$", "", element).strip()


def check_self_contained(html: str, out: Path) -> None:
    """The device serves this page with no internet, so nothing may be remote.

    A stray CDN link would leave the phone staring at an unstyled page with no
    obvious cause, so fail the build instead.
    """
    remote = re.findall(r'(?:src|href)\s*=\s*["\'](https?:|//)', html)
    if remote:
        raise BuildError(
            f"{out}: page references {len(remote)} remote resource(s). "
            f"The device has no internet connection; inline everything."
        )


def build(ui_path: Path, lab_path: Path) -> str:
    ui = ui_path.read_text(encoding="utf-8")
    lab = lab_path.read_text(encoding="utf-8")

    for block_id in SHARED_BLOCKS:
        placeholder = PLACEHOLDER.format(block_id.upper())
        if placeholder not in ui:
            raise BuildError(
                f"{ui_path}: missing placeholder {placeholder}. "
                f"The mobile UI must mark where the {block_id!r} block is spliced in."
            )
        ui = ui.replace(placeholder, extract_block(lab, block_id, lab_path))

    leftover = re.findall(r"<!--@[A-Z]+-->", ui)
    if leftover:
        raise BuildError(f"{ui_path}: unresolved placeholder(s): {', '.join(leftover)}")

    return ui


def main(argv: list[str] | None = None) -> int:
    repo = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ui", type=Path, default=repo / "main/web/app.ui.html")
    ap.add_argument("--lab", type=Path, default=repo / "tools/prepare_photo/photo_lab.html")
    ap.add_argument("--out", type=Path, default=repo / "build/webapp/app.html")
    args = ap.parse_args(argv)

    try:
        html = build(args.ui, args.lab)
        check_self_contained(html, args.out)
    except (BuildError, OSError) as exc:
        print(f"build_webapp: {exc}", file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(html, encoding="utf-8")
    gz_path = args.out.with_suffix(args.out.suffix + ".gz")
    # mtime=0 so an unchanged input produces an identical .gz and CMake does
    # not relink the firmware on every build.
    gz = gzip.compress(html.encode("utf-8"), compresslevel=9, mtime=0)
    gz_path.write_bytes(gz)

    print(
        f"build_webapp: {args.out.name} {len(html):,} B -> "
        f"{gz_path.name} {len(gz):,} B ({100 * len(gz) / len(html):.0f}%)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
