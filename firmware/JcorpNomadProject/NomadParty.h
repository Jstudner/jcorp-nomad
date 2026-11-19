// NomadParty.h - Watch together: one phone hosts a film or an episode, everyone with the
// 4-letter code watches the same moment. Ported from Gallion's party.js (Voyager). A party
// can start before anything is picked, and the host can let everyone vote.
//
// The host is the clock. It sends {playing, position} on play, pause and seek and every few
// seconds. Nomad keeps when that arrived, and every answer says how long ago that was
// (age, ms), so a viewer never compares its own clock with anyone's. The hotspot has no
// internet time and phones on it disagree by seconds.
//
// Voting: the host opens a vote (most votes, a random pick from the picks, or two rounds).
// Everyone nominates one film or episode, the picks show live with who chose them. When the
// host closes it Nomad decides (ties at random), and the winner becomes what plays.
//
//   POST /api/party/create {name, path?, title?, cover?}      -> {code, token, member}
//   POST /api/party/join   {code, name}                        -> {code, member}
//   GET  /api/party/state?code=&m=&ready=&since=               -> the party (short when nothing changed)
//   POST /api/party/sync   {code, token, playing, position, event}   host: where playback is
//   POST /api/party/item   {code, token, path, title, cover}   host: play this instead
//   POST /api/party/vote   {code, token, action, mode}         host: open / close / cancel
//   POST /api/party/pick   {code, m, path, title, cover} | {code, m, choice}   a vote
//   POST /api/party/leave  {code, m}
//   POST /api/party/end    {code, token}
//
// No accounts: a member is a random id the page keeps, the host is whoever holds the token.
// The store lives in PSRAM (titles, covers and picks add up to ~18 KB) behind one mutex.
#ifndef NOMAD_PARTY_H
#define NOMAD_PARTY_H
#include <Arduino.h>
#include "src/AsyncWebServer/ESPAsyncWebServer.h"   // carried here, with Nomad's limits (src/AsyncWebServer/README.md)
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

static const int PARTY_MAX = 4;
static const int PARTY_MEMBERS = 10;          // the host and nine more
static const int PARTY_PATH = 192;
static const int PARTY_TITLE = 72;
static const int PARTY_NAME = 20;
static const int PARTY_RUNOFF = 3;
static const uint32_t PARTY_MEMBER_TTL = 25000;          // a phone not heard from in this long has left
static const uint32_t PARTY_HOST_AWAY = 20000;           // viewers see "waiting for the host"
static const uint32_t PARTY_IDLE = 10UL * 60 * 1000;     // no host for this long: the party ends

enum { VOTE_NONE = 0, VOTE_OPEN = 1, VOTE_RUNOFF = 2, VOTE_DONE = 3 };
enum { MODE_MOST = 0, MODE_RANDOM = 1, MODE_RUNOFF = 2 };
static const char *const VOTE_MODES[] = { "most", "random", "runoff" };

struct PartyThing { char path[PARTY_PATH]; char title[PARTY_TITLE]; char cover[PARTY_PATH]; };
struct PartyMember {
  uint32_t id;               // 0: free seat
  char name[PARTY_NAME];
  uint32_t seen;
  bool ready;
  int8_t vote;               // index into cand, -1 none
  int8_t vote2;              // index into runoff, -1 none
};
struct Party {
  bool active;
  char code[5];
  char token[17];
  PartyThing item;           // what plays, path empty: nothing yet
  bool playing;
  double position;
  uint32_t updMs;            // when position was true
  uint32_t hostMs;           // last word from the host
  uint32_t seq;
  bool ended;
  uint8_t phase, mode;
  PartyThing cand[PARTY_MEMBERS];
  uint8_t ncand;
  int8_t runoff[PARTY_RUNOFF];
  uint8_t nrunoff;
  int8_t winner;             // index into cand once decided
  PartyMember m[PARTY_MEMBERS];   // m[0] is the host
  uint32_t hostIp;           // the address that started it
};
static Party *g_party = nullptr;
static SemaphoreHandle_t g_partyMx = nullptr;

struct PartyLock {
  bool ok;
  PartyLock() { ok = g_partyMx && xSemaphoreTake(g_partyMx, pdMS_TO_TICKS(300)) == pdTRUE; }
  ~PartyLock() { if (ok) xSemaphoreGive(g_partyMx); }
};

// small helpers

static void partyCopy(char *dst, size_t n, const char *src) {
  // printable text only, trimmed to fit (whole UTF-8 characters, never half of one)
  size_t o = 0;
  for (const unsigned char *s = (const unsigned char *)(src ? src : ""); *s && o + 1 < n; ) {
    size_t len = *s < 0x80 ? 1 : (*s >> 5) == 6 ? 2 : (*s >> 4) == 14 ? 3 : (*s >> 3) == 30 ? 4 : 1;
    if (o + len >= n) break;
    if (*s < 0x20 || *s == 0x7f) { s++; continue; }
    size_t k = 0;
    for (; k < len && s[k]; k++) dst[o++] = s[k];
    s += k;   // a character cut short by the end of the text: stop at the end, not past it
  }
  dst[o] = 0;
  while (o && dst[o - 1] == ' ') dst[--o] = 0;
}

static bool partyMediaPath(const char *p, bool image) {
  if (!p || !*p) return false;
  const size_t n = strlen(p);
  if (n >= PARTY_PATH) return false;
  if (strncmp(p, "/Movies/", 8) != 0 && strncmp(p, "/Shows/", 7) != 0) return false;
  if (strstr(p, "..") || strstr(p, "//") || strchr(p, '\\')) return false;
  const char *dot = strrchr(p, '.');
  if (!dot) return false;
  String e(dot); e.toLowerCase();
  if (image) return e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".webp" || e == ".avif";
  return e == ".mp4" || e == ".m4v" || e == ".mkv" || e == ".webm" || e == ".mov";
}

static bool partyThing(PartyThing &t, JsonVariantConst b) {
  const char *path = b["path"] | "";
  const char *cover = b["cover"] | "";
  if (!partyMediaPath(path, false)) return false;
  memset(&t, 0, sizeof(t));
  strlcpy(t.path, path, sizeof(t.path));
  partyCopy(t.title, sizeof(t.title), b["title"] | "");
  if (!t.title[0]) {
    const char *slash = strrchr(path, '/');
    partyCopy(t.title, sizeof(t.title), slash ? slash + 1 : path);
  }
  if (partyMediaPath(cover, true)) strlcpy(t.cover, cover, sizeof(t.cover));
  return true;
}

static Party *partyFind(const char *code) {
  if (!g_party || !code) return nullptr;
  char c[5] = {0};
  for (int i = 0; i < 4 && code[i]; i++) c[i] = toupper((unsigned char)code[i]);
  for (int i = 0; i < PARTY_MAX; i++)
    if (g_party[i].active && strcmp(g_party[i].code, c) == 0) return &g_party[i];
  return nullptr;
}

static PartyMember *partyMember(Party &p, uint32_t id) {
  if (!id) return nullptr;
  for (auto &m : p.m) if (m.id == id) return &m;
  return nullptr;
}

static void partyReap() {
  const uint32_t now = millis();
  for (int i = 0; i < PARTY_MAX; i++) {
    Party &p = g_party[i];
    if (!p.active) continue;
    if (now - p.hostMs > PARTY_IDLE || (p.ended && now - p.hostMs > 60000)) { p.active = false; continue; }
    for (int k = 1; k < PARTY_MEMBERS; k++) {
      if (p.m[k].id && now - p.m[k].seen > PARTY_MEMBER_TTL) { memset(&p.m[k], 0, sizeof(PartyMember)); p.m[k].vote = p.m[k].vote2 = -1; p.seq++; }
    }
  }
}

static uint32_t partyId() { uint32_t v; do { v = esp_random(); } while (!v); return v; }

static void partyResetVote(Party &p) {
  p.phase = VOTE_NONE; p.ncand = 0; p.nrunoff = 0; p.winner = -1;
  for (auto &m : p.m) { m.vote = -1; m.vote2 = -1; }
}

// votes per candidate in this round
static void partyTally(Party &p, int out[PARTY_MEMBERS]) {
  for (int i = 0; i < PARTY_MEMBERS; i++) out[i] = 0;
  for (auto &m : p.m) {
    if (!m.id) continue;
    if (p.phase == VOTE_RUNOFF) { if (m.vote2 >= 0 && m.vote2 < p.nrunoff) out[p.runoff[m.vote2]]++; }
    else if (m.vote >= 0 && m.vote < p.ncand) out[m.vote]++;
  }
}

// the host closed the vote: the winner (or the second round). Returns false when nobody picked
static bool partyDecide(Party &p) {
  int t[PARTY_MEMBERS];
  partyTally(p, t);
  int total = 0, best = 0;
  for (int i = 0; i < p.ncand; i++) { total += t[i]; if (t[i] > best) best = t[i]; }
  if (!total) return false;
  if (p.phase == VOTE_OPEN && p.mode == MODE_RUNOFF) {
    // the leaders go through to a second round (up to three, ties included)
    int order[PARTY_MEMBERS], n = 0;
    for (int i = 0; i < p.ncand; i++) if (t[i]) order[n++] = i;
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (t[order[b]] > t[order[a]]) { int x = order[a]; order[a] = order[b]; order[b] = x; }
    if (n >= 2) {
      p.nrunoff = 0;
      for (int i = 0; i < n && p.nrunoff < PARTY_RUNOFF; i++) p.runoff[p.nrunoff++] = order[i];
      for (auto &m : p.m) m.vote2 = -1;
      p.phase = VOTE_RUNOFF;
      return true;
    }
  }
  int win = -1;
  if (p.mode == MODE_RANDOM && p.phase == VOTE_OPEN) {
    // every pick is a ticket: more votes, better odds
    int r = esp_random() % total;
    for (int i = 0; i < p.ncand; i++) { r -= t[i]; if (r < 0) { win = i; break; } }
  } else {
    int ties[PARTY_MEMBERS], n = 0;
    for (int i = 0; i < p.ncand; i++) if (t[i] == best) ties[n++] = i;
    win = ties[esp_random() % n];
  }
  p.winner = win;
  p.phase = VOTE_DONE;
  p.item = p.cand[win];
  p.playing = false; p.position = 0; p.updMs = millis();
  for (auto &m : p.m) m.ready = false;
  return true;
}

// answers

static void partyThingJson(JsonObject o, const PartyThing &t) {
  o["path"] = t.path; o["title"] = t.title;
  if (t.cover[0]) o["cover"] = t.cover;
}

static String partyStateJson(Party &p, uint32_t me, bool full) {
  const uint32_t now = millis();
  JsonDocument d;
  d["seq"] = p.seq;
  d["playing"] = p.playing;
  d["position"] = p.position;
  d["age"] = now - p.updMs;
  d["hostAway"] = now - p.hostMs > PARTY_HOST_AWAY;
  if (p.ended) d["ended"] = true;
  if (full) {
    d["code"] = p.code;
    if (p.item.path[0]) partyThingJson(d["item"].to<JsonObject>(), p.item);
    JsonArray ms = d["members"].to<JsonArray>();
    int watching = 0, ready = 0;
    for (int i = 0; i < PARTY_MEMBERS; i++) {
      const PartyMember &m = p.m[i];
      if (!m.id) continue;
      JsonObject o = ms.add<JsonObject>();
      o["name"] = m.name; o["ready"] = m.ready;
      if (i == 0) o["host"] = true;
      if (m.id == me) o["me"] = true;
      watching++; ready += m.ready;
    }
    d["watching"] = watching; d["ready"] = ready;
    JsonObject v = d["vote"].to<JsonObject>();
    static const char *const PH[] = { "none", "open", "runoff", "done" };
    v["phase"] = PH[p.phase];
    v["mode"] = VOTE_MODES[p.mode];
    if (p.phase != VOTE_NONE) {
      int t[PARTY_MEMBERS];
      partyTally(p, t);
      // round one: every pick with its votes and who, round two: the finalists
      JsonArray c = v["picks"].to<JsonArray>();
      const PartyMember *mine = partyMember(p, me);
      if (p.phase == VOTE_RUNOFF || (p.phase == VOTE_DONE && p.nrunoff)) {
        for (int r = 0; r < p.nrunoff; r++) {
          const int ci = p.runoff[r];
          JsonObject o = c.add<JsonObject>();
          partyThingJson(o, p.cand[ci]);
          o["votes"] = t[ci]; o["choice"] = r;
          JsonArray who = o["who"].to<JsonArray>();
          for (auto &m : p.m) if (m.id && m.vote2 == r) who.add(m.name);
          if (mine && mine->vote2 == r) o["mine"] = true;
        }
      } else {
        for (int i = 0; i < p.ncand; i++) {
          if (!t[i]) continue;   // a pick everyone moved away from
          JsonObject o = c.add<JsonObject>();
          partyThingJson(o, p.cand[i]);
          o["votes"] = t[i];
          JsonArray who = o["who"].to<JsonArray>();
          for (auto &m : p.m) if (m.id && m.vote == i) who.add(m.name);
          if (mine && mine->vote == i) o["mine"] = true;
        }
      }
      if (p.phase == VOTE_DONE && p.winner >= 0) partyThingJson(v["winner"].to<JsonObject>(), p.cand[p.winner]);
    }
  }
  String out;
  serializeJson(d, out);
  return out;
}

static void partySend(AsyncWebServerRequest *r, int code, const String &body) {
  AsyncWebServerResponse *res = r->beginResponse(code, "application/json", body);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}
static void partyErr(AsyncWebServerRequest *r, int code, const char *msg) {
  partySend(r, code, String("{\"error\":\"") + msg + "\"}");
}

// a small JSON body, in one piece (they all are)
typedef void (*PartyBodyFn)(AsyncWebServerRequest *, JsonDocument &);
static void partyBody(AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total, PartyBodyFn fn) {
  if (index != 0 || len != total || total > 1024) { partyErr(r, 413, "Too large"); return; }
  JsonDocument b;
  if (deserializeJson(b, data, len)) { partyErr(r, 400, "Invalid JSON"); return; }
  if (!g_party) { partyErr(r, 503, "Watch together is not available"); return; }
  fn(r, b);
}

#define PARTY_HOST_OR_FAIL(p, b) \
  Party *p = partyFind(b["code"] | ""); \
  if (!p) { partyErr(r, 404, "No party with that code"); return; } \
  if (strcmp(p->token, b["token"] | "") != 0) { partyErr(r, 403, "Only the host can do that"); return; }

static void partyCreate(AsyncWebServerRequest *r, JsonDocument &b) {
  PartyLock lk;
  if (!lk.ok) { partyErr(r, 503, "busy"); return; }
  partyReap();
  // One party per phone: starting another ends the one it was hosting, so one phone cannot
  // hold all four (each lasts 10 minutes from its host's last word).
  const uint32_t ip = r->client() ? (uint32_t)r->client()->remoteIP() : 0;
#if NOMAD_BENCH
  const uint32_t ipKey = r->hasParam("benchip") ? (uint32_t)r->getParam("benchip")->value().toInt() : ip;
#else
  const uint32_t ipKey = ip;
#endif
  for (int i = 0; i < PARTY_MAX; i++) {
    if (g_party[i].active && ipKey && g_party[i].hostIp == ipKey) {
      webLogf("info", "Watch together: party %s ended, its host started another", g_party[i].code);
      g_party[i].active = false;
    }
  }
  Party *p = nullptr;
  for (int i = 0; i < PARTY_MAX; i++) if (!g_party[i].active) { p = &g_party[i]; break; }
  if (!p) { partyErr(r, 503, "Every watch party slot is in use. Try again when one ends."); return; }
  memset(p, 0, sizeof(Party));
  for (auto &m : p->m) { m.vote = -1; m.vote2 = -1; }
  if (b["path"].is<const char *>() && !partyThing(p->item, b.as<JsonVariantConst>())) { partyErr(r, 400, "That isn't a film or episode on the card"); return; }
  static const char *A = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  do { for (int i = 0; i < 4; i++) p->code[i] = A[esp_random() % 32]; p->code[4] = 0; } while (partyFind(p->code));
  for (int i = 0; i < 16; i++) p->token[i] = "0123456789abcdef"[esp_random() % 16];
  p->m[0].id = partyId();
  partyCopy(p->m[0].name, PARTY_NAME, b["name"] | "Host");
  if (!p->m[0].name[0]) strcpy(p->m[0].name, "Host");
  p->m[0].seen = p->hostMs = p->updMs = millis();
  p->winner = -1;
  p->seq = 1;
  p->hostIp = ipKey;
  p->active = true;
  JsonDocument o;
  o["code"] = p->code; o["token"] = p->token; o["member"] = p->m[0].id;
  String out; serializeJson(o, out);
  partySend(r, 200, out);
  webLogf("info", "Watch together: party %s started", p->code);
}

static void partyJoin(AsyncWebServerRequest *r, JsonDocument &b) {
  PartyLock lk;
  if (!lk.ok) { partyErr(r, 503, "busy"); return; }
  partyReap();
  Party *p = partyFind(b["code"] | "");
  if (!p) { partyErr(r, 404, "No party with that code"); return; }
  if (p->ended) { partyErr(r, 410, "That party has ended"); return; }
  PartyMember *seat = nullptr;
  for (int i = 1; i < PARTY_MEMBERS; i++) if (!p->m[i].id) { seat = &p->m[i]; break; }
  if (!seat) { partyErr(r, 409, "That party is full"); return; }
  seat->id = partyId();
  partyCopy(seat->name, PARTY_NAME, b["name"] | "");
  if (!seat->name[0]) snprintf(seat->name, PARTY_NAME, "Guest %d", (int)(seat - p->m));
  seat->seen = millis();
  seat->vote = seat->vote2 = -1;
  p->seq++;
  JsonDocument o;
  o["code"] = p->code; o["member"] = seat->id;
  String out; serializeJson(o, out);
  partySend(r, 200, out);
}

static void partyStateRoute(AsyncWebServerRequest *r) {
  if (!g_party) { partyErr(r, 503, "Watch together is not available"); return; }
  auto q = [&](const char *n) { return r->hasParam(n) ? r->getParam(n)->value() : String(); };
  PartyLock lk;
  if (!lk.ok) { partyErr(r, 503, "busy"); return; }
  partyReap();
  Party *p = partyFind(q("code").c_str());
  if (!p) { partyErr(r, 404, "No party with that code"); return; }
  const uint32_t me = strtoul(q("m").c_str(), nullptr, 10);
  PartyMember *m = partyMember(*p, me);
  if (m) {
    m->seen = millis();
    if (m == &p->m[0]) p->hostMs = m->seen;
    const bool ready = q("ready") == "1";
    if (ready != m->ready) { m->ready = ready; p->seq++; }
  }
  const bool full = q("since").length() == 0 || strtoul(q("since").c_str(), nullptr, 10) != p->seq;
  partySend(r, 200, partyStateJson(*p, me, full));
}

static void partySync(AsyncWebServerRequest *r, JsonDocument &b) {
  PartyLock lk;
  if (!lk.ok) { partyErr(r, 503, "busy"); return; }
  PARTY_HOST_OR_FAIL(p, b);
  const bool playing = b["playing"] | false;
  double pos = b["position"] | 0.0;
  if (!(pos >= 0) || pos > 360000) pos = 0;
  // a play, pause or seek is news for everyone, a heartbeat only refreshes the clock
  if (playing != p->playing || (b["event"] | false)) p->seq++;
  p->playing = playing; p->position = pos;
  p->updMs = p->hostMs = p->m[0].seen = millis();
  partySend(r, 200, String("{\"seq\":") + p->seq + "}");
}

static void partyItem(AsyncWebServerRequest *r, JsonDocument &b) {
  PartyLock lk;
  if (!lk.ok) { partyErr(r, 503, "busy"); return; }
  PARTY_HOST_OR_FAIL(p, b);
  PartyThing t;
  if (!partyThing(t, b.as<JsonVariantConst>())) { partyErr(r, 400, "That isn't a film or episode on the card"); return; }
  p->item = t;
  p->playing = false; p->position = 0; p->updMs = p->hostMs = millis();
  if (p->phase != VOTE_NONE) partyResetVote(*p);
  for (auto &m : p->m) m.ready = false;
  p->seq++;
  partySend(r, 200, String("{\"seq\":") + p->seq + "}");
}

static void partyVote(AsyncWebServerRequest *r, JsonDocument &b) {
  PartyLock lk;
  if (!lk.ok) { partyErr(r, 503, "busy"); return; }
  PARTY_HOST_OR_FAIL(p, b);
  const String action = b["action"] | "";
  p->hostMs = millis();
  if (action == "open") {
    const String mode = b["mode"] | "most";
    partyResetVote(*p);
    p->mode = mode == "random" ? MODE_RANDOM : mode == "runoff" ? MODE_RUNOFF : MODE_MOST;
    p->phase = VOTE_OPEN;
  } else if (action == "close") {
    if (p->phase != VOTE_OPEN && p->phase != VOTE_RUNOFF) { partyErr(r, 409, "No vote is open"); return; }
    if (!partyDecide(*p)) { partyErr(r, 409, "Nobody has picked anything yet"); return; }
  } else if (action == "cancel") {
    partyResetVote(*p);
  } else { partyErr(r, 400, "Unknown action"); return; }
  p->seq++;
  partySend(r, 200, partyStateJson(*p, p->m[0].id, true));
}

static void partyPick(AsyncWebServerRequest *r, JsonDocument &b) {
  PartyLock lk;
  if (!lk.ok) { partyErr(r, 503, "busy"); return; }
  Party *p = partyFind(b["code"] | "");
  if (!p) { partyErr(r, 404, "No party with that code"); return; }
  PartyMember *m = partyMember(*p, b["m"] | 0u);
  if (!m) { partyErr(r, 403, "Join the party first"); return; }
  m->seen = millis();
  if (p->phase == VOTE_RUNOFF) {
    const int c = b["choice"] | -1;
    if (c < 0 || c >= p->nrunoff) { partyErr(r, 400, "Pick one of the finalists"); return; }
    m->vote2 = c;
  } else if (p->phase == VOTE_OPEN) {
    PartyThing t;
    if (!partyThing(t, b.as<JsonVariantConst>())) { partyErr(r, 400, "That isn't a film or episode on the card"); return; }
    int at = -1;
    for (int i = 0; i < p->ncand; i++) if (strcmp(p->cand[i].path, t.path) == 0) { at = i; break; }
    if (at < 0) {
      // reuse a pick nobody votes for any more before taking a new place
      int t2[PARTY_MEMBERS]; partyTally(*p, t2);
      for (int i = 0; i < p->ncand && at < 0; i++) if (!t2[i] && i != m->vote) at = i;
      if (at < 0) { if (p->ncand >= PARTY_MEMBERS) { partyErr(r, 409, "Too many picks"); return; } at = p->ncand++; }
      p->cand[at] = t;
    }
    m->vote = at;
  } else { partyErr(r, 409, "There's no vote open"); return; }
  p->seq++;
  partySend(r, 200, String("{\"seq\":") + p->seq + "}");
}

static void partyLeave(AsyncWebServerRequest *r, JsonDocument &b) {
  PartyLock lk;
  if (!lk.ok) { partyErr(r, 503, "busy"); return; }
  Party *p = partyFind(b["code"] | "");
  if (p) {
    PartyMember *m = partyMember(*p, b["m"] | 0u);
    if (m && m != &p->m[0]) { memset(m, 0, sizeof(PartyMember)); m->vote = m->vote2 = -1; p->seq++; }
  }
  partySend(r, 200, "{\"ok\":true}");
}

static void partyEnd(AsyncWebServerRequest *r, JsonDocument &b) {
  PartyLock lk;
  if (!lk.ok) { partyErr(r, 503, "busy"); return; }
  PARTY_HOST_OR_FAIL(p, b);
  p->ended = true; p->playing = false; p->seq++;
  partySend(r, 200, "{\"ok\":true}");
  webLogf("info", "Watch together: party %s ended", p->code);
}

static void nomadPartyRegister(AsyncWebServer &server) {
  g_party = (Party *)heap_caps_calloc(PARTY_MAX, sizeof(Party), MALLOC_CAP_SPIRAM);
  g_partyMx = xSemaphoreCreateMutex();
  auto post = [&](const char *url, PartyBodyFn fn) {
    server.on(url, HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr,
      [fn](AsyncWebServerRequest *r, uint8_t *d, size_t l, size_t i, size_t t) { partyBody(r, d, l, i, t, fn); });
  };
  post("/api/party/create", partyCreate);
  post("/api/party/join", partyJoin);
  post("/api/party/sync", partySync);
  post("/api/party/item", partyItem);
  post("/api/party/vote", partyVote);
  post("/api/party/pick", partyPick);
  post("/api/party/leave", partyLeave);
  post("/api/party/end", partyEnd);
  server.on("/api/party/state", HTTP_GET, partyStateRoute);
}

#endif  // NOMAD_PARTY_H
