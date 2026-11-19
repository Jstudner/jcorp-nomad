// NomadAuth.h - the admin password and who is signed in.
//
// The card holds  v2$<iterations>$<salt hex>$<key hex>,  key = PBKDF2-HMAC-SHA256 of
// the password. Signing in:
//   GET  /auth/challenge  {"mode":"none"|"legacy"|"v2","salt","iter","nonce"}
//   POST /auth/login      body={"nonce","proof","remember"}, proof = HMAC-SHA256(key, nonce)
// so the password itself never leaves the phone and a proof can't be replayed (each nonce
// is good once, for a minute). A password stored the old way (as typed, or the SHA-256 hex
// an older page sent) is taken once as {"password"} and turned into a v2 record on the spot.
// Up to AUTH_SESSIONS devices stay signed in at once. A session not kept ("Keep me signed
// in" off) ends after AUTH_IDLE_MS without use. A kept one lasts until it is signed out or
// the password changes, and is remembered across restarts in /config/sessions.json (only
// SHA-256 hashes of the tokens, so the file can't be used to sign in).
//   POST /auth/logout     body={"all":true} signs every device out
//   GET  /auth/sessions   {"signedIn":n,"kept":n}               (admin)
//   POST /auth/password   body={"salt","iter","key"} | {"password"} | {"disable":true}  (admin)
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "src/AsyncWebServer/ESPAsyncWebServer.h"   // carried here, with Nomad's limits (src/AsyncWebServer/README.md)
#include <esp_random.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/sha256.h>

#define AUTH_ITER 10000
#define AUTH_SESSIONS 8
#define AUTH_IDLE_MS (60UL * 60UL * 1000UL)
#define AUTH_NONCES 8
#define AUTH_NONCE_MS 60000UL
static const char *const AUTH_SESSIONS_PATH = "/config/sessions.json";

struct AuthSession { uint8_t hash[32]; uint32_t lastMs; bool remember; bool used; };
static AuthSession g_authSessions[AUTH_SESSIONS];
struct AuthNonce { uint8_t n[16]; uint32_t at; bool used; };
static AuthNonce g_authNonces[AUTH_NONCES];
static portMUX_TYPE g_authMux = portMUX_INITIALIZER_UNLOCKED;   // short critical sections only

static String authHex(const uint8_t *b, size_t n) {
  static const char *H = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { s += H[b[i] >> 4]; s += H[b[i] & 15]; }
  return s;
}
static bool authUnhex(const String &s, uint8_t *out, size_t n) {
  if (s.length() != n * 2) return false;
  for (size_t i = 0; i < n; ++i) {
    int v = 0;
    for (int k = 0; k < 2; ++k) {
      const char c = s.charAt(i * 2 + k);
      const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0) return false;
      v = v * 16 + d;
    }
    out[i] = (uint8_t)v;
  }
  return true;
}
static bool authSame(const uint8_t *a, const uint8_t *b, size_t n) {   // constant time
  uint8_t d = 0;
  for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i];
  return d == 0;
}
static void authSha(const uint8_t *in, size_t n, uint8_t out[32]) { mbedtls_sha256(in, n, out, 0); }
static void authRandom(uint8_t *out, size_t n) { esp_fill_random(out, n); }

struct AuthRecord { uint32_t iter; uint8_t salt[16]; uint8_t key[32]; };
// The most rounds a new password may be stored with (the page uses 10,000). A typed password
// is checked on the network task at the stored count, and a big count stalls every phone.
static const uint32_t AUTH_MAX_NEW_ITER = 20000;
static bool authParse(const String &stored, AuthRecord &r) {
  if (!stored.startsWith("v2$")) return false;
  const int a = stored.indexOf('$', 3), b = a > 0 ? stored.indexOf('$', a + 1) : -1;
  if (a < 0 || b < 0) return false;
  r.iter = (uint32_t)strtoul(stored.substring(3, a).c_str(), nullptr, 10);
  return r.iter >= 1000 && r.iter <= 200000 && authUnhex(stored.substring(a + 1, b), r.salt, 16) &&
         authUnhex(stored.substring(b + 1), r.key, 32);
}
static String authRecordString(uint32_t iter, const uint8_t salt[16], const uint8_t key[32]) {
  return "v2$" + String(iter) + "$" + authHex(salt, 16) + "$" + authHex(key, 32);
}
static void authDerive(const String &pw, const uint8_t *salt, uint32_t iter, uint8_t out[32]) {
  mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw.c_str(), pw.length(), salt, 16, iter, 32, out);
}

bool isAdminAuthRequired() {
  return settings.adminPassword.length() > 0 && settings.adminPassword != "null";
}
static const char *authMode() {
  if (!isAdminAuthRequired()) return "none";
  AuthRecord r;
  return authParse(settings.adminPassword, r) ? "v2" : "legacy";
}

// remembered sessions on the card
static void authSaveKept() {
  JsonDocument d;
  d["v"] = 1;
  JsonArray a = d["s"].to<JsonArray>();
  // copied under the lock, turned into text after it (no allocating inside a critical section)
  uint8_t hashes[AUTH_SESSIONS][32];
  int n = 0;
  portENTER_CRITICAL(&g_authMux);
  for (auto &s : g_authSessions) if (s.used && s.remember) memcpy(hashes[n++], s.hash, 32);
  portEXIT_CRITICAL(&g_authMux);
  for (int i = 0; i < n; ++i) a.add(authHex(hashes[i], 32));
  String json;
  serializeJson(d, json);
  queueConfigWrite(AUTH_SESSIONS_PATH, json);
}
void nomadAuthLoad() {
  File f = SD_MMC.open(AUTH_SESSIONS_PATH, FILE_READ);
  if (!f) return;
  JsonDocument d;
  const bool ok = !deserializeJson(d, f);
  f.close();
  if (!ok || !isAdminAuthRequired()) return;
  int i = 0;
  for (JsonVariant v : d["s"].as<JsonArray>()) {
    if (i >= AUTH_SESSIONS) break;
    AuthSession &s = g_authSessions[i];
    if (authUnhex(String((const char *)(v | "")), s.hash, 32)) { s.used = true; s.remember = true; s.lastMs = millis(); ++i; }
  }
  if (i) Serial.printf("[Auth] %d kept sign-in(s) restored\n", i);
}

// a new signed-in device, returns its token
static String authNewSession(bool remember) {
  uint8_t tok[32], h[32];
  authRandom(tok, sizeof(tok));
  authSha(tok, sizeof(tok), h);
  portENTER_CRITICAL(&g_authMux);
  int slot = -1;
  // a free slot, else the longest-idle session that isn't kept, else the longest-idle one
  for (int i = 0; i < AUTH_SESSIONS && slot < 0; ++i) if (!g_authSessions[i].used) slot = i;
  for (int pass = 0; pass < 2 && slot < 0; ++pass) {
    uint32_t oldest = 0;
    for (int i = 0; i < AUTH_SESSIONS; ++i) {
      if (pass == 0 && g_authSessions[i].remember) continue;
      const uint32_t idle = millis() - g_authSessions[i].lastMs;
      if (slot < 0 || idle > oldest) { slot = i; oldest = idle; }
    }
  }
  AuthSession &s = g_authSessions[slot];
  memcpy(s.hash, h, 32);
  s.lastMs = millis();
  s.remember = remember;
  s.used = true;
  portEXIT_CRITICAL(&g_authMux);
  return authHex(tok, sizeof(tok));
}
static int authFindSession(AsyncWebServerRequest *request) {
  if (!request->hasHeader("X-Admin-Token")) return -1;
  uint8_t tok[32], h[32];
  if (!authUnhex(request->getHeader("X-Admin-Token")->value(), tok, 32)) return -1;
  authSha(tok, 32, h);
  int found = -1;
  portENTER_CRITICAL(&g_authMux);
  for (int i = 0; i < AUTH_SESSIONS; ++i) {
    AuthSession &s = g_authSessions[i];
    if (!s.used || !authSame(s.hash, h, 32)) continue;
    if (!s.remember && millis() - s.lastMs > AUTH_IDLE_MS) { s.used = false; break; }   // idle too long
    s.lastMs = millis();
    found = i;
    break;
  }
  portEXIT_CRITICAL(&g_authMux);
  return found;
}
static void authEndAll() {
  portENTER_CRITICAL(&g_authMux);
  for (auto &s : g_authSessions) s.used = false;
  portEXIT_CRITICAL(&g_authMux);
}

// Returns true if the request is allowed to perform an admin/state-changing action.
bool checkAdminAuth(AsyncWebServerRequest *request) {
  if (!isAdminAuthRequired()) return true;   // no password: whoever has the device is its owner
  return authFindSession(request) >= 0;
}

// the password as typed, against whatever the card holds (v2, or the two old forms)
static bool authPlainMatches(const String &pw) {
  AuthRecord r;
  if (authParse(settings.adminPassword, r)) {
    // runs on the network task: a record with more rounds than the page sets signs in by
    // challenge only
    if (r.iter > AUTH_MAX_NEW_ITER) return false;
    uint8_t k[32];
    authDerive(pw, r.salt, r.iter, k);
    return authSame(k, r.key, 32);
  }
  uint8_t h[32];
  authSha((const uint8_t *)pw.c_str(), pw.length(), h);
  return nomadSecretEquals(pw, settings.adminPassword) || nomadSecretEquals(authHex(h, 32), settings.adminPassword);
}
static void authStoreRecord(const String &rec) {
  settings.adminPassword = rec;
  authEndAll();
  saveSettings();
  authSaveKept();
}
static void authSetPlain(const String &pw) {
  uint8_t salt[16], k[32];
  authRandom(salt, sizeof(salt));
  authDerive(pw, salt, AUTH_ITER, k);
  authStoreRecord(authRecordString(AUTH_ITER, salt, k));
}

static void authSend(AsyncWebServerRequest *request, int code, const String &body) {
  AsyncWebServerResponse *r = request->beginResponse(code, "application/json", body);
  if (!r) { request->send(code); return; }
  r->addHeader("Cache-Control", "no-store");
  request->send(r);
}
static bool authBody(AsyncWebServerRequest *request, JsonDocument &d) {
  return request->hasParam("body", true) && !deserializeJson(d, request->getParam("body", true)->value());
}

void nomadAuthRegister(AsyncWebServer &server) {
  server.on("/auth/challenge", HTTP_GET, [](AsyncWebServerRequest *request) {
    const char *mode = authMode();
    String body = String("{\"mode\":\"") + mode + "\"";
    if (!strcmp(mode, "v2")) {
      AuthRecord r;
      authParse(settings.adminPassword, r);
      uint8_t n[16];
      authRandom(n, sizeof(n));
      portENTER_CRITICAL(&g_authMux);
      int slot = 0;
      for (int i = 0; i < AUTH_NONCES; ++i) {
        if (!g_authNonces[i].used || millis() - g_authNonces[i].at > AUTH_NONCE_MS) { slot = i; break; }
        if (g_authNonces[i].at < g_authNonces[slot].at) slot = i;
      }
      memcpy(g_authNonces[slot].n, n, 16);
      g_authNonces[slot].at = millis();
      g_authNonces[slot].used = true;
      portEXIT_CRITICAL(&g_authMux);
      body += ",\"salt\":\"" + authHex(r.salt, 16) + "\",\"iter\":" + String(r.iter) + ",\"nonce\":\"" + authHex(n, 16) + "\"";
    }
    authSend(request, 200, body + "}");
  });

  server.on("/auth/login", HTTP_POST, [](AsyncWebServerRequest *request) {
    JsonDocument d;
    const bool hasBody = authBody(request, d);
    const bool remember = hasBody && (d["remember"] | false);
    if (!isAdminAuthRequired()) {
      authSend(request, 200, "{\"token\":\"" + authNewSession(remember) + "\"}");
      return;
    }
    if (!hasBody) { authSend(request, 400, "{\"error\":\"Missing body\"}"); return; }
    // checked before the guess is even read, so a locked-out device does no work
    const uint32_t who = request->client() ? (uint32_t)request->client()->remoteIP() : 0;
#if NOMAD_BENCH
    const uint32_t whoKey = request->hasParam("benchip") ? (uint32_t)request->getParam("benchip")->value().toInt() : who;   // a made-up address, to try the cap across addresses
#else
    const uint32_t whoKey = who;
#endif
    const uint32_t waitMs = nomadLoginWaitMs(whoKey);
    if (waitMs) {
      AsyncWebServerResponse *r = request->beginResponse(429, "application/json",
          "{\"error\":\"Too many wrong passwords\",\"wait\":" + String((waitMs + 999) / 1000) + "}");
      if (r) { r->addHeader("Retry-After", String((waitMs + 999) / 1000)); request->send(r); }
      else request->send(429);
      return;
    }
    bool ok = false, upgrade = false;
    String plain;
    AuthRecord r;
    const bool v2 = authParse(settings.adminPassword, r);
    if (d["proof"].is<const char *>() && d["nonce"].is<const char *>() && v2) {
      uint8_t n[16], proof[32], want[32];
      if (authUnhex(String((const char *)d["nonce"]), n, 16) && authUnhex(String((const char *)d["proof"]), proof, 32)) {
        bool fresh = false;
        portENTER_CRITICAL(&g_authMux);
        for (auto &x : g_authNonces) {
          if (x.used && millis() - x.at <= AUTH_NONCE_MS && authSame(x.n, n, 16)) { x.used = false; fresh = true; break; }   // good once
        }
        portEXIT_CRITICAL(&g_authMux);
        if (fresh) {
          mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), r.key, 32, n, 16, want);
          ok = authSame(proof, want, 32);
        }
      }
    } else {
      // the password as typed: a card still holding the old form, or an older page ("hash")
      plain = String((const char *)(d["password"] | (d["hash"] | "")));
      ok = plain.length() && authPlainMatches(plain);
      upgrade = ok && !v2;
    }
    nomadLoginResult(whoKey, ok);
    if (!ok) { authSend(request, 401, "{\"error\":\"Invalid password\"}"); return; }
    if (upgrade) {
      authSetPlain(plain);          // stored the new way from now on (ends other sessions)
      webLogf("info", "Admin password upgraded to the salted form");
    }
    authSend(request, 200, "{\"token\":\"" + authNewSession(remember) + "\"}");
    if (remember) authSaveKept();
  });

  server.on("/auth/logout", HTTP_POST, [](AsyncWebServerRequest *request) {
    const int me = authFindSession(request);
    if (isAdminAuthRequired() && me < 0) { authSend(request, 401, "{\"error\":\"Unauthorized\"}"); return; }
    JsonDocument d;
    const bool all = authBody(request, d) && (d["all"] | false);
    bool keptChanged = false;
    portENTER_CRITICAL(&g_authMux);
    for (int i = 0; i < AUTH_SESSIONS; ++i) {
      if (!g_authSessions[i].used || (!all && i != me)) continue;
      keptChanged |= g_authSessions[i].remember;
      g_authSessions[i].used = false;
    }
    portEXIT_CRITICAL(&g_authMux);
    if (keptChanged) authSaveKept();
    authSend(request, 200, "{\"status\":\"ok\"}");
  });

  server.on("/auth/sessions", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!checkAdminAuth(request)) { authSend(request, 401, "{\"error\":\"Unauthorized\"}"); return; }
    int n = 0, kept = 0;
    portENTER_CRITICAL(&g_authMux);
    for (auto &s : g_authSessions) {
      if (!s.used || (!s.remember && millis() - s.lastMs > AUTH_IDLE_MS)) continue;
      ++n;
      kept += s.remember;
    }
    portEXIT_CRITICAL(&g_authMux);
    authSend(request, 200, "{\"mode\":\"" + String(authMode()) + "\",\"signedIn\":" + String(n) + ",\"kept\":" + String(kept) + "}");
  });

  server.on("/auth/password", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAdminAuth(request)) { authSend(request, 401, "{\"error\":\"Unauthorized\"}"); return; }
    JsonDocument d;
    if (!authBody(request, d)) { authSend(request, 400, "{\"error\":\"Missing body\"}"); return; }
    if (d["disable"] | false) {
      authStoreRecord("");
      webLogf("info", "Admin password turned off");
      authSend(request, 200, "{\"status\":\"off\"}");
      return;
    }
    // worked out on the phone: the password itself never comes over the air
    uint8_t salt[16], key[32];
    const uint32_t iter = d["iter"] | 0u;
    if (authUnhex(String((const char *)(d["salt"] | "")), salt, 16) && authUnhex(String((const char *)(d["key"] | "")), key, 32) &&
        iter >= 1000 && iter <= AUTH_MAX_NEW_ITER) {
      authStoreRecord(authRecordString(iter, salt, key));
    } else {
      const String pw = String((const char *)(d["password"] | ""));
      if (pw.length() < 4) { authSend(request, 400, "{\"error\":\"Password too short\"}"); return; }
      authSetPlain(pw);
    }
    webLogf("info", "Admin password changed: every device signs in again");
    // this device stays signed in
    const bool remember = d["remember"] | false;
    authSend(request, 200, "{\"status\":\"set\",\"token\":\"" + authNewSession(remember) + "\"}");
    if (remember) authSaveKept();
  });
}
