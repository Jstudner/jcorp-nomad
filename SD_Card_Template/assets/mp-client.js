// <!-- Version 5 -->
// Nomad multiplayer client: short-polling wrapper over /api/mp/*.
// Seat 0 is the creator, turn is seq parity, state is an opaque string owned by
// the game page.
'use strict';

/* Firmware without /api/mp/* answers with the captive portal page (status 200),
   so a JSON parse failure means the routes are missing. */
async function mpJson(r) {
  try { return await r.json(); }
  catch (e) {
    throw new Error('Multiplayer needs the Mk4.3+ firmware update.');
  }
}

const MP = {
  room: null,          // {code, token, seat, seq, joined}
  _pollTimer: null,

  async create(game) {
    const r = await fetch('/api/mp/create', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ game }),
    });
    if (!r.ok) throw new Error('Could not create room');
    const j = await mpJson(r);
    this.room = { code: j.code, token: j.token, seat: 0, seq: j.seq || 0, joined: false };
    this._seen = -1;
    return this.room;
  },

  async join(code) {
    const r = await fetch('/api/mp/join', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code: String(code).trim().toUpperCase() }),
    });
    if (r.status === 404) throw new Error('Room not found');
    if (r.status === 409) throw new Error('Room is full');
    if (!r.ok) throw new Error('Join failed');
    const j = await mpJson(r);
    this.room = {
      code: String(code).trim().toUpperCase(),
      token: j.token, seat: j.seat, seq: j.seq || 0, joined: true, game: j.game,
    };
    this._seen = -1; // first poll delivers current state, for a mid-game join
    return this.room;
  },

  // Send my move; `stateStr` is the full serialized game state after the move.
  async move(stateStr) {
    const r = await fetch('/api/mp/move', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        code: this.room.code, token: this.room.token,
        move: stateStr, seq: this.room.seq,
      }),
    });
    if (!r.ok) {
      const j = await r.json().catch(() => ({}));
      throw new Error(j.error || 'Move rejected');
    }
    const j = await mpJson(r);
    this.room.seq = j.seq;
    this._seen = j.seq; // don't re-apply our own move on the next poll
    return j.seq;
  },

  // Play again: either player may restart the room with a fresh state, no need to
  // reshare the code. The firmware alternates who moves first unless firstSeat pins it.
  async reset(stateStr, firstSeat) {
    const body = {
      code: this.room.code, token: this.room.token,
      move: stateStr, seq: this.room.seq, reset: true,
    };
    if (firstSeat === 0 || firstSeat === 1) body.first = firstSeat;
    const r = await fetch('/api/mp/move', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    if (!r.ok) {
      const j = await r.json().catch(() => ({}));
      throw new Error(j.error || 'Reset rejected');
    }
    const j = await mpJson(r);
    this.room.seq = j.seq;
    this._seen = j.seq; // we already hold the fresh state locally
    return j.seq;
  },

  myTurn() {
    return this.room && this.room.joined !== false && this.room.seat === this.room.seq % 2;
  },

  // Poll the room; onUpdate(state) fires whenever the server has a newer seq.
  // onInfo(j) fires every poll (join detection etc.). Call stop() to end.
  poll(onUpdate, onInfo, interval) {
    // a fetch still in flight from an old poll() sees the gen changed and stops
    this.stop();
    this._gen = (this._gen || 0) + 1;
    const gen = this._gen;
    this._stopped = false;
    // hidden: one look every 10 s. failing: twice as long each time, up to 10 s, back to
    // normal on an answer
    let wait = interval || 1200, busy = false;
    const base = interval || 1200;
    const tick = async () => {
      if (!this.room || this._stopped || this._gen !== gen || busy) return;   // one request at a time per poll()
      busy = true;
      try {
        const r = await fetch(`/api/mp/state?code=${this.room.code}&since=${this._seen}`,
          { cache: 'no-store' });
        if (r.status === 404) { onInfo && onInfo({ gone: true }); return; }
        if (!r.ok) throw new Error('HTTP ' + r.status);
        const j = await r.json();
        wait = base;
        if (this.room.joined === false && j.joined) this.room.joined = true;
        onInfo && onInfo(j);
        if (j.seq > this._seen) {
          this._seen = j.seq;
          this.room.seq = j.seq;
          if (j.state) onUpdate && onUpdate(j.state, j);
        }
      } catch (e) { wait = Math.min(10000, wait * 2); /* transient network error, keep polling */ }
      finally { busy = false; }
      // stop() or another poll() may have run during the fetch
      if (this._stopped || this._gen !== gen) return;
      this._pollTimer = setTimeout(tick, document.hidden ? Math.max(wait, 10000) : wait);
    };
    // shown again: look at once rather than finishing a long wait
    if (!this._visHooked) {
      this._visHooked = true;
      document.addEventListener('visibilitychange', () => {
        if (document.hidden || this._stopped || !this._tick) return;
        clearTimeout(this._pollTimer); this._tick();
      });
    }
    this._tick = tick;
    tick();
  },

  stop() { this._stopped = true; clearTimeout(this._pollTimer); },
};
