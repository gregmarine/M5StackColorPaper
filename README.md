# M5StackColorPaper

Custom ESP-IDF firmware turning the [M5Stack PaperColor](https://docs.m5stack.com/en/core/PaperColor)
(SKU C151) into a simple USB-fed photo frame.

## How it works

- Plug the device into a computer over USB-C: it enumerates as a USB drive.
  Copy processed photos onto it (see `tools/prepare_photo/`), then eject and
  unplug. Holding any of the A/B/C keys for the first two seconds after
  power-on with USB attached skips drive mode and runs the slideshow instead,
  with the serial console still available (useful while charging, and for
  debugging with `idf.py monitor`).
- Press the side power button: the device wakes and stays awake for two
  minutes. The panel keeps its last photo while powered off, so waking does
  not redraw anything. The two side keys step through the photos: the upper
  key (G10) goes to the previous photo, the lower key (G9) to the next. The
  key on the top edge (G1) is reserved. Each press restarts the two-minute
  timer; when it expires the device powers itself off.

Photo changes are always button-driven, never automatic - the panel takes
~15-30s to do a full-color refresh, so the device stays off between viewing
sessions to save battery and only pays for a refresh when asked.

This firmware reuses the hardware-abstraction and slideshow code from
M5Stack's own official demo ([m5stack/M5PaperColor-UserDemo](https://github.com/m5stack/M5PaperColor-UserDemo),
MIT licensed) rather than re-deriving a driver for the Spectra 6 e-paper
panel from scratch. See `main/hal/` and `main/apps/local_photo_slideshow/`.

## Build & flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) v5.5.1+.

```
git submodule update --init --recursive
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

**After flashing, press the side power button once.** On the PaperColor the
ESP32-S3's USB-Serial-JTAG reset (what esptool does after flashing, and what
opening the serial port can also trigger) always lands the chip in ROM download
mode, so the freshly flashed app does not start on its own. The ROM sits at
`waiting for download` until the PM1 power manager restarts the chip via the
button. If a single press does nothing, press twice quickly to power off, then
once to power on.

Console output goes to the USB-Serial-JTAG port only until the app starts
TinyUSB; at that point the port disappears and the device re-enumerates as a
`PaperColor` mass-storage drive (VID 0x303A, PID 0x4002).

Full documentation is in [`docs/`](docs/README.md): firmware behaviour and
flashing, the photo workflow and Photo Lab, dithering notes and experiment
log, and hardware facts.

## Adding photos

The easiest path is the browser tool: open `tools/prepare_photo/photo_lab.html`,
drop photos on it, pick a preset, tweak while watching the simulated panel,
and save the BMP. See [`docs/photos.md`](docs/photos.md).

For batches, run photos through `tools/prepare_photo/prepare_photo.py` (see its README). It dithers
them on the computer against the panel's six real ink colours and writes
4-bit indexed BMPs; the firmware recognises those, turns the panel's own
dithering off, and maps each pixel 1:1 to a panel colour. Plain JPEG/PNG/BMP
files also work without pre-processing, but the firmware's on-the-fly ordered
dither gives visibly muddier colour.

The storage partition is not touched by `idf.py flash`, so photos survive
firmware updates. A brand-new board needs the empty filesystem written once
with `idf.py -p <PORT> storage-flash` (the drive mounts with auto-format
disabled).

## Out of scope (for now)

microSD card support, WiFi sync, and cloud photo push are not implemented -
this is intentionally a minimal USB-only, button-advanced photo frame. The
storage layer already supports switching to an SD-backed drive
(`hal_storage_switch(APP_STORAGE_MEDIA_SDMMC)`), so SD support is a natural
follow-up.
