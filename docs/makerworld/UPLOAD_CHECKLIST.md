# MakerWorld Upload Checklist

## Post it as a REMIX
The Mk4 case is derived from [ESP32 C6 with LCD Screen Enclosure Case](https://makerworld.com/en/models/2121443-esp32-c6-with-lcd-screen-enclosure-case) by Adrian. When uploading, use MakerWorld's **"Remix" flow from that model page** (or link it as the remix source in the upload form) — that handles attribution properly and keeps you compliant with the original's license. Check the original model's license tag before picking yours; a remix must carry a compatible license.

- Suggested license: **CC BY-NC-SA 4.0** (matches the Nomad repo), if the original allows it.

## Files to upload
- [ ] Case 3MF / print profile — from repo `docs/`: `NomadFullTest.3mf` (plate) — verify it's the current Mk4 parts before upload
- [ ] `AssembledFullTest.3mf` / `AssembledFullTestsingle.3mf` — assembled-view variants, decide which is the print plate vs. display model
- [ ] Consider exporting clean per-part STLs with real names (e.g. `NomadMk4_Body.stl`, `NomadMk4_Cover.stl`) — MakerWorld shows filenames to users
- [ ] `BOM.csv` / `BOM.md` (attach as project files)
- [ ] `BUILD_GUIDE.md` content → paste into the MakerWorld "Assembly/Guide" or description section

## Images (MakerWorld wants several; real photos rank better than renders)
- [ ] Cover: `NomadCoverMK4v2.png` (repo root)
- [ ] Exploded view: `NomadMk4Explode.png`
- [ ] Screen close-up: `NomadNewScreen.png`
- [ ] Real photo of the assembled unit in hand / plugged into a power bank
- [ ] Screenshot of the web UI on a phone (menu + a movie playing)
- [ ] Photo of the printed parts on the plate

## Description
- Paste `DESCRIPTION.md` → fix the image placeholder, verify all links.
- ⚠️ **Affiliate links:** the BOM uses your Amazon affiliate links (`amzn.to/...`). MakerWorld has restrictions on external/affiliate links in descriptions — check current policy; safest is plain product names in the description and affiliate links only on GitHub/Instructables.
- Category suggestion: Electronics → Gadgets. Tags: `esp32`, `esp32-s3`, `media-server`, `offline`, `wifi`, `travel`, `camping`, `open-source`.

## Print profile settings (for the profile upload)
- PETG, 0.2 mm layer, 2+ walls, 15% infill, no supports
- Note in profile description: scale 101% if snap fit is too tight

## Cross-link when live
- [ ] Add the MakerWorld model link to the GitHub README ("Mk4 case files" section currently just says "in this repo")
- [ ] Add it to the Instructable's Case step
- [ ] `[ADD MAKERWORLD LINK]` placeholders anywhere you keep project links (jcorptech.net, Ko-fi page)
