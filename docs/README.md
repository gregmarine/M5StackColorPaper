# Documentation

Firmware and tooling for turning the M5Stack PaperColor into a USB-fed,
button-advanced photo frame.

| Document | What it covers |
|----------|----------------|
| [firmware.md](firmware.md) | Boot flow, USB drive mode, button mapping, panel rotation, build and flash, the USB-JTAG reset quirk, code map |
| [photos.md](photos.md) | Getting photos onto the frame: the Photo Lab browser tool, presets, the command-line tool, ink calibration, copying to the drive |
| [dithering.md](dithering.md) | How six-ink rendering works, the algorithms available, the A/B experiments run so far and what they showed, open ideas |
| [hardware.md](hardware.md) | Board facts: USB identity, GPIO map, panel geometry, power manager, flash partitions |

Quick start:

1. Build and flash once (see firmware.md), press the side button.
2. Open `tools/prepare_photo/photo_lab.html`, drop in photos, save BMPs.
3. Plug the frame in, copy the BMPs to the `Espressif` drive, eject, unplug.
4. Press the side button, then the lower side key to draw the first photo.
