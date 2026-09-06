"""tools/build_webapp.py assembles the page the frame serves over its hotspot.

The build exists to stop the dither pipeline forking: it splices #core and
#shared out of photo_lab.html rather than letting the device app keep its own
copy. These tests guard the things that would quietly break that -- a renamed
block, a dropped placeholder, a stray CDN reference the phone can never load.
"""

from __future__ import annotations

import contextlib
import gzip
import io
import re
import tempfile
import unittest
from pathlib import Path

import common
from build_webapp import BuildError, build, check_self_contained, main

UI = common.REPO / "main" / "web" / "app.ui.html"


def built() -> str:
    return build(UI, common.LAB_HTML)


class BuildOutputTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.html = built()

    def test_both_blocks_are_spliced_in(self):
        for block in ("core", "shared"):
            self.assertIn(f'<script id="{block}">', self.html)

    def test_script_ids_survive(self):
        """coreWorkerSource() does getElementById('core') at runtime.

        If the build ever stripped or renamed the id, the worker would be
        built from an empty string and every render would fail on the phone
        with nothing in the page to explain why.
        """
        self.assertEqual(len(re.findall(r'<script\s+id="core"\s*>', self.html)), 1)

    def test_pipeline_actually_came_across(self):
        """Spot-check symbols the mobile UI depends on, not just the tags."""
        for symbol in ("PANEL_PALETTE", "SIM_ESTIMATE", "BUILTIN_PRESETS",
                       "function adjustTone", "function ditherToIndices",
                       "function writeBmp4", "function fitBitmap",
                       "function decodeBmp4", "function makeCoreWorker"):
            self.assertIn(symbol, self.html, f"{symbol} missing from built page")

    def test_no_remote_resources(self):
        """The frame has no internet and the phone is on an isolated AP."""
        check_self_contained(self.html, Path("app.html"))

    def test_no_unresolved_placeholders(self):
        self.assertEqual(re.findall(r"<!--@[A-Z]+-->", self.html), [])

    def test_spliced_javascript_parses(self):
        """Evaluate the shared pipeline under jsc to catch a splice that
        produced syntactically broken JS."""
        program = (
            common.core_js() + "\n" + common.shared_js() + "\n"
            "print(typeof adjustTone === 'function' && typeof fitBitmap === 'function' "
            "&& typeof decodeBmp4 === 'function' ? 'OK' : 'MISSING');"
        )
        self.assertEqual(common.run_jsc(program).strip(), "OK")


    def test_mobile_block_compiles(self):
        """The device UI can only really run on a phone, but `new Function`
        compiles it without executing, which catches a syntax error before it
        ships inside the firmware image."""
        import json

        body = re.search(
            r'<script id="mobile">(.*?)</script>', self.html, re.DOTALL
        )
        self.assertIsNotNone(body, "built page has no #mobile block")
        program = (
            "var src = " + json.dumps(body.group(1)) + ";\n"
            "try { new Function(src); print('OK'); }\n"
            "catch (e) { print('SYNTAX ERROR: ' + e); }"
        )
        self.assertEqual(common.run_jsc(program).strip(), "OK")

    def test_mobile_uses_no_blocking_dialogs(self):
        """confirm(), alert() and prompt() are unusable on the device.

        iOS opens captive portals in a browser that silently suppresses them:
        the dialog never appears and confirm() reports "cancelled". Deleting a
        photo was gated behind confirm() and so did nothing at all on an
        iPhone, with no error to explain it. Anything that needs a decision
        from the user has to be in the page.
        """
        body = re.search(r'<script id="mobile">(.*?)</script>', self.html, re.DOTALL)
        self.assertIsNotNone(body)
        code = re.sub(r"//[^\n]*", "", body.group(1))
        for call in ("confirm(", "alert(", "prompt("):
            self.assertNotIn(
                call, code,
                f"the device UI calls {call}); captive-portal browsers suppress it",
            )

    def test_mobile_only_uses_symbols_the_pipeline_exports(self):
        """The device UI leans on #core/#shared globals. If one is renamed
        upstream the page would fail at runtime on the phone, with the frame
        the only place to see the error."""
        needed = ["DEFAULT_PARAMS", "BUILTIN_PRESETS", "SIM_ESTIMATE",
                  "PANEL_PALETTE", "fitBitmap", "decodeBmp4", "writeBmp4",
                  "makeCoreWorker"]
        pipeline = common.core_js() + common.shared_js()
        for symbol in needed:
            self.assertIn(symbol, pipeline, f"{symbol} is not defined by #core/#shared")


class BuildFailureTest(unittest.TestCase):
    """The build must fail loudly rather than ship a broken page."""

    def _lab_and_ui(self, lab: str | None = None, ui: str | None = None):
        tmp = Path(tempfile.mkdtemp())
        lab_path, ui_path = tmp / "lab.html", tmp / "ui.html"
        lab_path.write_text(
            common.LAB_HTML.read_text(encoding="utf-8") if lab is None else lab,
            encoding="utf-8",
        )
        ui_path.write_text(
            UI.read_text(encoding="utf-8") if ui is None else ui, encoding="utf-8"
        )
        return lab_path, ui_path

    def test_missing_block_is_an_error(self):
        lab = common.LAB_HTML.read_text(encoding="utf-8").replace(
            '<script id="shared">', '<script id="renamed">', 1
        )
        lab_path, ui_path = self._lab_and_ui(lab=lab)
        with self.assertRaises(BuildError) as cm:
            build(ui_path, lab_path)
        self.assertIn("shared", str(cm.exception))

    def test_duplicate_block_is_an_error(self):
        text = common.LAB_HTML.read_text(encoding="utf-8")
        lab = text + '\n<script id="core">/* stray copy */</script>\n'
        lab_path, ui_path = self._lab_and_ui(lab=lab)
        with self.assertRaises(BuildError):
            build(ui_path, lab_path)

    def test_missing_placeholder_is_an_error(self):
        ui = UI.read_text(encoding="utf-8").replace("<!--@SHARED-->", "")
        lab_path, ui_path = self._lab_and_ui(ui=ui)
        with self.assertRaises(BuildError) as cm:
            build(ui_path, lab_path)
        self.assertIn("SHARED", str(cm.exception))

    def test_remote_resource_is_an_error(self):
        for html in (
            '<link rel="stylesheet" href="https://cdn.example.com/x.css">',
            '<script src="https://cdn.example.com/x.js"></script>',
            '<img src="//example.com/x.png">',
        ):
            with self.subTest(html=html), self.assertRaises(BuildError):
                check_self_contained(html, Path("app.html"))

    def test_link_to_the_device_itself_is_allowed(self):
        """The app links to http://192.168.4.1 so people can escape the
        captive-portal browser, which cannot open a file picker. That is an
        anchor, not a resource load, and must not fail the build."""
        check_self_contained(
            '<a href="http://192.168.4.1/" target="_blank">Open</a>', Path("app.html")
        )


class ArtifactTest(unittest.TestCase):
    def test_writes_both_files_and_gzip_is_reproducible(self):
        """A byte-identical .gz for unchanged input keeps CMake from relinking
        the firmware on every build."""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "app.html"
            args = ["--ui", str(UI), "--lab", str(common.LAB_HTML), "--out", str(out)]
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(args), 0)
            gz = out.with_suffix(".html.gz")
            self.assertTrue(out.exists() and gz.exists())
            first = gz.read_bytes()
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(args), 0)
            self.assertEqual(gz.read_bytes(), first, "gzip output is not reproducible")
            self.assertEqual(
                gzip.decompress(first).decode("utf-8"),
                out.read_text(encoding="utf-8"),
            )

    def test_gzip_is_small_enough_to_embed(self):
        """It rides in the app partition and is served from flash in one go."""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "app.html"
            with contextlib.redirect_stdout(io.StringIO()):
                main(["--ui", str(UI), "--lab", str(common.LAB_HTML), "--out", str(out)])
            size = out.with_suffix(".html.gz").stat().st_size
            self.assertLess(size, 256 * 1024, f"built app.html.gz is {size:,} B")


if __name__ == "__main__":
    unittest.main()
