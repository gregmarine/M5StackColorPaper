"""Shared helpers for the PaperColor test suite.

Everything here sticks to the standard library plus the packages already in
tools/prepare_photo/requirements.txt, so the tests run against the existing
venv with nothing new to install.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LAB_HTML = REPO / "tools" / "prepare_photo" / "photo_lab.html"
PREPARE_PHOTO = REPO / "tools" / "prepare_photo" / "prepare_photo.py"

# node is not installed on this machine; macOS ships JavaScriptCore's shell,
# which is what the parity procedure in CLAUDE.md has always used.
JSC = Path(
    "/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc"
)

sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "tools" / "prepare_photo"))


def core_js() -> str:
    """The #core block of photo_lab.html as bare JavaScript."""
    from build_webapp import extract_block_body

    return extract_block_body(LAB_HTML.read_text(encoding="utf-8"), "core", LAB_HTML)


def shared_js() -> str:
    """The #shared block as bare JavaScript.

    It touches `document`, `Worker` and `Blob`, but only inside function
    bodies, so the block itself evaluates fine under jsc. Anything that would
    actually call those (fitBitmap, makeCoreWorker) is untestable here and is
    left to the browser.
    """
    from build_webapp import extract_block_body

    return extract_block_body(LAB_HTML.read_text(encoding="utf-8"), "shared", LAB_HTML)


def run_jsc(source: str) -> str:
    """Runs a JS program under jsc and returns its stdout.

    jsc reports a syntax error on stdout with a zero exit status in some
    versions, so treat any stderr output as a failure too.
    """
    if not JSC.exists():
        raise RuntimeError(f"jsc not found at {JSC}")
    # jsc has no stdin mode, so the program goes through a temp file.
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as fh:
        fh.write(source)
        script = fh.name
    try:
        proc = subprocess.run([str(JSC), script], capture_output=True, text=True)
    finally:
        os.unlink(script)
    if proc.returncode != 0 or proc.stderr.strip():
        raise RuntimeError(
            f"jsc failed (exit {proc.returncode})\n"
            f"stderr:\n{proc.stderr}\nstdout:\n{proc.stdout[:2000]}"
        )
    return proc.stdout


def fixture_rgb(w: int = 64, h: int = 48) -> bytes:
    """A deterministic test image: smooth gradients plus reproducible noise.

    Flat synthetic colour would under-exercise the dither (every pixel picks
    the same mix), and a real photo would be a binary blob in the repo. A
    gradient with a fixed-seed LCG jitter covers a wide slice of the gamut and
    gives the error-diffusion path something to actually diffuse.
    """
    out = bytearray(w * h * 3)
    seed = 0x12345678
    for y in range(h):
        for x in range(w):
            seed = (1103515245 * seed + 12345) & 0x7FFFFFFF
            jitter = (seed >> 16) % 41 - 20
            i = (y * w + x) * 3
            out[i] = _clamp(x * 255 // (w - 1) + jitter)
            out[i + 1] = _clamp(y * 255 // (h - 1) + jitter)
            out[i + 2] = _clamp((x + y) * 255 // (w + h - 2) + jitter)
    return bytes(out)


def _clamp(v: int) -> int:
    return 0 if v < 0 else (255 if v > 255 else v)


def js_array_literal(data: bytes) -> str:
    """Inlines bytes into the jsc program.

    jsc's file/binary APIs vary between macOS versions, so the harness is
    generated self-contained rather than reading a fixture off disk.
    """
    return "[" + ",".join(str(b) for b in data) + "]"


def default_args(**overrides):
    """An argparse.Namespace matching prepare_photo.py's CLI defaults.

    Built from prepare_photo's own parser, so the tests cannot drift from the
    real defaults the way a hand-copied dict would.
    """
    import prepare_photo

    ns = prepare_photo.build_parser().parse_args(["fixture.png", "-o", "unused"])
    for key, value in overrides.items():
        if not hasattr(ns, key):
            raise AttributeError(f"prepare_photo has no option {key!r}")
        setattr(ns, key, value)
    return ns
