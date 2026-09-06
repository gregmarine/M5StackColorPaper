# Dithering notes

Why this project spends most of its effort on the computer side, what has
been tried on the real panel, and what is still open.

## The problem

E Ink Spectra 6 has six inks. The driver's palette (what M5GFX's author
measured as the panel's real colours, and what the BMP palette carries):

| Ink | RGB | Note |
|-----|-----|------|
| black | 0, 0, 0 | |
| white | 255, 255, 255 | on paper it is a light grey |
| yellow | 255, 243, 56 | the only bright saturated ink |
| red | 191, 0, 0 | strong |
| blue | 100, 64, 255 | dark, violet-leaning |
| green | 67, 138, 28 | dark and dull, the weakest ink |

Consequences that drive every decision below:

- Anything pale and coloured (sky, pastel clothing) has no nearby ink. A
  plain nearest-colour match calls it white or grey.
- Bright greens (sunlit grass) come out yellow, because yellow is the only
  bright ink near that hue.
- Midtones must be built from black/white/ink mixes, so the dither pattern
  is visible everywhere and its character matters more than on an LCD.
- The panel exaggerates random grain. Regular patterns read calmer.

## Pipeline

Three front-ends now run this pipeline: `prepare_photo.py`, the `#core` block
of `photo_lab.html`, and the phone web app the frame serves over its hotspot
(which is that same `#core` block, spliced in at build time — see
[webapp.md](webapp.md)). They are held to **byte-identical output** by
`tools/test/run.sh`, for every preset and every dither mode:

1. EXIF orientation, resize/crop to 400x600 or 600x400.
2. Tone: optional blur, 1% auto-levels, gamma, brightness, contrast,
   saturation, then per-hue chroma gains in LCh.
3. Convert to CIELAB. Ink matching and error diffusion happen there so greys
   resolve to black/white mixes rather than green/blue speckle (the naive RGB
   distance thinks the panel's green is "close" to grey).
4. Dither to palette indices.
5. Write a 4-bit BMP whose palette holds the driver's RGB values, so the
   firmware maps each index to one ink with no re-quantisation.

### Keeping them identical

"Byte-identical" is a stronger claim than it was, and it took making `#core`
port Pillow's actual implementations rather than reimplement its ideas. Four
places where the obvious JavaScript is subtly not what Pillow does, all found
in 2026-09 when the parity check was finally scripted:

- **Gaussian blur.** `ImageFilter.GaussianBlur` does not convolve a sampled
  Gaussian. It approximates one with *three box blurs* (`libImaging/BoxBlur.c`,
  box length from Gwosdek et al. 2011), in 24-bit fixed point, quantising back
  to 8 bits between passes. A textbook separable kernel is a different filter.
- **Auto levels.** `ImageOps.autocontrast` builds its LUT as `i*scale + offset`
  and **truncates**; its cutoff eats whole histogram bins rather than counting
  to a threshold. Assigning into a `Uint8ClampedArray` rounds, which is off by
  one in the other direction.
- **RGB to greyscale.** Pillow's is fixed point: `(R*19595 + G*38470 +
  B*7471 + 32768) >> 16`.
- **Brightness, contrast and colour.** Every `ImageEnhance` is
  `Image.blend(degenerate, image, factor)`, which **truncates**, and for
  factors outside 0..1 computes in 32-bit float before clipping.

Each of these is off by at most one unit out of 255, which sounds ignorable
and is not: a one-unit tone difference lands on the wrong side of a dither
threshold often enough to change 1-2% of pixels, and with error diffusion it
compounds to a third of them. Before the fixes the browser tool and the CLI
disagreed on ~1.8% of pixels at the default settings — which mattered,
because every panel test below was run through the CLI, so the browser (and
now the phone) was not rendering quite what won.

Two residual divergences are recorded and bounded rather than chased, since
nothing ships at those settings: `smooth` at 0.75 or 1.0 (radii where the
fixed-point box weights sum to exactly 2^24) differs by up to 2 units, and
contrast below 1 by up to 1. The shipped default of `smooth: 0.5` and
`contrast: 1.05` are both exact.

## Algorithms available

**Ordered (threshold-mask) dithers.** A fixed mask decides, per pixel, which
of a small set of candidate inks to place. No error travels between pixels,
so the texture is regular and the panel renders it cleanly.

- Masks: Bayer 8x8, Bayer 16x16 (finer steps), clustered-dot halftone 8x8,
  a 64x64 void-and-cluster blue-noise mask (CLI only).
- Candidate selection follows Yliluoma's arbitrary-palette algorithms:
  *greedy* (algorithm 1) accumulates up to `levels` inks whose running
  average approaches the target; *pair* (algorithm 2) picks the best two
  inks and a ratio with a penalty on distant pairs.
- *Mix gamma* sets the space in which candidate inks are averaged: Lab
  (0), linear light (2.2, physically what the eye does with neighbouring
  dots), or a power law between.
- *Chroma weight* scales the a/b axes in the Lab distance so hue accuracy can
  outrank lightness accuracy.

**Error diffusion.** Floyd-Steinberg, Jarvis, Stucki, Atkinson, serpentine
scan, with separate strength for the chroma channels. Tonally the most
accurate, but the panel turns the grain into visible noise.

## Experiment log

All variants of the same portrait test photo (a two-person selfie), judged
on the device. Letters are the suffixes burned into each file.

| Variant | Settings | Verdict |
|---------|----------|---------|
| A raw | Floyd-Steinberg, no tone changes | noisy, muddy |
| B default | Floyd-Steinberg, gamma 1.2, sat 1.25 | noisy |
| C bright | + gamma 1.4 | better shadows, still noisy |
| D jarvis | Jarvis kernel | smoother grain, still grain |
| E calm | blur 1.0, chroma strength 0.6 | calmer, soft |
| F blue-noise | ordered, blue-noise mask | regular texture helps, pattern visible |
| G bayer | Bayer 8x8, greedy 16 levels, gamma 1.2, sat 1.25 | **good**, clean |
| H | blue-noise 32 levels | not kept |
| I halftone | clustered dot | too coarse |
| J bayer6 | Bayer, 6 levels | too few steps |
| K bayer vivid | G + gamma 1.4, sat 1.5 | **good**, punchier |
| L / Lv pair | Bayer, pair mixing | clean flat areas, skin posterises to flat red/yellow |
| M / Mv linear | Bayer, mix gamma 2.2 | richer, darker; "close" |
| N / Nv pair+linear | both | solid red faces on black, rejected |
| O / Ov bayer16 | Bayer 16x16, 64 levels | **good**, softer crosshatch |
| P / Pv | Bayer 8x8, mix gamma 1.5 | between K and Mv |
| Q / Qv | Bayer 16x16, 64 levels, mix gamma 1.5 | **best** of the second round on three of five photos |
| R | Qv, matched against the driver table (control) | grey sky |
| S | Qv, matched against the calibrated inks, no boosts | **winner**: blue sky, natural faces |
| T | S + chroma weight 1.5 | close second, hard to tell from S |
| U | Qv + hue boosts (blue 1.7, green 1.4) + chroma weight 2.2, driver table | bluer than R, but S is better and needs no boosts |

The third round (group selfie with a vivid sky) settled the matching
palette question for skies: **S is the default** in both tools now (16x16
Bayer, 64 levels, mix gamma 1.5, gamma 1.4, saturation 1.5, calibrated inks).

Fourth round, butterfly on red flowers over blurred sunlit grass:

| Variant | Settings | Verdict |
|---------|----------|---------|
| R | driver-table matching (control) | closest, but not right |
| S | calibrated inks | grass solid yellow, flower off |
| Z / X / Y | S + green lean 15 / 25 / 35 (+ green boost) | grass and flower both wrong |

None looked correct; R was closest. The photo is a poor test (the whole
background is bokeh, which dithers into mush regardless), but the red flower
going wrong with calibrated matching is a real signal: the camera-sampled red
(156, 8, 5) is darker than the driver's (191, 0, 0), so a bright red flower
matches to red+white/yellow mixes instead of solid red. Cameras under-record
saturated inks, so the measured red and yellow are probably too dull while
the measured blue (the sky fix) is fine. Hypothesis to test: a hybrid palette
with measured blue/green/white/black and driver red/yellow.

Fifth round, yellow-orange bird among green leaves and red-brown litter
(sharp subject, strong red-orange and greens):

| Variant | Settings | Verdict |
|---------|----------|---------|
| H | hybrid palette (measured, with driver red/yellow) | close second |
| R | driver-table matching | third |
| S | calibrated inks | **best** |

So the hybrid hypothesis did not hold: the measured red and yellow are good
enough, and the butterfly round was the photo (out-of-focus background), not
the palette. **S stays the default.** The `hybrid` option remains in both
tools for experiments.

Sixth round, palm trees with a mountain and cloudy sky:

| Variant | Settings | Verdict |
|---------|----------|---------|
| R | driver-table matching | not good |
| S | calibrated inks | okay, but a little too green overall |
| Z | S + green lean 15 | not good |

Green lean is for sunlit yellow-green grass only; on ordinary foliage it
overshoots. Follow-up on the same photo:

| Variant | Settings | Verdict |
|---------|----------|---------|
| G | S + green gain 0.8 | **more** green than S, rejected |
| T | S + saturation 1.3 | **better than S** |

Lowering only the green chroma made the scene read greener, probably because
a duller green matches to green+black/white mixes where a saturated one
matches to green+yellow. Lowering global saturation is the lever for
foliage-heavy scenes, so T is now the *Foliage / landscape* preset. S stays
the default for people and skies.

Seventh round, portrait with a medium-brown complexion and a pink top:

| Variant | Settings | Verdict |
|---------|----------|---------|
| R / S / T | driver, calibrated, saturation 1.3 | none landed: skin too light, pink flat |
| V / W / X | gamma 1.1-1.2 + red boost 1.15-1.4 | rejected in simulation: top goes solid red, skin orange |
| U | gamma 1.2 | close second |
| Y | gamma 1.2, saturation 1.7, yellow 0.85 | far off |
| Z | gamma 1.0, saturation 1.6, yellow 0.85 | **closest**, now the *Darker complexion* preset |

Gamma is the lever for complexion: the default 1.4 lifts midtones for the
panel's crushed shadows, but on darker skin it lightens too much. Pinks are a
limit of the ink set: a light magenta-pink can only be red ink diluted with
white, and adding red makes it darker and warmer, never more vivid.

Findings from the second round (five photos: two selfies, a bird, a
butterfly on red flowers, palm trees with sky):

- Qv (16x16 Bayer, mix gamma 1.5, gamma 1.4, saturation 1.5) was the common
  favourite on the people photos and the bird.
- Nothing worked well on the butterfly or the palm trees. Those are the
  green-dominated scenes, where the palette is weakest.
- Blue skies rendered grey in every variant. Cause: a pale sky is closer to
  white than to the dark blue ink in Lab distance, so the matcher never
  reaches for blue. Fix implemented afterwards: per-hue chroma boost and
  chroma weighting. On the group selfie the sky preset takes blue ink from
  16% to 24% of pixels and the sky reads blue in the raw output. Not yet
  judged on the panel.

## Calibration result

A daylight photo of the chart on the panel (2026-09-02) gave these ink
colours after correcting the colour cast against the neutral grey bezel and
scaling so the panel's own white sits at 245 (the panel white is brighter
than the bezel; the camera had flattened the bright end):

| Ink | Driver table | Measured |
|-----|--------------|----------|
| black | 0, 0, 0 | 81, 62, 60 |
| white | 255, 255, 255 | 245, 245, 247 |
| yellow | 255, 243, 56 | 253, 244, 63 |
| red | 191, 0, 0 | 156, 8, 5 |
| blue | 100, 64, 255 | 91, 149, 218 |
| green | 67, 138, 28 | 133, 168, 64 |

The blue is the important one. The driver table says dark violet; the panel
shows a light sky blue. Every variant up to Q matched against the violet,
which is why pale skies always came out grey: in Lab a sky pixel really is
closer to white than to violet. Against the measured inks the same sky is
close to the blue ink itself. Both tools now offer matching against the
measured palette (*Match inks: calibrated* in the Photo Lab,
`--match-palette measured` in the CLI); the BMP palette stays the driver
table so the firmware addresses the same inks. The measured black is lighter
than reality because of camera flare; treat it as approximate.

Eighth round, butterfly again with the Foliage preset:

| Variant | Settings | Verdict |
|---------|----------|---------|
| R | driver table | still closest |
| S | default | not right |
| T | Foliage (saturation 1.3) | not right |

Conclusion: this photo is the outlier, not the settings. Its background is
entirely out of focus, and smooth bokeh has no structure for a dither to
hold on to, so it turns to mush with any palette. Photos with sharp subjects
and defined edges suit the panel; this one is better enjoyed elsewhere. If
it ever matters, the direction to try is treating the bokeh differently
from the subject (heavier blur or fewer levels in the background), not more
palette work.

## Open ideas

- **Background-aware processing** for bokeh-heavy photos (see the butterfly
  rounds): blur or posterise smooth regions harder than the subject so the
  dither has less noise to chase.
- **Hue-aware green handling.** Sunlit grass could be steered toward
  green+yellow+white mixes rather than pure yellow with a hue-band penalty.
- **Gamut mapping before dithering.** Compress out-of-gamut colours toward
  reachable mixes instead of letting the dither pick the nearest ink.
- **Structure-aware masks.** Rotate or vary the Bayer mask per hue to break
  the crosshatch where it is most visible (large sky areas).
- **Presets per subject** exist in the Photo Lab as starting values; they
  need tuning on the panel and then baking into the built-in list.
