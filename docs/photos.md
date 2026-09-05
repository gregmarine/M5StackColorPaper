# Preparing photos

The panel shows exactly six colours: black, white, yellow, red, blue, green.
Everything else is faked by dithering, and the quality of that dithering is
decided on the computer, not on the device. The workflow is:

1. Turn a photo into a 400x600 (portrait) or 600x400 (landscape) 4-bit
   indexed BMP with the Photo Lab or the command-line tool.
2. Copy the BMP to the frame's USB drive.
3. Step to it with the side keys.

Both tools live in `tools/prepare_photo/` and produce identical files for the
same settings; the browser tool's dither is a line-for-line port of the
Python one, and `tools/test/run.sh` holds them to byte-identical output.

**Or skip the computer entirely.** Press the frame's top-edge key and it
raises a Wi-Fi hotspot serving the same pipeline as a phone web app: pick a
photo, choose a preset, send it. That is the shortest path and usually the
right one — see [webapp.md](webapp.md). The rest of this page is the desktop
route, which is still what you want for batches, for tuning, and for the
A/B rounds in [dithering.md](dithering.md).

## Photo Lab (browser)

Open `tools/prepare_photo/photo_lab.html` by double-clicking it. Chrome and
Safari both work. Chrome cannot decode HEIC, so convert iPhone photos first:

```
sips -s format jpeg IMG_1234.heic --out IMG_1234.jpg
```

Then:

- **Drop photos** onto the page or use the file picker. Several can be
  loaded; the dropdown switches between them and each remembers its own
  settings while the page is open.
- **Pick a preset**, then adjust. The presets are starting points by subject:
  *Default (S)* for people and skies; *Foliage / landscape (T)*, which is
  S with saturation 1.3 for scenes that are mostly green; and *Darker
  complexion (Z)*, gamma 1.0 with saturation 1.6 and a little less yellow,
  for portraits the default renders too light. All three are panel-tested.
  Once a slider moves the preset shows "modified".
- **Save as new…** stores your settings as a preset in the browser. *Update*
  overwrites one of yours, *Delete* removes it. *Export JSON* writes all your
  presets (and the calibrated ink colours) to a file; *Import JSON* reads one
  back, so presets can be moved between browsers or committed to the repo.
- **Save BMP** (or Cmd-S) writes the file to your Downloads folder with the
  optional filename suffix. The optional *Label* burns text into the corner,
  which is handy for A/B comparisons on the device.

Controls, top to bottom:

| Group | Control | What it does |
|-------|---------|--------------|
| Crop | Fit | *cover* crops to fill the frame; *contain* letterboxes on white |
| | Anchor | With cover, which part of the cropped axis to keep (0 top, 1 bottom) |
| | Orientation | auto picks portrait/landscape from the photo; the firmware rotates the panel to match |
| Tone | Smooth | Gaussian blur before dithering. Calms the pattern, removes detail the panel can't show anyway |
| | Auto levels | 1% histogram stretch. Usually on: six inks have so little range that an unstretched photo comes out mostly black or white |
| | Gamma | >1 lifts shadows, which the panel otherwise crushes to black. 1.3-1.4 has been the sweet spot |
| | Brightness, Contrast | As in any editor, applied before dithering |
| Colour | Saturation | Global boost. The inks are dull; 1.5 compensates |
| | Red/Yellow/Green/Blue boost | Chroma multiplier for that hue band only. Skin lives in red/yellow, sky in blue, foliage in green |
| | Green lean | Rotates yellow-green hues toward green before matching, for sunlit grass that otherwise renders solid yellow. Overshoots on ordinary foliage; leave at 0 unless grass is the subject |
| | Chroma weight | How much the ink matcher cares about hue versus lightness. With calibrated inks this is rarely needed; it was the workaround for grey skies before calibration |
| Dither | Match inks | Which ink colours the matcher believes in. *calibrated* (default, panel-tested) uses the simulated swatches; *driver table* is M5's palette; the *Hybrid* button swaps in the driver's red and yellow |
| Dither | Pattern | *bayer 16x16* with 64 levels is the current best. Error-diffusion patterns are there for comparison; the panel exaggerates their grain |
| | Levels | Tonal steps per pixel (greedy) or ratio steps (pair) |
| | Mix | *greedy* lets any inks average toward the colour. *pair* uses two inks: cleaner flat areas but skin posterises |
| | Mix gamma | Space in which inks are averaged. 0 = Lab, 2.2 = linear light (darker, richer), 1.5 = between (the tested favourite) |
| Simulation | View | *simulated panel* (estimated inks + blur), *raw palette pixels* (exactly what the file holds), *adjusted* (before dithering, for judging tone) |
| | Zoom | *actual size* is the panel's ~180 dpi |
| | Blur | Viewing-distance blur in panel pixels |
| | Ink swatches | The six simulated ink colours, see calibration below |

## Calibrating the simulated inks

The Photo Lab's simulated view is only as good as its six ink colours. The
built-in estimate was sampled from a daylight photo of the chart on the
panel (2026-09-02). The photo's colour cast was corrected against the bezel
(a neutral grey) and the values scaled so the panel's own white sits at 245,
because the panel white is brighter than the bezel and the camera had
flattened the bright end. Relative colours are trustworthy; absolute levels
less so, and black is lighter than reality because of camera flare.

| Ink | Driver table | Measured on panel |
|-----|--------------|-------------------|
| black | 0, 0, 0 | 81, 62, 60 |
| white | 255, 255, 255 | 245, 245, 247 |
| yellow | 255, 243, 56 | 253, 244, 63 |
| red | 191, 0, 0 | 156, 8, 5 |
| blue | 100, 64, 255 | 91, 149, 218 |
| green | 67, 138, 28 | 133, 168, 64 |

The blue and green differ a lot from the driver table: the panel's blue is a
light sky blue, not a dark violet. That is why the *Match inks* control
exists in the Dither group (and `--match-palette measured` in the CLI): with
*calibrated* the dither matches colours against the real inks, so a pale sky
maps to blue/white mixes naturally instead of needing a blue boost. The BMP
still addresses inks through the driver table either way.

To re-calibrate (different unit, different light):

1. Generate the chart (already in `tools/prepare_photo/out/chart.bmp`, or
   `python3 prepare_photo.py -o out/ --chart`). The top band is six solid
   swatches; below are dithered ramps.
2. Copy `chart.bmp` to the drive and display it on the frame.
3. In daylight or neutral white light, photograph the panel straight on, or
   simply hold it next to the screen.
4. In the Photo Lab, open the *Panel simulation* section and set each swatch
   to the colour you see on the panel. A photo can be sampled with any colour
   picker. The values persist in the browser and are included in *Export
   JSON*.

The ramps on the chart are also the quickest way to judge how a dither
pattern reads at viewing distance.

## Command-line tool

For batches, the same pipeline is available as `prepare_photo.py`:

```
cd tools/prepare_photo
python3 -m venv venv && source venv/bin/activate && pip install -r requirements.txt
python3 prepare_photo.py ~/Pictures/trip/ -o out/
python3 prepare_photo.py photo.jpg -o out/ --gamma 1.3 --saturation 1.6 --hue-blue 1.7 --chroma-weight 2.2
```

It opens HEIC directly. Every Photo Lab control has a flag; the full table is
in `tools/prepare_photo/README.md`. The defaults are the Photo Lab's
*Default (S)* preset: 16x16 Bayer with 64 levels, mix gamma 1.5, gamma 1.4,
saturation 1.5, matched against the calibrated inks. A plain
`python3 prepare_photo.py photo.jpg -o out/` therefore produces the same
file the Photo Lab saves with that preset.

## Copying to the frame

(Or use the hotspot instead: [webapp.md](webapp.md).)

1. Plug the frame in. If the `Espressif` disk does not appear within a few
   seconds, press the side button once.
2. Copy the `.bmp` files to the root of the disk. If you use Finder, run
   `dot_clean -m /Volumes/Espressif` before ejecting so the hidden `._*`
   companion files don't clutter the drive (the firmware skips them anyway).
3. Eject, unplug. The frame powers itself off.
4. Press the side button, then the lower side key to draw the first photo.

The disk is 6 MB and each BMP is about 176 KB at 400x600, so roughly 30-48
photos fit. Files are
shown in name order. The volume label `Espressif` comes from the empty
filesystem image; the USB device name is `PaperColor`.
