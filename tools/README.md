# nomad-setup

One command to take a bare ESP32-S3 stick and a blank microSD card to a working
Jcorp Nomad: it builds and flashes the firmware with the right flash size and
partition layout, then formats the card as FAT32 and copies the web interface
and folder structure onto it.

Pure Python 3.8+, standard library only. Works on Linux, macOS and Windows.

```
cd jcorp-nomad
./tools/nomad-setup                 # guided setup, both steps
tools\nomad-setup.bat               # same thing on Windows
```

---

## What it does for you

**Firmware**

* Finds `arduino-cli` and `esptool`, or installs the ESP32 core and the five
  required libraries with `--install-deps`
* Copies `lv_conf.h` into your Arduino libraries folder — the single most
  common reason a first Nomad build fails
* Asks the chip how much flash it actually has, then picks a partition scheme
  to match (a 4 MB board gets `huge_app`; the stock layout only gives the
  sketch 1.25 MB and the firmware is ~1.45 MB, so it would build fine and then
  boot to nothing)
* **Refuses to flash** if the firmware would not fit its partition, if it was
  built for more flash than the chip has, or if the board is not an ESP32-S3
* Flashes using the offsets in the build's own `flash_args` file, so nothing is
  hardcoded

**SD card**

* Lists removable disks only; the disk holding your OS is never offered
* Requires you to type `ERASE` before touching anything
* Partitions and formats FAT32 — via `mkfs.vfat` on Linux, `diskutil` on macOS,
  `diskpart` on Windows
* On Windows above 32 GB, where the built-in formatter refuses FAT32 outright,
  it writes the filesystem itself rather than sending you off for a third-party
  utility. FAT32 is not optional here: ESP-IDF's FATFS is built without exFAT
  support, so an exFAT card will not mount on the device.
* Copies the web interface and creates `Movies`, `Shows`, `Books`, `Music`,
  `Gallery`, `Files` and `config`
* Size-verifies every copied file, then checks the finished card against a
  manifest of the 28 files the firmware serves by name plus the folders the
  indexer expects — a card that is missing one of them boots into a web UI that
  half works, which is miserable to diagnose from the device

---

## Commands

| Command | What it does |
| --- | --- |
| `nomad-setup` | Guided run: firmware, then card |
| `nomad-setup doctor` | Report tools, ports and disks. Changes nothing. |
| `nomad-setup selftest` | Verify the FAT32 formatter and the pre-flash checks offline |
| `nomad-setup flash` | Build (or take prebuilt binaries) and write to the board |
| `nomad-setup sdcard` | Format a card and copy the Nomad files onto it |
| `nomad-setup list-disks` / `list-ports` | Just the lists |

Start with `doctor`. It tells you what is missing before anything is at stake.

### Useful flags

```
--board <profile>                      pocket-dongle (GNPE stick, default),
                                       t-dongle (LilyGO), waveshare-1.47
--dry-run                              print every step, touch nothing
--install-deps                         fetch the ESP32 core and libraries
--firmware <dir>                       flash prebuilt binaries, skip the build
--port <port>  --disk <disk>           skip the interactive pickers
--no-placeholders                      copy the web UI only, no demo media
--mount <path>                         copy into an already-mounted card
--no-format                            keep the existing filesystem
--erase                                erase the whole flash before writing
--yes                                  accept ordinary prompts
--build-property KEY=VALUE             extra arduino-cli build property
```

`--yes` deliberately does *not* skip the `ERASE` confirmation. Add
`--i-know-what-im-doing` if you genuinely want an unattended format.

---

## Typical runs

First time, everything from scratch:

```
sudo ./tools/nomad-setup --install-deps
```

Firmware only, board already on a known port:

```
./tools/nomad-setup flash --port /dev/ttyACM0
```

Card only, no demo media:

```
sudo ./tools/nomad-setup sdcard --disk /dev/sdb --no-placeholders
```

Reflash from binaries somebody else built:

```
./tools/nomad-setup flash --firmware firmware/JcorpNomadProject/build/esp32.esp32.esp32s3
```

---

## Getting the board into the bootloader

The dongle has no reset button. If `flash` cannot find or talk to it: unplug
it, hold the boot button, plug it back in while still holding, then release.
It will appear as a serial port in download mode.

## Getting at the card

Two options, both fine:

* Put the microSD in a card reader.
* Leave it in the stick and boot the stick into USB mass-storage mode (hold the
  boot button for ~1.2 s once the firmware is running). The card then shows up
  as an ordinary removable drive and `nomad-setup sdcard` can format it there.

The second only works after the firmware is flashed, which is why the guided
run does the firmware first.

---

## Permissions

Partitioning a disk needs elevation on every platform:

* Linux / macOS: `sudo ./tools/nomad-setup sdcard`
* Windows: run `nomad-setup.bat` from an **Administrator** command prompt

Flashing usually does not. On Linux you may need to be in the `dialout` group
to open the serial port:

```
sudo usermod -aG dialout $USER      # log out and back in
```

---

## What actually goes on the card

Everything in `SD_Card_Template/` except the four `img.py` scripts, which are
repo-side placeholder-image generators and have no business on the card — 70
files of 74. That covers the web interface (`index.html`, `menu.html`, the
per-section and reader pages, `admin.html`/`admin.js`), the Mk4 shared
front-end (`master.css`, `theme-manager.js`, `theme-boot.js`,
`nomad-utils.js`, `default-themes.json`) and the whole `assets/` tree, plus the
demo media unless you pass `--no-placeholders`.

The tool then creates the seven directories the firmware's indexer expects:
`Movies`, `Shows`, `Books`, `Music`, `Gallery`, `Files`, `config`.

Three things are deliberately *not* written, because the firmware makes them
itself on first boot: `config/settings.json`, the `.system-index/` index files,
and the `*.flag` marker files.

Three paths are routed or present without being required: `/maps.html` (the
firmware routes it, no file is shipped), `/assets/kiwix/…` (ZIM reader assets
you supply yourself) and `zimtest.html` (a development page nothing links to).
`--verbose` lists them.

`nomad-setup selftest` cross-checks the manifest against the template in the
repo, so the two cannot drift apart without a test failing.

## What has and has not been tested

`nomad-setup selftest` covers three things, all offline: the FAT32 formatter
(volumes from 64 MB to 128 GB, boot sector checked against the planned
geometry, `fsck.vfat` over each one, and a file round-tripped through
`mtools`), the pre-flash partition checks, and the card manifest against the
shipped template. All pass.

The disk enumeration and format paths were developed and exercised on Linux.
The macOS (`diskutil`) and Windows (`diskpart`, plus the >32 GB raw-volume
path) branches follow each platform's documented behaviour but have not been
run against real hardware. Use `--dry-run` first on those, and please report
anything that misbehaves.

`--mount <path>` is the escape hatch: if the disk layer misreads your reader,
format the card however you like and point the tool at the mounted path to do
the copy step only.
