# Documentation

Firmware and tooling for turning the M5Stack PaperColor into a
button-advanced photo frame that takes photos over USB or over its own Wi-Fi
hotspot.

| Document | What it covers |
|----------|----------------|
| [webapp.md](webapp.md) | The on-device photo manager: the Wi-Fi hotspot, the phone web app, the HTTP API, how the app is built from Photo Lab |
| [firmware.md](firmware.md) | Boot flow, USB drive mode, button mapping, panel rotation, build and flash, the USB-JTAG reset quirk, code map |
| [photos.md](photos.md) | Getting photos onto the frame: the Photo Lab browser tool, presets, the command-line tool, ink calibration, copying to the drive |
| [dithering.md](dithering.md) | How six-ink rendering works, the algorithms available, the A/B experiments run so far and what they showed, open ideas |
| [hardware.md](hardware.md) | Board facts: USB identity, GPIO map, panel geometry, power manager, flash partitions |

Quick start:

1. Build and flash once (see firmware.md), press the side button.
2. Press the top-edge key, join the Wi-Fi network shown on the panel, and add
   photos from your phone (see webapp.md).

Or, from a computer:

1. Open `tools/prepare_photo/photo_lab.html`, drop in photos, save BMPs.
2. Plug the frame in, copy the BMPs to the `Espressif` drive, eject, unplug.
3. Press the side button, then the lower side key to draw the first photo.
