# Firmware

Custom ESP-IDF app for the M5Stack PaperColor (SKU C151, ESP32-S3, 4.0"
E Ink Spectra 6 panel, 400x600). It reuses the hardware abstraction layer and
slideshow class from M5Stack's MIT-licensed
[M5PaperColor-UserDemo](https://github.com/m5stack/M5PaperColor-UserDemo)
and replaces the demo's menu/cloud system with a single-purpose photo frame.

## Boot flow

```
power on / side button
        |
   hal.init()  -> panel rotation 3 (landscape), 24-bit canvas in PSRAM
        |
   USB power present?
     yes -> hold any of A/B/C for 2 s?  no  -> DRIVE mode until unplugged, then power off
                                      yes -> SLIDESHOW with serial console attached
     no  -> SLIDESHOW
        |
   slideshow.resume()      restore saved photo index, draw nothing
   slideshow.runInteractive(120 s)
        |
        +-- top key -----> HOTSPOT mode, then back to the interactive window
        +-- USB appears -> DRIVE mode
        +-- idle 2 min --> power off
```

Three modes, never two at once. They are exclusive because the FAT volume has
a single owner: it is either exported to a USB host as a mass-storage device,
or mounted by the app, never both.

| Mode | Entered when | Leaves when |
|------|--------------|-------------|
| Drive | USB power present at boot | USB power removed, or the top key |
| Slideshow | no USB power at boot | 2 min idle, or the top key |
| Hotspot | the top key, from either | the top key, 15 min idle, or 1 h total |

Hotspot mode is the on-device photo manager: a WPA2 access point and a web
app served from flash. It is documented separately in [webapp.md](webapp.md).
Entering it detaches the device from USB (`hal_storage_usb_detach()`), which
makes the drive disappear from the host and hands the port back to the
USB-Serial-JTAG console — so unlike drive mode, the logs are readable while
it runs.

Key behaviours:

- **Nothing is drawn on wake.** The e-paper panel keeps its last image with
  the power off, so waking only restores the saved list position. A photo is
  drawn only when a side key asks for one. This is deliberate: a full-colour
  refresh takes 15-30 s and costs battery.
- **Photo index persists** in the RX8130 real-time clock's RAM (2 bytes),
  which survives power-off. It is clamped to the current file count on each
  wake, so adding or removing photos never breaks it.
- **USB drive mode** is entered whenever USB power is present at boot. The
  "host present" decision uses the PM1 power manager's VIN reading, not
  TinyUSB enumeration, because macOS can take seconds (and a permission
  prompt) before it configures the device. Drive mode ends when USB power
  goes away; ejecting alone does not end it.
- **Photo list** is every `.bmp`/`.jpg`/`.jpeg`/`.png` in the root of the
  drive, sorted by name. That sort is also what the web app's reordering
  manipulates, by rewriting the filenames with a `001_` prefix. Hidden files (`._*` AppleDouble files that Finder
  writes) are skipped.

## Buttons

M5Unified maps the PaperColor keys as A = GPIO 10, B = GPIO 9, C = GPIO 1
(all active low). Physically:

| Key | GPIO | M5 name | Function |
|-----|------|---------|----------|
| Upper side key | 10 | BtnA | Previous photo (up the list) |
| Lower side key | 9 | BtnB | Next photo (down the list) |
| Top-edge key | 1 | BtnC | Open / close the photo-manager hotspot |
| Side power button | (PM1) | – | Wake / power. Long press = ROM download mode |

The top key works from both drive and slideshow mode. It cannot wake the
device: `hal.powerOff()` is a PM1 system-off, not a sleep, so only the side
power button starts the board. Wake first, then press the top key.

Each press of a side key restarts the two-minute idle timer. A quick double press is
coalesced into one refresh (400 ms settle) so skipping two photos costs one
panel refresh. Presses made during a refresh are discarded.

## Panel rotation and the canvas

The panel is natively 400x600 portrait (rotation 0). `hal.init()` sets
rotation 3 for landscape. `PhotoSlideshow::displayPhoto()` reads the image's
dimensions and sets the panel to rotation 0 for portrait images and 3 for
landscape ones, then rebuilds the canvas at the panel's size with the canvas's
own rotation forced to 0. A pre-dithered 400x600 or 600x400 BMP therefore
lands 1:1 on panel pixels with scale 1.0 and no letterboxing.

Two things to know if this is ever touched again:

- An `M5Canvas` keeps its rotation across `deleteSprite()`/`createSprite()`.
  The upstream demo applied a saved rotation setting to the canvas, and that
  leftover rotation is what made every photo come out sideways with bars.
  `Hal::settingsInit()` no longer applies it.
- If portrait images appear upside down for the way the frame is held, swap
  `ROTATION_PORTRAIT` between 0 and 2 at the top of
  `main/apps/local_photo_slideshow/local_photo_slideshow.cpp`.

## Indexed BMPs and dithering

If the file is a 4-bit indexed BMP (what `tools/prepare_photo` writes), the
firmware switches the panel driver to `epd_fastest`, which disables the
driver's own ordered dither. The driver's nearest-colour lookup then maps
each palette entry to exactly one ink, so the pre-dithered pattern is
preserved. Plain JPEG/PNG/BMP files still work but go through the driver's
dither, which is visibly muddier. The canvas is 24-bit for the same reason: a
16-bit sprite would put the six palette colours through a 565 round trip.

## Build and flash

Requires ESP-IDF v5.5.1 or later.

```
git submodule update --init --recursive
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash
```

The `storage` partition (the photo drive) is not written by `idf.py flash`,
so photos survive firmware updates. A brand-new board needs the empty
filesystem written once with `idf.py -p PORT storage-flash`; the partition is
mounted with auto-format disabled.

### The USB-JTAG reset quirk

On this board every reset driven over the ESP32-S3's built-in USB-Serial-JTAG
(esptool's `hard_reset` after flashing, DTR/RTS toggles, sometimes just
opening the port) lands the chip in ROM download mode with
`boot:0x23 (DOWNLOAD(USB/UART0))`. The freshly flashed app does not start on
its own.

Practical sequence:

1. Plug in, **hold the side button** until the board enters download mode.
   It enumerates as "USB JTAG/serial debug unit" and `/dev/cu.usbmodemNNN`
   appears.
2. `idf.py -p /dev/cu.usbmodem* flash`
3. **Press the side button once.** The app starts. With USB attached it
   goes to drive mode unless a side key is held during the first two seconds.

While the app runs TinyUSB, the USB-JTAG port disappears and the device
shows up only as the `PaperColor` mass-storage device, so serial logs stop at
that point. To see logs, boot with a key held (slideshow mode keeps the
console) and capture with a serial tool that tolerates re-enumeration;
`idf.py monitor` needs a real TTY and its port open can itself reset the
board.

## Code map

```
main/main.cpp                          boot flow, the three modes, interactive window
main/net/                              Wi-Fi AP, HTTP server, photo API, hotspot session
main/web/app.ui.html                   the phone web app (pipeline spliced in at build time)
components/dns_server/                 wildcard DNS for the captive portal (from ESP-IDF's example)
main/hal/hal.cpp, hal.h                board init, PM1 power manager, RTC RAM, settings (from the M5 demo)
main/hal/storage/                      FAT on internal flash, TinyUSB MSC, SD switching (unused)
main/hal/utils/image/image_utils.cpp   image size probing, indexed-BMP detection
main/apps/local_photo_slideshow/       photo list, index persistence, display, buttons
main/fatfs_image/                      empty; source for the pre-formatted storage image
partitions.csv                         nvs, phy, factory app (0x9F0000), storage FAT (6 MB)
components/M5GFX, M5Unified            git submodules (panel driver Panel_ED2208 lives in M5GFX)
```

Timing constants worth knowing: `INTERACTIVE_IDLE_MS` (120 s) in
`main/main.cpp`; the 2 s key-hold window is in the same file; the 400 ms
double-press settle is in `PhotoSlideshow::runInteractive()`; `AP_IDLE_MS`
(15 min), `AP_MAX_SESSION_MS` (1 h) and `AP_MIN_BATTERY_MV` (3500) are in
`main/net/ap_mode.cpp`.

`runInteractive()` returns an `InteractiveExit` saying why it stopped (idle,
aborted by USB, or the top key) so `main.cpp` can decide what happens next.
`PhotoSlideshow` also gained `showIndex()` and `redrawCurrent()`, which the
web app and the hotspot's exit path use to draw a specific photo and persist
the position to RTC RAM.
