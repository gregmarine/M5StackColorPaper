# M5StackColorPaper

ESP-IDF firmware that turns the M5Stack PaperColor (ESP32-S3, 4" E Ink
Spectra 6, 400x600) into a button-advanced photo frame fed either over USB or
over its own Wi-Fi hotspot, plus the tools that prepare photos for it. Full
documentation lives in `docs/`; read `docs/README.md` first.

## Layout

- `main/` firmware (`main.cpp` boot flow; `hal/` and `apps/` reused from M5Stack's
  demo). `docs/firmware.md` explains the boot flow, the three modes, buttons,
  rotation.
- `main/net/` the on-device photo manager: soft AP, HTTP server, photo API,
  hotspot session. `main/web/app.ui.html` is its phone UI.
  `docs/webapp.md` covers all of it.
- `tools/prepare_photo/` photo pipeline: `photo_lab.html` (browser tool,
  primary) and `prepare_photo.py` (CLI, batch).
- `tools/test/` the host test suite. `tools/build_webapp.py` builds the
  on-device web app.
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

## Tests

```
tools/test/run.sh          # 40+ tests, ~20 s; needs the venv and macOS's jsc
idf.py build               # must also pass
```

Stdlib `unittest` on purpose: no pytest, nothing to install beyond the
existing `tools/prepare_photo/venv`. Run both before committing.

What they cover: byte-for-byte parity between `prepare_photo.py`, Photo Lab's
`#core` and the phone app; the 4-bit BMP header the firmware depends on; and
that the on-device web app is genuinely built from Photo Lab's pipeline
rather than a fork. What they cannot cover is how a photo *looks* — that is
still a panel test, and the panel is the judge.

## Working with the device

Every physical step (plug in, press or hold the side button, press the top
key, mount the drive) is Greg's. Ask for it as a short numbered list, end the
turn, and act only after they confirm. Do not start background watchers that
wait for the board; they time out while Greg is away.

The photo drive mounts as `/Volumes/Espressif`. To load a test set: delete
the old `.bmp` files, copy the new ones with absolute paths, `dot_clean -m`,
`sync`, then `diskutil unmount`. The firmware shows files in name order and
each test file has its variant letter burned into the corner (`--label auto`).

For anything involving the hotspot, note that entering it detaches USB, so
the drive disappears and the serial console comes back — which makes it the
one mode whose logs can be watched over the cable.

If the web app is unreachable from an Android phone, it is almost certainly
the "stay connected to a network with no internet?" prompt going unanswered,
not the firmware. Android will not route the browser to the frame until that
is accepted, and the notification is silent on Do Not Disturb. The panel says
so; `docs/webapp.md` explains why it cannot be avoided.

## Photo pipeline conventions

- Output is always a 4-bit indexed BMP whose palette is `PANEL_PALETTE` (the
  driver's table). The palette the dither *matches against* is separate and
  defaults to the calibrated `MEASURED_PALETTE`.
- Current panel-tested default (variant S): 16x16 Bayer, 64 levels, mix gamma
  1.5, gamma 1.4, saturation 1.5, calibrated inks, no hue boosts. Presets in
  `photo_lab.html` are only ones that won a panel test; keep it that way.
- **Parity is now enforced, not documented.** `photo_lab.html`'s `#core` is a
  port of `prepare_photo.py` and `tools/test/test_parity.py` holds them to
  byte-identical BMPs. If you change either, run the tests; if they fail, the
  fix is to make the port faithful, not to loosen the bound. `#core` ports
  Pillow's *implementations* (three-box-blur Gaussian, truncating autocontrast
  LUT, fixed-point RGB->L, `Image.blend`), not just its intent —
  `docs/dithering.md` explains why each one matters.
- `photo_lab.html` has three script blocks and the split is load-bearing:
  `#core` (DOM-free pipeline) and `#shared` (canvas helpers) are spliced into
  the device web app by `tools/build_webapp.py`; `#ui` is desktop-only. Adding
  something the phone needs to `#ui` will not fail the build, it will fail on
  the phone.
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
