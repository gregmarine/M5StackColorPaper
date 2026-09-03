#!/usr/bin/env python3
"""Pre-process photos for the M5Stack PaperColor photo frame.

Why this exists
---------------
The PaperColor's E Ink Spectra 6 panel can only show six colours: black,
white, yellow, red, blue and green. Everything else has to be faked by
dithering. The firmware's own on-the-fly conversion (M5GFX Panel_ED2208) uses
an ordered/Bayer dither with a nearest-colour lookup, which is fast but gives
muddy, low-contrast photos. This tool does the colour reduction on the
computer instead, with proper error-diffusion dithering, and writes a file the
firmware can push to the panel *without* converting it a second time.

Output format
-------------
A 4-bit indexed BMP whose palette holds the exact RGB values the panel driver
uses for its six colours (PANEL_PALETTE below, copied from
components/M5GFX/src/lgfx/v1/panel/Panel_ED2208.cpp). The firmware detects an
indexed BMP, switches the panel to its no-dither mode, and the driver's
nearest-colour lookup then maps every pixel 1:1 to the intended panel colour.
Because the file is a plain BMP it still previews normally on the computer.

Do NOT save the result as JPEG: JPEG compression smears the dither pattern
into thousands of intermediate colours and the firmware has to re-dither.
"""
import argparse
import struct
import sys
import time
from pathlib import Path

from PIL import Image, ImageEnhance, ImageOps

try:
    # iPhone photos. Optional; only needed to open .heic/.heif inputs.
    from pillow_heif import register_heif_opener
    register_heif_opener()
    HAVE_HEIF = True
except ImportError:
    HAVE_HEIF = False

# The panel driver's palette: (r, g, b, name). Index order here is arbitrary;
# the BMP palette carries the RGB values, and the driver looks colours up by
# nearest match. These are the "what the panel really shows" values that
# M5GFX's author tuned against real hardware, so dithering error is computed
# against them. If your panel looks different, measure and tweak here - the
# firmware side needs no change as long as each entry is still nearest to
# the driver's own table entry.
PANEL_PALETTE = [
    (0, 0, 0, "black"),
    (255, 255, 255, "white"),
    (255, 243, 56, "yellow"),
    (191, 0, 0, "red"),
    (100, 64, 255, "blue"),
    (67, 138, 28, "green"),
]

# What the inks actually look like, sampled from a daylight photo of chart.bmp
# on the panel (2026-09-02), colour-cast corrected against the neutral grey
# bezel and scaled so the panel's own white sits at 245 (the camera flattened
# the bright end, so relative colours are trustworthy, absolute levels less
# so; black is lighter than reality because of flare). Same index order as
# PANEL_PALETTE. Used for *matching* with --match-palette measured; the BMP
# still carries PANEL_PALETTE so the firmware addresses the same inks. Note
# the panel's blue is a light sky blue and its green a light leaf green.
MEASURED_PALETTE = [
    (81, 62, 60, "black"),
    (245, 245, 247, "white"),
    (253, 244, 63, "yellow"),
    (156, 8, 5, "red"),
    (91, 149, 218, "blue"),
    (133, 168, 64, "green"),
]

# Error-diffusion kernels: list of (dx, dy, weight). dx is relative to the
# scan direction (flipped on serpentine rows).
KERNELS = {
    "floyd-steinberg": [(1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16)],
    "jarvis": [
        (1, 0, 7 / 48), (2, 0, 5 / 48),
        (-2, 1, 3 / 48), (-1, 1, 5 / 48), (0, 1, 7 / 48), (1, 1, 5 / 48), (2, 1, 3 / 48),
        (-2, 2, 1 / 48), (-1, 2, 3 / 48), (0, 2, 5 / 48), (1, 2, 3 / 48), (2, 2, 1 / 48),
    ],
    "stucki": [
        (1, 0, 8 / 42), (2, 0, 4 / 42),
        (-2, 1, 2 / 42), (-1, 1, 4 / 42), (0, 1, 8 / 42), (1, 1, 4 / 42), (2, 1, 2 / 42),
        (-2, 2, 1 / 42), (-1, 2, 2 / 42), (0, 2, 4 / 42), (1, 2, 2 / 42), (2, 2, 1 / 42),
    ],
    # Atkinson only propagates 6/8 of the error: cleaner, punchier, loses a
    # little detail in very light/dark areas.
    "atkinson": [(1, 0, 1 / 8), (2, 0, 1 / 8), (-1, 1, 1 / 8), (0, 1, 1 / 8), (1, 1, 1 / 8), (0, 2, 1 / 8)],
    "none": [],
}

# Ordered (threshold-mask) dithers: no error diffusion, so the pattern is a
# fixed texture instead of random-looking grain. "blue-noise" uses a
# void-and-cluster mask (generated once and cached next to this script);
# "bayer" is the classic 8x8 crosshatch.
ORDERED = {"blue-noise", "bayer", "bayer16", "halftone"}

# 8x8 clustered-dot mask: dots grow from the centre of each cell like a
# printed halftone screen. Coarser than Bayer but the most "regular" texture.
HALFTONE8 = [
    [24, 10, 12, 26, 35, 47, 49, 37],
    [8, 0, 2, 14, 45, 59, 61, 51],
    [22, 6, 4, 16, 43, 57, 63, 53],
    [30, 20, 18, 28, 33, 41, 55, 39],
    [34, 46, 48, 36, 25, 11, 13, 27],
    [44, 58, 60, 50, 9, 1, 3, 15],
    [42, 56, 62, 52, 23, 7, 5, 17],
    [32, 40, 54, 38, 31, 21, 19, 29],
]

BAYER8 = [
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
]


def bayer_matrix(size):
    """Bayer threshold matrix of the given power-of-two size, values 0..size^2-1."""
    m = [[0]]
    n = 1
    while n < size:
        m = [[4 * v for v in row] + [4 * v + 2 for v in row] for row in m] + \
            [[4 * v + 3 for v in row] + [4 * v + 1 for v in row] for row in m]
        n *= 2
    return m


def blue_noise_mask(size=64, sigma=1.5, seed=0):
    """Void-and-cluster blue-noise threshold mask in [0, 1), cached on disk."""
    import numpy as np

    cache = Path(__file__).with_name(f"bluenoise{size}.npy")
    if cache.exists():
        return np.load(cache)

    rng = np.random.default_rng(seed)
    n = size * size
    yy, xx = np.meshgrid(np.arange(size), np.arange(size), indexing="ij")
    d = np.minimum(yy, size - yy) ** 2 + np.minimum(xx, size - xx) ** 2
    kernel = np.exp(-d / (2 * sigma * sigma))
    kernel_f = np.fft.fft2(kernel)

    def energy(binary):
        return np.real(np.fft.ifft2(np.fft.fft2(binary) * kernel_f))

    # Initial pattern: ~10% random ones, relaxed by swapping tightest cluster / largest void.
    binary = np.zeros((size, size))
    ones = rng.choice(n, n // 10, replace=False)
    binary.flat[ones] = 1
    while True:
        e = energy(binary)
        cluster = np.argmax(np.where(binary == 1, e, -np.inf))
        binary.flat[cluster] = 0
        e = energy(binary)
        void = np.argmin(np.where(binary == 0, e, np.inf))
        if void == cluster:
            binary.flat[cluster] = 1
            break
        binary.flat[void] = 1

    rank = np.zeros((size, size), dtype=np.int32)
    proto = binary.copy()
    count = int(proto.sum())
    # Phase 1: remove ones from the tightest clusters, ranking downward.
    work = proto.copy()
    for r in range(count - 1, -1, -1):
        e = energy(work)
        i = np.argmax(np.where(work == 1, e, -np.inf))
        work.flat[i] = 0
        rank.flat[i] = r
    # Phase 2: add ones into the largest voids, ranking upward.
    work = proto.copy()
    for r in range(count, n):
        e = energy(work)
        i = np.argmin(np.where(work == 0, e, np.inf))
        work.flat[i] = 1
        rank.flat[i] = r

    mask = (rank + 0.5) / n
    np.save(cache, mask)
    return mask

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp", ".heic", ".heif"}


# --------------------------------------------------------------------------
# Geometry / tone
# --------------------------------------------------------------------------
def fit_to_canvas(img, width, height, fit, anchor=0.5):
    if fit == "cover":
        src_ratio = img.width / img.height
        dst_ratio = width / height
        if src_ratio > dst_ratio:
            new_height = height
            new_width = round(height * src_ratio)
        else:
            new_width = width
            new_height = round(width / src_ratio)
        resized = img.resize((new_width, new_height), Image.LANCZOS)
        # anchor: where along the cropped axis to keep, 0 = top/left, 0.5 =
        # centre, 1 = bottom/right. Faces in portrait shots sit high, so the
        # default leans toward the top when the crop is vertical.
        left = round((new_width - width) * anchor)
        top = round((new_height - height) * anchor)
        return resized.crop((left, top, left + width, top + height))

    # contain: fit within, letterbox on white
    scale = min(width / img.width, height / img.height)
    new_size = (max(1, round(img.width * scale)), max(1, round(img.height * scale)))
    resized = img.resize(new_size, Image.LANCZOS)
    canvas = Image.new("RGB", (width, height), (255, 255, 255))
    offset = ((width - new_size[0]) // 2, (height - new_size[1]) // 2)
    canvas.paste(resized, offset)
    return canvas


def adjust_tone(img, args):
    if args.smooth > 0:
        # A touch of blur before dithering removes fine texture the panel can't
        # resolve anyway, and the dither pattern reads calmer for it.
        from PIL import ImageFilter
        img = img.filter(ImageFilter.GaussianBlur(args.smooth))
    if args.auto_levels:
        # Stretch the histogram so the darkest 1% is black and the lightest 1%
        # is white. Six inks have very little tonal range, so a photo that
        # doesn't use the full range comes out mostly black or mostly white.
        img = ImageOps.autocontrast(img, cutoff=1, preserve_tone=True)
    if args.gamma != 1.0:
        lut = [round(255 * ((i / 255) ** (1 / args.gamma))) for i in range(256)]
        img = img.point(lut * 3)
    if args.brightness != 1.0:
        img = ImageEnhance.Brightness(img).enhance(args.brightness)
    if args.contrast != 1.0:
        img = ImageEnhance.Contrast(img).enhance(args.contrast)
    if args.saturation != 1.0:
        img = ImageEnhance.Color(img).enhance(args.saturation)
    img = hue_gains(img, {"red": args.hue_red, "yellow": args.hue_yellow, "green": args.hue_green, "blue": args.hue_blue},
                    getattr(args, "green_lean", 0.0))
    return img


# --------------------------------------------------------------------------
# Dithering
# --------------------------------------------------------------------------
def srgb_to_linear(arr):
    """sRGB uint8 array (N, 3) -> linear-light float array (N, 3) in 0..1."""
    import numpy as np

    c = arr.astype(np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(lin):
    """Linear-light float array in 0..1 -> sRGB float array in 0..255."""
    import numpy as np

    lin = np.clip(lin, 0.0, 1.0)
    c = np.where(lin <= 0.0031308, lin * 12.92, 1.055 * lin ** (1 / 2.4) - 0.055)
    return c * 255.0


def linear_to_lab(lin):
    """Linear-light float array (N, 3) -> CIELAB float array (N, 3), D65 white."""
    import numpy as np

    m = np.array([[0.4124564, 0.3575761, 0.1804375],
                  [0.2126729, 0.7151522, 0.0721750],
                  [0.0193339, 0.1191920, 0.9503041]])
    xyz = np.clip(lin, 0.0, None) @ m.T
    xyz = xyz / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > 0.008856, np.cbrt(xyz), 7.787 * xyz + 16.0 / 116.0)
    L = 116.0 * f[..., 1] - 16.0
    a = 500.0 * (f[..., 0] - f[..., 1])
    b = 200.0 * (f[..., 1] - f[..., 2])
    return np.stack([L, a, b], axis=-1)


def srgb_to_lab(arr):
    """sRGB uint8 array (N, 3) -> CIELAB float array (N, 3), D65 white."""
    return linear_to_lab(srgb_to_linear(arr))


def lab_to_linear(lab):
    """CIELAB float array (N, 3) -> linear-light float array (N, 3), unclipped."""
    import numpy as np

    fy = (lab[..., 0] + 16.0) / 116.0
    fx = fy + lab[..., 1] / 500.0
    fz = fy - lab[..., 2] / 200.0
    f = np.stack([fx, fy, fz], axis=-1)
    t = f ** 3
    xyz = np.where(t > 0.008856, t, (f - 16.0 / 116.0) / 7.787) * np.array([0.95047, 1.0, 1.08883])
    m_inv = np.array([[3.2404542, -1.5371385, -0.4985314],
                      [-0.9692660, 1.8760108, 0.0415560],
                      [0.0556434, -0.2040259, 1.0572252]])
    return xyz @ m_inv.T


# Lab hue angles (degrees) at which each --hue-* gain applies in full; the gain
# is interpolated linearly between neighbours around the circle. Sky blue sits
# near 240 and the panel's blue ink near 306, both inside the blue band.
HUE_ANCHORS = [("red", 40.0), ("yellow", 100.0), ("green", 140.0), ("blue", 270.0)]


def hue_gains(img, gains, green_lean=0.0):
    """Scale Lab chroma per hue band and optionally lean yellow-greens toward green.

    gains: dict name -> chroma multiplier. green_lean: degrees of hue rotation
    applied to the yellow-green band (full between Lab hues 105 and 135,
    tapering to nothing at 80 and 160). Sunlit grass and leaves sit around
    hue 105-115, closer to the panel's yellow than to its green, so without
    this they render solid yellow.
    """
    import numpy as np

    if all(abs(g - 1.0) < 1e-9 for g in gains.values()) and green_lean == 0:
        return img
    raw = np.frombuffer(img.tobytes(), dtype=np.uint8).reshape(-1, 3)
    lab = srgb_to_lab(raw)
    chroma = np.hypot(lab[:, 1], lab[:, 2])
    hue = np.degrees(np.arctan2(lab[:, 2], lab[:, 1])) % 360.0
    if green_lean:
        window = np.clip(np.minimum((hue - 80.0) / 25.0, (160.0 - hue) / 25.0), 0.0, 1.0)
        hue = (hue + green_lean * window) % 360.0
        rad = np.radians(hue)
        lab[:, 1] = chroma * np.cos(rad)
        lab[:, 2] = chroma * np.sin(rad)
    angles = np.array([a for _, a in HUE_ANCHORS] + [HUE_ANCHORS[0][1] + 360.0])
    values = np.array([gains[n] for n, _ in HUE_ANCHORS] + [gains[HUE_ANCHORS[0][0]]])
    h = np.where(hue < angles[0], hue + 360.0, hue)
    gain = np.interp(h, angles, values)
    gain = np.where(chroma < 2.0, 1.0, gain)  # leave neutrals alone
    lab[:, 1] *= gain
    lab[:, 2] *= gain
    out = linear_to_srgb(lab_to_linear(lab))
    out = np.clip(np.rint(out), 0, 255).astype(np.uint8)
    return Image.frombytes("RGB", img.size, out.tobytes())


def to_work_space(img, palette, space):
    """Returns (pixel list, palette list) as float triples in the chosen space."""
    import numpy as np

    raw = np.frombuffer(img.tobytes(), dtype=np.uint8).reshape(-1, 3)
    pal = np.array(palette, dtype=np.uint8)
    if space == "lab":
        raw = srgb_to_lab(raw)
        pal = srgb_to_lab(pal)
    return raw.astype(np.float64).tolist(), pal.astype(np.float64).tolist()


def nearest_index(c0, c1, c2, palette, cw=1.0):
    """Index of the closest palette entry; cw weights channels 1-2 (chroma in Lab)."""
    best = 0
    best_d = 1e30
    for i, (p0, p1, p2) in enumerate(palette):
        d0 = c0 - p0
        d1 = c1 - p1
        d2 = c2 - p2
        d = d0 * d0 + cw * (d1 * d1 + d2 * d2)
        if d < best_d:
            best_d = d
            best = i
    return best


def ordered_dither_to_indices(img, palette, mode, space, levels, mix="greedy", mix_gamma=0.0, spread=0.1,
                              chroma_weight=1.0):
    """Ordered dither for an arbitrary palette (Yliluoma's algorithms 1 and 2).

    mix="greedy" (algorithm 1): for each pixel, build a list of `levels`
    palette entries chosen greedily so that their running average approaches
    the target colour, sort the list by lightness, and let the threshold mask
    pick one entry. Any number of inks can take part in one pixel's mix.

    mix="pair" (algorithm 2): for each pixel, pick the best *pair* of palette
    entries and a mix ratio (in 1/`levels` steps). The penalty is the distance
    from the mixed colour to the target plus `spread` times the distance
    between the two inks, so mixes of very different colours (the stray
    green/blue dots in skin and greys) are avoided when a closer pair exists.

    Mixing happens in the working space by default (mix_gamma == 0). With
    mix_gamma > 0 the candidate averages are formed in a power-law RGB space,
    (sRGB/255) ** mix_gamma, and only the *comparison* happens in `space`.
    2.2 is linear light, which is what the eye does with neighbouring dots
    but comes out darker than Lab mixing on this panel; 1.0 is close to Lab
    mixing; values between blend the two.

    Results are cached per quantized input colour.
    """
    import numpy as np

    w, h = img.size
    if mode == "bayer":
        m = (np.array(BAYER8, dtype=np.float64) + 0.5) / 64.0
    elif mode == "bayer16":
        m = (np.array(bayer_matrix(16), dtype=np.float64) + 0.5) / 256.0
    elif mode == "halftone":
        m = (np.array(HALFTONE8, dtype=np.float64) + 0.5) / 64.0
    else:
        m = blue_noise_mask()
    ms = m.shape[0]
    n = len(palette)

    raw = np.frombuffer(img.tobytes(), dtype=np.uint8).reshape(-1, 3)
    pal8 = np.array(palette, dtype=np.uint8)
    if space == "lab":
        px_match, pal_match = srgb_to_lab(raw), srgb_to_lab(pal8)
    else:
        px_match, pal_match = raw.astype(np.float64), pal8.astype(np.float64)

    if mix_gamma > 0:
        g = float(mix_gamma)
        pal_mix = (pal8.astype(np.float64) / 255.0) ** g

        def to_match(a):
            srgb = np.clip(a, 0.0, 1.0) ** (1.0 / g) * 255.0
            return linear_to_lab(srgb_to_linear(srgb)) if space == "lab" else srgb
    else:
        pal_mix = pal_match
        to_match = lambda a: a  # noqa: E731

    cw = chroma_weight if space == "lab" else 1.0
    wvec = np.array([1.0, cw, cw])

    def dist(a, b):
        d = a - b
        return np.sqrt(np.sum(d * d * wvec, axis=-1))

    cache = {}

    if mix == "greedy":
        lightness = pal_match[:, 0] if space == "lab" else pal_match.sum(axis=1)

        def candidates(c_match):
            acc = np.zeros(3)
            chosen = []
            for i in range(1, levels + 1):
                avg = (acc + pal_mix) / i            # (n, 3): average if ink j is added
                best = int(np.argmin(dist(to_match(avg), c_match)))
                chosen.append(best)
                acc += pal_mix[best]
            chosen.sort(key=lambda j: lightness[j])
            return np.array(chosen, dtype=np.int32)

        def pick(cand, thr):
            return cand[int(thr * levels)]
    else:
        # Precompute every (ink a, ink b, ratio) mix once; per pixel it is a
        # single vectorized distance over this table.
        pa, pb, ratio = [], [], []
        for a in range(n):
            pa.append(a); pb.append(a); ratio.append(0.0)
            for b in range(a + 1, n):
                for k in range(1, levels):
                    pa.append(a); pb.append(b); ratio.append(k / levels)
        pa = np.array(pa); pb = np.array(pb); ratio = np.array(ratio)
        mixed = pal_mix[pa] * (1 - ratio)[:, None] + pal_mix[pb] * ratio[:, None]
        mixed_match = to_match(mixed)
        # Yliluoma's spread penalty: mixes of distant inks cost extra, most
        # of all near 50/50 where both inks are equally visible.
        penalty = dist(pal_match[pa], pal_match[pb]) * spread * (np.abs(ratio - 0.5) + 0.5)

        def candidates(c_match):
            i = int(np.argmin(dist(mixed_match, c_match) + penalty))
            return (int(pa[i]), int(pb[i]), float(ratio[i]))

        def pick(cand, thr):
            return cand[1] if thr < cand[2] else cand[0]

    def lookup(i):
        c = px_match[i]
        key = (round(c[0]), round(c[1] / 2), round(c[2] / 2)) if space == "lab" else tuple((raw[i] >> 2).tolist())
        hit = cache.get(key)
        if hit is None:
            hit = candidates(c)
            cache[key] = hit
        return hit

    out = [0] * (w * h)
    for y in range(h):
        row = y * w
        mrow = m[y % ms]
        for x in range(w):
            out[row + x] = int(pick(lookup(row + x), mrow[x % ms]))
    return out


def dither_to_indices(img, palette, kernel, serpentine, error_limit, space, strength=1.0, chroma_strength=1.0,
                      chroma_weight=1.0):
    """Error-diffusion dither of an RGB image to palette indices.

    Colour matching and error diffusion happen in `space`: "lab" (CIELAB,
    perceptual; greys resolve to black/white mixes, hues stay put) or "rgb"
    (plain sRGB distance; cheaper but mid-greys turn into green/blue/yellow
    speckle because the panel's green is numerically closest to grey).

    Returns a flat list of palette indices, row-major. Pure Python, a few
    seconds for a 600x400 image; fine for a batch tool.
    """
    w, h = img.size
    px, pal = to_work_space(img, palette, space)
    # strength < 1 diffuses only part of the error: less accurate tones but a
    # visibly calmer, less grainy pattern. Atkinson is this idea baked in (6/8).
    kernel = [(dx, dy, wt * strength) for dx, dy, wt in kernel]
    # In Lab, channel 0 is lightness and 1-2 are chroma. Diffusing chroma error
    # at full strength makes stray green/blue dots appear in skin and greys as
    # the accumulated colour error finally trips a saturated ink; damping it
    # trades a little saturation for a much calmer picture.
    cs = chroma_strength if space == "lab" else 1.0
    cw = chroma_weight if space == "lab" else 1.0
    # Working buffer accumulates diffused error; px keeps the originals for clamping.
    buf = [list(p) for p in px]
    out = [0] * (w * h)
    lim = float(error_limit)

    for y in range(h):
        reverse = serpentine and (y & 1)
        xs = range(w - 1, -1, -1) if reverse else range(w)
        row = y * w
        for x in xs:
            i = row + x
            r, g, b = buf[i]
            # Clamp the accumulated value so runaway error from a region the
            # palette can't reach doesn't drag streaks across the picture.
            if lim > 0:
                o = px[i]
                r = min(o[0] + lim, max(o[0] - lim, r))
                g = min(o[1] + lim, max(o[1] - lim, g))
                b = min(o[2] + lim, max(o[2] - lim, b))
            idx = nearest_index(r, g, b, pal, cw)
            out[i] = idx
            pr, pg, pb = pal[idx]
            er = r - pr
            eg = (g - pg) * cs
            eb = (b - pb) * cs
            if not kernel or (er == 0 and eg == 0 and eb == 0):
                continue
            for dx, dy, wt in kernel:
                if reverse:
                    dx = -dx
                nx = x + dx
                ny = y + dy
                if nx < 0 or nx >= w or ny >= h:
                    continue
                t = buf[ny * w + nx]
                t[0] += er * wt
                t[1] += eg * wt
                t[2] += eb * wt
    return out


# --------------------------------------------------------------------------
# BMP writer (4 bits per pixel, indexed)
# --------------------------------------------------------------------------
def write_bmp_4bpp(path, width, height, indices, palette):
    """Writes a bottom-up, uncompressed, 16-colour BMP (BITMAPINFOHEADER)."""
    row_bytes = ((width * 4 + 31) // 32) * 4
    pixel_bytes = row_bytes * height
    palette_entries = 16
    off_bits = 14 + 40 + palette_entries * 4
    file_size = off_bits + pixel_bytes

    hdr = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, off_bits)
    info = struct.pack(
        "<IiiHHIIiiII",
        40, width, height, 1, 4, 0, pixel_bytes, 2835, 2835, palette_entries, palette_entries,
    )
    pal = bytearray()
    for i in range(palette_entries):
        r, g, b = palette[i] if i < len(palette) else palette[-1]
        pal += bytes((b, g, r, 0))

    rows = bytearray()
    pad = b"\0" * (row_bytes - (width + 1) // 2)
    for y in range(height - 1, -1, -1):
        base = y * width
        line = bytearray((width + 1) // 2)
        for x in range(0, width - 1, 2):
            line[x >> 1] = (indices[base + x] << 4) | indices[base + x + 1]
        if width & 1:
            line[width >> 1] = indices[base + width - 1] << 4
        rows += line + pad

    with open(path, "wb") as f:
        f.write(hdr)
        f.write(info)
        f.write(pal)
        f.write(rows)


# --------------------------------------------------------------------------
# Test chart
# --------------------------------------------------------------------------
def make_chart(width, height):
    """A calibration image: solid swatches plus ramps, dithered like a photo.

    Look at it on the panel to judge whether PANEL_PALETTE matches what the
    hardware really shows, and how the dithering of smooth ramps looks.
    """
    from PIL import ImageDraw

    img = Image.new("RGB", (width, height), (255, 255, 255))
    d = ImageDraw.Draw(img)
    n = len(PANEL_PALETTE)
    sw = width // n
    swatch_h = height // 4
    for i, (r, g, b, _) in enumerate(PANEL_PALETTE):
        d.rectangle([i * sw, 0, (i + 1) * sw - 1, swatch_h - 1], fill=(r, g, b))

    ramps = [
        ((0, 0, 0), (255, 255, 255)),      # grey ramp
        ((255, 255, 255), (191, 0, 0)),    # white -> red
        ((255, 255, 255), (100, 64, 255)),  # white -> blue
        ((255, 255, 255), (67, 138, 28)),   # white -> green
        ((255, 243, 56), (191, 0, 0)),      # yellow -> red (oranges)
        ((100, 64, 255), (67, 138, 28)),    # blue -> green (cyans)
    ]
    ramp_h = (height - swatch_h) // len(ramps)
    for k, (c0, c1) in enumerate(ramps):
        top = swatch_h + k * ramp_h
        for x in range(width):
            t = x / (width - 1)
            c = tuple(round(c0[j] + (c1[j] - c0[j]) * t) for j in range(3))
            d.line([(x, top), (x, top + ramp_h - 1)], fill=c)
    return img


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------
def process_image(img, args, out_path):
    # The BMP palette is always the driver's table (that is how the firmware
    # addresses inks). The palette the dither *matches against* can be the
    # measured ink colours instead; index order is the same so no remap.
    bmp_palette = [(r, g, b) for r, g, b, _ in PANEL_PALETTE]
    mode = getattr(args, "match_palette", "driver")
    if mode == "measured":
        source = MEASURED_PALETTE
    elif mode == "hybrid":
        # Cameras under-record saturated inks: the measured red and yellow are
        # probably too dull, while the measured blue is what fixed skies.
        source = [PANEL_PALETTE[i] if PANEL_PALETTE[i][3] in ("red", "yellow") else MEASURED_PALETTE[i]
                  for i in range(len(PANEL_PALETTE))]
    else:
        source = PANEL_PALETTE
    palette = [(r, g, b) for r, g, b, _ in source]
    t0 = time.time()
    # Unclamped by default: in Lab the lightness error in shadows legitimately
    # runs to +/-50 and clamping it turns dark skin and shadows solid black.
    limit = args.error_limit if args.error_limit is not None else (0 if args.space == "lab" else 160)
    if args.dither in ORDERED:
        indices = ordered_dither_to_indices(img, palette, args.dither, args.space, args.ordered_levels,
                                            args.mix, args.mix_gamma, args.mix_spread, args.chroma_weight)
    else:
        indices = dither_to_indices(img, palette, KERNELS[args.dither], not args.no_serpentine, limit, args.space,
                                    args.strength, args.chroma_strength, args.chroma_weight)
    write_bmp_4bpp(out_path, img.width, img.height, indices, bmp_palette)

    counts = [0] * len(palette)
    for i in indices:
        counts[i] += 1
    total = len(indices)
    usage = ", ".join(f"{PANEL_PALETTE[i][3]} {100 * counts[i] / total:.0f}%" for i in range(len(palette)))
    return time.time() - t0, usage


def process_one(path, out_dir, args):
    try:
        img = Image.open(path)
    except Exception as exc:
        print(f"skip {path}: cannot open ({exc})", file=sys.stderr)
        return False

    img = ImageOps.exif_transpose(img).convert("RGB")
    in_size = img.size
    width, height = args.width, args.height
    if args.orientation == "auto" and img.height > img.width:
        width, height = min(args.width, args.height), max(args.width, args.height)
    elif args.orientation == "auto":
        width, height = max(args.width, args.height), min(args.width, args.height)
    img = fit_to_canvas(img, width, height, args.fit, args.anchor)
    img = adjust_tone(img, args)
    if args.label:
        # Black-on-white text survives dithering untouched, so the label stays crisp.
        from PIL import ImageDraw, ImageFont
        text = args.label if args.label != "auto" else (args.suffix.strip("_") or path.stem)
        font = ImageFont.load_default(size=18)
        d = ImageDraw.Draw(img)
        box = d.textbbox((0, 0), text, font=font)
        tw, th = box[2] - box[0], box[3] - box[1]
        x, y = img.width - tw - 12, img.height - th - 12
        d.rectangle([x - 6, y - 4, x + tw + 6, y + th + 6], fill=(255, 255, 255))
        d.text((x, y), text, fill=(0, 0, 0), font=font)

    out_path = out_dir / (path.stem + args.suffix + ".bmp")
    secs, usage = process_image(img, args, out_path)
    print(f"{path.name}: {in_size[0]}x{in_size[1]} -> {img.width}x{img.height} ({args.fit}, {args.dither}, "
          f"{secs:.1f}s) -> {out_path.name}\n    {usage}")
    return True


def collect_inputs(inputs):
    paths = []
    for item in inputs:
        p = Path(item)
        if p.is_dir():
            for child in sorted(p.iterdir()):
                if child.suffix.lower() in IMAGE_EXTENSIONS:
                    paths.append(child)
        elif p.is_file():
            paths.append(p)
        else:
            print(f"skip {item}: not a file or directory", file=sys.stderr)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="*", help="Image files and/or directories of images")
    parser.add_argument("-o", "--output-dir", required=True, type=Path,
                        help="Directory to write the .bmp files into")
    parser.add_argument("--width", type=int, default=600, help="Target width (default: 600, landscape)")
    parser.add_argument("--height", type=int, default=400, help="Target height (default: 400, landscape)")
    parser.add_argument("--portrait", action="store_true", help="Shorthand for --width 400 --height 600")
    parser.add_argument("--orientation", choices=["auto", "fixed"], default="auto",
                        help="auto = portrait photos become 400x600 and landscape ones 600x400; the firmware "
                             "rotates the panel to match each image (default). fixed = always --width x --height")
    parser.add_argument("--ordered-levels", type=int, default=64,
                        help="For the ordered dithers: candidate colours mixed per pixel (--mix greedy) or "
                             "ratio steps between the two inks (--mix pair); more = finer tonal steps (default: 64)")
    parser.add_argument("--mix", choices=["greedy", "pair"], default="greedy",
                        help="How the ordered dithers pick the inks for a pixel. greedy = any number of inks whose "
                             "average approaches the colour (Yliluoma 1). pair = the best two inks and a ratio, "
                             "penalising pairs of very different inks, which removes stray coloured dots (Yliluoma 2)")
    parser.add_argument("--mix-gamma", type=float, default=1.5,
                        help="Average candidate inks in (sRGB)**gamma instead of the working space; comparison "
                             "still happens in --space. 2.2 = linear light (physically true, darker midtones), "
                             "1.0 = close to Lab mixing, 0 = off (mix in Lab). Default 1.5, the panel-tested winner")
    parser.add_argument("--mix-spread", type=float, default=0.1,
                        help="--mix pair only: penalty weight on the distance between the two inks (default: 0.1); "
                             "higher = fewer two-colour mixes, flatter colour")
    parser.add_argument("--fit", choices=["cover", "contain"], default="cover",
                        help="cover = crop to fill the frame (default, see --anchor); "
                             "contain = letterbox on white, keeps the whole photo but portrait shots come out small")
    parser.add_argument("--anchor", type=float, default=0.3,
                        help="For --fit cover: which part of the cropped axis to keep, 0 = top/left, 0.5 = centre, "
                             "1 = bottom/right (default: 0.3, favours faces near the top of portrait shots)")
    parser.add_argument("--strength", type=float, default=1.0,
                        help="Fraction of the quantization error to diffuse, 0..1. Below 1.0 the shadows crush to "
                             "solid black on this palette; keep at 1.0 and use --chroma-strength/--smooth instead")
    parser.add_argument("--chroma-strength", type=float, default=0.75,
                        help="Fraction of the colour (a/b) error to diffuse in lab mode, 0..1. Lower = fewer stray "
                             "coloured dots in skin/greys, slightly less saturation")
    parser.add_argument("--smooth", type=float, default=0.5,
                        help="Gaussian blur radius in pixels applied before dithering; 0 disables (default: 0.5)")
    parser.add_argument("--suffix", default="",
                        help="Appended to output filenames, handy for A/B variants (e.g. --suffix _atk)")
    parser.add_argument("--label", default="",
                        help="Burn text into the bottom-right corner; 'auto' uses the suffix (or the filename)")
    parser.add_argument("--no-auto-levels", dest="auto_levels", action="store_false",
                        help="Don't stretch the histogram to full range before dithering")
    parser.add_argument("--dither", choices=sorted(KERNELS) + sorted(ORDERED), default="bayer16",
                        help="Ordered dithers (bayer, bayer16, halftone, blue-noise) give a fixed regular texture, which "
                             "reads far calmer on the panel than error diffusion (floyd-steinberg, jarvis, "
                             "stucki, atkinson), whose random grain the panel exaggerates (default: bayer16)")
    parser.add_argument("--no-serpentine", action="store_true",
                        help="Scan every row left-to-right instead of alternating (more visible worm artifacts)")
    parser.add_argument("--space", choices=["lab", "rgb"], default="lab",
                        help="Colour space for matching/diffusion (default: lab, perceptual)")
    parser.add_argument("--error-limit", type=float, default=None,
                        help="Clamp diffused error to +/- this much per channel, in the working space's units; "
                             "0 = no clamp (default: 0 for lab, 160 for rgb)")
    parser.add_argument("--saturation", type=float, default=1.5, help="Colour boost before dithering (default: 1.5)")
    for name in ("red", "yellow", "green", "blue"):
        parser.add_argument(f"--hue-{name}", type=float, default=1.0, dest=f"hue_{name}",
                            help=f"Chroma multiplier for the {name} hue band only (default: 1.0)")
    parser.add_argument("--green-lean", type=float, default=0.0,
                        help="Degrees to rotate yellow-green hues toward green before matching (0-40). Sunlit "
                             "grass and leaves otherwise render solid yellow (default: 0)")
    parser.add_argument("--match-palette", choices=["driver", "measured", "hybrid"], default="measured",
                        help="Ink colours the dither matches against: MEASURED_PALETTE, the panel's real inks "
                             "from the chart photo (default; light sky blue, leaf green, tested best on the "
                             "panel), the driver's table, or hybrid (measured black/white/blue/green with the "
                             "driver's red/yellow). The BMP palette is always the driver table")
    parser.add_argument("--chroma-weight", type=float, default=1.0,
                        help="Weight of hue/saturation vs lightness when matching inks (lab only). Above 1 the "
                             "dither prefers a darker-but-coloured mix over a grey one; ~2 makes skies blue "
                             "(default: 1.0)")
    parser.add_argument("--contrast", type=float, default=1.05, help="Contrast before dithering (default: 1.05)")
    parser.add_argument("--brightness", type=float, default=1.0, help="Brightness before dithering (default: 1.0)")
    parser.add_argument("--gamma", type=float, default=1.4,
                        help="Gamma before dithering; >1 lifts midtones/shadows, which the panel otherwise "
                             "renders as solid black (default: 1.4)")
    parser.add_argument("--chart", action="store_true",
                        help="Also write chart.bmp, a swatch/ramp calibration image, into the output dir")
    args = parser.parse_args()

    if args.portrait:
        args.width, args.height = 400, 600

    args.output_dir.mkdir(parents=True, exist_ok=True)

    done = 0
    if args.chart:
        chart = make_chart(args.width, args.height)
        secs, usage = process_image(chart, args, args.output_dir / "chart.bmp")
        print(f"chart: {chart.width}x{chart.height} ({args.dither}, {secs:.1f}s) -> chart.bmp\n    {usage}")
        done += 1

    paths = collect_inputs(args.inputs)
    if not paths and not args.chart:
        print("No input images found.", file=sys.stderr)
        sys.exit(1)

    ok = sum(process_one(p, args.output_dir, args) for p in paths)
    print(f"\nProcessed {ok}/{len(paths)} photo(s){' + chart' if args.chart else ''} into {args.output_dir}")
    if not HAVE_HEIF and any(p.suffix.lower() in {".heic", ".heif"} for p in paths):
        print("note: HEIC/HEIF inputs need `pip install pillow-heif` to open")


if __name__ == "__main__":
    main()
