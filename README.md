# <div align="center">Jcorp Nomad</div>

<div align="center">
  <img src="NomadCoverMK4v2.png" alt="Jcorp Nomad Offline Media Server" width="800">
</div>

<p align="center"><b>A portable, offline media server powered by the ESP32-S3 in a thumbdrive form factor.</b><br>
Stream movies, music, books, and shows anywhere - no internet required.</p>

<p align="center">
  <img src="https://img.shields.io/badge/branch-experimental-orange.svg" alt="Branch: Experimental" />
  <img src="https://img.shields.io/badge/license-CC--BY--NC--SA%204.0-blue.svg" alt="License: CC BY-NC-SA 4.0" />
  <img src="https://img.shields.io/badge/platform-ESP32--S3-orange" alt="Platform: ESP32-S3" />
  <img src="https://img.shields.io/badge/status-rough-red" alt="Status: Stable" />
</p>

<p align="center">
  <a href="https://nomad.jcorptech.net"><b>Buy a Prebuilt Nomad</b></a> &nbsp;|&nbsp;
  <a href="https://ko-fi.com/jcorptech"><b>Support on Ko-fi</b></a>
</p>

---

> **Experimental Branch** - This is where new stuff lands before it's ready for main. Right now that means exFAT card support, offline maps with turn by turn directions, game ROMs and a shelf of built-in games, local multiplayer, TV support over DLNA, a WiFi Mode that joins your home network, offline translation, and recipe + 3D model libraries. Everything from Mk4 is still here and still works, this branch just adds on top of it.
>
> Fair warning, these features are rough. They work, I use them, but they haven't been through anywhere near the testing the main branch has. Expect bugs, expect things to change, and don't put this on a Nomad you're relying on. If you want something stable, use main.
>
> Firmware and the SD card template both change here, so you'll need to reflash and refresh your card files.

---

## What is Nomad

Jcorp Nomad is an open-source offline media server designed for travel, remote work, classrooms, camping, and more. It runs entirely on an ESP32-S3, creates a local Wi-Fi hotspot, and serves media through a browser interface. Multiple users can access separate media streams simultaneously, all without internet access.

This project is compact, easy to modify, and includes optional 3D-printable hardware. Both firmware and web interface are fully open-source.

---

## Get a Nomad

### Build It Yourself (Recommended)

I strongly recommend building your own Nomad. It's not a very difficult project, if you can follow instructions and plug in a USB cable, you can do it. The parts are cheap, widely available, and the whole build takes under an hour. See Hardware Requirements and Quick Start below. If nothing else please check out the DIY option before purchasing. 

### Buy a Prebuilt

That said, I also won't say no to money. If you'd rather skip the DIY and get a ready-to-go unit, prebuilt Nomads are available at **[nomad.jcorptech.net](https://nomad.jcorptech.net)**.

Every Nomad, whether you build it or buy it, runs the same open-source firmware and web interface. When new features and updates are released, you can always flash the latest code yourself to stay up to date. This project isn't going anywhere. 

### Support Development

If you just want to support the project, donations are always appreciated:  
**[ko-fi.com/jcorptech](https://ko-fi.com/jcorptech)**

---

## What's New in Experimental

### exFAT Support

This is the big one, and it's the thing people have been asking for the longest. FAT32 has been Nomad's biggest limitation since day one, cards over 32GB needed a reformat first, and no single file could go over 4GB, which is why large ZIMs had to be split into parts. exFAT fixes both of those.

- Cards mount as **exFAT, FAT32 or FAT16**, auto detected at boot, nothing for you to configure
- SDXC cards (anything over 32GB) work exactly as they come out of the package, no reformat step at all
- Single files can go over 4GB now, so a full size Wikipedia can sit on the card as one file instead of split parts
- Old FAT32 cards keep working exactly like they always did, this is additive, not a migration
- Writing to a nearly full card is a lot less likely to stall out, that was behind a lot of the upload and save timeouts

The reason this took so long, the Arduino ESP32 core ships a prebuilt filesystem library with exFAT compiled out of it, and there is no way to turn it back on from a sketch. The fix was to stop using it entirely and swap in SdFat, which brings its own filesystem code. Everything else in the firmware talks to it the same way it always did.

**You'll need the SdFat library installed** from the Arduino Library Manager before this will compile.

### Offline Maps

- Map regions live in `/Maps` and get browsed offline, with place search and real turn by turn directions (Drive and Walk)
- Regions are prepped on your PC with Nomad Tools, which downloads the map picture and the road network for the areas you pick
- Reads map data straight out of a packed archive instead of unpacking it onto the card, which keeps a region at roughly its download size instead of several times bigger
- The Maps tile only shows up on the menu if you actually have a region on the card

This one is the roughest of the bunch. It works, but it's slow, big regions take a while to draw and I'm still working on that.

### WiFi Mode (join your home network)

Instead of running its own hotspot, Nomad can now join a WiFi network you already have and serve everything to that network, the browser interface, DLNA, all of it. Your phone stays on your normal internet-connected WiFi and Nomad is just another device on it. This is also the fix for TV devices like Fire Sticks that refuse to do anything on a network with no internet.

- Set it up from the admin panel: **WiFi Mode** button in the top bar opens a popup with a network scanner, password field, and an auto-reconnect option
- The captive portal turns itself off in this mode (hijacking DNS on someone's home network is uhh.. lets say frowned upon), and the screen shows the IP address the router assigned instead of a hotspot name

### TV Support (DLNA)

Nomad now shows up as a media server on anything that speaks DLNA - smart TVs, VLC, Kodi, Roku Media Player.

- Browse Movies, Shows, Music and Gallery straight from the TV, with cover art, and seeking works
- On the TV side there's nothing to set up, it just appears in the device list
- Works on the hotspot or in WiFi Mode; for Fire Sticks use WiFi Mode and the VLC app, since Fire OS breaks without internet now (so lame)
- Toggle it off in the admin panel if you don't want the Nomad announcing itself / makes everything else a bit faster

### Offline Translation

A Translate page that works with zero internet. Two people can pass a phone back and forth and each type in their own language.

- Language packs live in `/Translate` on the card and are installed with Nomad Tools, which always grabs both directions of a pair
- Translation runs in the browser of whoever connected, nothing heavy runs on the Nomad itself
- Can be very slow to load the first time, but after that back and forth is fairly quick, and the info is cached so its quick if you load it again.  

### Cookbook & Workshop

Two new library pages, both optional, both invisible until you enable them.

- **Cookbook:** drop `.md`, `.cook` or `.json` recipe files into `/Cookbook` (photos work, same-name image convention). Recipes render with ingredient and step checklists you can tick off while cooking.
- **Workshop:** drop `.stl`, `.3mf` or `.obj` files into `/Workshop` and preview them in 3D right in the browser. A README or notes file in a folder describes every model in it. The viewer is a few hundred lines of raw WebGL, not a bundled 3D engine, so it stays fast off the card.

### Game ROMs & Built-in Games

- Drop ROMs into `/Games` and play them in the browser through EmulatorJS
- Ships with Go, Chess, Connect Four, Tic-Tac-Toe, 2048, Minesweeper, Snake, Crazy Eights, Dots and Boxes, and a shared Whiteboard, all just plain HTML files
- Also ships with **DOOM** - the shareware WAD is included, and any `.wad` you drop in `/Games` runs through the same engine (this is here so I can say it runs doom, you must keep this on the card it is load bearing.. I swear lol)
- Cover art works the same as everywhere else, drop an image with the same name next to the file
- Add or remove games by adding or deleting files, there's nothing to configure, this works for the inbuilt games aswell. 

### Local Multiplayer

- Two player game rooms for multidevice use. 
- One person makes a room, shares the 4 character code, the other joins from their own phone
- Chess, Go, Connect Four and Tic-Tac-Toe all support it, the Whiteboard is just a shared freeform board
- There are also "pass and play games" that only need one device, I will eventualy make this an option for all of the defaults. 

---

## Mk4 Highlights

### Offline Wikipedia & Archive Support (ZIM)
- Browse and search full offline Wikipedia (and other ZIM archives like Gutenberg and TED) directly from the SD card
- Search is fast even on massive archives, the companion [Nomad Tools](https://github.com/Jstudner/Nomad-Tools) app prebuilds a compact index on your PC, so the device never has to search the raw multi-gigabyte file itself
- Embedded videos and epub books inside archives play/read right in the browser
- Works with zero extra UI cost if you don't use it, no archives on the card means the feature stays completely out of the way
- Currently tested with Gutenburg epubs, TedX Videos, and wikipedia from the tiny 0.8 file all the way to the 140gb maxi with images. 

### Redesigned Case
<p align="center">
  <img src="NomadMk4Explode.png" alt="Nomad Mk4 exploded case view" width="700">
</p>

- New case slides together **front-to-back** instead of the old top-to-bottom design
- No more direct pressure on the screen, which was a common cause of cracked/broken screens on the old case
- Buttons stay exposed on the outside, so you can still flash firmware or hit the boot button without disassembling anything

- Based on a remix of [ESP32 C6 with LCD Screen Enclosure Case](https://makerworld.com/en/models/2121443-esp32-c6-with-lcd-screen-enclosure-case) on MakerWorld by [**Adrian**](https://makerworld.com/en/@user_1765744671), full credit to the original design this was built on

### Indexing & Stability
- Root-caused and fixed a long-standing random reboot bug tied to files over 2GB, this was the actual cause of crashes on image-heavy Wikipedia pages and big movie scrubbing
- Fixed a heap-corruption crash that could hit when indexing and refreshing SD totals at the same time
- Boot-time indexing now only re-scans when files have actually changed, instead of a full scan on every boot
- Removed the screens loading spinner that was silently forcing a full-screen redraw every loop, pulling it out made the whole device noticeably more stable

### Reader & Memory Improvements
- Comic and PDF readers now free old pages from memory as you scroll, fixing crashes on long comics and scanned PDFs
- PDF viewer shows a real loading percentage instead of a blank screen
- Cleaned out a bunch of dead code and unused libraries that were loading on every Books page

### UI & Admin Updates
- Unified header and button styling across pages so themes apply consistently everywhere
- Fixed several dark mode readability bugs (unreadable resume text, buttons that ignored custom themes, etc.)
- Admin panel settings are now gated behind a login
- Fixed a stuck brightness slider caused by an out-of-range default value

### Default Themes (28)

Default Blue, Forest Night, Cherry Blossom, Mocha Latte, Ocean Depths,
Autumn Leaves, Lavender Fields, Sunset Horizon, Coral Reef, Mountain Mist,
Jade Garden, Desert Sand, Arctic Aurora, DeLorean, Midnight Code, 90s Retro,
Mint Breeze, Rose Gold, Crimson Night, Emerald Dream, Royal Purple,
Copper Sunset, Sapphire Sea, Peach Cream, Slate Storm, Lime Zest,
Burgundy Wine, Teal Oasis

---

## Features

- **exFAT & FAT32:** Any card format, auto detected at boot. Cards over 32GB and files over 4GB both work.
- **WiFi Mode:** Join an existing WiFi network instead of running the hotspot, with automatic fallback so you can't lock yourself out. (rough)
- **TV Support (DLNA):** Shows up as a media server for smart TVs, VLC, Kodi and friends, with cover art and seeking. (rough)
- **Offline Encyclopedia:** ZIM archive support for offline Wikipedia and other offline wikis, with fast on-device search.
- **Offline Maps:** Browsable map regions with place search and turn by turn directions, served straight off the card. (rough)
- **Offline Translation:** In-browser translation between installed language pairs, no internet ever. (rough)
- **Games:** Browser-based ROM playback through EmulatorJS, DOOM, plus ten built-in games. (rough)
- **Local Multiplayer:** Two player game rooms over the Nomad's own Wi-Fi, no internet needed. (rough)
- **Cookbook:** Recipe library with tick-off ingredient and step checklists. (rough)
- **Workshop:** 3D-print model library with an in-browser STL/3MF/OBJ preview. (rough)
- **Chat:** A simple local chat room for everyone connected to the Nomad.
- **Admin Panel:** Full device controls, library indexing, Theme Customizer, menu page toggles, login-gated settings.
- **File Browser:** Upload, rename, delete, download, and inline file editing. (Recommended to use a PC)
- **Global Search:** Quickly find media across all categories from the Menu page.
- **Music Player:** Seamless background playback with subdirectory playlists and a dynamic Queue.
- **Movies & Shows:** Plyr-integrated playback with season/special folder support.
- **Digital Library:** EPUB support, PDF handling, and a dedicated Comic/Webtoon reader.
- **Resume Tracking:** Saves playback progress for Movies, Shows, and certain Books.
- **Gallery & Files:** Dedicated pages for image viewing, video clips, and general file sharing.
- **Captive Portal:** Automatic login/redirection for easy access.
- **Persistent Settings:** Themes and system configurations saved across reboots.
- **Mobile-Friendly UI:** Fully responsive design optimized for handheld offline streaming.

---

## Hardware Compatibility

Nomad is built specifically for the **Waveshare ESP32-S3 Dev Board (1.47" LCD version)**. Due to the number of low-level tricks used to squeeze this much functionality out of the hardware, it is difficult to get Nomad running on other boards.

Both the **1.47** (micro-USB) and **1.47B** (USB-C) variants of this board are supported out of the box. The default firmware targets the micro-USB board. To build for the 1.47B / USB-C variant, add `-D BOARD_USB_C=1` to your Arduino IDE board build flags (or define it in your `platformio.ini` / compile options) **before** compiling. You can also set `#define BOARD_USB_C 1` directly in `Display_ST7789.h` before the header's include guards.

There are a few community forks that target other ESP32 boards, but your mileage will vary. I'm also actively working on a **Nomad Lite** version with wider board compatibility, focused on basic streaming without all the advanced features.

---

## Hardware Requirements

- **Waveshare ESP32-S3 Dev Board (1.47" LCD version)**
  [Amazon Link](https://amzn.to/4ktB6oT)

- **microSD card, exFAT or FAT32 (16-128GB recommended, up to 2TB)**
  [Amazon Link](https://amzn.to/44tM1c4)

- **SD-Card Extender (optional, 3DP case compatible)**
  [Amazon Link](https://amzn.to/45IWIJz)

- **USB power source**
- **Optional:** 3D-printed enclosure (STL files included)

---

## Software Requirements

- Arduino IDE
- **SdFat library (2.3.0 or newer)**, from the Library Manager, required for exFAT support
- SquareLine Studio (optional, for UI editing)

---

## Quick Start

1. Flash ESP32-S3 firmware from `/firmware/`.
2. Copy `/SD_Card_Template/` files onto the card. Any exFAT or FAT32 card works as-is, you only need to format if the card is brand new or broken.
3. Place media in `/Movies`, `/Shows`, `/Books`, `/Music`, `/Gallery`, `/Files`, `/Games`, `/Cookbook`, `/Workshop`. (ZIMs, Maps and Translate packs get prepped with Nomad Tools, see below.)
4. Insert SD card and power device via USB.
5. Connect to Wi-Fi `Jcorp_Nomad` with password: `password`.
6. Open the browser interface.
7. Click the gear icon → Library Index → **Full Scan Now**.
8. Monitor Admin Console for progress; scan may take minutes.
9. Return to Menu page and enjoy your media!
10. Optional: to serve your home network (and TVs) instead of running a hotspot, open Admin → **WiFi Mode** and join your network from there.

---

## Key Improvements

1. **Faster & More Reliable Indexing**
   - Non-blocking, background indexing for large libraries.
   - Safe on power loss; partial indexes remain intact.
   - Auto-updates changes; frontend detects updates automatically.
   - Boot-time indexing now only triggers on an actual file change, not every boot.

2. **Resume Functionality**
   - Movies and Shows track playback progress.
   - Options for **Play from Start** or **Resume**.
   - Menu displays last three movies/shows; mobile shows most recent.

3. **Dark Mode**
   - Toggleable across all pages from the menu.
   - Consistent theme tokens across pages, no more mismatched dark colors.

4. **Admin Page**
   - Full device control: shutdown, restart, flash mode, Wi-Fi, RGB LEDs, brightness, credentials, indexing, and file management.
   - Login-gated settings so changes require the admin password.
   - Safe shutdown option for SD card health.
   - Real-time system console feedback.

5. **Stability Improvements**
   - Fixed frontend NDJSON sync issues.
   - Crash recovery on large indexes.
   - Fixed a random-reboot bug tied to files over 2GB.
   - Dynamic LCD brightness adjustment.
   - Streaming stability enhancements.

6. **Improved Library Support**
   - Supports deeper folder structures for Shows and Music.
   - Flexible organization; media files can be nested at any level.

---

```
Folder Structure

/Movies
    Interstellar.mp4
    Interstellar.jpg

/Shows
    /The Office
        S01E01 - Pilot.mp4
        S01E02 - Diversity Day.mp4
    The Office.jpg

    /Gravity Falls
        /Season 1
            S1E1 - Tourist Trapped.mp4
            S1E2 - The Legend of the Gobblewonker.mp4
        /Season 2
            S2E1 - Scary-oke.mp4
            S2E2 - Into the Bunker.mp4
        Alex Hirsch Interview.mp4
    Gravity Falls.jpg

/Books
    The Martian.pdf
    The Martian.jpg
    /How to Train Your Dragon
        book1.pdf
        book2.mp3
        book1.jpg
        book2.jpg
    How to Train Your Dragon.jpg

/Music
    track01.mp3
    /Artist1
        track01.mp3
        /Album1
            track02.mp3
    /PersonName
        /Playlist1
            track01.mp3
        /Playlist2
            track02.mp3

/Gallery
    image01.jpg
    video01.mp4

/Files
    document.pdf
    example.txt

/Games
    Pokemon Red.gb
    Pokemon Red.png
    Doom.wad
    Chess.html
    Go.html

/Cookbook
    Grandma's Chili.md
    Grandma's Chili.jpg
    /Desserts
        Brownies.md

/Workshop
    /Brackets
        shelf bracket.stl
        README.txt

/Maps
    /pennsylvania
        manifest.json
        (map data, prepped with Nomad Tools)

/Translate
    manifest.json
    (language packs, installed with Nomad Tools)

/Archive
    wikipedia_en_all_maxi.zim
    /.nomad-zim
        (search index, built by Nomad Tools)

index.html
appleindex.html
menu.html
movies.html
shows.html
books.html
music.html
gallery.html
files.html
archive.html
games.html
maps.html
translate.html
cookbook.html
workshop.html
Logo.png
favicon.ico
```

---

## Supported Formats

- **Video:** `.mp4, .webm, .m4v, .mov, .mkv, .ts, .m2ts` 
- **Audio:** `.mp3, .flac, .wav, .ogg, .aac, .m4a`
- **Books:** `.pdf, .epub, .cbz, .cbr` 
- **Images:** `.jpg, .jpeg, .png` 
- **Archives:** `.zim` (offline Wikipedia and other ZIM-format wikis), needs special processing, you cant just drop a .zim in sadly. Prep them with [Nomad Tools](https://github.com/Jstudner/Nomad-Tools) first (still rough, but handles most common ZIMs)
- **Games:** ROMs for the systems EmulatorJS supports (GB, GBC, GBA, NES, SNES, Genesis, and others), `.wad` files for DOOM, plus `.html` files for built-in games
- **Maps:** map regions prepped with Nomad Tools, dropped into `/Maps`
- **Recipes:** `.md`, `.cook`, or `.json` files in `/Cookbook`
- **3D Models:** `.stl`, `.3mf`, `.obj` in `/Workshop`
- **Translation:** language packs installed into `/Translate` with Nomad Tools

---

## Nomad Tools (companion PC app)

Some content needs a one-time prep step on your computer before the Nomad can use it, ZIM archives need a search index built, maps need downloading, translation packs need fetching. [Nomad Tools](https://github.com/Jstudner/Nomad-Tools) is a simple menu that does all of it. Windows and Linux (macOS untested).

Basic usage:

1. Download Nomad Tools and put your Nomad card in the computer.
2. **Windows:** double-click `START-Windows.bat`. **Linux:** run `./start-linux.sh`.
3. Pick your card from the list (or type its path), then pick a job from the menu.

What the menu covers:

- **Add ZIM archives** - copies Wikipedia/Gutenberg/etc. onto the card and builds the search index. Adds to what's there, never wipes your existing archives, and splits oversized files automatically.
- **Optimize cover images** - shrinks Movie/Show/Book covers so pages load fast on the device.
- **Rebuild media index** - refreshes the library listing after you've added or removed files from the PC.
- **Download offline maps** - grabs map tiles for an area you pick, with a size estimate and free-space check before anything downloads.
- **Build a routing map** - maps plus the road network and place search, for turn by turn directions. Pick from ready-made areas (any US state, big regions, or the whole country) and they join into one seamless map. US only for now
- **Add translation languages** - installs offline translation packs, always both directions of a pair. Comes in collections (`starter`, `traveler`, `europe`, `asia`, `world`) or single languages. (I recomend just grabbing all as its less than 4GB)

A good order for a fresh card: media and ZIMs first, optimize images, rebuild the index last. Maps and languages can be added any time.

---


## 3D Printed Case Files

The Mk4 default case is a remix of [ESP32 C6 with LCD Screen Enclosure Case](https://makerworld.com/en/models/2121443-esp32-c6-with-lcd-screen-enclosure-case) on MakerWorld, credit to [**Adrian**](https://makerworld.com/en/@user_1765744671) for the original design. It's a front-to-back slide design that keeps pressure off the screen while still exposing the buttons for firmware access.

- Mk4 case files: in this repo
- Original Mk3 top/bottom case (still works, just more prone to screen pressure): [Thingiverse](https://www.thingiverse.com/thing:7223398)

---

## Known Rough Edges

Since this is the experimental branch, here's what I already know isn't great:

- **Maps are slow.** Big regions take a while to load and pan. It works, it's just not snappy yet, and that's the main thing I'm working on.
- **WiFi Mode and DLNA are days old.** They work (tested with VLC on a Fire Stick, desktop VLC, and phones), but they haven't seen many routers or many TVs yet. If your TV can't find Nomad, tell me what TV it is and I will see what I can do.
- **Set an admin password before using WiFi Mode.** On the hotspot only people you gave the password to can reach the admin panel. On your home network, everything on that network can.
- **Free space on FAT32 cards can read slightly wrong.** SdFat doesn't update the counter FAT32 keeps for it, so the number drifts by however much Nomad itself writes. It fixes itself the next time you plug the card into a PC, and it doesn't affect your files at all. exFAT cards aren't affected.
- **Multiplayer is polling based.** It's a room code and a refresh loop, not a live connection. Fine for turn based games, wouldn't hold up for anything realtime.
- **EmulatorJS cores download on first play.** Once cached they're fine, but the first launch of a system pulls the core off the card and takes a moment.
- Everything here has had a fraction of the testing main has. If something breaks, tell me, that's what this branch is for.

---

## What's Next

**Nomad Lite** - A stripped-down version of Nomad with wider board compatibility, focused on core streaming features. In active development now that Mk4 is out.

**Nomad Manager** - A companion application for Nomad that integrates with Jellyfin to handle automated media downcoding and transfers, and builds the offline archive indexes used by the ZIM reader. Keep your Nomad stocked and ready to go without manual file management.

**Gallion** - A larger-scale sibling to Nomad, built on more capable hardware. Gallion is designed to handle everything that couldn't fit on Nomad's current platform > ROM emulation, 4k video, and expanded media compatibility across the board. The current version is [here](https://github.com/Jstudner/Gallion).

---

## Project Inspiration

Inspired by my experience running a Jellyfin server, I wanted a portable, low-cost solution for offline media streaming. Challenges with SBCs (Raspberry Pi, etc.) included high power usage, heat, and instability.

Nomad focuses on delivering:

- Offline access
- Wide device compatibility
- Simple frontend for media browsing and playback
- Multiple user support
- High customization potential

The ESP32-S3 provides enough performance to handle these requirements efficiently, in a pocket-sized form factor.

---

## License

[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) - free to remix and share for non-commercial use with attribution.

---

## Credits

Developed by **Jackson Studner (Jcorp Tech)**.
Mk4 case design based on a remix of [**Adrian**](https://makerworld.com/en/@user_1765744671)'s [ESP32 C6 LCD Screen Enclosure Case](https://makerworld.com/en/models/2121443-esp32-c6-with-lcd-screen-enclosure-case) on MakerWorld.
Inspired by open-source offline media projects. Contributions via PRs welcome.

<p align="center">
  <a href="https://ko-fi.com/jcorptech"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support on Ko-fi"></a>
</p>
