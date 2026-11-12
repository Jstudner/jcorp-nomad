// <!-- Version 1 -->
/* =============================================================================
   NOMAD GAME SHELL

   Classic script, no modules or build step.
   Load it AFTER /assets/mp-client.js and BEFORE the page's own <script>.

   The shell owns the lobby, the mode buttons and whose turn it is. For
   pass-and-play it swaps in a local version of MP, so a game's room code runs
   unchanged on one phone.

   On a P4, firmware/build-p4.sh copies assets/ into the flash UI image, so this
   file is served from flash. Editing it on the SD card does nothing there until
   the UI image is rebuilt. Files under /Games/ aren't copied and update from the
   card straight away. While testing, keep a copy at /Games/_game-shell.js
   (games.html skips .js files in /Games, so it won't show as a card).

   Version 1 covers:
     - modes 'pass' and 'room'
     - the lobby, the handoff screen, the leave button
     - a local seat driver that matches the firmware member for member

   Not in Version 1: a bot scheduler and a 'solo' mode (Chess, Checkers and
   Reversi have their own bots), and a rules layer that would let the shell
   check moves. Adding them shouldn't change anything below.
   ============================================================================= */
'use strict';

/* =============================================================================
   0.  HOW THE SHELL ATTACHES
   ============================================================================= */

/* mp-client.js declares `const MP = {`. A top-level const isn't window.MP and
   can't be replaced, but later scripts can see it by name and its members are
   writable. For local play the shell saves the six network members and
   overwrites them. For room play it doesn't touch MP at all. A game that never
   loads this file just gets stock mp-client.

   These six must match the list in mp-client.js. A network member added there
   and not here would let a local game call straight through to the server. */
var NET = null;    // the MP binding, captured at init()
var SAVED = null;  // the real members, while a local driver is installed

function takeOverMP(driver) {
  SAVED = { room: NET.room, myTurn: NET.myTurn, move: NET.move,
            reset: NET.reset, poll: NET.poll, stop: NET.stop };
  NET.room   = driver.room;
  NET.myTurn = function ()        { return driver.myTurn(); };
  NET.move   = function (s)       { return driver.move(s); };
  NET.reset  = function (s, f)    { return driver.reset(s, f); };
  NET.poll   = function (u, i, m) { return driver.poll(u, i, m); };
  NET.stop   = function ()        { return driver.stop(); };
}

function restoreMP() {
  if (!SAVED) return;
  for (var k in SAVED) NET[k] = SAVED[k];
  SAVED = null;
}

/* =============================================================================
   1.  MARKUP AND CSS
   ============================================================================= */

/* lobby, game, lobbyMsg, joinCode, roomCode, status, board and againBtn are the
   ids the room games already use, so keep them as is. boardId defaults to
   'board' so existing `#board{...}` rules keep working. Everything the shell
   adds is prefixed gs. */
var MARKUP =
  '<div id="lobby">' +
    '<h2 id="gsTitle"></h2>' +
    '<div class="hidden" id="gsOptRow"></div>' +
    '<div id="gsModes"></div>' +
    '<div class="hidden" id="gsPrimary"></div>' +
    '<div class="hidden" id="gsJoin">' +
      '<input type="text" id="joinCode" maxlength="4" placeholder="CODE" ' +
        'inputmode="text" autocapitalize="characters" autocomplete="off" ' +
        'spellcheck="false" aria-label="Room code">' +
      '<button class="btn secondary" id="gsJoinBtn">Join</button>' +
    '</div>' +
    '<div class="status" id="lobbyMsg" role="status" aria-live="polite"></div>' +
  '</div>' +
  '<div class="hidden" id="game">' +
    '<div class="code-big hidden" id="roomCode"></div>' +
    '<div class="status" id="status" role="status" aria-live="polite">Waiting…</div>' +
    '<div id="gsSlot"></div>' +
    '<div id="__BOARD__"></div>' +
    '<div id="gsActions">' +
      '<button class="btn hidden" id="againBtn">Play again</button>' +
      '<button class="btn secondary" id="gsLeave">Leave</button>' +
    '</div>' +
    '<div class="status" id="gsNote" role="status" aria-live="polite"></div>' +
  '</div>' +
  '<div class="gs-ov hidden" id="gsHandoff" role="dialog" aria-modal="true">' +
    '<div class="gs-ov-box">' +
      '<div class="gs-ov-title" id="gsHandoffTitle"></div>' +
      '<div class="gs-ov-note" id="gsHandoffNote"></div>' +
      '<button class="btn" id="gsReady">I\'m ready</button>' +
    '</div>' +
  '</div>';

/* Injected at parse time (this is a <head> script), so the markup never shows
   unstyled. */
(function injectCss() {
  if (document.getElementById('gs-style')) return;
  var s = document.createElement('style');
  s.id = 'gs-style';
  s.textContent =
    /* Styled by id. Don't use master.css's .card or .row here, their hover
       rules and `.card h2{display:flex}` break the lobby layout. */
    '#lobby,#game{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;margin-bottom:14px}' +
    '#gsOptRow,#gsModes,#gsPrimary,#gsJoin,#gsActions{display:flex;gap:10px;align-items:center;' +
      'flex-wrap:wrap;margin:8px 0;justify-content:center}' +
    '#gsActions{margin-top:14px}' +
    '#joinCode{background:var(--bg);color:var(--text);border:1px solid var(--line);' +
      'border-radius:8px;padding:10px 12px;font-size:1.1rem;text-transform:uppercase;' +
      'width:120px;text-align:center;letter-spacing:.2em}' +
    '#lobby select,#game select{background:var(--bg);color:var(--text);border:1px solid var(--line);' +
      'border-radius:8px;padding:9px 10px}' +
    '#lobby .btn,#game .btn,.gs-ov .btn{background:var(--primary);color:var(--color-white,#fff);' +
      'border:none;border-radius:8px;padding:10px 18px;cursor:pointer;font-size:1rem;min-height:44px}' +
    '#lobby .btn.secondary,#game .btn.secondary,.gs-ov .btn.secondary{background:transparent;' +
      'color:var(--primary);border:1px solid var(--primary)}' +
    '#lobby .btn:disabled,#game .btn:disabled{opacity:.5;cursor:not-allowed}' +
    '#roomCode{font-size:2rem;letter-spacing:.35em;font-weight:700;color:var(--primary);text-align:center;margin:6px 0}' +
    '#lobby .status,#game .status{text-align:center;color:var(--muted);min-height:1.4em;margin:8px 0}' +
    '#lobby .status.hot,#game .status.hot{color:var(--primary);font-weight:600}' +
    '#gsNote:empty{display:none}' +
    '#gsModeCaption{text-align:center;color:var(--muted);margin:2px 0 10px;font-size:.92rem}' +
    /* master.css hides `header h1` below 600px, so this is the game's name on phones */
    '#gsTitle{text-align:center;margin:0 0 14px;font-size:1.25rem;color:var(--text)}' +
    '.hidden{display:none!important}' +
    /* the handoff screen covers the whole board so the next player can't see it */
    '.gs-ov{position:fixed;inset:0;z-index:200;background:var(--bg);display:flex;' +
      'align-items:center;justify-content:center;padding:24px}' +
    '.gs-ov-box{text-align:center;max-width:320px}' +
    '.gs-ov-title{font-size:1.5rem;font-weight:700;color:var(--text);margin-bottom:8px}' +
    '.gs-ov-note{color:var(--muted);margin-bottom:20px}';
  document.head.appendChild(s);
})();

/* =============================================================================
   2.  THE LOCAL SEAT DRIVER  (pass mode)
   ============================================================================= */

/* Matches the firmware's room API member for member, so a game can't tell the
   difference. _locked stops the same player moving twice before the handoff. */
function LocalDriver(shell) {
  var self = this;
  this.shell = shell;
  this._state = '';
  this._seq = 0;
  this._games = 0;
  this._locked = false;
  this._onUpdate = null;
  this._onInfo = null;

  /* Empty code, since there's nothing to share. joined:true keeps the games'
     `!MP.room.joined` checks and #againBtn toggles working unchanged. */
  this.room = { code: '', token: '', seat: 0, seq: 0, joined: true, game: shell.cfg.id };

  this.myTurn = function () {
    return !self._locked && self.room.seat === self.room.seq % 2;
  };

  /* No 512-byte state limit here. That limit only applies to rooms, and Go's
     19x19 pass-and-play goes past it. Games that also run in rooms check the
     limit themselves (Go's capReached()). Don't add a check here, it freezes
     long local games. */
  this.move = function (stateStr) {
    self._state = String(stateStr);
    self._seq += 1;
    self.room.seq = self._seq;
    self._locked = true;
    // hand over on the next tick, so the game renders the move from the
    // mover's side first
    setTimeout(function () { self.shell._handOver(); }, 0);
    return Promise.resolve(self._seq);
  };

  // same as the firmware's reset (handleMpMove), so MP.reset(fresh, 0) works
  // the same offline
  this.reset = function (stateStr, firstSeat) {
    self._state = String(stateStr);
    self._games += 1;
    var want = (firstSeat === 0 || firstSeat === 1) ? firstSeat : (self._games % 2);
    do { self._seq += 1; } while (self._seq % 2 !== want);
    self.room.seq = self._seq;
    self.room.seat = self._seq % 2;
    self._locked = false;
    return Promise.resolve(self._seq);
  };

  /* No polling locally, there's nothing to fetch. onInfo fires once so the
     game shows its opening status. */
  this.poll = function (onUpdate, onInfo) {
    self._onUpdate = onUpdate;
    self._onInfo = onInfo;
    setTimeout(function () { if (self._onInfo) self._onInfo({}); }, 0);
  };

  this.stop = function () { self._locked = false; };
}

/* =============================================================================
   3.  THE SHELL
   ============================================================================= */

var GameShell = {
  cfg: null,
  mode: null,        // 'pass' | 'room', null until a mode is chosen
  driver: null,
  _el: {},

  init: function (cfg) {
    if (!cfg || typeof cfg !== 'object') throw new Error('GameShell.init needs a config object');
    if (!cfg.id || cfg.id.length > 15) throw new Error('GameShell: id must be 1..15 chars');
    if (!cfg.onStart) throw new Error('GameShell: cfg.onStart is required');
    var modes = (cfg.modes || ['pass', 'room']).slice();

    this.cfg = cfg;
    NET = (typeof MP !== 'undefined') ? MP : null;

    // A mode that can't work is left out instead of shown disabled.
    if (!NET) modes = modes.filter(function (m) { return m !== 'room'; });

    var mount = cfg.mount ? document.querySelector(cfg.mount)
      : (document.getElementById('gs-root') || document.querySelector('main'));
    if (!mount) throw new Error('GameShell: no mount point (<main> or #gs-root)');
    mount.innerHTML = MARKUP.replace('__BOARD__', cfg.boardId || 'board');

    var ids = ['lobby', 'game', 'lobbyMsg', 'joinCode', 'roomCode', 'status', 'gsTitle',
               'againBtn', 'gsOptRow', 'gsModes', 'gsPrimary', 'gsJoin',
               'gsJoinBtn', 'gsSlot', 'gsActions', 'gsLeave', 'gsNote',
               'gsHandoff', 'gsHandoffTitle', 'gsHandoffNote', 'gsReady'];
    for (var i = 0; i < ids.length; i++) this._el[ids[i]] = document.getElementById(ids[i]);

    this._el.gsTitle.textContent = cfg.name || '';
    if (cfg.hudHtml) this._el.gsSlot.innerHTML = cfg.hudHtml;
    if (cfg.preCreateHtml) {
      this._el.gsOptRow.innerHTML = cfg.preCreateHtml;
    }
    if (cfg.actionsHtml) {
      this._el.gsActions.insertAdjacentHTML('afterbegin', cfg.actionsHtml);
    }

    this._buildLobby(modes);
    this._wire();
    return this;
  },

  /* ---- lobby ------------------------------------------------------------ */

  _buildLobby: function (modes) {
    var self = this;
    var CAPTION = { pass: 'Two players, one phone', room: 'Two phones' };
    if (!modes.length) {
      this._el.gsModes.innerHTML =
        '<p class="status">This game needs the Mk4.3+ firmware update.</p>';
      return;
    }
    this._el.gsModes.innerHTML = '';
    modes.forEach(function (m) {
      var b = document.createElement('button');
      // all modes start unselected, a primary one looks already picked
      b.className = 'btn secondary';
      b.id = 'gsMode_' + m;
      b.textContent = CAPTION[m];
      b.onclick = function () { self._chooseMode(m); };
      self._el.gsModes.appendChild(b);
    });
    // a single mode needs no choosing, show its controls straight away
    if (modes.length === 1) this._chooseMode(modes[0]);
  },

  _chooseMode: function (m) {
    var cfg = this.cfg;
    var named = cfg.seats_named;
    this.mode = m;

    [].forEach.call(this._el.gsModes.children, function (b) {
      b.classList.toggle('secondary', b.id !== 'gsMode_' + m);
    });
    this._el.gsOptRow.classList.toggle('hidden', !cfg.preCreateHtml);
    this._el.gsPrimary.classList.remove('hidden');
    this._el.gsJoin.classList.toggle('hidden', m !== 'room');

    var label = m === 'room'
      ? 'Create Room' + (named ? ' (play ' + named[0] + ')' : '')
      : 'Start';
    this._el.gsPrimary.innerHTML = '';
    var b = document.createElement('button');
    b.className = 'btn';
    b.id = 'gsStart';
    b.textContent = label;
    var self = this;
    b.onclick = function () { m === 'room' ? self._create() : self._startLocal(); };
    this._el.gsPrimary.appendChild(b);

    /* Some options only work in some modes (Go's 19x19 board doesn't fit a
       room's 512-byte state), so tell the game which mode was picked. */
    if (cfg.onMode) cfg.onMode(m);

    var cap = document.getElementById('gsModeCaption');
    if (m === 'room') {
      if (!cap) {
        cap = document.createElement('p');
        cap.id = 'gsModeCaption';
        this._el.gsJoin.parentNode.insertBefore(cap, this._el.gsJoin);
      }
      cap.textContent = named ? 'or join a friend as ' + named[1] : 'or join a friend';
    } else if (cap) {
      cap.remove();
    }
    this._el.lobbyMsg.textContent = '';
  },

  _wire: function () {
    var self = this;
    this._el.gsJoinBtn.onclick = function () { self._join(); };
    this._el.joinCode.addEventListener('keydown', function (e) {
      if (e.key === 'Enter') self._join();
    });
    /* Room options (board size etc.) come from the room when you join, so hide
       them once there's a code in the box. */
    this._el.joinCode.addEventListener('input', function () {
      if (!self.cfg.preCreateHtml) return;
      self._el.gsOptRow.classList.toggle('hidden', !!self._el.joinCode.value.trim());
    });
    this._el.gsLeave.onclick = function () { location.reload(); };
    this._el.againBtn.onclick = function () {
      if (self.cfg.onRematch) self.cfg.onRematch();
    };
    this._el.gsReady.onclick = function () { self._dismissHandoff(); };
  },

  /* ---- entering play ---------------------------------------------------- */

  _startLocal: function () {
    this.driver = new LocalDriver(this);
    takeOverMP(this.driver);
    this._el.gsLeave.textContent = 'Back to Lobby';   // there is no room to leave
    this._enter();
  },

  _create: function () {
    var self = this;
    /* Disable the button while the request is out, a second tap makes a second room */
    var btn = document.getElementById('gsStart');
    if (btn) { if (btn.disabled) return; btn.disabled = true; }
    var release = function () { if (btn) btn.disabled = false; };
    NET.create(this.cfg.id).then(function () {
      // mp-client's create() doesn't set room.game (only join() does)
      NET.room.game = self.cfg.id;
      self._el.roomCode.textContent = NET.room.code;
      self._el.roomCode.classList.remove('hidden');
      self._enter();
    }).catch(function (e) {
      release();
      self._el.lobbyMsg.textContent = e.message;
    });
  },

  _join: function () {
    var self = this;
    var code = this._el.joinCode.value.trim();
    if (!code) return;
    NET.join(code).then(function () {
      // refuse a code for a different game (a Chess code typed into Tic-Tac-Toe)
      if (NET.room.game && NET.room.game !== self.cfg.id) {
        self._el.lobbyMsg.textContent =
          'That code is a different game (' + NET.room.game + ').';
        NET.room = null;
        return;
      }
      self._el.roomCode.textContent = NET.room.code;
      self._el.roomCode.classList.remove('hidden');
      self._enter();
    }).catch(function (e) { self._el.lobbyMsg.textContent = e.message; });
  },

  _enter: function () {
    this._el.lobby.classList.add('hidden');
    this._el.game.classList.remove('hidden');
    this.cfg.onStart();
    NET.poll(this.cfg.onUpdate, this.cfg.onInfo, this.cfg.pollInterval || 1000);
  },

  /* ---- pass-and-play handover ------------------------------------------- */

  /* In pass mode the seat follows the turn, so mySymbol(), myDisc() and
     myColor() return whoever is holding the phone and MP.myTurn() is always
     true for whoever is up. */
  _handOver: function () {
    var d = this.driver;
    if (!d) return;
    /* Don't hand over after the winning move. The seat stays with the winner so
       the game shows "You win!", and _locked keeps the finished board frozen. */
    if (this.cfg.isOver && this.cfg.isOver()) {
      if (d._onInfo) d._onInfo({});
      return;
    }
    if (!this.cfg.handoff) {
      d.room.seat = d.room.seq % 2;
      d._locked = false;
      if (d._onInfo) d._onInfo({});
      return;
    }
    var named = this.cfg.seats_named;
    var next = d.room.seq % 2;
    this._el.gsHandoffTitle.textContent =
      named ? 'Pass to ' + named[next] : 'Pass the phone';
    this._el.gsHandoffNote.textContent = this.cfg.handoffNote || '';
    this._el.gsHandoff.classList.remove('hidden');
  },

  _dismissHandoff: function () {
    var d = this.driver;
    this._el.gsHandoff.classList.add('hidden');
    if (!d) return;
    d.room.seat = d.room.seq % 2;
    d._locked = false;
    if (d._onInfo) d._onInfo({});
  },

  /* ---- what a game may call --------------------------------------------- */

  // extra status line the game's updateStatus() won't overwrite ("Thinking…")
  note: function (text) {
    if (this._el.gsNote) this._el.gsNote.textContent = text || '';
  },

  isLocal: function () { return this.mode === 'pass'; },

  // room mode names a side; on one phone nobody is "you"
  seatName: function (seat) {
    var n = this.cfg.seats_named;
    return n ? n[seat] : String(seat);
  }
};
