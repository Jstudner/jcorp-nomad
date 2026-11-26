# U Update hosting on the owner's repo (tunlezah/nomad)

> Every update path in this fork still leads to the upstream author's server, nomadflash.jcorptech.net. That covers the firmware pull, the interface manifest the phone fetches, the "Flash it" links and the card tools' map packs. Devices also trust only the upstream author's signing key, which the owner does not hold, and the stable build follows the "experimental" channel. The fix moves the owner's update channels to one GitHub Pages site built from the owner's GitHub Releases: `https://tunlezah.github.io/nomad/update/v1/<channel>/…`. It puts every fork-specific value in one new header, `NomadUpdateConfig.h`, and compiles in the owner's own P-256 keys (a primary plus an offline backup). Channels become an admin setting (stable by default), so one binary serves both. Each device shows which key it trusts. The owner signs releases on their own machine, never in CI. What users see: the admin panel says "Updates from tunlezah/nomad", shows the key fingerprint and offers a Stable/Experimental choice. Each existing device needs one USB reflash, through a browser flash page on the same site, to move from the upstream key to the owner's. The device still goes online only when an admin presses a button in WiFi Mode.

## 1. Scope (in / out)

**In:** where update files live and their exact URLs; `NomadUpdateConfig.h`; the key-list header contract; key fingerprints in the API; the runtime update channel; repointing every update endpoint and user-facing link or string; the decision on card-tools map packs, plus confinement of pack file paths; the Pages assembly workflow, the channel pointer file, the owner's sign-and-publish script and the browser flash page; key custody, backup, rotation and the one-time transition from the upstream key; the end-to-end release, rollback and unpublish process; the README fork notice.

**Out:** the signed UI manifest's format and on-device verification (6.6; this doc only says where its files live). The build itself: pinning, reproducibility, the `.nomadfw` packer/signer and the key generator (6.7; this doc fixes the header format and asset names they must produce). On-device CORS and Host checks (6.5). Card tools running JS from the card (6.4). First-run passwords (6.1).

## 2. Current behaviour (file:line)

Every `jcorptech` / `nomadflash` reference (from a grep of the repo plus the extracted card tools):

| # | Where | What it is | Class | Action here |
|---|---|---|---|---|
| 1 | `firmware/JcorpNomadProject/NomadUpdate.h:40-42` `NOMAD_UPDATE_BASE "https://nomadflash.jcorptech.net/update/v1/"` | base for both the firmware list and the UI manifest | **update endpoint** | set in `NomadUpdateConfig.h` |
| 2 | `NomadUpdate.h:37-39` `NOMAD_UPDATE_CHANNEL "experimental"` | channel the stable build follows | **update endpoint (bug)** | default `"stable"`, admin-selectable |
| 3 | `NomadUpdate.h:19`, `NomadFirmware.h:2,22,66` | comments | docs | reword |
| 4 | `NomadFirmware.h:181,290,292,296,298,302,304,307,309,311` | error messages that name "nomadflash" | user-facing text | neutral wording + `NOMAD_UPDATE_SOURCE` / `NOMAD_FLASH_URL` |
| 5 | `NomadFirmwareKey.h:1-19` | upstream public key, SHA-256(DER) = `9e956fde71ccad28…` (computed for this doc) | **trust anchor** | no longer included by owner builds; file left untouched |
| 6 | `SD_Card_Template/admin.html:1199,1239,1240` | button title, "Check nomadflash", file-picker title | user-facing text | neutral |
| 7 | `SD_Card_Template/admin.js:1897` | "Flash it" link to `https://nomadflash.jcorptech.net` | **user-facing link** | device-supplied `flash` URL, owner fallback |
| 8 | `admin.js:2348` (comment), `2360, 2385, 2395, 2410` | firmware-card text | user-facing text | neutral + link |
| 9 | `README.md:154` | browser flasher | user-facing link | add the fork's flash page and explain the key |
| 10 | `README.md:17,18,41,48,352` | the upstream author's shop and Ko-fi | attribution/commerce | **keep** (they credit the original author) and add a fork notice |
| 11 | card tools `map_packs.py:44` `INDEX = "https://nomadflash.jcorptech.net/packs/v2/index.json"` | map-pack download | **card-tools download** | keep upstream by default, make configurable, confine paths (§4.6) |
| 12 | card tools `map_catalog.py:26` | docstring | docs | leave |

How it works today:
- **Firmware pull** (`NomadFirmware.h:273-353`, task started by `POST /api/firmware/pull` at `:531-548`, 12 KB internal stack at `:541`). It fetches `NOMAD_FW_BASE + NOMAD_FW_CHANNEL + "/firmware.json?t=<esp_random>"` (`:289`) over `WiFiClientSecure` with `setInsecure()` (`:280`), with a 15 s timeout and `HTTPC_STRICT_FOLLOW_REDIRECTS` (`:283-284`). It reads `boards[NOMAD_BOARD_ID]` (`:297-304`), resolves a relative `url` against the base (`:305`), checks Content-Length against `size` (`:311`) and streams into the spare slot with a 20 s quiet timeout (`:320-335`). Media is paused (`:274`). Pulls happen only in WiFi Mode (`:533`).
- **Upstream's live data** (fetched 2026-11-26): the channels are `main` and `experimental` (`stable` returns 404). `firmware.json` uses `format:1`, holds `boards.s3-usba|s3-usbc.{version,url,size,sha256,usbOnly,notes,published}`, and its `url` is relative to the *base* (`firmware/files/s3-usba/nomad-s3-usba-5.0-….nomadfw`, 2,218,800 B). The UI manifest has `format:1, build, minFirmware, base:"../../../payload/main/sd/"` and 913 files totalling 39.5 MB; the largest is `/Nomad-Card-Tools.zip` at 2.47 MB.
- **Trust:** `fwCheckTrailer` (`NomadFirmware.h:162-193`) verifies the trailer signature against the single compiled-in `NOMAD_FW_PUBKEY` (`:173-181`; the include is at `:40-45`). The pull and the manual upload (`/api/firmware/begin|stage`, `:489-529`) both go through it. The signed meta `channel` is read (`:186`) but not enforced. Nothing can add a key at run time.
- **Interface updates:** the phone gets the manifest URL from `GET /api/update/env` (`NomadUpdate.h:321-331`, built from the macros at `:324-325`). It then fetches the manifest and each file cross-origin (`admin.js:1878-1888, 1984-1989, 2006`), so the host must send `Access-Control-Allow-Origin`.
- `NOMAD_FW_VERSION` defaults to `"5.0"` at `NomadUpdate.h:34-36`. `NomadSupport.h:243-245` reports version, board and channel. Include order is `JcorpNomadProject.ino:6296` (NomadUpdate.h) → `:6299` (NomadFirmware.h) → `:6300` (NomadSupport.h); routes are registered at `:8420-8425`.
- The owner's repo is a public fork of `Jstudner/jcorp-nomad` with no Pages site and no releases yet (GitHub API, 2026-11-26).

## 3. Threat or failure being fixed (concrete scenarios)

- **F1 Owner can't ship.** Every build points at nomadflash, so the owner's fixes never reach a device over the air. The upstream operator, or anyone who takes over that domain, still controls the UI-update channel for the owner's devices. The UI manifest is unsigned today; 6.6 adds the signature.
- **F2 Wrong channel.** A "stable" build pulls `experimental` (`NomadUpdate.h:38`).
- **F3 Key lock-in and confusion.** Owner-signed images fail with "Only firmware from nomadflash can go on over the air" (`:181`), and nothing shows which key a device trusts.
- **F4 Hosting that silently breaks.** GitHub release-asset URLs send no CORS header, so the phone's UI update would fail (measured, §4.1). raw.githubusercontent.com has unpublished, tightened anonymous rate limits [R6].
- **F5 Key theft through CI.** A signing key held as an Actions secret is readable by any step in its job. The tj-actions/changed-files compromise dumped runner secrets into public logs (CVE-2025-30066) [R11].
- **F6 Untrusted host.** `setInsecure()`, a compromised GitHub account, or the `tunlezah` name being re-registered after deletion can all serve arbitrary bytes. Only signatures protect the device. `firmware.json` is unsigned, so a man-in-the-middle can lie about versions or point at a different signed file.
- **F7 CDN staleness.** Pages caches for `max-age=600`, so a phone can get a new manifest together with an old cached file under the same path.
- **F8 Map-pack index controls file paths.** `map_packs.py:243` writes `card.root / f["path"].lstrip("/")` with no check. A malicious or compromised index can write `../../…` outside the card on the PC, or replace `/admin.js` on the card (code in the admin origin).
- **F9 Host disappears.** A deleted repo or account, a renamed repo, or a Pages outage.
- **F10 Privacy.** GitHub logs the IP of every visitor to a Pages site [R3]. The phone's fetch sends an `Origin` (and by default a `Referer`) that reveals the device's LAN address or name.

## 4. Design

### 4.1 Hosting choice (measured 2026-11-26 with curl through the session proxy; certificates could not be seen because the proxy re-terminates TLS)

| | GitHub Pages | Release asset (`github.com/…/releases/download/…`) | raw.githubusercontent.com |
|---|---|---|---|
| CORS | `access-control-allow-origin: *` | none on the 302 **or** on the target (with `Origin:` sent) | `*` |
| Hops | 0 (301 only for a missing trailing slash) | 1: 302 → `release-assets.githubusercontent.com` (Azure blob, signed URL valid ~1 h, Location 926 chars; host changed from objects.githubusercontent.com in 2025 [R8]) | 0 |
| Content-Length / Range | yes / `accept-ranges: bytes` | target: yes / 206 works | yes / yes |
| Cache | `max-age=600`; `?t=` query is a cache miss | `no-cache` on 302 | `max-age=300` |
| Header size | ~0.7 KB | 302 ≈ 5.8 KB incl. a 4,204-char CSP line | small |
| Limits | site ≤ 1 GB, soft 100 GB/month, deploy ≤ 10 min; the 10-builds/hour limit doesn't apply to Actions deploys [R1]; public repos on Free [R2] | file < 2 GiB, ≤ 1000 assets, no total or bandwidth cap [R4] | anonymous limits tightened May 2025, numbers unpublished [R6] |

On the device, Arduino-ESP32 3.3.3 `HTTPClient` follows a 302 on any method and 301/307 for GET. It does **not** follow 308. It refuses a protocol change and reconnects when the host changes (`HTTPClient.cpp` `sendRequest` 620-675, `setURL` 1444-1471). `WiFiClientSecure` sets `MBEDTLS_SSL_VERIFY_NONE` when insecure and still sends SNI (`ssl_client.cpp:194-196, 296`) [R9]. A release URL would therefore work, at the cost of two extra TLS handshakes and a 5.8 KB header block. This is code reading, not a device test.

**Decision: one Pages site serves everything the device and the phone fetch. GitHub Releases are the immutable archive and the source of truth.** The Pages site is rebuilt by a workflow from release assets and a committed channel-pointer file, so Pages never holds anything that was not in a published release. raw and direct release URLs are rejected for the reasons above. Because the device already accepts absolute URLs (`NomadFirmware.h:305`), `firmware.json` can point at a release asset later if Pages ever gets too big. No firmware change is needed for that.

### 4.2 URL layout (Pages project site)

```
https://tunlezah.github.io/nomad/
  index.html                          landing: channels → versions, key fingerprints, links
  update/v1/stable/firmware.json      device (format 1, unchanged parser)
  update/v1/stable/manifest.json      phone (6.6's signed UI manifest; file name per 6.6)
  update/v1/experimental/{firmware.json,manifest.json}
  update/v1/fw/<ver>/nomad-<ver>-<board>.nomadfw        version-addressed, never overwritten
  update/v1/ui/<ver>/<card path>      e.g. ui/5.0.1/admin.js, ui/5.0.1/EmulatorJS/…
  flash/index.html                    ESP Web Tools page (USB, Chrome/Edge desktop, Chrome Android)
  flash/esp-web-tools/…               vendored esp-web-tools@10.4.0 (published 2026-07-15)
  flash/<ver>/<board>.json            ESP Web Tools manifest
  flash/<ver>/<board>/{bootloader,partitions,boot_app0,app}.bin
```
`NOMAD_UPDATE_BASE = "https://tunlezah.github.io/nomad/update/v1/"`. Files are addressed by version, which fixes F7: a new manifest names paths no cache has seen. The manifest's `base` is `"../ui/<ver>/"`, relative to the manifest URL as `admin.js:1984-1989` resolves it today. One signed manifest therefore works for both channels; 6.6 must keep `base` relative or absolute, and must not bind the signature to a channel name. Size is about 40 MB of UI, 4.4 MB of firmware and 4.6 MB of flash parts per published version, so two versions come to about 100 MB of the 1 GB limit.

### 4.3 Data formats

**`release/channels.json`** (new and committed; the only thing the owner edits to move a channel; it never conflicts with upstream):
```json
{ "format": 1,
  "stable":       { "tag": "v5.0.1" },
  "experimental": { "tag": "v5.0.2", "usbOnly": false } }
```
`tag` must be a published, non-draft release. `null` publishes an empty channel (emergency stop). An optional `"bench-test"` key serves `NOMAD_FW_BENCH=2` builds (`NomadFirmware.h:66-67`).

**`firmware.json`** is generated and keeps format 1, so the existing parser at `NomadFirmware.h:293-304` works unchanged. Keep it under 2 KB:
```json
{ "format": 1, "channel": "stable", "release": "v5.0.1", "updated": "2026-12-01T10:00:00Z",
  "boards": { "s3-usba": { "version": "5.0.1", "url": "fw/5.0.1/nomad-5.0.1-s3-usba.nomadfw",
      "size": 2218800, "sha256": "…", "usbOnly": false,
      "notes": "https://github.com/tunlezah/nomad/releases/tag/v5.0.1" },
    "s3-usbc": { … } } }
```

**Release asset contract** (6.7's tag-build CI creates a *draft* release; the owner's `publish.sh` signs and publishes it). If 6.7 picks other names, change them in the one `ASSETS` table in `assemble_site.py`.

| Asset | Made by | Used by |
|---|---|---|
| `nomad-<v>-<board>.bin`, `-bootloader.bin`, `-partitions.bin`, `boot_app0.bin` | CI | USB (esptool, flash page) |
| `nomad-<v>-<board>.nomadfw.unsigned` | CI (6.7 packer) | owner signs it, then deletes it before publishing |
| `nomad-<v>-<board>.nomadfw` | owner (6.7 signer) | OTA pull, manual upload |
| `nomad-ui-<v>.zip`, `ui-manifest-<v>.json` (signed per 6.6) | CI, then the owner signs the manifest | Pages `ui/<v>/`, `<channel>/manifest.json`, manual card copy |
| `SHA256SUMS`, `build-info.json` | `publish.sh` regenerates the first after signing; 6.7 makes the second | people, `assemble_site.py` |

**ESP Web Tools manifest.** Offsets match `tools/flash-nomad.sh:67-70` and the partition table (nvs at 0x9000 is left alone) [R10]:
```json
{ "name": "Jcorp Nomad (tunlezah/nomad)", "version": "5.0.1", "new_install_prompt_erase": true,
  "builds": [{ "chipFamily": "ESP32-S3", "parts": [
    { "path": "s3-usba/bootloader.bin", "offset": 0 }, { "path": "s3-usba/partitions.bin", "offset": 32768 },
    { "path": "s3-usba/boot_app0.bin", "offset": 57344 }, { "path": "s3-usba/app.bin", "offset": 65536 } ] }] }
```
`new_install_prompt_erase: true` is mandatory. Without it ESP Web Tools erases the whole flash, NVS included, before a new install (`install-dialog.ts` 239-243 and 363-368: "Default is to erase") [R7].

### 4.4 Firmware

**New `firmware/JcorpNomadProject/NomadUpdateConfig.h`.** This is the only file that differs from upstream in values:
```cpp
// NomadUpdateConfig.h - where this build of Nomad looks for updates, and whose signature it
// accepts. Everything that makes this fork (github.com/tunlezah/nomad) different from the
// original Jcorp Nomad (nomadflash.jcorptech.net) lives here, so merging the original's
// changes rarely touches it. Nothing here makes Nomad go online by itself: the device fetches
// only when an admin presses Check for updates in WiFi Mode, and the phone fetches the rest.
#ifndef NOMAD_UPDATE_CONFIG_H
#define NOMAD_UPDATE_CONFIG_H
#include <stddef.h>
#ifndef NOMAD_FW_VERSION
#define NOMAD_FW_VERSION "5.0.1"            // releases are tagged v<this>; tools/release checks it
#endif
#define NOMAD_UPDATE_SOURCE "tunlezah/nomad on GitHub"   // named in messages and the admin panel
#ifndef NOMAD_UPDATE_BASE                   // <base><channel>/firmware.json|manifest.json
#define NOMAD_UPDATE_BASE "https://tunlezah.github.io/nomad/update/v1/"
#endif
#ifndef NOMAD_UPDATE_CHANNEL                // a new device's channel; the admin can switch
#define NOMAD_UPDATE_CHANNEL "stable"
#endif
#define NOMAD_FLASH_URL "https://tunlezah.github.io/nomad/flash/"     // flashing over USB
#define NOMAD_RELEASES_URL "https://github.com/tunlezah/nomad/releases"
// one public key updates may be signed with: a short name people see, and its DER bytes
struct NomadPubKey { const char *name; const unsigned char *der; size_t len; };
#if NOMAD_FW_DEVKEY
#include "NomadFirmwareKeyDev.h"            // bench: a throwaway key, never committed
#else
#include "NomadReleaseKeys.h"               // tunlezah's keys, written by 6.7's key tool
#endif
#endif
```
**`NomadReleaseKeys.h` contract** (6.7's keygen writes it; if 6.7's syntax differs, 6.7 wins as long as these symbols exist): `NOMAD_FW_KEYS[]` of `NomadPubKey`, primary first, at least two 91-byte P-256 SPKI DER keys; `#define NOMAD_FW_KEY_COUNT n`; a comment giving each key's id. Until the owner runs keygen, the committed placeholder holds a one-element dummy array, `#define NOMAD_FW_KEY_COUNT 0` and a `#warning`. With a count of 0 the build refuses every OTA image (the existing message at `:173`), which is safe.

**`NomadUpdate.h`.** Add `#include "NomadUpdateConfig.h"` and `#include <Preferences.h>` on new lines right after line 32, above upstream's `#ifndef` defaults at `:34-42`. Those defaults stay untouched (dead code), so upstream version or URL bumps merge without conflict and are correctly ignored. Then add the runtime channel:
```cpp
// The update channel. One build serves both, so moving a release from experimental to stable is
// just moving a pointer on the site. Read from NVS once (primed in nomadUpdateRegister, in setup)
// and kept as a byte; written only when the admin changes it.
static const char *const NOMAD_CHANNELS[] = { "stable", "experimental" };
static volatile uint8_t g_updChan = 0xFF;          // index into NOMAD_CHANNELS; 0xFF: not read yet
static uint8_t nomadChannelIndex(const String &c) {
  for (uint8_t i = 0; i < 2; i++) if (c == NOMAD_CHANNELS[i]) return i;
  return 0xFF;
}
static const char *nomadUpdateChannel() {
  if (g_updChan == 0xFF) {
    uint8_t i = 0xFF;
    Preferences p;
    if (p.begin("nomadfw", true)) { i = nomadChannelIndex(p.getString("chan", "")); p.end(); }
    if (i == 0xFF) i = nomadChannelIndex(NOMAD_UPDATE_CHANNEL);
    g_updChan = (i == 0xFF) ? 0 : i;               // an unknown default ("main") means stable
  }
  return NOMAD_CHANNELS[g_updChan];
}
```
In `nomadUpdateRegister` (`:321`), call `nomadUpdateChannel()` once to prime it. Change `/api/update/env` so `channel` and `manifest` use `NOMAD_UPDATE_BASE + String(nomadUpdateChannel()) + "/manifest.json"`, and add `"source"`, `"flash"` and `"channels":["stable","experimental"]`. 6.6 owns this route's other fields. Add `POST /api/update/channel?name=stable|experimental` (admin; `checkAdminAuth`). It validates against `NOMAD_CHANNELS`, does nothing if the value is unchanged, otherwise does `Preferences("nomadfw").putString("chan", …)` (the same in-callback NVS write `/api/firmware/apply` does at `NomadFirmware.h:564-565`), sets `g_updChan`, writes `webLogf`, and replies `{"channel":…}`. It needs no body, so `NomadLimitHandler` (`.ino:4613-4627`) is unaffected.

**`NomadFirmware.h`**
1. Replace `:40-45` with `#include "NomadUpdateConfig.h"`, plus `#if !defined(NOMAD_UPDATE_CONFIG_H)` `#error` as a guard. Add a shim for single-key headers (the dev key, or an upstream-style header): `#ifndef NOMAD_FW_KEY_COUNT` → `static const NomadPubKey NOMAD_FW_KEYS[] = {{"bench", NOMAD_FW_PUBKEY, NOMAD_FW_PUBKEY_LEN}}; #define NOMAD_FW_KEY_COUNT 1`.
2. Replace the channel default at `:76-78` with `static inline const char *fwChannel()`: it returns `NOMAD_FW_CHANNEL` if a bench build defined one (`:66-67`), otherwise `nomadUpdateChannel()`. Use it at `:289, :298, :453` and in `NomadSupport.h:245`. In `fwPullWork`, copy the channel into a local `String` at the start, so a mid-pull switch can't mix URLs.
3. `fwCheckTrailer` `:173-181`: try each key, keeping one `mbedtls_pk_context` at a time (same stack as today), and record `g_fw.signer = NOMAD_FW_KEYS[k].name` (new `const char *` in `FwStage`). The count check becomes `NOMAD_FW_KEY_COUNT == 0`. The failure message becomes `"The signature doesn't check out. This Nomad only takes firmware signed by " NOMAD_UPDATE_SOURCE "."`
4. The messages at `:290-311` drop "nomadflash" (e.g. `"Couldn't reach the update site (" NOMAD_UPDATE_SOURCE ")."`, `"The update site answered " + code + …`). `:302` becomes `version + " has to go on over USB: " NOMAD_FLASH_URL`.
5. Version binding (the list is unsigned, the file is signed). After the read loop, if `g_fw.state == FW_READY && g_fw.version != version`, call `fwFail("The file is " + g_fw.version + " but the list said " + version + ".")`. The slot is never booted, because `apply` needs `FW_READY`.
6. `GET /api/firmware` (`:448-487`): add `source`, `flash`, `channel: fwChannel()`, `channels` (omitted when a bench build forces a channel), `signed: NOMAD_FW_KEY_COUNT > 0`, `keys:[{name,id}]` and `ready.signer`. The id is the first 8 bytes of SHA-256 over the DER, in hex, which is what the tools print:
```cpp
// a key's short id: the first 8 bytes of SHA-256 over its DER, as hex (the SHA hardware makes
// this microseconds). Shown so a person can compare it with the one on the release page.
static void fwKeyId(const NomadPubKey &k, char out[17]) {
  uint8_t h[32];
  mbedtls_sha256(k.der, k.len, h, 0);
  for (int i = 0; i < 8; i++) snprintf(out + i * 2, 3, "%02x", h[i]);
}
```
7. `NomadSupport.h:243-245`: use `fwChannel()` and add `keys` (the id strings) and `updateBase`, so a support file shows whose updates the device takes.

The device must not trust the upstream key too (decision O6). If it did, a manually uploaded upstream image would be accepted, would point the device back at nomadflash, and would lock it to the upstream key.

### 4.5 Web UI (`SD_Card_Template/`)
- `admin.html:1199` title becomes "Needed for online updates". `:1239` "Check nomadflash" becomes "Check for updates". `:1240` title becomes ".nomadfw from the Releases page, works offline". After `#fw-now` (`:1234`) add `<div id="fw-source" class="adm-hint"></div>` and a hidden `<label id="fw-chan-wrap">Channel <select id="fw-chan">` with Stable and Experimental options.
- `admin.js`: add a fallback `const NOMAD_FLASH_PAGE = 'https://tunlezah.github.io/nomad/flash/'` for firmware that doesn't send `flash`. `:1895-1897` uses `env.flash || NOMAD_FLASH_PAGE`. `say()` links use `rel="noopener noreferrer"`. The manifest and file fetches (`:1881, :2006`) add `referrerPolicy: 'no-referrer'`. `Firmware.render` (`:2371`) fills `#fw-source` with `textContent` and created `<a>` elements only, never `innerHTML` from the device. With `d.keys` it shows "Updates from ${d.source}. Accepts firmware signed with ${name} (${id})…". Without `d.keys` (upstream firmware) it shows "This firmware only takes updates from nomadflash (the original Jcorp Nomad). To get tunlezah/nomad updates, flash it once over USB:" plus a link. When `d.channels` is present it shows the select, and on change POSTs `/api/update/channel` and then `load()`. The text at `:2360, :2385` becomes "Update it once over USB" plus a link, `:2395` "Asking the update site…", and `:2410` "Check ${d.source || 'the update site'} for new firmware? …". The comment at `:2348` gets reworded.

### 4.6 Card tools map packs
**Keep upstream as the default source; make it configurable; confine paths.** A Pages mirror is ruled out by the 1 GB limit. A Releases mirror means 1000-asset releases and re-hosting someone else's multi-GB catalog. In the card tools (inside `SD_Card_Template/Nomad-Card-Tools.zip`; this lands with 6.4 if 6.4's doc already touches `map_packs.py`):
- `INDEX = os.environ.get("NOMAD_PACKS_INDEX", "https://nomadflash.jcorptech.net/packs/v2/index.json")` (`:44`); the `--index` flag (`:481`) still works.
- Every pack file path must match `^/Maps/[A-Za-z0-9][A-Za-z0-9._-]*(/[A-Za-z0-9][A-Za-z0-9._-]*)*$` and end in `.pmtiles|.json|.bin|.idx`, the only types `map_catalog.py` produces (`:71-73,200,212,469,490-491`). Pack ids must match `^[a-z0-9][a-z0-9_-]*$`. A block `dir` must equal `/Maps/blocks`. Anything else is skipped with a warning before `_download` (`:243`) and before removal (`:309-325`). There is never `.html/.js/.svg/.css`, so a pack can't add code to the admin origin.
- Add a README line: "Map packs come from the original author's server, nomadflash.jcorptech.net; set NOMAD_PACKS_INDEX to use another."

### 4.7 Key custody

| | A: offline, owner signs locally (**recommended**) | B: Actions environment secret + required reviewer |
|---|---|---|
| Who can sign | someone with the key file and its passphrase | anyone who controls the GitHub account, the workflow, or any action the signing job runs |
| Reviewer gate | n/a | for a one-person repo it's the owner clicking Approve; "Prevent self-review" can't be on with a single reviewer [R5] |
| Availability on Free | yes | environments and protection rules are on all plans for public repos [R5] |
| Cost | ~5 min per release on the owner's computer | automatic |

Signing happens a few times a year, and the key protects every device's firmware, so the recommendation is A:
- **Primary key** `tunlezah-<yyyy>a`: made with 6.7's keygen as an encrypted PKCS#8 PEM (AES-256, passphrase) in `~/.nomad-keys/`, never in the repo (a `.gitignore` pattern covers `*.key.pem`). Two encrypted copies go on separate USB sticks; the passphrase goes in a password manager.
- **Backup key** `tunlezah-<yyyy>b`: generated in the same session under a different passphrase. It goes on one USB stick plus a paper printout, both kept at a second location, and is never copied to the everyday computer. Both public halves are compiled into every build.
- **Rotation.**
  - Planned: release N, signed with A, trusts [A, C, B]; later releases signed with C drop A. A device that skipped N installs N first, by manual upload from Releases.
  - Primary lost: sign the next release with B; it trusts [B, new D].
  - Primary compromised: do the same at once, and announce it in the README and release notes. An attacker still has to get a device to fetch their file, either by man-in-the-middle on the home network (`setInsecure`) or by talking someone into a manual upload, because devices pull only from the owner's Pages and only on a click.
  - Both keys lost: USB flashing still works (physical access).
- An optional upgrade is a hardware token (YubiKey PIV supports P-256); see §10.
- **GitHub account hygiene:** 2FA with a passkey or security key; immutable releases on (once published, assets and tag can't change; title, notes and the pre-release flag still can) [R12]; a tag ruleset so only the owner can create `v*`; actions pinned by commit SHA (6.7).

### 4.8 Transition from the upstream key (one USB flash per device)
OTA can't do it. Both OTA paths (pull and upload) end in `fwCheckTrailer`, which accepts only signatures from the compiled-in upstream key `9e956fde71ccad28…` (`NomadFirmwareKey.h`). Nothing at run time adds keys, and only the upstream author can make such a signature. The ROM bootloader's USB path checks no signature, so:
1. Open `https://tunlezah.github.io/nomad/flash/` in Chrome or Edge (desktop), or Chrome on Android. Plug Nomad in; the admin "Flash mode" button gets it into download mode. Pick USB-A or USB-C, choose Install, and **leave "Erase device" unticked**. That keeps NVS (firmware status, boot mode, and anything 6.1/6.5 move there) and the card's settings. The equivalent esptool command from the release notes mirrors `tools/flash-nomad.sh:66-70`.
2. `boot_app0.bin` resets otadata, so app0 boots the new image. There's no PENDING_VERIFY, so the self-test isn't involved.
3. Check: admin → Firmware shows "Updates from tunlezah/nomad … tunlezah-2026a (xxxxxxxxxxxxxxxx)", matching the fingerprint in the README and on the Pages landing page. `GET /api/firmware` has `keys`. Then, in WiFi Mode, the interface update now offers the owner's UI.
A device still on upstream firmware keeps working and keeps following nomadflash.

### 4.9 Release process end to end
1. **Bump** `NOMAD_FW_VERSION` in `NomadUpdateConfig.h` (numeric: 5.0.1, 5.0.2…; `fwCompare` reads 4 numeric parts and would treat `5.1.0-rc1` as equal to `5.1.0`). Commit, then push the annotated tag `v5.0.1`.
2. **CI (6.7)** builds both boards reproducibly, checks that the tag matches the version, and creates a **draft** release with the unsigned assets from §4.3. CI holds no private key.
3. **The owner runs `tools/release/publish.sh v5.0.1 [--channel experimental]`.** It:
   - downloads the draft and checks `SHA256SUMS`;
   - optionally rebuilds locally and compares (6.7);
   - signs both `.nomadfw` files and the UI manifest (6.7/6.6 tools; one passphrase prompt) and verifies them against `NomadReleaseKeys.h`;
   - uploads the signed files and the new `SHA256SUMS`, deletes the `*.unsigned` assets and publishes the release;
   - with `--channel`, edits `release/channels.json`, commits and pushes.
4. **`.github/workflows/publish-channels.yml`** runs on a push to `main` touching `release/channels.json`, and on `workflow_dispatch`. Running from `main` avoids the github-pages environment's default rule against deploying from tags [R13], and avoids `release` events running the tag's old copy of the workflow. `assemble_site.py`:
   - downloads the referenced releases (`gh release download`, `GH_TOKEN`) and refuses drafts;
   - checks `SHA256SUMS`;
   - checks each `.nomadfw` trailer: magic, size, image SHA-256, `meta.board == board`, `meta.version == tag`, and the signature with `openssl dgst -sha256 -verify` over `trailer[0:sigAt]` under one of the keys parsed from `NomadReleaseKeys.h`;
   - verifies the UI manifest (6.6's verifier) and every zip entry's size and hash; rejects `..`, absolute paths and symlinks, and requires `base == "../ui/<v>/"`;
   - lays out §4.2, writes `firmware.json`, vendors esp-web-tools from the npm tarball checked against a pinned sha512, and fails if the site is over 900 MB.
   Everything fails closed. Then `actions/upload-pages-artifact@v4` and `actions/deploy-pages@v4` run (permissions `pages: write, id-token: write`, environment `github-pages`) [R14], followed by a non-blocking `check-site.sh` smoke test.
5. **Pick-up.** A device on that channel sees the release on the next admin click: `firmware.json` is requested with `?t=`, which misses the CDN cache. Phones see the new manifest within about 10 min (`max-age=600`).
- **Channels.** Experimental gets every release first. Stable is moved by hand once a release has proven itself. Promoting means editing one line, not rebuilding, because the channel is a device setting and the signed meta `channel` is informational.
- **Rollback or unpublish.** Revert the `channels.json` commit; it redeploys in about 2 min and propagates within about 12 min. Devices that already installed the bad version keep it (they only accept newer versions), so fix forward with a new version built from the good code. A withdrawn release stays on GitHub marked "withdrawn" (immutable; deleting it burns the tag name). `"tag": null` is the emergency stop: devices say there's no firmware for their board on that channel, and phones get a 404, so the update card stays hidden.
- **If the repo or Pages goes away:** media serving is unaffected. "Check for updates" shows "Couldn't reach the update site (tunlezah/nomad on GitHub)". Manual `.nomadfw` upload still works, because trust comes from the key, not the host. USB always works. A squatter on a re-registered name can't produce an accepted signature. Before renaming the account or repo, ship a release whose `NOMAD_UPDATE_BASE` points at the new URL; project Pages URLs don't redirect after a rename (not verified).

### 4.10 Privacy and power posture
No new background traffic. The device connects only on an admin click in WiFi Mode, exactly as now. GitHub/Fastly see the device's public IP and `User-Agent: ESP32HTTPClient` for those requests, and the phone's IP and `Origin` for UI updates. The `Referer` is dropped (§4.5). This is stated on the Pages landing page.

### 4.11 Rejected alternatives
- Device pulls directly from release URLs: extra handshakes, 5.8 KB headers, and it depends on GitHub's redirect host.
- The phone fetching from Releases: no CORS.
- raw.githubusercontent.com: unpublished anonymous limits, `text/plain`.
- Device TLS certificate verification: needs a CA bundle in flash and correct time (Nomad often has no clock); signatures already protect integrity. Signing `firmware.json` is pushed to §10.
- A compile-time channel with two builds per release: promotion would mean a rebuild, and an experimental device installing a final would silently switch to stable.
- Replacing `NomadFirmwareKey.h` in place: guaranteed merge conflicts and a misleading file history.
- `release`-event Pages deploys: tag-ref protection and stale workflow copies.

## 5. Power, memory and performance budget

| Change | Flash | Internal RAM | CPU | Radio | Writes |
|---|---|---|---|---|---|
| Config header + 2-key list | +~0.3 KB | 0 | 0 | 0 | 0 |
| Key loop in `fwCheckTrailer` | +~0.1 KB | 0 extra (one pk context at a time, on the pull or writer task) | +1 ECDSA verify only when the primary fails (backup-signed or bad file): estimated tens to hundreds of ms, never on the async task | 0 | 0 |
| `keys` in `GET /api/firmware` (admin, rare) | +~0.2 KB | ~150 B transient JSON | 2× SHA-256 of 91 B (µs, hardware) | 0 | 0 |
| Runtime channel | +~0.6 KB, 1 B .bss | 1 B | one NVS read at boot, under 1 ms | 0 | 1 NVS write per admin change |
| Strings, version binding | ±0.3 KB | 0 | 0 | 0 | 0 |
| Hosting on Pages instead of nomadflash | 0 | handshake peak expected about equal to today (~40 KB transient during a pull). Arduino's lib-builder sets `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` and `DYNAMIC_FREE_PEER_CERT=y` [R15], and GitHub's edge chain is leaf + 1 intermediate, est. 3-4 KB. Unverified: measure | handshake est. 0.5-1.5 s per pull | same ~2.2 MB per admin pull; 1-2 handshakes (keep-alive reuse on one host) | 0 |

The total is under 2 KB of a 3 MB slot (today's image is 2.2 MB). There are no new tasks, timers, polling, screen or LED activity, and no per-request work outside the two rare admin routes. **Measure:**
- Before and after a pull: `/api/support` `mem.min` and `mem.tasks` (unchanged at idle), and `/api/health` `memLow`.
- Add one `webLogf` at the end of `fwPullWork` with the internal heap free and minimum and the pull's duration in ms. That's one log line per admin pull.
- `/api/performance` at idle, compared with the previous build.
- The `Sketch uses N bytes` line in 6.7's CI log.

## 6. Implementation plan (one reviewable commit each)
1. **Config and keys placeholder.** Add `NomadUpdateConfig.h`, a placeholder `NomadReleaseKeys.h` (count 0, `#warning`) and the `*.key.pem` ignore pattern in `firmware/JcorpNomadProject/.gitignore`. Include the config from `NomadUpdate.h` and `NomadFirmware.h`, with the shim and `#error` guard (§4.4 items 1-2).
2. **Trust.** Key loop, `signer`, `fwKeyId`, `keys` and `source` in `GET /api/firmware`, the support file fields, and the new signature message (§4.4 items 3, 6, 7).
3. **Pull.** Local channel copy, neutral messages, the `NOMAD_FLASH_URL` text, version binding and the end-of-pull heap log (§4.4 items 2, 4, 5).
4. **Channel.** `nomadUpdateChannel()`, the priming call, the `/api/update/env` fields, and `POST /api/update/channel`.
5. **Web UI.** `admin.html`/`admin.js` strings, the source and signer line, the upstream-firmware hint, the channel select, the flash link fallback, and the referrer changes (§4.5).
6. **Host checks.** `tools/updatecfg/check.sh` (§7).
7. **Release plumbing.** `release/channels.json` (pointing at nothing until the first release); `tools/release/assemble_site.py` (stdlib-only, runs under `python3 -I`); `tools/release/test_assemble.py`; `tools/release/site/{index.html,flash.html}`.
8. **Deploy.** `.github/workflows/publish-channels.yml` and `tools/release/check-site.sh`.
9. **Owner script.** `tools/release/publish.sh`, which calls the 6.7 and 6.6 sign/verify tools.
10. **Card tools.** `map_packs.py` env override and path confinement, a unit test, the README line, and a rebuilt zip (coordinate with 6.4/6.7).
11. **README.** Fork notice at the top; "Moving a Nomad to this fork's updates" (§4.8); the "Flash It From Your Browser" section updated; key fingerprints. Lands after the owner generates the keys.
12. **Owner setup (not code).** Generate keys and commit `NomadReleaseKeys.h`. Enable Actions on the fork (they're off by default on forks), set Pages → Source: GitHub Actions and Enforce HTTPS, turn on immutable releases, add the `v*` tag ruleset. Cut v5.0.1, USB-reflash each device, then update each card's UI.

## 7. Tests and acceptance criteria

**Host-side** (`bash tools/updatecfg/check.sh`; `python3 -I tools/release/test_assemble.py`; the existing `bash tools/guardtest/run.sh` must still pass):
- No `jcorptech` or `nomadflash` string literals in `firmware/**/*.{h,cpp,ino}`. Comments and the untouched, unused `NomadFirmwareKey.h` are allowlisted. In `SD_Card_Template/*.{html,js}` the only allowed hits are the README-style attribution, of which there are none in the UI.
- `NomadUpdate.h` includes `NomadUpdateConfig.h` before the first `#ifndef NOMAD_UPDATE_BASE`. Neither `NomadFirmware.h` nor `NomadUpdateConfig.h` includes `NomadFirmwareKey.h`. The base starts with `https://` and ends in `/`. The default channel is `"stable"`.
- On release tags: `NomadReleaseKeys.h` has at least 2 distinct keys, each a valid 91-byte P-256 SPKI (`openssl pkey -pubin -inform DER`), none equal to `9e956fde71ccad28…`, and `NOMAD_FW_KEY_COUNT` matches.
- `test_assemble.py` uses fake releases signed with a throwaway key made by `openssl` in the test. It checks the right tag per channel; refusal of drafts, unsigned files, bad signatures, wrong board, meta version ≠ tag, UI hash mismatch, `..` or symlinks in the zip; that `firmware.json` is under 2 KB with relative URLs that exist in the output; the size cap; and `tag: null` producing empty boards.
- The map-pack test rejects `../x`, `/admin.js` and `/Maps/a/b.js`, accepts `/Maps/blocks/9/1/2.pmtiles`, and rejects pack id `..`.

**Live site** (`check-site.sh`), for every channel:
- `firmware.json` returns 200 with `access-control-allow-origin: *` and `num_redirects=0`.
- Each `.nomadfw` returns 200, its Content-Length equals `size` and its SHA-256 matches.
- The manifest and 10 random files return 200 with ACAO `*`, matching size and hash.

**On device (manual):**
1. Upstream-firmware device with the new card UI: the Firmware card shows the "flash once over USB" hint and link.
2. Flash page, Erase unticked: the device boots, the admin password and hotspot settings are kept, `GET /api/firmware` lists the two keys, and their ids match the README.
3. WiFi Mode, Check for updates with stable at the running version: "Already up to date (5.0.1)".
4. Publish 5.0.2 to experimental. Stable still says up to date. Switch to Experimental, check, ready (`ready.signer` = primary), Apply, the self-test passes, and "Updated to 5.0.2" appears. Reboot: the channel is still experimental.
5. Upload upstream's 5.0 `.nomadfw`: refused, and the message names tunlezah/nomad.
6. Bench-only build signed with the backup key: accepted, and `signer` shows the backup.
7. Hand-edit a test `firmware.json` so its version disagrees with the file: refused with the version message, and Apply is not offered.
8. UI update over WiFi Mode: devtools shows requests only to `tunlezah.github.io`, no CORS errors and no `Referer`.
9. Hotspot mode: no requests to github.io.
10. Record `memLow`, `mem.min`, pull duration and task count before and after (§5). Media resumes after the pull.
11. Regressions: manual upload, rollback paths (NOMAD_FW_BENCH_FAIL builds), USB mode, Flash mode, the support file shows the channel and keys, and safe mode.

**Acceptance:**
- No build-time path reaches nomadflash except card-tools map packs, which are documented and configurable.
- Stable is the default.
- The key fingerprint is visible in the panel and the API.
- OTA works end to end from Pages.
- The Pages site is reproducible from releases plus `channels.json`.
- No private key exists in the repo or CI.
- The budget in §5 holds as measured.

## 8. Compatibility, rollout and rollback
- `firmware.json` keeps format 1. The new NVS key `nomadfw/chan` is ignored by older firmware, and an unknown value falls back to stable.
- Upstream-firmware devices are untouched until they're USB-flashed.
- Going back to upstream means USB-flashing from nomadflash; that restores the upstream key and base. The owner's fork sits on the card UI, which works on either firmware: `d.keys` and `env.flash` are optional fields.
- A bad owner release is caught first by OTA self-test rollback (`NomadFirmware.h:399-438`), then fixed forward (§4.9).
- Merging upstream later: conflicts are expected only in the edited lines of `NomadFirmware.h`, `NomadUpdate.h` and `admin.*`. Offering the generic parts upstream (key list, `NOMAD_UPDATE_SOURCE` with a "nomadflash" default, runtime channel) would remove them (O9).
- Order: commits 1-6 can land before the keys exist (OTA stays safely refused); the first release waits for step 12.

## 9. Dependencies on other workstreams
- **6.6:** puts its signed manifest at `update/v1/<channel>/manifest.json`; `base` stays relative, `"../ui/<v>/"`; consumes `NOMAD_FW_KEYS`/`NOMAD_FW_KEY_COUNT` (or separate keys, O7) and `nomadUpdateChannel()`; ships a sign/verify CLI for `publish.sh`/`assemble_site.py`; doesn't bind the signature to a channel. Also note the pre-existing bug: `NOMAD_UPDATE_MAX_FILES = 400` (`NomadUpdate.h:46`) against upstream's 913-file manifest, so paths past 400 are re-downloaded on every update. Any UI anti-rollback is 6.6's.
- **6.7:** keygen writes `NomadReleaseKeys.h` per §4.4; the packer emits `.nomadfw.unsigned` plus a signer; CI makes the draft release with the §4.3 asset names and checks tag == `NOMAD_FW_VERSION`; actions pinned by SHA; a deterministic rebuild of `Nomad-Card-Tools.zip`.
- **6.5:** pulls stay WiFi-Mode-only; if WiFi credentials move to NVS, the flash page's "don't erase" instruction matters more.
- **6.4:** owns card-tools hardening generally; takes §4.6 confinement if already editing `map_packs.py`.
- **6.1:** an accidental "Erase device" leads into 6.1's first-run flow.
- **6.3:** the channel route uses `checkAdminAuth`, as the other admin routes do.

## 10. Pushed to future fixes
- Signed `firmware.json` (stops version lies and update denial).
- Phone-relayed firmware ("download on this phone, send to Nomad") through the existing upload path. Pages CORS makes this possible, and it would remove device TLS and the media pause.
- eFuse or secure-version anti-rollback.
- A custom domain to decouple the URL from the account and repo name.
- Hardware-token signing.
- An owner-run map-pack catalog or mirror.
- A full card-setup web flasher like nomadflash's (File System Access).
- Moving the card-tools source out of the zip into `tools/`.

## 11. Open decisions for the owner (recommended default first)
- **O1 Custody:** offline local signing (A) **[recommended]**, or an Actions environment secret (B).
- **O2 Channel:** runtime admin setting stored in NVS **[recommended]**, or a compile-time channel with two builds per release.
- **O3 Map packs:** keep upstream with path confinement and `NOMAD_PACKS_INDEX` **[recommended]**, disable, or mirror.
- **O4 Browser flash page on Pages:** yes **[recommended]**, or README esptool instructions only.
- **O5 Version scheme:** numeric `5.0.1`, `5.0.2`… and jump to `X.Y.1` when merging upstream `X.Y` **[recommended]**.
- **O6 Also trust the upstream key:** no **[recommended]**.
- **O7 Same keys for firmware and UI manifest:** yes **[recommended]**, or a separate UI key.
- **O8 Immutable releases and the `v*` tag ruleset:** on **[recommended]**.
- **O9 Offer the generic changes upstream to Jstudner/jcorp-nomad:** yes **[recommended]**, to cut merge conflicts.
- **O10 README:** keep the original author's shop and Ko-fi links and add a fork notice **[recommended]**.

## References
- [R1] GitHub Pages limits: https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits
- [R2] Plans (Pages: public repos on Free, private on Pro and up): https://docs.github.com/en/get-started/learning-about-github/githubs-plans
- [R3] Pages logs visitor IPs: https://docs.github.com/en/pages/getting-started-with-github-pages/what-is-github-pages
- [R4] Release asset limits: https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases
- [R5] Environments, required reviewers, prevent self-review: https://docs.github.com/en/actions/how-tos/deploy/configure-and-manage-deployments/manage-environments ; https://github.blog/changelog/2023-10-16-actions-prevent-self-reviews-for-secure-deployments-across-actions-environments
- [R6] Unauthenticated rate limits incl. raw.githubusercontent.com: https://github.blog/changelog/2025-05-08-updated-rate-limits-for-unauthenticated-requests/
- [R7] ESP Web Tools manifest and erase default: https://esphome.github.io/esp-web-tools/ ; https://github.com/esphome/esp-web-tools/blob/main/src/install-dialog.ts ; npm esp-web-tools 10.4.0 (2026-07-15)
- [R8] release-assets.githubusercontent.com rollout: https://www.stepsecurity.io/blog/harden-runner-detects-new-traffic-to-release-assets-githubusercontent-com-across-multiple-customers
- [R9] arduino-esp32 3.3.3 HTTPClient and ssl_client: https://github.com/espressif/arduino-esp32/blob/3.3.3/libraries/HTTPClient/src/HTTPClient.cpp ; https://github.com/espressif/arduino-esp32/blob/3.3.3/libraries/NetworkClientSecure/src/ssl_client.cpp
- [R10] Partition table: https://github.com/espressif/arduino-esp32/blob/3.3.3/tools/partitions/app3M_fat9M_16MB.csv
- [R11] CVE-2025-30066: https://nvd.nist.gov/vuln/detail/CVE-2025-30066
- [R12] Immutable releases: https://github.blog/changelog/2025-10-28-immutable-releases-are-now-generally-available/ ; https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases ; asset digests: https://github.blog/changelog/2025-06-03-releases-now-expose-digests-for-release-assets/
- [R13] github-pages environment blocks tag deploys by default: https://github.com/orgs/community/discussions/39054 ; https://github.blog/changelog/2023-10-06-actions-secure-deployment-rollouts-to-protected-environments-based-on-select-tag-patterns/
- [R14] Custom Pages workflows: https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages ; forks have workflows off until enabled: https://docs.github.com/en/actions/how-tos/manage-workflow-runs/disable-and-enable-workflows
- [R15] Arduino lib-builder mbedTLS config: https://github.com/espressif/esp32-arduino-lib-builder/blob/release/v5.5/configs/defconfig.common
- Live measurements (2026-11-26, curl): Pages `esphome.github.io` (ACAO `*`, `max-age=600`); release asset `cli/cli` v2.60.0 (302 → release-assets, no ACAO, 206); `raw.githubusercontent.com` (ACAO `*`, `max-age=300`); upstream `nomadflash.jcorptech.net/update/v1/{main,experimental}/{firmware,manifest}.json`.
- Code: `NomadUpdate.h:19,34-46,321-331`; `NomadFirmware.h:40-78,162-193,266-353,448-487,531-548,564-565`; `NomadFirmwareKey.h:1-19`; `NomadSupport.h:243-245`; `JcorpNomadProject.ino:6296-6300,8420-8425,4613-4627,8335`; `admin.html:1199,1234,1239-1240`; `admin.js:1847-1898,1984-1989,2006,2347-2410`; `README.md:17-18,41,48,152-154,352`; `tools/flash-nomad.sh:40,66-70`; card tools `map_packs.py:44,50-55,243,277-325,481`, `map_catalog.py:26,71-73,200,212,469,490-491`.
