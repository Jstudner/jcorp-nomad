# Jcorp Nomad — Pocket Offline Media Server (ESP32-S3)

**A portable, offline media server in a thumbdrive form factor.** Nomad creates its own Wi-Fi hotspot and streams movies, shows, music, books, comics — even full offline Wikipedia — to any phone, tablet, or laptop nearby. No internet, no app, no accounts. Just connect and play.

This is the 3D-printed case + full build for the open-source **Jcorp Nomad Mk4** project. If you can plug in a USB cable and follow instructions, you can build one — the whole build takes under an hour and the parts cost about $20–30.

![Cover image — NomadCoverMK4v2.png]

---

## What it does

- Creates a local Wi-Fi hotspot (captive portal — devices auto-open the media menu)
- Streams to **multiple users at once**, each watching their own thing
- **Movies & Shows** with resume playback, poster art, and season folders
- **Music** with playlists, shuffle, and a live queue
- **Books**: PDF, EPUB, comics (CBZ/CBR), and MP3 audiobooks
- **Offline Wikipedia** and other ZIM archives, with fast search (Mk4)
- Photo gallery, general file sharing, and a full admin panel
- 28 built-in themes + dark mode
- Runs entirely off a microSD card — no soldering, no wiring, no assembly beyond the case

## The case (Mk4)

Slides together **front-to-back** so there's never pressure on the fragile screen (the #1 cause of broken displays on the old design). The buttons stay exposed, so you can flash firmware or reboot without opening it up.

This case is a remix of [ESP32 C6 with LCD Screen Enclosure Case](https://makerworld.com/en/models/2121443-esp32-c6-with-lcd-screen-enclosure-case) by [Adrian](https://makerworld.com/en/@user_1765744671) — full credit for the original design.

## What you need (full BOM in the files)

| Part | Notes |
|---|---|
| Waveshare ESP32-S3 Dev Board, **1.47" LCD version** | The only board Nomad targets — it's the whole computer |
| microSD card, 16–128 GB, Class 10/U1+ | FAT32. Bigger works (up to 2 TB) |
| USB power source | Any USB-A port or charger |
| SD card extender *(optional)* | Lets you swap the card without opening the case |
| ~10–15 g of filament | PETG recommended — the board runs warm |

## Print settings

- **Material:** PETG strongly recommended (device gets warm under load); quality PLA works
- **Layer height:** 0.2 mm · **Walls:** 2+ · **Infill:** 15%+
- **Supports:** none needed
- Snap-fit tolerances: if your printer runs tight, scale to **101%** and adjust down from there

## Setting it up (full guide in the files)

1. Print the case
2. Flash the firmware with Arduino IDE (settings + library versions in the build guide)
3. Format the SD card FAT32, copy the SD template from GitHub, add your media
4. Assemble the case — **after** testing everything works
5. Connect to Wi-Fi `Jcorp_Nomad` (password: `password`) and enjoy

## Links

- **Source code, firmware, and SD template:** https://github.com/Jstudner/jcorp-nomad
- **Step-by-step Instructable:** https://www.instructables.com/Jcorp-Nomad-Mini-WIFI-Media-Server/
- **Prebuilt units** (if DIY isn't your thing): https://nomad.jcorptech.net
- **Support the project:** https://ko-fi.com/jcorptech

Fully open source under **CC BY-NC-SA 4.0**. Firmware and web UI are actively developed — flash the latest code any time to get new features. Featured on [Hackaday](https://hackaday.com/2025/07/13/jcorp-nomad-esp32-s3-offline-media-server-in-a-thumbdrive/).

*Built something with it? I'd love to see it — drop a comment or a build photo.*
