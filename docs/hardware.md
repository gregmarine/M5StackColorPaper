# Hardware notes

Facts about the M5Stack PaperColor (SKU C151) that the firmware depends on
or that were learned the hard way.

## Board

- ESP32-S3 with 16 MB flash and octal PSRAM (M5GFX refuses to drive the panel
  without OPI PSRAM enabled; `sdkconfig.defaults` sets it).
- 4.0" E Ink Spectra 6 panel, 400x600, driver `Panel_ED2208` in M5GFX. Native
  orientation is portrait (rotation 0). About 180 pixels per inch. A full
  colour refresh takes roughly 15-30 s. The image persists with power off.
- M5 PM1 power manager on I2C (SDA GPIO 3, SCL GPIO 2). It owns the side
  power button, VIN sensing, EPD and SD power rails, and a watchdog that the
  driver disables at init.
- RX8130 real-time clock at I2C 0x32. Its battery-backed RAM holds the
  slideshow's current photo index.
- SHT40 temperature/humidity sensor at 0x44 (unused).
- Speaker and microphone on I2S (unused apart from a button click tone).
- 2.4 GHz Wi-Fi, used only as a soft AP for the on-device photo manager
  ([webapp.md](webapp.md)). The frame never joins another network. The radio
  is the heaviest current draw on the board, so the hotspot refuses to start
  below 3.5 V and caps its own session at an hour.

## GPIO map (as used here)

| Function | GPIO |
|----------|------|
| Key A, upper side key | 10 (active low) |
| Key B, lower side key | 9 (active low) |
| Key C, top edge | 1 (active low) — opens the photo-manager hotspot |
| Panel SPI MOSI / MISO / SCLK | 13 / 14 / 15 |
| Panel DC / CS / RST / BUSY | 43 / 44 / 12 / 11 |
| SD card CS | 47 |
| I2C SDA / SCL | 3 / 2 |
| Speaker I2S MCK / BCK / WS / DOUT | 42 / 40 / 41 / 38 |
| Mic I2S DIN | 39 |

## USB identity

The same physical board appears in two ways depending on what is running:

| State | idVendor | idProduct | Product string |
|-------|----------|-----------|----------------|
| ROM download mode / stock USB-Serial-JTAG | 0x303A | 0x1001 | "USB JTAG/serial debug unit" |
| This firmware in drive mode | 0x303A | 0x4002 | "PaperColor" (manufacturer "M5Stack") |

The USB serial number is the ESP32-S3 factory MAC and identifies this
specific unit if several devices are attached. The macOS device path
`/dev/cu.usbmodemNNNN` is not stable; re-derive it each time
(`ls /dev/cu.usbmodem*`). The mass-storage volume mounts as `Espressif`.

On macOS, `system_profiler SPUSBDataType` may return nothing from a sandboxed
shell; `ioreg -r -c IOUSBHostDevice -l` works.

## Reset behaviour

Any reset driven through the USB-Serial-JTAG peripheral lands the chip in
ROM download mode instead of running the app (`rst:0x15
(USB_UART_CHIP_RESET), boot:0x23 (DOWNLOAD(USB/UART0))`). This includes
esptool's post-flash hard reset, DTR/RTS pulses, and sometimes merely opening
the port. Software resets do not re-sample the strap pins. The only reliable
way to start the app is the side button (press once; if nothing happens,
press twice quickly to power off, then once). Holding the side button enters
download mode on purpose.

## Flash layout (`partitions.csv`)

| Name | Type | Offset | Size |
|------|------|--------|------|
| nvs | data/nvs | 0x9000 | 24 KB |
| phy_init | data/phy | 0xF000 | 4 KB |
| factory | app | 0x10000 | 9.94 MB |
| storage | data/fat | 0xA00000 | 6 MB |

`storage` is the USB drive. It is mounted with auto-format disabled and is
not touched by `idf.py flash`; write the empty image once with
`idf.py storage-flash` on a new board.
