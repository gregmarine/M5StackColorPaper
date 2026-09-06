"""photo_lab.html's #core must stay a faithful port of prepare_photo.py.

This is the check CLAUDE.md has always described as a manual jsc procedure.
It matters more now than it did: the same #core block is spliced into the
on-device web app by tools/build_webapp.py, so a drift here changes what the
frame displays, not just what the desktop tool previews.

The bar is byte-identical output. That is stronger than CLAUDE.md's older
"tone stages may differ by a unit from rounding", and it holds because #core
does not merely reimplement Pillow's operations, it ports them: PIL's
three-box-blur Gaussian, its autocontrast LUT (which truncates, and eats whole
histogram bins), its fixed-point RGB->L, and the Image.blend that every
ImageEnhance reduces to. Get any of those subtly wrong and the tone output
moves by one unit, which is enough to flip dither decisions on 1-2% of pixels.

Resizing is deliberately not compared: PIL's LANCZOS and the browser's
drawImage are different resamplers and never will match. The device web app
does its own fitting in the browser via #shared, upstream of all of this.
"""

from __future__ import annotations

import unittest

import common

W, H = 64, 48

# Every tone knob neutral, so one stage at a time can be switched back on and
# a failure names the culprit instead of just reporting that output moved.
NEUTRAL_PY = dict(smooth=0, auto_levels=False, gamma=1.0, contrast=1.0,
                  brightness=1.0, saturation=1.0)
NEUTRAL_JS = ("smooth: 0, autoLevels: '0', gamma: 1.0, contrast: 1.0, "
              "brightness: 1.0, saturation: 1.0")

# The three panel-tested presets from #core's BUILTIN_PRESETS, as (name,
# prepare_photo kwargs, #core params).
PRESETS = [
    ("Default (S)", {}, "{}"),
    ("Foliage / landscape (T)", dict(saturation=1.3), "{saturation: 1.3}"),
    ("Darker complexion (Z)",
     dict(gamma=1.0, saturation=1.6, hue_yellow=0.85),
     "{gamma: 1.0, saturation: 1.6, hueYellow: 0.85}"),
]

DITHERS = [
    ("bayer16", {}, "{}"),
    ("bayer8", dict(dither="bayer"), "{dither: 'bayer'}"),
    ("halftone", dict(dither="halftone"), "{dither: 'halftone'}"),
    ("pair mix", dict(mix="pair"), "{mix: 'pair'}"),
    ("floyd-steinberg", dict(dither="floyd-steinberg"), "{dither: 'floyd-steinberg'}"),
]


# --------------------------------------------------------------------------
# Harnesses
# --------------------------------------------------------------------------
def py_tone(rgb: bytes, **overrides) -> list[int]:
    from PIL import Image

    import prepare_photo

    args = common.default_args(**overrides)
    return list(
        prepare_photo.adjust_tone(Image.frombytes("RGB", (W, H), rgb), args).tobytes()
    )


def js_tone(rgb: bytes, params_js: str) -> list[int]:
    program = f"""
{common.core_js()}
var rgb = new Uint8ClampedArray({common.js_array_literal(rgb)});
var p = Object.assign({{}}, DEFAULT_PARAMS, {params_js});
var adj = adjustTone(rgb, {W}, {H}, p);
var o = []; for (var i = 0; i < adj.length; i++) o.push(adj[i]);
print(o.join(','));
"""
    return [int(v) for v in common.run_jsc(program).strip().split(",")]


def py_bmp(rgb: bytes, **overrides) -> bytes:
    """The whole Python pipeline, ending at the file it would write."""
    import tempfile
    from pathlib import Path

    from PIL import Image

    import prepare_photo

    args = common.default_args(**overrides)
    img = prepare_photo.adjust_tone(Image.frombytes("RGB", (W, H), rgb), args)

    source = (prepare_photo.MEASURED_PALETTE if args.match_palette == "measured"
              else prepare_photo.PANEL_PALETTE)
    match = [(r, g, b) for r, g, b, _ in source]
    if args.dither in prepare_photo.ORDERED:
        idx = prepare_photo.ordered_dither_to_indices(
            img, match, args.dither, args.space, args.ordered_levels,
            args.mix, args.mix_gamma, args.mix_spread, args.chroma_weight)
    else:
        idx = prepare_photo.dither_to_indices(
            img, match, prepare_photo.KERNELS[args.dither], not args.no_serpentine,
            0, args.space, args.strength, args.chroma_strength, args.chroma_weight)

    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "t.bmp"
        prepare_photo.write_bmp_4bpp(
            out, W, H, idx, [(r, g, b) for r, g, b, _ in prepare_photo.PANEL_PALETTE]
        )
        return out.read_bytes()


def js_bmp(rgb: bytes, params_js: str) -> bytes:
    """The whole browser pipeline, ending at the bytes it would upload."""
    program = f"""
{common.core_js()}
var rgb = new Uint8ClampedArray({common.js_array_literal(rgb)});
var p = Object.assign({{}}, DEFAULT_PARAMS, {params_js});
var pal = p.matchInks === 'calibrated' ? SIM_ESTIMATE : PANEL_PALETTE;
var adj = adjustTone(rgb, {W}, {H}, p);
var idx = ditherToIndices(adj, {W}, {H}, pal, p);
var u8 = new Uint8Array(writeBmp4({W}, {H}, idx, PANEL_PALETTE));
var o = []; for (var i = 0; i < u8.length; i++) o.push(u8[i]);
print(o.join(','));
"""
    return bytes(int(v) for v in common.run_jsc(program).strip().split(","))


# --------------------------------------------------------------------------
# End to end: the guarantee that actually matters
# --------------------------------------------------------------------------
class EndToEndTest(unittest.TestCase):
    """Same photo, same settings, same file -- from either tool.

    This is what lets the phone be trusted: a BMP the browser uploads is the
    file prepare_photo.py would have produced, so the panel tests logged in
    docs/dithering.md describe the phone's output too.
    """

    @classmethod
    def setUpClass(cls):
        cls.rgb = common.fixture_rgb(W, H)

    def _assert_identical(self, name, py_over, js_over):
        py, js = py_bmp(self.rgb, **py_over), js_bmp(self.rgb, js_over)
        self.assertEqual(len(py), len(js), f"{name}: BMP sizes differ")
        first = next((i for i, (a, b) in enumerate(zip(py, js)) if a != b), None)
        self.assertIsNone(first, f"{name}: BMPs differ from byte {first}")

    def test_panel_tested_presets(self):
        for name, py_over, js_over in PRESETS:
            with self.subTest(preset=name):
                self._assert_identical(name, py_over, js_over)

    def test_every_dither_mode(self):
        for name, py_over, js_over in DITHERS:
            with self.subTest(dither=name):
                self._assert_identical(name, py_over, js_over)

    def test_driver_palette_matching(self):
        self._assert_identical(
            "driver", dict(match_palette="driver"), "{matchInks: 'driver'}"
        )


# --------------------------------------------------------------------------
# Tone stages, one at a time
# --------------------------------------------------------------------------
class ToneParityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rgb = common.fixture_rgb(W, H)

    def _max_delta(self, py_over: dict, js_over: str) -> int:
        py = py_tone(self.rgb, **{**NEUTRAL_PY, **py_over})
        js = js_tone(self.rgb, "{" + NEUTRAL_JS + ", " + js_over + "}")
        self.assertEqual(len(py), len(js))
        return max(abs(a - b) for a, b in zip(py, js))

    def _assert_exact(self, name: str, py_over: dict, js_over: str):
        delta = self._max_delta(py_over, js_over)
        self.assertEqual(delta, 0, f"{name}: differs by up to {delta} units")

    def test_identity(self):
        self._assert_exact("identity", {}, "gamma: 1.0")

    def test_gamma(self):
        self._assert_exact("gamma", {"gamma": 1.4}, "gamma: 1.4")

    def test_brightness(self):
        self._assert_exact("brightness", {"brightness": 1.2}, "brightness: 1.2")

    def test_contrast(self):
        """The shipped default is 1.05."""
        self._assert_exact("contrast", {"contrast": 1.05}, "contrast: 1.05")

    def test_saturation(self):
        self._assert_exact("saturation", {"saturation": 1.5}, "saturation: 1.5")

    def test_auto_levels(self):
        self._assert_exact("autoLevels", {"auto_levels": True}, "autoLevels: '1'")

    def test_hue_gains(self):
        self._assert_exact(
            "hue gains", {"hue_red": 1.2, "hue_blue": 0.8}, "hueRed: 1.2, hueBlue: 0.8"
        )

    def test_blur_disabled(self):
        self._assert_exact("smooth 0", {"smooth": 0}, "smooth: 0")

    def test_blur_at_the_shipped_default(self):
        """`smooth: 0.5` is what all three presets use."""
        self._assert_exact("smooth 0.5", {"smooth": 0.5}, "smooth: 0.5")

    def test_blur_at_other_radii(self):
        """Away from the default the two can differ by a unit or two.

        Pillow's box weights are 24-bit fixed point, and at radii where they
        sum to exactly 2**24 a rounding difference survives into the second
        pass. No preset uses those values, so the bound is asserted rather
        than chased further.
        """
        for sigma, bound in ((0.25, 1), (0.75, 2), (1.0, 2), (1.5, 0), (2.0, 0)):
            with self.subTest(sigma=sigma):
                delta = self._max_delta({"smooth": sigma}, f"smooth: {sigma}")
                self.assertLessEqual(delta, bound, f"smooth {sigma}: {delta} units")

    def test_contrast_below_one(self):
        """Contrast < 1 takes Pillow's interpolating (rather than
        extrapolating) blend branch, whose exact rounding is not reproduced.

        Off by at most a unit on a fraction of a percent of samples. Nothing
        ships with contrast below 1 -- the default is 1.05 -- so this is
        bounded rather than fixed.
        """
        self.assertLessEqual(self._max_delta({"contrast": 0.8}, "contrast: 0.8"), 1)


if __name__ == "__main__":
    unittest.main()
