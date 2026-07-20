# Built-in games system (Games bucket)

## How it works

Any `.html` file inside `/Games` on the SD card is treated as a **built-in game**
and shows up on the Games page (`games.html`) as a card in the same grid as ROMs:

- Built-ins are **pinned above the ROM list** and are not re-ordered by the sort
  dropdown. Pin order is the `BUILTIN_ORDER` list in `games.html`
  (`go`, `connect four`, `chess`, `tic-tac-toe`, `whiteboard`); anything not in
  the list follows alphabetically.
- The card label is the filename without `.html` (`Connect Four.html` becomes "Connect Four").
- **Cover art** uses the same sidecar convention as ROMs: put an image with the
  same basename next to the file (`Go.png` beside `Go.html`) and it becomes the
  card cover. Until then the shared `placeholder.jpg` is used.
- **Add / remove**: drop an `.html` in `/Games` to add it; delete the file to
  remove it from the frontend. Nothing else to configure. (The Games index
  rebuilds on demand: after changing files via USB, the next visit may briefly
  show "Building game index".)
- The console filter dropdown hides built-ins (they have no console); the search
  box still matches their names.

Shipped built-ins live in `SD_Card_Template/Games/`:

| File | Needs | Notes |
|---|---|---|
| `Go.html` | `/assets/tenuki.min.js` (MIT, engine-only esbuild bundle of tenuki 0.3.1) | 9×9 / 13×13. 19x19 is not offered because the move list would outgrow the 512-byte MP state. |
| `Connect Four.html` | none | |
| `Chess.html` | `/assets/chess.min.js` (chess.js **0.12.1**, BSD-2) | 0.13+ is ESM-only and breaks as a classic `<script>`, so do not "upgrade" it. |
| `Tic-Tac-Toe.html` | none | |
| `Whiteboard.html` | none | Not a room game, see below. |

All two-player games share `/assets/mp-client.js` (HTTP short-polling against the
firmware's `/api/mp/*` room store: 4-char room codes, seat 0 = creator, turn =
`seq` parity, state = one opaque string ≤ ~500 bytes replaced on every move).

### Go state encoding

`{"s":13,"m":"ddqf..pp"}` where `m` is the move list, two chars per move
(`'a'+x`,`'a'+y`), `..` = pass. Every client rebuilds the position by replaying
the list into a fresh tenuki game, which also hands mid-game joiners the whole
board on their first poll. Scoring note: territory score is shown after two
passes **without dead-stone marking**, so casual-play accuracy only.

## Whiteboard

Same control set as the old Gallion whiteboard (Private/Live modes, undo/redo,
color, pen size, erase, clear, save, load) but the live board syncs through the
SD card instead of sockets (the firmware has none):

- Every participant writes **their own stroke layer** to
  `/.whiteboard/wb_<clientid>.json` via the open `POST /save` handler (atomic
  temp+rename on the firmware side), throttled to ≥0.9 s between writes and
  capped at 24 KB per layer.
- Everyone polls `GET /listfiles?dir=/.whiteboard` every 2.5 s and refetches only
  layers whose file size changed. Per-client layers mean no lost strokes from
  concurrent drawing.
- **Clear** on the live board `POST /delete`s every layer file. Other clients
  detect the wipe because their own file disappears from the listing, and reset.
- The private board autosaves strokes to `localStorage`; **Save** downloads a
  PNG, **Load** (private only) draws an image file onto the canvas.
- The `/.whiteboard` dotfolder is invisible to the media indexers (dot-prefixed)
  and is created on first save via `POST /mkdir`.

## EmulatorJS (ROM cards)

- Runtime lives at `/EmulatorJS/data/` (loader.js, emulator.min.js/.css,
  version.json, `compression/`, `localization/`, `cores/`).
- Cores staged on the card (4 build variants each): gambatte (GB/GBC),
  fceumm (NES), snes9x (SNES), genesis_plus_gx (Genesis/Master System),
  mgba (GBA) ≈ 24 MB. More cores: copy `<core>/*.data` from the EmulatorJS
  repo's per-core directories into `data/cores/`.
- `data/cores/reports/<core>.json` must exist per core or EmulatorJS disables
  **core caching** (the ~1 MB core re-downloads on every play, costly over the
  ESP32 link). The report content is trivial build metadata
  (`{"core":...,"buildStart":...,"buildEnd":...,"options":{}}`); synthesized
  ones are fine. The core itself downloads only after the user taps Start Game.
- `games.html` maps UI console ids to EmulatorJS system keys before launch
  (`gbc` to `gb`, `genesis` to `segaMD`, `sms` to `segaMS`), since EmulatorJS has **no** `gbc`
  system key; an unmapped id makes the loader fetch a nonexistent core and die.
- An "Exit Game" bar appears above the emulator; exiting reloads the page
  (EmulatorJS cannot be torn down and restarted in one page lifetime).
- Save states/saves persist in the browser (IndexedDB), keyed by ROM path
  (`EJS_gameID`).
