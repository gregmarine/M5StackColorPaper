#!/usr/bin/env bash
# Runs the host-side test suite.
#
# These tests cover everything that can be checked without the panel: that
# photo_lab.html's #core and prepare_photo.py still agree, that the 4-bit BMP
# both of them write is the file the firmware expects, and that the on-device
# web app is assembled from those same blocks rather than a fork of them.
#
# What they cannot cover is how a photo actually looks on e-paper. That is
# still a panel test, logged in docs/dithering.md.
#
# Usage:  tools/test/run.sh [-v]        (any args are passed to unittest)
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
venv_python="$repo/tools/prepare_photo/venv/bin/python"

if [[ ! -x "$venv_python" ]]; then
    echo "error: no venv at $venv_python" >&2
    echo "create it with:" >&2
    echo "  python3 -m venv tools/prepare_photo/venv" >&2
    echo "  tools/prepare_photo/venv/bin/pip install -r tools/prepare_photo/requirements.txt" >&2
    exit 1
fi

jsc="/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc"
if [[ ! -x "$jsc" ]]; then
    echo "error: jsc not found at $jsc" >&2
    echo "The parity tests run photo_lab.html's #core under JavaScriptCore" >&2
    echo "(node is not installed on this machine)." >&2
    exit 1
fi

exec "$venv_python" -m unittest discover \
    --start-directory "$repo/tools/test" \
    --top-level-directory "$repo/tools/test" \
    "$@"
