// NomadChat.h - the chat page's messages, kept by Nomad.
//
// Nomad numbers each message as it arrives and keeps the last CHAT_KEEP in PSRAM, so
// the order doesn't depend on phone clocks (often wrong offline):
//   GET  /api/chat?after=N   {"epoch":E,"seq":S,"msgs":[{"s","t","n","x","c","m"}...]}
//                            everything after N (at most CHAT_PAGE), from memory: no card
//   POST /api/chat           n=name x=text c=client id m=message id t=the phone's clock
//                            {"s":seq}. The same (c, m) again gets the same seq, so a page
//                            may resend until it hears back
//   POST /api/chat/clear     admin: empties it, card included
// "epoch" changes when the history is replaced (boot, clear): a page that sees a new one
// starts over from 0. Messages are appended to /.chat/log.jsonl from loop(), never on the
// network task. The log is rewritten down to the kept messages once it doubles. On the
// first boot with no log, the old per-tab /.chat/chat_<id>.json files are read in once.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <algorithm>
#include <map>
#include <vector>

#ifndef CHAT_KEEP
#define CHAT_KEEP 500
#endif
#define CHAT_PAGE 100
#define CHAT_NAME_MAX 24       // characters, as the page allows
#define CHAT_TEXT_MAX 500
#define CHAT_ID_MAX 16

static const char *const CHAT_DIR = "/.chat";
static const char *const CHAT_LOG = "/.chat/log.jsonl";

struct ChatMsg {
  uint32_t seq;
  uint64_t ts;                 // the sender's clock, for showing a time, order is seq
  char *name;                  // PSRAM, exact size
  char *text;                  // PSRAM, exact size
  char cid[CHAT_ID_MAX + 1];
  char mid[CHAT_ID_MAX + 1];
};

static ChatMsg *g_chat = nullptr;          // ring of CHAT_KEEP, in PSRAM
static uint32_t g_chatCount = 0, g_chatHead = 0;   // head: index of the oldest
static uint32_t g_chatSeq = 0;             // last seq handed out
static uint32_t g_chatSaved = 0;           // last seq on the card
static uint32_t g_chatEpoch = 0;
static uint32_t g_chatLogLines = 0;        // lines in log.jsonl, for compaction
static volatile bool g_chatClearCard = false;
static SemaphoreHandle_t g_chatLock = nullptr;

struct ChatLockGuard {
  ChatLockGuard() { xSemaphoreTake(g_chatLock, portMAX_DELAY); }
  ~ChatLockGuard() { xSemaphoreGive(g_chatLock); }
};

static char *chatDup(const String &s) {
  char *p = (char *)heap_caps_malloc(s.length() + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) memcpy(p, s.c_str(), s.length() + 1);
  return p;
}

static ChatMsg &chatAt(uint32_t i) { return g_chat[(g_chatHead + i) % CHAT_KEEP]; }

static void chatFree(ChatMsg &m) {
  if (m.name) heap_caps_free(m.name);
  if (m.text) heap_caps_free(m.text);
  m.name = m.text = nullptr;
}

// At most `max` UTF-8 characters, control characters dropped, outer spaces trimmed.
// Only whole, well-formed characters are copied (a page can send anything), so what is
// stored is always valid UTF-8 and the history always parses.
static String chatClean(const String &in, size_t max) {
  String out;
  size_t chars = 0;
  const size_t len = in.length();
  for (size_t i = 0; i < len && chars < max;) {
    const uint8_t c = (uint8_t)in.charAt(i);
    const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    bool whole = n > 0 && i + n <= len;
    for (int k = 1; whole && k < n; ++k) whole = ((uint8_t)in.charAt(i + k) & 0xC0) == 0x80;
    if (!whole) { ++i; continue; }                 // a stray or cut-off byte
    if (n == 1 && (c < 0x20 || c == 0x7f)) {       // control characters, a tab becomes a space
      if (c == '\t') { out += ' '; ++chars; }
      ++i;
      continue;
    }
    for (int k = 0; k < n; ++k) out += in.charAt(i + k);
    i += n;
    ++chars;
  }
  out.trim();
  return out;
}

static String chatId(const String &in) {
  String out;
  for (size_t i = 0; i < in.length() && out.length() < CHAT_ID_MAX; ++i) {
    const char c = in.charAt(i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_') out += c;
  }
  return out;
}

// Into the ring, the oldest goes when it's full. Caller holds the lock.
static uint32_t chatPush(uint32_t seq, uint64_t ts, const String &name, const String &text,
                         const String &cid, const String &mid) {
  if (g_chatCount == CHAT_KEEP) {
    chatFree(chatAt(0));
    g_chatHead = (g_chatHead + 1) % CHAT_KEEP;
    --g_chatCount;
  }
  ChatMsg &m = g_chat[(g_chatHead + g_chatCount) % CHAT_KEEP];
  m.seq = seq;
  m.ts = ts;
  m.name = chatDup(name);
  m.text = chatDup(text);
  strlcpy(m.cid, cid.c_str(), sizeof(m.cid));
  strlcpy(m.mid, mid.c_str(), sizeof(m.mid));
  ++g_chatCount;
  if (seq > g_chatSeq) g_chatSeq = seq;
  return seq;
}

static void chatJsonString(String &out, const char *s) {
  out += '"';
  for (const char *p = s ? s : ""; *p; ++p) {
    const char c = *p;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) { char e[8]; snprintf(e, sizeof(e), "\\u%04x", c); out += e; }
    else out += c;
  }
  out += '"';
}

static void chatJsonMsg(String &out, const ChatMsg &m) {
  char head[64];
  snprintf(head, sizeof(head), "{\"s\":%u,\"t\":%llu,\"n\":", (unsigned)m.seq, (unsigned long long)m.ts);
  out += head;
  chatJsonString(out, m.name);
  out += ",\"x\":";
  chatJsonString(out, m.text);
  out += ",\"c\":";
  chatJsonString(out, m.cid);
  out += ",\"m\":";
  chatJsonString(out, m.mid);
  out += '}';
}

// the card

static bool chatTakeSd(uint32_t ms) {
  return !sdMutex || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}
static void chatGiveSd() { if (sdMutex) xSemaphoreGive(sdMutex); }

// One line of log.jsonl back into the ring (boot only, before the server starts).
static void chatLoadLine(const String &line) {
  JsonDocument d;
  if (deserializeJson(d, line)) return;
  const uint32_t seq = d["s"] | 0u;
  if (!seq) return;
  // cleaned again: a line written by an older build, or damaged on the card, can't break the history
  chatPush(seq, d["t"] | (uint64_t)0, chatClean(String((const char *)(d["n"] | "")), CHAT_NAME_MAX),
           chatClean(String((const char *)(d["x"] | "")), CHAT_TEXT_MAX),
           chatId(String((const char *)(d["c"] | ""))), chatId(String((const char *)(d["m"] | ""))));
}

// The old per-tab files, read in once when there is no log yet: oldest first, by the
// phones' clocks (all there was to go by).
static void chatImportOld() {
  File dir = SD_MMC.open(CHAT_DIR);
  if (!dir || !dir.isDirectory()) return;
  struct Old { uint64_t ts; String name, text, cid; uint32_t k; };
  std::vector<Old> old;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String n = String(f.name());
    n = n.substring(n.lastIndexOf('/') + 1);
    if (f.isDirectory() || !n.startsWith("chat_") || !n.endsWith(".json") || f.size() > 256 * 1024) { f.close(); continue; }
    JsonDocument d;
    const DeserializationError err = deserializeJson(d, f);
    f.close();
    if (err) continue;
    const String who = chatClean(String((const char *)(d["n"] | "Guest")), CHAT_NAME_MAX);
    const String cid = chatId("old" + n.substring(5, n.length() - 5));
    uint32_t k = 0;
    for (JsonArray m : d["m"].as<JsonArray>()) {
      const String text = chatClean(String((const char *)(m[1] | "")), CHAT_TEXT_MAX);
      if (text.length()) old.push_back({ m[0] | (uint64_t)0, who, text, cid, k++ });
    }
  }
  dir.close();
  std::sort(old.begin(), old.end(), [](const Old &a, const Old &b) { return a.ts < b.ts; });
  const size_t from = old.size() > CHAT_KEEP ? old.size() - CHAT_KEEP : 0;
  for (size_t i = from; i < old.size(); ++i)
    chatPush(g_chatSeq + 1, old[i].ts, old[i].name, old[i].text, old[i].cid, "old" + String(old[i].k));
  if (g_chatCount) Serial.printf("[Chat] read in %u messages from the old per-tab files\n", (unsigned)g_chatCount);
}

static void chatLoad() {
  if (!chatTakeSd(5000)) return;
  File f = SD_MMC.open(CHAT_LOG, FILE_READ);
  if (f) {
    while (f.available()) {
      const String line = f.readStringUntil('\n');
      if (line.length() > 2) { chatLoadLine(line); ++g_chatLogLines; }
    }
    f.close();
    g_chatSaved = g_chatSeq;
  } else {
    chatImportOld();          // g_chatSaved stays 0: the next tick writes them to the log
  }
  chatGiveSd();
  Serial.printf("[Chat] %u messages, last #%u\n", (unsigned)g_chatCount, (unsigned)g_chatSeq);
}

// From loop(): what arrived since the last tick goes onto the card, then the log is
// rewritten to the kept messages if it has grown to twice that.
void nomadChatTick() {
  static uint32_t last = 0;
  if (!g_chat || millis() - last < 1000) return;
  last = millis();
  if (g_chatClearCard) {
    if (!chatTakeSd(2000)) return;
    File dir = SD_MMC.open(CHAT_DIR);
    if (dir && dir.isDirectory()) {
      std::vector<String> gone;
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) gone.push_back(String(CHAT_DIR) + "/" + String(f.name()).substring(String(f.name()).lastIndexOf('/') + 1));
        f.close();
      }
      dir.close();
      for (const String &p : gone) SD_MMC.remove(p);
    }
    chatGiveSd();
    g_chatClearCard = false;
    g_chatLogLines = 0;
  }

  String lines;
  uint32_t upTo = 0;
  {
    ChatLockGuard lk;
    if (g_chatSaved >= g_chatSeq) return;
    for (uint32_t i = 0; i < g_chatCount; ++i) {
      const ChatMsg &m = chatAt(i);
      if (m.seq <= g_chatSaved) continue;
      chatJsonMsg(lines, m);
      lines += '\n';
      upTo = m.seq;
    }
  }
  if (!upTo || !chatTakeSd(500)) return;              // busy: next tick
  if (!SD_MMC.exists(CHAT_DIR)) SD_MMC.mkdir(CHAT_DIR);
  File f = SD_MMC.open(CHAT_LOG, FILE_APPEND);
  bool ok = f && f.print(lines) == lines.length();
  if (f) f.close();
  uint32_t added = 0;
  for (size_t i = 0; i < lines.length(); ++i) added += lines.charAt(i) == '\n';
  if (ok) g_chatLogLines += added;

  if (ok && g_chatLogLines > 2 * CHAT_KEEP) {
    // down to the kept messages: written beside the log, then swapped in
    String all;
    {
      ChatLockGuard lk;
      for (uint32_t i = 0; i < g_chatCount; ++i) { chatJsonMsg(all, chatAt(i)); all += '\n'; }
    }
    const String tmp = String(CHAT_LOG) + ".tmp";
    File t = SD_MMC.open(tmp, FILE_WRITE);
    const bool wrote = t && t.print(all) == all.length();
    if (t) t.close();
    if (wrote) {
      SD_MMC.remove(CHAT_LOG);
      if (SD_MMC.rename(tmp, CHAT_LOG)) g_chatLogLines = g_chatCount;
    } else {
      SD_MMC.remove(tmp);
    }
  }
  chatGiveSd();
  if (ok) g_chatSaved = upTo;
}

// the routes

// a sender may post at most CHAT_BURST messages in CHAT_WINDOW_MS (by address: a page
// can make up client ids)
#define CHAT_BURST 15
#define CHAT_WINDOW_MS 20000
static std::map<uint32_t, std::pair<uint32_t, uint8_t>> g_chatRate;   // ip -> (window start, count)

static bool chatRateOk(AsyncWebServerRequest *request) {
  const uint32_t ip = (uint32_t)request->client()->remoteIP();
  const uint32_t now = millis();
  if (g_chatRate.size() > 64) {
    for (auto it = g_chatRate.begin(); it != g_chatRate.end();)
      it = (now - it->second.first > CHAT_WINDOW_MS) ? g_chatRate.erase(it) : std::next(it);
  }
  auto &r = g_chatRate[ip];
  if (now - r.first > CHAT_WINDOW_MS) r = { now, 0 };
  return ++r.second <= CHAT_BURST;
}

static void chatSend(AsyncWebServerRequest *request, int code, const String &body) {
  AsyncWebServerResponse *r = request->beginResponse(code, "application/json", body);
  if (!r) { request->send(503); return; }
  r->addHeader("Cache-Control", "no-store");
  if (code == 429 || code == 503) r->addHeader("Retry-After", "5");
  request->send(r);
}

void nomadChatRegister(AsyncWebServer &server) {
  g_chatLock = xSemaphoreCreateMutex();
  g_chat = (ChatMsg *)heap_caps_calloc(CHAT_KEEP, sizeof(ChatMsg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_chat || !g_chatLock) { Serial.println("[Chat] no memory, chat off"); return; }
  g_chatEpoch = esp_random() | 1;
  chatLoad();

  server.on("/api/chat", HTTP_GET, [](AsyncWebServerRequest *request) {
    const uint32_t after = request->hasParam("after") ? (uint32_t)strtoul(request->getParam("after")->value().c_str(), nullptr, 10) : 0;
    const uint32_t epoch = request->hasParam("epoch") ? (uint32_t)strtoul(request->getParam("epoch")->value().c_str(), nullptr, 10) : 0;
    String out;
    {
      ChatLockGuard lk;
      // a page from before a clear or a reboot starts over
      const uint32_t from = (epoch && epoch != g_chatEpoch) ? 0 : after;
      out.reserve(64);
      out += "{\"epoch\":" + String(g_chatEpoch) + ",\"seq\":" + String(g_chatSeq) + ",\"msgs\":[";
      uint32_t n = 0;
      for (uint32_t i = 0; i < g_chatCount && n < CHAT_PAGE; ++i) {
        const ChatMsg &m = chatAt(i);
        if (m.seq <= from) continue;
        if (n++) out += ',';
        chatJsonMsg(out, m);
      }
      out += "]}";
    }
    chatSend(request, 200, out);
  });

  server.on("/api/chat/clear", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!checkAdminAuth(request)) { chatSend(request, 401, "{\"error\":\"Admin login required\"}"); return; }
    {
      ChatLockGuard lk;
      for (uint32_t i = 0; i < g_chatCount; ++i) chatFree(chatAt(i));
      g_chatCount = g_chatHead = 0;
      g_chatSaved = g_chatSeq;            // nothing left to write, numbering carries on
      g_chatEpoch = esp_random() | 1;
    }
    g_chatClearCard = true;               // the files go from loop()
    chatSend(request, 200, "{\"status\":\"cleared\"}");
  });

  server.on("/api/chat", HTTP_POST, [](AsyncWebServerRequest *request) {
    auto param = [request](const char *k) { return request->hasParam(k, true) ? request->getParam(k, true)->value() : String(); };
    const String name = chatClean(param("n"), CHAT_NAME_MAX);
    const String text = chatClean(param("x"), CHAT_TEXT_MAX);
    const String cid = chatId(param("c")), mid = chatId(param("m"));
    if (!text.length() || !cid.length() || !mid.length()) { chatSend(request, 400, "{\"error\":\"Empty message\"}"); return; }
    const uint64_t ts = strtoull(param("t").c_str(), nullptr, 10);
    uint32_t seq = 0;
    {
      ChatLockGuard lk;
      // sent before and answered too late for the phone: the same message, the same number
      for (uint32_t i = g_chatCount; i-- > 0;) {
        const ChatMsg &m = chatAt(i);
        if (!strcmp(m.cid, cid.c_str()) && !strcmp(m.mid, mid.c_str())) { seq = m.seq; break; }
      }
    }
    if (!seq) {
      if (!chatRateOk(request)) { chatSend(request, 429, "{\"error\":\"Slow down\"}"); return; }
      ChatLockGuard lk;
      seq = chatPush(g_chatSeq + 1, ts, name.length() ? name : String("Guest"), text, cid, mid);
    }
    chatSend(request, 200, "{\"s\":" + String(seq) + ",\"epoch\":" + String(g_chatEpoch) + "}");
  });
}
