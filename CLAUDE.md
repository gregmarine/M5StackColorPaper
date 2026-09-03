# M5StackColorPaper

ESP-IDF firmware that turns the M5Stack PaperColor (ESP32-S3, 4" E Ink
Spectra 6, 400x600) into a USB-fed, button-advanced photo frame, plus the
tools that prepare photos for it. Full documentation lives in `docs/`; read
`docs/README.md` first.

## Layout

- `main/` firmware (`main.cpp` boot flow; `hal/` and `apps/` reused from
  M5's demo). `docs/firmware.md` explains the boot flow, buttons, rotation.
- `tools/prepare_photo/` photo pipeline: `photo_lab.html` (browser tool,
  primary) and `prepare_photo.py` (CLI, batch). Both must produce identical
  files for identical settings; see "Parity" below.
- `docs/dithering.md` is the experiment log. Every panel test goes there.

## Build and flash

```
source ~/esp/esp-idf/export.sh && idf.py build
idf.py -p /dev/cu.usbmodem* flash
```

The board must be in ROM download mode to flash (hold the side button), and
after flashing it stays in download mode until the side button is pressed
once. Any USB-JTAG-driven reset lands in download mode; do not try to reset
over USB. Details and the USB identity are in `docs/hardware.md`.

## Working with the device

Every physical step (plug in, press or hold the side button, mount the
drive) is Greg's. Ask for it as a short numbered list, end the turn, and act
only after they confirm. Do not start background watchers that wait for the
board; they time out while Greg is away.

The photo drive mounts as `/Volumes/Espressif`. To load a test set: delete
the old `.bmp` files, copy the new ones with absolute paths, `dot_clean -m`,
`sync`, then `diskutil unmount`. The firmware shows files in name order and
each test file has its variant letter burned into the corner (`--label auto`).

## Photo pipeline conventions

- Output is always a 4-bit indexed BMP whose palette is `PANEL_PALETTE` (the
  driver's table). The palette the dither *matches against* is separate and
  defaults to the calibrated `MEASURED_PALETTE`.
- Current panel-tested default (variant S): 16x16 Bayer, 64 levels, mix gamma
  1.5, gamma 1.4, saturation 1.5, calibrated inks, no hue boosts. Presets in
  `photo_lab.html` are only ones that won a panel test; keep it that way.
- Parity: `photo_lab.html`'s `#core` script is a port of `prepare_photo.py`.
  After changing either, extract the core and run it under
  `/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc`
  against Python reference output (node is not installed). Ordered dithers
  must match 0.000% of pixels; error diffusion uses float64 for the same
  reason. Tone-stage changes may differ by 1 unit from rounding.
- The CLI is pure Python and slow (~40 s per photo at the default settings);
  run variants in parallel with `&` and `wait`.
- `tools/prepare_photo/out/` is gitignored scratch: A/B variants, the chart,
  test photos.

## Testing loop

Generate labelled variants, preview a simulated contact sheet (map the BMP
palette to `MEASURED_PALETTE` values), load them on the drive, and let Greg
judge on the panel. Keep sets to 3-5 files: each refresh takes 20-30 s.
Record every round in `docs/dithering.md` with settings and verdict, and
promote a winner to a preset only after it beats the current default on the
panel. Simulation has been directionally right but not decisive; the panel
is the judge.
