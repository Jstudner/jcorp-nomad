# <div align="center">Jcorp Nomad</div>


<div align="center">
  <img src="Screenshot 2025-07-07 193521.png" alt="Jcorp Nomad Offline Media Server" width="800">
</div>

<p align="center"><b>A portable, offline media server powered by the ESP32-S3 in a thumbdrive form factor.</b><br>
Stream movies, music, books, and shows anywhere — no internet required.</p>

<p align="center">
  <img src="https://img.shields.io/badge/license-CC--BY--NC--SA%204.0-blue.svg" alt="License: CC BY-NC-SA 4.0" />
  <img src="https://img.shields.io/badge/platform-ESP32--S3-orange" alt="Platform: ESP32-S3" />
  <img src="https://img.shields.io/badge/status-beta-lightgrey" alt="Status: Beta" />
</p>


---


## Experimental Branch Enhancements

This branch includes several experimental features not yet available in `main`. These are actively being tested and refined:

- **OPDS eBook Support** - Connect with eBook readers and track reading progress.
- **Minimal M3U-based DLNA Support** - Allows media discovery on VLC and some Smart TVs.
- **Admin Page** - Upload, rename, and delete files directly in the browser.
- **RGB LED Controls** - Change colors and patterns remotely.
- **Improved Web UI** - Cleaner layout and more responsive mobile design.
- **Storage Status Display** - View available SD card space on the interface.
- **Better Video Quality Support** - Preliminary improvements for high-bitrate playback.
- **SD Card Recovery** Slow or low quality SD cards can have read failures when they get too hot, or too many requsts. Adds recovery function so users shouldnt see this happen on the frontend anymore. 
- **UI Updates** Squareline studio files are in /docs, you can use it to customize the LCD display. I also made it so the SSID actualy reflects the one in settings automaticaly. Finaly the bar at the top of the screen shows how full the SD card is!

Use this branch if you want the latest features and don't mind the occasional rough edge.

---


## What is Nomad

Jcorp Nomad is an open-source offline media server built for travel, remote work, classrooms, camping, and more. It runs entirely on an ESP32-S3 dev board, creates a local Wi-Fi hotspot, and serves media through a browser-accessible interface. It does not require internet access and works similarly to in-flight entertainment systems. It also allows multiple users watching seperate media streams at the same time. 

This project is designed to be compact, simple, and easily modifiable. It includes optional 3D-printable hardware and a fully open source firmware and web interface.

---

## Project Inspiration
This project was inspired by my experience running a Jellyfin server at home. I love having full control over my media library, and Jellyfin gave me everything I wanted > streaming movies, shows, books, and music, all from my own hardware. Naturally, I started looking for ways to take that experience on the go.

My first thought was to build a portable server, but I quickly ran into some major hurdles:

- Power-hungry hardware — Even low-power x86 boxes needed a hefty battery setup to stay running for long trips.

- High cost — SBCs like the Raspberry Pi 4, plus power banks, USB storage, and screen interfaces added up quickly.

- Heat and reliability — Trying to cram a full stack of services into a compact case often led to thermal issues and instability when running software meant for server hardware.

That’s when I pivoted.

Instead of replicating a full home media server, I focused on delivering the core experience:

- Offline access

- Local Wi-Fi hotspot

- Simple media browsing and playback

- Support for multiple users

The ESP32-S3 offered just enough performance to handle all of that - with a fraction of the power draw and cost.
The result is Nomad: a minimalist, reliable, and low-cost media server that delivers the essential features of a home streaming setup in a smaller than pocket sized format.

Is it fancy? No.
Does it work? Absolutely.
And it’s open-source, so anyone can expand, improve, and adapt it for their own needs.

---

## Features

- Creates a local Wi-Fi hotspot with captive portal  
- Serves HTML media interface to any connected device  
- Streams content directly from a FAT32-formatted microSD card  
- Supports simultaneous connections (tested with up to 4 video streams)  
- No app or internet connection required  
- Open source firmware and UI  
- Customizable web interface / Features

---


## Hardware Requirements

**Disclaimer:**  
The following links are Amazon affiliate links. Purchasing through these links supports this project at no additional cost to you. Thank you for your support!

- **Waveshare ESP32-S3 Dev Board (1.47" LCD version)**  
  [https://amzn.to/4ktB6oT](https://amzn.to/4ktB6oT)

- **FAT32-formatted microSD card (16 GB minimum recommended, 64 GB preferred)**  
  [https://amzn.to/44tM1c4](https://amzn.to/44tM1c4)

- **USB power source** (e.g., battery bank or computer)

- **Optional:** 3D-printed enclosure (STL files included in the repository)

---

## Software Requirements

- Arduino IDE or PlatformIO (to flash firmware)
- Python 3.x (to run `media.py`)
- SquareLine Studio (optional, for screen UI editing)

All software used is free and available on Windows, macOS, and Linux.

---

## Quick Start

1. Flash the ESP32-S3 with the firmware in the `/firmware/` directory.
2. Format your SD card as **FAT32** and copy the web files from `/SD_Card_Template/`.
3. Place your media files into the appropriate folders (see structure below).
4. Run `media.py` to generate `media.json` automatically.
5. Insert the SD card and power the device.
6. Connect to the Wi-Fi network named `NomadServer`.
7. Your browser will be redirected to the offline media interface.

---

## Folder Structure (on SD Card)

```
/Movies/
    Interstellar.mp4
    Interstellar.jpg
/Shows/
    The Office/
        S01E01 - Pilot.mp4
        S01E02 - Diversity Day.mp4
    The Office.jpg
/Books/
    The Martian.pdf
    The Martian.jpg
/Music/
    track01.mp3

index.html
appleindex.html
menu.html
movies.html
shows.html
books.html
music.html
media.py
media.json
placeholder.jpg
Logo.png
favicon.ico
```
---

## Supported Formats

- Video: `.mp4`
- Audio: `.mp3`
- Books: `.pdf`
- Images: `.jpg` (used for covers and folder images only)

Ensure all images and media files use matching names for proper display.

---

## Customization

- Wi-Fi name and password can be changed in `firmware/Nomad.ino`
- Branding (logo, favicon) can be replaced in `/SD_Card_Template/`
- Sections (Movies, Music, etc.) can be removed from `menu.html`
- Web interface can be edited using any text editor
- The screen UI can be edited with SquareLine Studio (the free version is fine) 

---

## What's Included

- `/firmware/` – Arduino firmware for ESP32-S3
- `/SD_Card_Template/` – Web UI, HTML files, and `media.py`
- `/case/` – STL files for optional 3D-printed enclosure
- `/docs/` – Logos, screenshots, and design files

---

## Troubleshooting

Common issues and their solutions are detailed in the Troubleshooting section on instructibles.

Topics covered include:

- SD card read errors
- Captive portal issues on some phones
- Video playback and seeking bugs
- UI glitches or screen failure

---

## Future Plans

These are features I'd like to explore in future updates. If you'd like to contribute, feel free to fork the repo or open a pull request!


### Offline Maps with GPS Support
Inspired by [Backcountry Beacon](https://www.instructables.com/USB-Powered-Offline-Map-Server/), the goal is to serve cached map tiles and display the live GPS position from the user's phone or connected device — entirely offline.

### HTML5 Games
Similar to [Gams Offline](https://github.com/Gams-Offline/Gams), I would like to embed simple HTML5 games that work in-browser. The selection in Gams is amazing and make me think of cool math games, would be neat to have even though most require a keyboard.

### Audiobook-Friendly Mode
Improve playback for long-form audio by adding bookmarks, chapter display, and smart pause/resume. Intended for better audiobook handling in the Music section. Potentialy add an entire new audiobook section or a seperate handling system for it in books (Under read it could have a listen option).

### File Upload Over USB
Enable USB mass storage or dual-mode operation to allow users to drag and drop files directly to the SD card without removing it. This may require dynamic switching between USB and Wi-Fi server modes, or dual-core task handling. I worked on this for a long while, but couldnt find a relible way to have it switch modes and still function, usualy crashes trying to reboot media, I also wasnt able to get it to run the python script to generate media.json, a new system would be needed.

---

## Build Guide on Instructables

Looking for a step-by-step tutorial?  
Check out the full build guide on **Instructables** for detailed instructions, photos, and tips on setting up Jcorp Nomad.

👉 [Read the Instructables Guide](https://www.instructables.com/Jcorp-Nomad-Mini-WIFI-Media-Server/) 

---

## License

This project is licensed under the [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License](https://creativecommons.org/licenses/by-nc-sa/4.0/).

You may remix, adapt, and share this project **for non-commercial use**, as long as you give credit and share under the same terms.

For commercial licensing, please contact the author.


---

## Credits

Developed by **Jackson Studner (Jcorp Tech)**.  
Inspired by open-source offline projects like Backcountry Beacon.

If you build, remix, or improve this project, please consider submitting a pull request or tagging the project.

---


 
