"""The 4-bit indexed BMP is the contract between the tools and the firmware.

`PhotoSlideshow::displayPhoto()` switches the panel driver to `epd_fastest`
(which disables the driver's own dither) only when
`is_indexed_bmp_file()` recognises the header, and the driver then maps each
palette entry to exactly one ink. A malformed header silently costs you the
pre-dithered image -- it still displays, just muddier -- so the writers are
worth pinning down.

Three things are checked: the two writers agree byte for byte, the file is
shaped the way the firmware expects, and #shared's decodeBmp4 (which the
device web app uses to draw library thumbnails) reads back what was written.
"""

from __future__ import annotations

import struct
import unittest

import common

W, H = 37, 11  # deliberately odd: exercises row padding and the odd-width path


def indices_fixture(w: int = W, h: int = H) -> list[int]:
    """Every ink, in a pattern that is not symmetric in either axis."""
    return [(x * 3 + y * 5 + (x * y) % 7) % 6 for y in range(h) for x in range(w)]


def python_bmp(idx: list[int], w: int = W, h: int = H) -> bytes:
    import tempfile
    from pathlib import Path

    import prepare_photo

    palette = [(r, g, b) for r, g, b, _ in prepare_photo.PANEL_PALETTE]
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "t.bmp"
        prepare_photo.write_bmp_4bpp(out, w, h, idx, palette)
        return out.read_bytes()


def js_bmp(idx: list[int], w: int = W, h: int = H) -> bytes:
    program = f"""
{common.core_js()}
var idx = new Uint8Array({common.js_array_literal(bytes(idx))});
var buf = writeBmp4({w}, {h}, idx, PANEL_PALETTE);
var u8 = new Uint8Array(buf), o = [];
for (var i = 0; i < u8.length; i++) o.push(u8[i]);
print(o.join(','));
"""
    return bytes(int(v) for v in common.run_jsc(program).strip().split(","))


class WriterParityTest(unittest.TestCase):
    def test_writers_agree_byte_for_byte(self):
        """prepare_photo.py and #core must emit the identical file."""
        idx = indices_fixture()
        py, js = python_bmp(idx), js_bmp(idx)
        self.assertEqual(len(py), len(js), "file sizes differ")
        first = next((i for i, (a, b) in enumerate(zip(py, js)) if a != b), None)
        self.assertIsNone(
            first,
            f"files differ from byte {first} "
            f"(py={py[first:first+8].hex() if first is not None else ''}, "
            f"js={js[first:first+8].hex() if first is not None else ''})",
        )


class HeaderShapeTest(unittest.TestCase):
    """The exact fields main/hal/utils/image/image_utils.cpp reads."""

    @classmethod
    def setUpClass(cls):
        cls.data = python_bmp(indices_fixture())

    def test_signature_and_offset(self):
        self.assertEqual(self.data[:2], b"BM")
        # 14-byte file header + 40-byte info header + 16 palette entries x 4 B.
        self.assertEqual(struct.unpack_from("<I", self.data, 10)[0], 118)

    def test_dimensions_where_the_firmware_looks(self):
        """image_utils.cpp reads width at offset 18 and height at 22."""
        self.assertEqual(struct.unpack_from("<i", self.data, 18)[0], W)
        self.assertEqual(struct.unpack_from("<i", self.data, 22)[0], H)

    def test_is_recognised_as_indexed(self):
        """biBitCount must be 4, or the driver keeps its own dither on."""
        self.assertEqual(struct.unpack_from("<H", self.data, 28)[0], 4)
        self.assertEqual(struct.unpack_from("<I", self.data, 30)[0], 0, "must be uncompressed")

    def test_palette_is_the_drivers_table(self):
        """Sixteen BGRA entries; the six inks then padding, matching Panel_ED2208."""
        import prepare_photo

        self.assertEqual(struct.unpack_from("<I", self.data, 46)[0], 16)
        for i, (r, g, b, _) in enumerate(prepare_photo.PANEL_PALETTE):
            o = 54 + i * 4
            self.assertEqual(
                (self.data[o + 2], self.data[o + 1], self.data[o]), (r, g, b),
                f"palette entry {i} is not the driver's colour",
            )

    def test_file_length_matches_padded_rows(self):
        row = ((W * 4 + 31) // 32) * 4
        self.assertEqual(len(self.data), 118 + row * H)


class RoundTripTest(unittest.TestCase):
    def test_pillow_reads_back_the_indices(self):
        from io import BytesIO

        from PIL import Image

        idx = indices_fixture()
        img = Image.open(BytesIO(python_bmp(idx)))
        self.assertEqual(img.size, (W, H))
        self.assertEqual(list(img.convert("P").tobytes()), idx)

    def test_decode_bmp4_reads_back_the_indices(self):
        """#shared's decoder, which draws the library grid on the phone."""
        idx = indices_fixture()
        data = python_bmp(idx)
        program = f"""
{common.core_js()}
{common.shared_js()}
var buf = new Uint8Array({common.js_array_literal(data)}).buffer;
var r = decodeBmp4(buf);
print(r.w + ' ' + r.h);
var o = []; for (var i = 0; i < r.idx.length; i++) o.push(r.idx[i]);
print(o.join(','));
"""
        out = common.run_jsc(program).strip().splitlines()
        self.assertEqual(out[0], f"{W} {H}")
        self.assertEqual([int(v) for v in out[1].split(",")], idx)


if __name__ == "__main__":
    unittest.main()
