#pragma once
// NomadGuardWeb: NomadGuard's rules applied to the web server.
//
// Identical on every firmware line, like NomadGuard.h. The sketch calls
// installNomadGuard() once, before server.begin(), with three hooks into its own state.
//
// It runs as server middleware so no route gets missed. ESPAsyncWebServer calls it for
// every request (registered routes, serveStatic mounts and the onNotFound catch-all)
// after the request is parsed and before its handler runs. Upload bodies are the
// exception: their chunks reach the handler as they arrive, before any middleware, so
// upload handlers call nomadUploadAllowed() on their first chunk.

#include "src/AsyncWebServer/ESPAsyncWebServer.h"   // carried here, with Nomad's limits (src/AsyncWebServer/README.md)
#include "NomadGuard.h"

struct NomadGuardHooks {
  bool (*isAdmin)(AsyncWebServerRequest *request);   // checkAdminAuth
  bool (*guestWritesOff)(AsyncWebServerRequest *request, const String &path);   // uploads off here, for this caller
  bool (*staMode)();                                // joined to a home network
};

static NomadGuardHooks g_nomadGuardHooks = { nullptr, nullptr, nullptr };

static inline bool nomadGuardIsAdmin(AsyncWebServerRequest *r) {
  return g_nomadGuardHooks.isAdmin ? g_nomadGuardHooks.isAdmin(r) : false;
}
static inline bool nomadGuardGuestWritesOff(AsyncWebServerRequest *r, const String &path) {
  return g_nomadGuardHooks.guestWritesOff ? g_nomadGuardHooks.guestWritesOff(r, path) : false;
}
static inline bool nomadGuardStaMode() {
  return g_nomadGuardHooks.staMode ? g_nomadGuardHooks.staMode() : false;
}

static inline void nomadGuardSend(AsyncWebServerRequest *r, int code, const char *msg) {
  AsyncWebServerResponse *res =
      r->beginResponse(code, "application/json", String("{\"error\":\"") + msg + "\"}");
  if (!res) { r->send(code); return; }
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

// Every parameter name a handler in any firmware line opens a path from.
static const char *const kNomadPathParams[] = {
  "path", "dir", "file", "filename", "oldname", "newname", "dirname",
};

static inline bool nomadIsPathParam(const String &name) {
  for (const char *p : kNomadPathParams) if (name == p) return true;
  return false;
}

// Form routes whose path parameters name something to create, replace, move or delete.
struct NomadWriteRoute { const char *url; const char *p1; const char *p2; };
static const NomadWriteRoute kNomadWriteRoutes[] = {
  { "/delete", "filename", nullptr   },
  { "/rename", "oldname",  "newname" },
  { "/mkdir",  "dirname",  nullptr   },
  { "/save",   "filename", nullptr   },
  { "/upload-cancel", "path", nullptr },
};

// A write the browser was made to send by some other site. A form POST is a "simple"
// request with no preflight, so in WiFi Mode any page open on a laptop on the same
// network could otherwise empty the card. Browsers mark these: Sec-Fetch-Site says
// cross-site, or Origin names a different host than the request. Pages served from here
// pass, including through a captive-portal hostname (Origin and Host are both that name).
static inline bool nomadCrossSiteWrite(AsyncWebServerRequest *r) {
  const WebRequestMethodComposite m = r->method();
  if (m == HTTP_GET || m == HTTP_HEAD || m == HTTP_OPTIONS) return false;
  if (r->hasHeader("Sec-Fetch-Site") && r->header("Sec-Fetch-Site").equalsIgnoreCase("cross-site"))
    return true;
  if (!r->hasHeader("Origin")) return false;
  const String &o = r->header("Origin");
  // An opaque origin comes from a sandboxed frame or a file:// page. On the hotspot
  // nothing outside can serve one. On a home network anything can.
  if (o == "null") return nomadGuardStaMode();
  const int s = o.indexOf("://");
  String host = s >= 0 ? o.substring(s + 3) : o;
  const int slash = host.indexOf('/');
  if (slash >= 0) host = host.substring(0, slash);
  return !host.equalsIgnoreCase(r->host());
}

static inline String nomadParamValue(AsyncWebServerRequest *r, const char *name) {
  if (r->hasParam(name, true)) return r->getParam(name, true)->value();
  if (r->hasParam(name)) return r->getParam(name)->value();
  return String();
}

static inline void nomadGuardMiddleware(AsyncWebServerRequest *request, ArMiddlewareNext next) {
  // 1. /config never leaves the device through a file route, in any spelling, as the
  //    URL itself or as any parameter a handler will open.
  if (nomadReadBlocked(request->url())) { nomadGuardSend(request, 404, "Not found"); return; }
  const size_t n = request->params();
  for (size_t i = 0; i < n; ++i) {
    const AsyncWebParameter *p = request->getParam(i);
    if (!p || p->isFile() || !nomadIsPathParam(p->name())) continue;
    if (nomadReadBlocked(p->value())) { nomadGuardSend(request, 404, "Not found"); return; }
  }

  // 2. No writes on another site's behalf.
  if (nomadCrossSiteWrite(request)) {
    nomadGuardSend(request, 403, "Cross-site request refused");
    return;
  }

  // 3. Form writes: decide on every path the request names. Matched the way the
  //    library routes: a handler on "/delete" also takes "/delete/anything".
  if (request->method() == HTTP_POST) {
    const String &url = request->url();
    for (const NomadWriteRoute &w : kNomadWriteRoutes) {
      if (!nomadRouteMatches(url, w.url)) continue;
      const bool admin = nomadGuardIsAdmin(request);
      for (const char *pn : { w.p1, w.p2 }) {
        if (!pn) continue;
        const String v = nomadParamValue(request, pn);
        if (v.length() == 0) continue;            // the handler answers a missing parameter
        const NomadWriteVerdict verdict = nomadWriteVerdict(v, admin, nomadGuardGuestWritesOff(request, v));
        if (verdict != NW_OK) {
          nomadGuardSend(request, nomadWriteStatus(verdict), nomadWriteMessage(verdict));
          return;
        }
      }
      break;
    }
  }

  next();
}

// For upload handlers, on the first chunk of each file: may this request create
// fullPath? Sends the refusal itself and returns false if not. Uploads also have to
// pass the cross-site rule here, since their chunks are written before middleware runs.
static inline bool nomadUploadAllowed(AsyncWebServerRequest *request, const String &fullPath) {
  if (nomadCrossSiteWrite(request)) {
    nomadGuardSend(request, 403, "Cross-site request refused");
    return false;
  }
  const NomadWriteVerdict v =
      nomadWriteVerdict(fullPath, nomadGuardIsAdmin(request), nomadGuardGuestWritesOff(request, fullPath));
  if (v == NW_OK) return true;
  nomadGuardSend(request, nomadWriteStatus(v), nomadWriteMessage(v));
  return false;
}

// ---------------------------------------------------------------------------------
// Admin login throttle. /auth/login takes the password hash, so guesses cost the
// attacker one request each. After five wrong answers in a row that phone waits 30 s,
// doubling to a five-minute ceiling, and a right answer resets it.
// Counted per phone (its address on the hotspot), so a guest guessing wrong can't lock the
// owner out. A cap across all addresses (20 wrong in a minute, then a minute's wait for
// everyone) stops a guesser who keeps changing address.
struct NomadLoginSlot { uint32_t ip; uint8_t fails; uint32_t lockedUntil; uint32_t lastMs; };
static NomadLoginSlot g_nomadLoginSlots[8] = {};
static uint32_t g_nomadLoginBurstAt = 0, g_nomadLoginAllLockedUntil = 0;
static uint8_t g_nomadLoginBurst = 0;

static inline uint32_t nomadLoginLeft(uint32_t &until) {
  if (until == 0) return 0;
  const int32_t left = (int32_t)(until - millis());
  if (left <= 0) { until = 0; return 0; }
  return (uint32_t)left;
}
static inline NomadLoginSlot *nomadLoginSlot(uint32_t ip, bool make) {
  NomadLoginSlot *pick = nullptr;
  for (auto &x : g_nomadLoginSlots) {
    if (x.fails && x.ip == ip) return &x;
    // for an address not seen yet: a free row, else the row untouched longest
    if (!pick || (pick->fails && (!x.fails || (int32_t)(x.lastMs - pick->lastMs) < 0))) pick = &x;
  }
  if (!make) return nullptr;
  *pick = NomadLoginSlot{ ip, 0, 0, millis() };
  return pick;
}

// Milliseconds this address has left to wait, 0 if a guess may be checked now.
static inline uint32_t nomadLoginWaitMs(uint32_t ip) {
  const uint32_t all = nomadLoginLeft(g_nomadLoginAllLockedUntil);
  NomadLoginSlot *x = nomadLoginSlot(ip, false);
  const uint32_t own = x ? nomadLoginLeft(x->lockedUntil) : 0;
  return own > all ? own : all;
}

static inline void nomadLoginResult(uint32_t ip, bool ok) {
  const uint32_t now = millis();
  if (ok) {
    NomadLoginSlot *x = nomadLoginSlot(ip, false);
    if (x) *x = NomadLoginSlot{};
    return;
  }
  NomadLoginSlot *x = nomadLoginSlot(ip, true);
  x->lastMs = now;
  if (x->fails < 255) x->fails++;
  if (x->fails >= 5) {
    uint32_t ms = 30000UL << (x->fails - 5 > 4 ? 4 : x->fails - 5);
    if (ms > 300000UL) ms = 300000UL;
    x->lockedUntil = (now + ms) | 1;
  }
  if (now - g_nomadLoginBurstAt > 60000UL) { g_nomadLoginBurstAt = now; g_nomadLoginBurst = 0; }
  if (++g_nomadLoginBurst >= 20) { g_nomadLoginAllLockedUntil = (now + 60000UL) | 1; g_nomadLoginBurst = 0; }
}

// ---------------------------------------------------------------------------------

static inline void installNomadGuard(AsyncWebServer &server, const NomadGuardHooks &hooks) {
  g_nomadGuardHooks = hooks;
  // A file served as text/plain must not be sniffed into HTML, and no page here
  // should be framed by anything but another page here.
  DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
  DefaultHeaders::Instance().addHeader("X-Frame-Options", "SAMEORIGIN");
  server.addMiddleware(nomadGuardMiddleware);
}
