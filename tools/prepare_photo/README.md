# prepare_photo

Converts ordinary photos into files the PaperColor photo frame can show with
the best colour the panel is capable of.

## Why pre-convert

The E Ink Spectra 6 panel has exactly six colours: black, white, yellow, red,
blue, green. Any photo has to be dithered down to those. The firmware can do
that itself (drop a JPEG/PNG on the drive and it works), but the panel driver
uses a fast ordered dither with a nearest-colour lookup, and the result is
muddy. This tool does the reduction on the computer with error-diffusion
dithering, tone adjustments, and the driver's own measured panel colours, and
writes a **4-bit indexed BMP** whose palette holds those exact RGB values.
The firmware recognises indexed BMPs, turns the panel's dithering off, and
maps every pixel 1:1 to its intended panel colour. Nothing is re-quantized.

A 600x400 4-bit BMP is 120 KB, so the 6 MB drive holds about 50 photos.

## Photo Lab (interactive, in the browser)

`photo_lab.html` is the interactive version of this tool: open it in a
browser (double-click, no server needed), drop photos on it, pick a preset,
tweak sliders while watching a simulated view of the panel, and press
**Save BMP** (or Cmd-S). The dithering code is a line-for-line port of
`prepare_photo.py` and produces identical files for the same settings.

- **Presets** (Default for people and skies, Foliage / landscape, Darker complexion) are panel-tested starting
  points; adjust anything, then *Save as new* to keep your own. They are
  stored in the browser; *Export JSON* writes them to a file you can commit or
  import elsewhere.
- **Per-hue boosts** and **chroma weight** are the knobs for the panel's weak
  blues and greens: a boost raises that hue's saturation before dithering,
  and chroma weight above 1 makes the matcher prefer a darker-but-coloured
  mix over a grey one (this is what turns a grey sky blue).
- **Panel simulation** draws the result with estimated ink colours and a
  slight blur, at the panel's real size (~180 dpi). The ink colours are a
  guess until calibrated: show `chart.bmp` on the device in daylight and
  adjust the six swatches until they match. The *raw palette pixels* view
  shows exactly what the file holds.
- Each loaded photo remembers its own settings while the page is open.
- HEIC opens in Safari; Chrome cannot decode it. Convert first with
  `sips -s format jpeg photo.heic --out photo.jpg`.
- Works on a phone too if served from this folder
  (`python3 -m http.server 8000`, then open `http://<mac-ip>:8000/photo_lab.html`).

## Setup (command-line tool)

```
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

## Usage

```
python3 prepare_photo.py photo1.jpg photo2.png -o out/
python3 prepare_photo.py ~/Pictures/vacation/ -o out/ --fit contain
python3 prepare_photo.py -o out/ --chart          # calibration chart only
```

Then copy the `.bmp` files from `out/` onto the PaperColor's USB drive. If
you copy with Finder on macOS, run `dot_clean -m /Volumes/Espressif` before
ejecting so the hidden `._*` companion files don't fill the drive.

Portrait photos become 400x600 and landscape ones 600x400; the firmware
rotates the panel to match each image. iPhone `.heic` files open directly
(`pillow-heif` is in `requirements.txt`).

## Tuning

Everything runs before dithering, so experiment freely and look at the
`.bmp` on screen; what you see is what the panel gets (modulo the panel's
real ink colours).

| Flag | Default | Effect |
|------|---------|--------|
| `--dither` | `bayer16` | Ordered: `bayer16` (16x16, the panel-tested winner with 64 levels), `bayer` (8x8, slightly crisper crosshatch), `blue-noise`, `halftone` (clustered dot). Error diffusion: `floyd-steinberg`, `jarvis`, `stucki` (smoother), `atkinson` (punchier, crushes shadows); the panel exaggerates their grain. `none` for flat graphics |
| `--ordered-levels` | 64 | For the ordered dithers: how many inks may mix per pixel (`--mix greedy`) or ratio steps between the two inks (`--mix pair`). Higher = finer tonal steps |
| `--mix` | `greedy` | How ordered dithers choose a pixel's inks. `greedy` lets any number of inks average toward the colour. `pair` picks the best two inks and a ratio, penalising distant pairs: cleaner flat areas and no stray dots, but skin needs three inks and posterises into flat red/yellow patches |
| `--mix-gamma` | 1.5 | Average inks in (sRGB)^gamma instead of Lab. `2.2` is linear light: physically truer, darker and richer midtones. `1.0` is close to Lab mixing, `0` mixes in Lab. `1.5` sits between and tested best |
| `--mix-spread` | 0.1 | `--mix pair` only: penalty weight on the distance between the two inks |
| `--orientation` | `auto` | Portrait photos become 400x600 and landscape ones 600x400; the firmware rotates the panel to match. `fixed` forces `--width`x`--height` |
| `--strength` | 1.0 | Fraction of the lightness error that gets diffused. Below 1.0 the shadows crush to solid black on this palette, so leave it |
| `--chroma-strength` | 0.75 | Fraction of the colour error diffused (lab only). Lower = fewer stray green/blue dots in skin and greys, less saturation. 0.5 already loses most colour in muted photos |
| `--smooth` | 0.5 | Gaussian blur radius before dithering. Removes texture the panel can't show and calms the pattern; 0 disables |
| `--anchor` | 0.3 | With `--fit cover`, which part to keep along the cropped axis: 0 top/left, 0.5 centre, 1 bottom/right |
| `--suffix` | | Appended to output names, for making A/B variants of one photo |
| `--space` | `lab` | Colour space for matching and error diffusion. `lab` is perceptual: greys become black/white mixes. `rgb` is naive and turns greys into green/blue/yellow speckle |
| `--error-limit` | 0 (lab) / 160 (rgb) | Clamps diffused error. 0 = classic unclamped. Clamping in Lab crushes shadows to black, so leave it off unless worm artifacts bother you |
| `--no-auto-levels` | off | Skip the 1% histogram stretch. Auto-levels is on by default because six inks have so little tonal range that an unstretched photo comes out mostly black or mostly white |
| `--saturation` | 1.5 | Panel inks are dull; a boost before dithering compensates |
| `--hue-red` `--hue-yellow` `--hue-green` `--hue-blue` | 1.0 | Chroma multiplier for one hue band only, applied after `--saturation`. Same as the Photo Lab's hue boosts |
| `--match-palette` | `measured` | Ink colours the dither matches against. `measured` (default, tested best) uses the panel's real inks sampled from the chart photo (its blue is a light sky blue, its green a leaf green), which lets pale skies reach blue without boosts. `driver` uses M5's table. `hybrid` takes measured black/white/blue/green with the driver's red/yellow, since cameras under-record saturated inks. The BMP palette is always the driver table |
| `--green-lean` | 0 | Degrees to rotate yellow-green hues toward green before matching. Sunlit grass and leaves sit closer to the panel's yellow than its green and otherwise come out solid yellow; 20-30 pulls them green |
| `--chroma-weight` | 1.0 | Weight of hue/saturation vs lightness when matching inks. Around 2 the dither prefers a darker-but-blue mix for a sky over a grey one |
| `--contrast` | 1.05 | |
| `--brightness` | 1.0 | |
| `--gamma` | 1.4 | >1 lifts shadows, which the panel tends to crush into black |
| `--fit` | `cover` | Crop to fill the panel; `--anchor` picks which part survives. `contain` letterboxes on white and keeps the whole photo |

`--chart` writes `chart.bmp`: six solid swatches on top, then dithered ramps
(grey, white->red, white->blue, white->green, yellow->red, blue->green).
Show it on the frame to judge two things: whether the solid swatches look
like the colours in `PANEL_PALETTE` at the top of `prepare_photo.py` (edit
those values if not; the firmware needs no change), and how the dithering
pattern reads at viewing distance.
