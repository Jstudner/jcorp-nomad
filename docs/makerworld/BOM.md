# Jcorp Nomad Mk4 — Bill of Materials

Total cost: roughly **$20–30** depending on SD card size.

## Electronics

| # | Part | Qty | Notes | Link |
|---|---|---|---|---|
| 1 | Waveshare ESP32-S3 Dev Board, **1.47" LCD version** | 1 | The only supported board — Wi-Fi, display, SD slot, and USB-A plug all on one PCB. No substitutes without firmware work. | [Amazon](https://amzn.to/4ktB6oT) |
| 2 | microSD card, 16–128 GB (up to 2 TB supported) | 1 | **Class 10 / U1 or better** — Nomad is very read-heavy and a fast card noticeably improves performance. Must be formatted FAT32. | [Amazon](https://amzn.to/44tM1c4) |
| 3 | USB power source | 1 | Any USB-A port, charger, or power bank. | — |

## Optional

| # | Part | Qty | Notes | Link |
|---|---|---|---|---|
| 4 | microSD extender (case-compatible length) | 1 | Swap the card from the back of the case without disassembly; also moves the card away from board heat. | [Amazon](https://amzn.to/45IWIJz) |
| 5 | Small thermal pad or dab of thermal paste | 1 | Between the ESP32-S3 chip and case shell. Not required — air cooling works fine. | — |

## Printed parts

| # | Part | Qty | Material | Notes |
|---|---|---|---|---|
| 6 | Nomad Mk4 case body | 1 | **PETG** (recommended) or quality PLA | ~10–15 g total. No supports. |
| 7 | Nomad Mk4 front cover (screen side) | 1 | PETG or PLA | Slides on front-to-back; snap fit (can be glued). |

## Software (all free)

| Tool | Purpose |
|---|---|
| [Arduino IDE](https://www.arduino.cc/en/software) | Flash the firmware |
| [FAT32Format GUI](https://fat32format-gui.en.lo4d.com/windows) | Format >32 GB cards as FAT32 on Windows |
| [HandBrake](https://handbrake.fr) | Compress video (Fast 480p preset recommended) |
| [Nomad Tools](https://github.com/Jstudner/Nomad-Tools) | Optional — prep offline Wikipedia/ZIM archives |
| [SquareLine Studio](https://squareline.io) | Optional/advanced — edit the device's on-screen UI |
