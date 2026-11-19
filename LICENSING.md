# Licensing

Jcorp Nomad is free software under the **GNU General Public License, version 3 or later**
(GPL-3.0-or-later). The full text is in [LICENSE](LICENSE).

| What | License |
|---|---|
| Firmware, web interface, card tools, scripts, documentation | GPL-3.0-or-later |
| Case models (`*.stl`) | CC BY-SA 4.0 |
| The Jcorp and Jcorp Nomad names, logos and product photos | Not licensed, see [Name and logo](#name-and-logo) |
| Other people's code and media that ship with Nomad | Their own licenses, see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) |

---

## What you can do

- Use Nomad for anything, at home, in a classroom, at work.
- Build your own, change it, and share your changes.
- Sell Nomads you built, with or without changes.

## What you have to do

The GPL has one main rule: whoever gets Nomad from you gets the same freedoms you got.

- **Keep the notices.** Leave the copyright and license notices in place, and say what you changed.
- **Share the source when you share the firmware.** If you pass on a Nomad, a card or a firmware
  file, the person getting it also gets the source for exactly that version, under GPL-3.0. If
  you didn't change anything, a link to the matching release here is enough. If you did, it's
  your changed source.
- **Don't lock it down.** Anyone who owns a Nomad has to be able to put their own build of the
  firmware on it. Flashing over USB already allows that, so leave it open.
- **Your changes stay GPL.** Anything built from Nomad's code is GPL-3.0 too. You can't make a
  closed version of it.

There's no warranty. See sections 15 and 16 of the license.

---

## Name and logo

The GPL covers the code, not the names. Under section 7(e) of the license, it gives no rights to
use "Jcorp", "Jcorp Nomad" or "Jcorp Tech" as names, or the Jcorp and Nomad logos. These are
trademarks of Jcorp Tech.

**Fine without asking:**

- Running and sharing unchanged copies of the firmware and card files, logos and all.
- Saying what your project is based on: "built on Jcorp Nomad", "a Jcorp Nomad fork", "works
  with Nomad cards".
- Talking about Nomad, reviewing it, making videos about it.

**Not without asking:**

- Calling your own build, fork or product "Jcorp Nomad", or a name that could be mistaken for
  it (like "Nomad Media Server").
- Selling devices, changed or not, with the Jcorp or Nomad logo on the device, the box, the
  listing or the boot screen.
- Saying or implying your product is made, sold or supported by Jcorp Tech.

If you sell units or share a changed version, give it your own name and swap out the logo
files: `SD_Card_Template/Logo.png`, `SD_Card_Template/favicon.ico` and the boot screen image
(`firmware/JcorpNomadProject/ui_img_1859342711.c`). The product photos in this repo
(`NomadCoverMK4v2.png`, `NomadMk4Explode.png`, `NomadNewScreen.png`) are for this README only.

Not sure if something's OK? Just ask.

---

## Additional permission for the ESP32 SDK

The firmware is built with Espressif's ESP-IDF and Arduino-ESP32, which include some
libraries (WiFi, Bluetooth, the radio) that Espressif only ships as compiled binaries. This
permission, under section 7 of the license, makes it clear a Nomad firmware can always be shared:

> If you modify this Program, or any covered work, by linking or combining it with Espressif's
> ESP-IDF or Arduino-ESP32 (or a modified version of those), including their binary-only
> libraries, the licensors of this Program grant you additional permission to convey the
> resulting work. Corresponding Source for a non-source form of such a combination shall include
> the source code for the parts of those libraries that is publicly available, as well as that
> of the covered work.

---

## Contributions

Pull requests are welcome. By sending one you agree it's released under the same license as the
files it changes: GPL-3.0-or-later for code and docs, CC BY-SA 4.0 for case models.

## Older versions

Releases before this file was added were published under CC BY-NC-SA 4.0, and copies of them
stay under that license. Everything from this release on is under the terms above.
