"""Structural guards on photo_lab.html itself.

The desktop tool can only really be tested in a browser, but its three script
blocks can at least be compiled, and the split between them can be enforced.
That split is load-bearing now: tools/build_webapp.py ships #core and #shared
to the phone and leaves #ui behind, so anything the device app needs has to
be on the right side of the line.
"""

from __future__ import annotations

import json
import re
import unittest

import common

BLOCKS = ("core", "shared", "ui")


def block_body(block_id: str) -> str:
    from build_webapp import extract_block_body

    return extract_block_body(
        common.LAB_HTML.read_text(encoding="utf-8"), block_id, common.LAB_HTML
    )


def strip_comments(js: str) -> str:
    """Removes // and /* */ comments so prose cannot trip the source checks.

    "Yellow-green window" in a comment is not a DOM reference.
    """
    js = re.sub(r"/\*.*?\*/", "", js, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", js)


class StructureTest(unittest.TestCase):
    def test_exactly_three_blocks_in_order(self):
        html = common.LAB_HTML.read_text(encoding="utf-8")
        found = re.findall(r'<script\s+id="([a-z]+)"\s*>', html)
        self.assertEqual(found, list(BLOCKS))

    def test_every_block_compiles(self):
        """`new Function` compiles without executing, so #ui can be checked
        for syntax even though it touches the DOM the moment it runs."""
        for block_id in BLOCKS:
            with self.subTest(block=block_id):
                program = (
                    "var src = " + json.dumps(block_body(block_id)) + ";\n"
                    "try { new Function(src); print('OK'); }\n"
                    "catch (e) { print('SYNTAX ERROR: ' + e); }"
                )
                self.assertEqual(common.run_jsc(program).strip(), "OK")

    def test_core_stays_dom_free(self):
        """#core runs inside a Web Worker, where there is no document."""
        code = strip_comments(block_body("core"))
        for forbidden in ("document", "window", "localStorage"):
            found = re.search(rf"\b{forbidden}\b", code)
            self.assertIsNone(
                found,
                f"#core references `{forbidden}` at offset {found.start() if found else 0}; "
                f"it must stay DOM-free so it can run in a worker and under jsc",
            )

    def test_shared_has_no_page_chrome(self):
        """#shared goes to a phone whose markup is entirely different.

        It may look up its own #core script tag, but must not reach for any of
        this page's controls or status elements.
        """
        body = strip_comments(block_body("shared"))
        selectors = re.findall(r"""getElementById\(['"]([^'"]+)['"]\)""", body)
        self.assertEqual(
            sorted(set(selectors)), ["core"],
            "#shared may only look up the #core script block",
        )
        self.assertNotIn("$('#", body, "#shared must not use #ui's $ selector helper")

    def test_device_app_dependencies_live_in_shared(self):
        """These are what main/web/app.ui.html calls; if one drifts back into
        #ui the device build silently loses it."""
        body = block_body("shared")
        for fn in ("fitBitmap", "burnLabel", "decodeBmp4",
                   "coreWorkerSource", "makeCoreWorker"):
            self.assertIn(f"function {fn}", body, f"{fn} must be defined in #shared")

    def test_ui_does_not_redefine_shared_helpers(self):
        """A leftover copy in #ui would shadow the shared one and diverge."""
        body = strip_comments(block_body("ui"))
        for fn in ("fitBitmap", "burnLabel", "decodeBmp4"):
            self.assertNotIn(
                f"function {fn}", body, f"#ui redefines {fn}; it belongs in #shared"
            )


if __name__ == "__main__":
    unittest.main()
