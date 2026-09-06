# The on-device photo manager

Press the top-edge key and the frame raises its own Wi-Fi network. Join it
from a phone and you get a web app that prepares photos, uploads them, and
manages what is already on the device — no cable and no computer.

```
top key
   |
   v
Wi-Fi AP "PaperColor-XXXX"  (WPA2)     <-- credentials drawn on the panel
   |
   +-- DNS: every name -> 192.168.4.1
   +-- HTTP on 192.168.4.1
         /            the web app
         /api/*       photos, storage, ordering
         probes       answered truthfully, so the phone routes here
```

## Using it

1. Wake the frame with the side button, then press the **top-edge key**.
2. The panel redraws with the network name, password and `http://192.168.4.1`.
   The access point is already up before the redraw starts, so you can join
   while it is still painting — a colour refresh takes 15–30 s.
3. Join that network from your phone, then open `http://192.168.4.1` in
   Chrome or Safari. Nothing pops up on its own, on purpose — see below.
   **On Android, answer the "stay connected to a network with no internet?"
   prompt first**, or the browser cannot reach the frame at all. It is a
   once-per-phone answer and it is easy to miss on Do Not Disturb.
4. **Library** lists what is on the frame: tap *Show* to draw one, the arrows
   to reorder, *Delete* to remove one. **Add photo** picks a photo, prepares
   it, and sends it.
5. Press the top key again when you are done. The frame redraws the current
   photo and goes back to the normal two-minute button window.

The credentials are generated once and kept in NVS, so they never change and
your phone will rejoin on its own after the first time.

### There is deliberately no captive portal

Joining the hotspot does **not** pop up a Wi-Fi sign-in window. The frame
answers the phone's connectivity probes truthfully — a real `204` to Android's
`/generate_204`, Apple's expected `Success` body, and the two Windows strings
— and advertises no RFC 8910 portal URI. The phone concludes the network is
fine and stops there.

That looks backwards for a device with no internet, and it is the single most
important thing about this page.

A phone that decides it is behind a captive portal **does not make that
network its default route**. On Android, everything except the sign-in WebView
carries on using mobile data, so Chrome cannot reach `192.168.4.1` at all —
the address simply never loads. The sign-in window it gives you instead is a
stock WebView whose host app implements no file chooser, so the photo picker
is dead in it: tapping *Choose photo* does nothing, with no error. Hijacking
the probes therefore costs the only browser that can do the job, to gain a
window that cannot.

Passing the probes gets iOS the whole way there: it validates the network and
routes to the frame with nothing further asked.

Android goes one step further than iOS and validates over **HTTPS**
(`https://www.google.com/generate_204`), which no amount of cooperation from
the frame can satisfy — that would need a valid certificate for a domain we do
not own. So Android concludes the hotspot has no internet and asks whether to
stay connected. **Until that prompt is answered, Android keeps mobile data as
the default route and the browser cannot reach `192.168.4.1` at all.**

That prompt is the single most likely thing to go wrong, and it does not look
like a network problem — it looks like the frame is broken. It is easy to miss
entirely with Do Not Disturb on. So the panel says so, next to the password.
Answering it once per phone is enough.

The cost is discovery: nothing opens on its own, so you go to
`http://192.168.4.1` yourself. The panel prints it every time you open the
hotspot, and bookmarking it or adding it to your home screen makes it one tap
after the first time.

Worth being explicit about the alternative, since it looks tempting: bringing
the captive portal back would restore the pop-up and break adding photos. The
sign-in window is the *only* thing Android routes to such a network, and it is
a WebView with no file chooser.

DNS still answers every query with the frame's address — that is how the
probes reach us to be answered at all — so browsing to any other site while
connected lands on a small page saying where the manager actually is, rather
than on the manager under someone else's hostname.

## What runs where

The frame does **no image processing**. The phone's browser runs the same
dither pipeline as the desktop Photo Lab and the command-line tool, and
uploads a finished 4-bit BMP; the firmware only writes bytes to the
filesystem.

That is not a coincidence to be maintained by hand. `main/web/app.ui.html`
contains only the mobile UI, with two placeholders:

```html
<!--@CORE-->
<!--@SHARED-->
```

At build time `tools/build_webapp.py` splices in the `#core` and `#shared`
blocks lifted out of `tools/prepare_photo/photo_lab.html`, gzips the result,
and `main/CMakeLists.txt` embeds it in the firmware image. There is no second
copy of the pipeline. `tools/test/` holds the Python, the desktop tool and
the phone to byte-identical output; see [dithering.md](dithering.md).

The web app is about 16 KB gzipped and is served straight out of flash.

## Modes

The FAT volume has exactly one owner at a time, so USB mass storage and the
hotspot are mutually exclusive. The firmware is a three-state machine:

| Mode | Entered when | Leaves when |
|------|--------------|-------------|
| Drive | USB power present at boot | USB power removed, or the top key |
| Slideshow | no USB power at boot | 2 min idle, or the top key |
| Hotspot | the top key, from either | the top key, 15 min idle, or 1 h total |

Entering hotspot mode detaches the device from USB
(`hal_storage_usb_detach()`), so **the drive disappears from your computer**
and the host will complain about improper removal if it still had it mounted.
That is unavoidable — the alternative is two writers on one filesystem.
Detaching also hands the port back to the USB-Serial-JTAG console, so unlike
drive mode you can watch the logs while the hotspot runs.

## Timeouts and power

The radio is the heaviest current draw on the board, so the hotspot is the
one mode that is deliberately allowed to stay up for a long time:

- **15 minutes idle**, where "idle" means no phone associated *and* no HTTP
  request in the last 30 s. Preparing a photo is a minutes-long job and the
  frame must not power off underneath it.
- **1 hour absolute**, whatever is happening. Phones stay associated long
  after anyone is looking at them, and without this a forgotten hotspot would
  flatten the battery.
- The frame **refuses to start the hotspot below 3.5 V** and says so on the
  panel. Wi-Fi transmits in bursts far heavier than anything else the board
  does, and on a nearly flat cell that is a brownout reset rather than a
  photo.

The slideshow's own window is unchanged at two minutes.

## HTTP API

Everything is under `http://192.168.4.1`. There is no authentication beyond
the WPA2 key.

| Method | Path | Does |
|--------|------|------|
| GET | `/` | the web app (gzipped, from flash); a signpost if the `Host` is not the frame |
| GET | `/api/photos` | `{"photos":[{"name","bytes"}]}`, in display order |
| POST | `/api/photos?name=X.bmp` | upload; the **raw BMP is the body** |
| DELETE | `/api/photos/X.bmp` | remove one |
| GET | `/photo/X.bmp` | the raw BMP, for library thumbnails |
| GET | `/api/storage` | `{"total","free","used"}` in bytes |
| GET | `/api/status` | `{"refreshing","showQueued"}` |
| POST | `/api/show?index=N` | queue a panel refresh |
| POST | `/api/reorder` | JSON array of every filename, in the new order |

Three things about it are worth knowing:

- **Uploads are not multipart.** The BMP is the request body and the filename
  is a query parameter. Parsing multipart on the device to move one file
  would be all cost.
- **`/api/show` only queues.** A colour refresh takes 15–30 s, far longer than
  a phone will hold an HTTP request open, so the handler records the request
  and returns; the hotspot loop performs it and the app watches
  `/api/status`.
- **Reordering is renaming.** The frame draws photos in filename order, so
  the endpoint rewrites the files with a `001_` prefix. It refuses anything
  that is not a permutation of exactly what is on the device, renames through
  temporary names in two passes (the new name for one photo is usually the
  current name of another), and puts things back if it fails part way.

Uploads land in a temporary file and are renamed into place only once
complete, so a dropped connection cannot leave half a photo in the library.

## Storage

The volume is 6 MB and a 400x600 4-bit BMP is about 176 KB, so roughly 30–48
photos fit depending on what else is on there. The Library tab shows what is
used and roughly how many more will fit; uploads are refused with a clear
error rather than filling the volume.

## Code map

```
main/net/wifi_ap.cpp     soft AP, credential generation and NVS persistence
main/net/web_server.cpp  esp_http_server, connectivity probes, the signpost page
main/net/photo_api.cpp   the /api endpoints, all FAT access under the lock
main/net/ap_mode.cpp     the session: panel screen, idle timers, queued refreshes
main/web/app.ui.html     the mobile UI (pipeline spliced in at build time)
tools/build_webapp.py    the splice
components/dns_server/   wildcard DNS, from ESP-IDF's captive_portal example
```

`main/hal/wifi/hal_wifi.h` is *not* used. It is a thirty-method STA/AP/scan
manager inherited from the M5 demo with no implementation anywhere in the
tree; the frame only ever raises one access point, so `net/wifi_ap.cpp` does
that directly against `esp_wifi` and the header stays the unimplemented thing
it already was rather than becoming a half-filled one.
