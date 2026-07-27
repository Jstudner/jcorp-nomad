# Jcorp Nomad — Simple Build Guide

Total time: about an hour (plus print time). No soldering, no wiring.

**Golden rule: test everything BEFORE snapping the case together.** The snap tabs weaken a little each time you open it, and the screen is fragile.

---

## Step 1 — Print the case

- **Material:** PETG recommended (the board runs warm); quality PLA is fine too
- **Supports:** none · **Layer height:** 0.2 mm · **Infill:** 15%+
- If your printer's tolerances run tight, print at **101% scale** and size down if loose
- Parts: case body + front screen cover (slides on front-to-back)

## Step 2 — Flash the firmware

1. Download/clone the repo: https://github.com/Jstudner/jcorp-nomad — you need **everything** in `firmware/JcorpNomadProject/`
2. Install [Arduino IDE](https://www.arduino.cc/en/software)
3. **File → Preferences → Additional Board Manager URLs**, add:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
4. **Tools → Board → Board Manager** → install **"esp32 by Espressif Systems"**
5. Plug the board into a USB port (it plugs in like a flash drive) and set under **Tools**:

   | Setting | Value |
   |---|---|
   | Board | ESP32S3 Dev Module |
   | Port | your COM port |
   | USB CDC On Boot | Enabled |
   | Flash Size | 16MB (128Mb) |
   | Partition Scheme | 16MB Flash (3MB APP/9.9MB FATFS) |
   | PSRAM | OPI PSRAM |
   | Upload Speed | 921600 |
   | USB Mode | USB-OTG (TinyUSB) |

6. **Tools → Manage Libraries**, install these **exact versions**:
   - ArduinoJson (Benoit Blanchon) **v7.3.0**
   - Async TCP (ESP32Async) **v3.4.7**
   - ESP Async WebServer (ESP32Async) **v3.7.1**
   - LVGL (kisvegabor) **v8.3.10**
   - SdFat (Bill Greiman) **v2.3.0**
7. Open `JcorpNomadProject.ino` and click **Upload**. Done when you see "Done uploading."

> **Stuck in a bad state / board not detected?** Enter flash mode manually: hold **BOOT**, press and release **RESET**, then release **BOOT**.
> **"No such file or directory" library errors?** Wrong library version — clean your Arduino libraries folder and reinstall just the ones above.

## Step 3 — Prepare the SD card

1. Format the card as **FAT32** (Windows won't do >32 GB cards natively — use the free [FAT32Format GUI tool](https://fat32format-gui.en.lo4d.com/windows), allocation size 32K, quick format ✅)
2. Copy everything from the repo's `SD_Card_Template/` folder to the card root
3. Add your media:

```
/Movies   Interstellar.mp4 + Interstellar.jpg   (poster = same name, .jpg)
/Shows    /The Office/S01E01 - Pilot.mp4 ... + The Office.jpg
/Music    mp3/flac/wav — artist/album/playlist folders all work
/Books    PDF, EPUB, CBZ/CBR comics, MP3 audiobooks + matching .jpg covers
/Gallery  photos and video clips
/Files    anything you want to share
```

**Media tips:**
- FAT32 limit: no single file over **4 GB**
- Compress video with [HandBrake](https://handbrake.fr): **Fast 480p preset, H.264, RF 22–24, "Web Optimized" checked, burn in subtitles** — smaller files = more simultaneous streams
- Name episodes `S01E01 - Title.mp4` — everything sorts alphabetically
- Get poster art from [themoviedb.org](https://www.themoviedb.org)
- Offline Wikipedia/ZIM archives need prep with [Nomad Tools](https://github.com/Jstudner/Nomad-Tools) first — you can't just drop a .zim on the card

## Step 4 — First boot & test

1. Insert the SD card, plug into USB power
2. Connect a phone/laptop to Wi-Fi **`Jcorp_Nomad`**, password **`password`**
3. The media menu should open automatically (captive portal); if not, browse to `http://nomad.local/` or `192.168.4.1`
4. Open the **gear icon → Library Index → Full Scan Now** and let it index (a few minutes for big libraries)
5. Play a video. If it streams, you're golden.

## Step 5 — Assemble the case

- Start from the **USB side**, press down gently, and slide the cover front-to-back — the far end from the USB goes on last
- **Be gentle around the screen** — the case exists to protect it, don't let it do the breaking
- Buttons stay exposed, so you never need to open it again for firmware updates

### Heat notes
The board runs warm under load — that's normal, air cooling is fine (all testing was done air-cooled). Optional: a small thermal pad between the ESP32-S3 chip and the shell, and the SD extender keeps the card away from the heat.

---

**Problems?** Check the [GitHub discussions](https://github.com/Jstudner/jcorp-nomad/discussions) — most issues are already documented — or drop a comment.
