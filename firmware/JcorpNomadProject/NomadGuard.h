#pragma once
// NomadGuard: which paths a web client may read, and which it may change.
//
// Every firmware line (S3 main, S3 experimental, P4) has an identical copy of this file.
// It's pure functions over String with no board, card or server dependency, so
// tools/guardtest can compile it on a PC against a small String stand-in and run the cases.
//
// The threat is a guest on the hotspot, or in WiFi Mode anyone on the home network or
// any web page one of them has open. The card root is the web root, so every rule here
// is about stopping a file request from reaching something it should not.

#ifndef NOMAD_GUARD_HOST_TEST
#include <Arduino.h>
#endif

// ---------------------------------------------------------------------------------
// Folding: decide on the path SdFat will open, not the one the client typed.
//
// SdFat matches names case-insensitively, skips repeated separators, skips leading
// spaces in a component and trims trailing dots and spaces off it. So
// "/CONFIG/settings.json", "//config/settings.json", "/config./settings.json" and
// "/config /settings.json" all open /config/settings.json. Its upcase table maps no
// non-ASCII letter onto an ASCII one, so folding ASCII case is enough.
//
// This is a comparison form only. The original string is what gets opened.
static inline String canonicalPath(const String &in) {
  String out;
  out.reserve(in.length() + 1);
  const int n = in.length();
  int i = 0;
  while (i < n) {
    while (i < n && in.charAt(i) == '/') i++;          // "//" is one separator
    if (i >= n) break;
    int j = i;
    while (j < n && in.charAt(j) != '/') j++;
    int b = i, e = j;
    while (b < e && in.charAt(b) == ' ') b++;                                    // leading spaces
    while (e > b && (in.charAt(e - 1) == '.' || in.charAt(e - 1) == ' ')) e--;    // trailing dots/spaces
    if (e > b) {
      out += '/';
      for (int k = b; k < e; ++k) {
        const char c = in.charAt(k);
        out += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
      }
    }
    i = j;
  }
  if (out.length() == 0) out = "/";
  return out;
}

// The server hands handlers an already-decoded URL and some handlers decode it again,
// so the guard has to look at every form a value can take by the time it reaches
// SD_MMC.open().
static inline int nomadHexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Strict %XX and '+' decoding: only well-formed escapes.
static inline String nomadPctDecode(const String &in, bool plusIsSpace) {
  String out;
  out.reserve(in.length());
  const int n = in.length();
  for (int i = 0; i < n; ++i) {
    const char c = in.charAt(i);
    if (c == '%' && i + 2 < n) {
      const int hi = nomadHexVal(in.charAt(i + 1)), lo = nomadHexVal(in.charAt(i + 2));
      if (hi >= 0 && lo >= 0) { out += (char)(hi * 16 + lo); i += 2; continue; }
    }
    out += (plusIsSpace && c == '+') ? ' ' : c;
  }
  return out;
}

// The sketch's own urlDecode(), byte for byte: a '%' with two characters after it is
// decoded by strtol, which takes "%6z" as 6 and "%zz" as 0, and a '%' too close to the
// end is dropped. So "/config%" reaches open() as "/config".
static inline String nomadSketchDecode(const String &in) {
  String out;
  out.reserve(in.length());
  const int n = in.length();
  for (int i = 0; i < n; ++i) {
    const char c = in.charAt(i);
    if (c == '%') {
      if (i + 2 < n) {
        const int hi = nomadHexVal(in.charAt(i + 1)), lo = nomadHexVal(in.charAt(i + 2));
        out += (char)(hi < 0 ? 0 : (lo < 0 ? hi : hi * 16 + lo));
        i += 2;
      }
    } else {
      out += c == '+' ? ' ' : c;
    }
  }
  return out;
}

// open() takes a C string, so whatever follows a NUL is never seen: "/config.<NUL>x"
// opens /config.
static inline String nomadUntilNul(const String &in) {
  const int n = in.length();
  for (int i = 0; i < n; ++i)
    if (in.charAt(i) == 0) return in.substring(0, i);
  return in;
}

// ---------------------------------------------------------------------------------
// Path classes. All take any form of the path and fold it first.

// /config holds settings.json: the hotspot password, the admin password hash (which
// /auth/login accepts as the credential) and, in WiFi Mode, the home network password.
// GET /settings withholds all three, so the raw file must never be reachable by a file
// route, and nothing writes it except the firmware itself.
static inline bool isConfigPath(const String &p) {
  const String c = canonicalPath(p);
  return c == "/config" || c.startsWith("/config/");
}

// Root dotfiles the firmware owns: the theme and UI config every page reads, and the
// index. Anyone may read them, only an admin may change them.
static inline bool isSystemPath(const String &p) {
  return canonicalPath(p).startsWith("/.system-");
}

// On a FAT32 card every long name also has an 8.3 alias, and SdFat opens a file by its
// alias as readily as by its name: /SYSTEM~1.JSO can be /.system-theme.json and
// /EMULAT~1 is /EmulatorJS. No rule above can see through that, so any component that
// looks like an alias is treated as protected. exFAT has no aliases.
static inline bool componentLooksLikeShortAlias(const String &c, int b, int e) {
  int t = -1;
  for (int k = b; k < e; ++k) if (c.charAt(k) == '~') { t = k; break; }
  if (t <= b) return false;                       // no '~', or nothing before it
  int k = t + 1;
  if (k >= e || c.charAt(k) < '0' || c.charAt(k) > '9') return false;
  while (k < e && c.charAt(k) >= '0' && c.charAt(k) <= '9') k++;
  if (k - b > 8) return false;                    // NAME~N is at most eight characters
  if (k == e) return true;
  if (c.charAt(k) != '.') return false;
  const int ext = e - (k + 1);
  if (ext > 3) return false;
  for (int m = k + 1; m < e; ++m) if (c.charAt(m) == '.') return false;
  return true;
}
static inline bool hasShortNameAlias(const String &p) {
  const String c = canonicalPath(p);
  const int n = c.length();
  int i = 1;
  while (i < n) {
    int j = i;
    while (j < n && c.charAt(j) != '/') j++;
    if (componentLooksLikeShortAlias(c, i, j)) return true;
    i = j + 1;
  }
  return false;
}

// The web interface itself: the pages and scripts at the card root, and the folders
// the pages load code from. Replacing admin.js or index.html would hand whoever did it
// every later visitor's browser, the admin's included.
static inline bool isInterfacePath(const String &p) {
  const String c = canonicalPath(p);
  if (c == "/assets" || c.startsWith("/assets/")) return true;
  if (c == "/emulatorjs" || c.startsWith("/emulatorjs/")) return true;
  if (c.length() < 2 || c.indexOf('/', 1) >= 0) return false;   // not at the root
  // a root entry with an extension is a file the interface ships. Root folders
  // (Movies, Files, ...) have none. Dotfiles are the community caches below.
  return c.charAt(1) != '.' && c.indexOf('.', 1) > 0;
}

// Places any visitor's page writes as part of using a feature (chat, the live
// whiteboard, the menu's search cache). Clearing them is in the admin panel.
static inline bool isCommunityPath(const String &p) {
  const String c = canonicalPath(p);
  return c == "/.chat" || c.startsWith("/.chat/") ||
         c == "/.whiteboard" || c.startsWith("/.whiteboard/") ||
         c == "/.search-index.json";
}

// File types a browser will run when this device serves them. The card is the web
// origin, so a guest who could leave one here could run script as every later visitor,
// including an admin whose session token sits in sessionStorage on that origin.
//
// A compressed sibling counts as what it unpacks to: serveStatic answers a request for
// page.html with page.html.gz if one exists, as text/html with Content-Encoding: gzip.
static inline bool isActiveContent(const String &p) {
  String c = canonicalPath(p);
  for (int peel = 0; peel < 3; ++peel) {
    const int slash = c.lastIndexOf('/');
    const int dot = c.lastIndexOf('.');
    if (dot <= slash + 1) return false;           // no extension, or a dotfile with none
    const String ext = c.substring(dot + 1);
    if (ext == "gz" || ext == "br" || ext == "gzip" || ext == "zst") {
      c = c.substring(0, dot);
      continue;
    }
    static const char *const kActive[] = {
      "html", "htm", "xhtml", "xht", "shtml", "svg", "svgz", "js", "mjs", "cjs",
      "css", "xml", "xsl", "xslt",
    };
    for (const char *a : kActive) if (ext == a) return true;
    return false;
  }
  return true;                                    // page.html.gz.gz.gz: not worth deciding
}

// Something directly under the card root: a bucket folder, an interface file, a dotfile.
// Visitors can work inside the root folders but can't change the root itself.
static inline bool isRootEntry(const String &p) {
  const String c = canonicalPath(p);
  return c.length() > 1 && c.indexOf('/', 1) < 0;
}

// A path a writer may even attempt: no control characters, no backslashes, and no
// component made only of dots or spaces ("..", ". ."), which SdFat would fold away or
// treat as navigation.
static inline bool isCleanWritePath(const String &raw) {
  const int n = raw.length();
  if (n == 0) return false;
  int compStart = 0;
  bool onlyDots = true;
  for (int i = 0; i <= n; ++i) {
    const char c = (i < n) ? raw.charAt(i) : '/';
    if (i < n && ((unsigned char)c < 0x20 || c == 0x7f || c == '\\')) return false;
    if (c == '/') {
      if (i > compStart && onlyDots) return false;
      compStart = i + 1;
      onlyDots = true;
    } else if (c != '.' && c != ' ') {
      onlyDots = false;
    }
  }
  return true;
}

// Coarse check for older callers: config, the system dotfiles, and anything that might
// be an alias for one of them.
static inline bool isProtectedWritePath(const String &p) {
  return isConfigPath(p) || isSystemPath(p) || hasShortNameAlias(p);
}

// ---------------------------------------------------------------------------------
// Decisions.

// A read is refused if any form the value can take on the way to SD_MMC.open() is
// under /config: as given, decoded again by the sketch's urlDecode() (twice, to be
// safe), or decoded strictly. Each is cut at the first NUL the way open() will cut it.
// A value that already holds a control character is refused outright: no file name a
// page links to has one.
static inline bool nomadReadBlocked(const String &raw) {
  const int n = raw.length();
  for (int i = 0; i < n; ++i)
    if ((unsigned char)raw.charAt(i) < 0x20 || raw.charAt(i) == 0x7f) return true;
  if (isConfigPath(raw)) return true;
  String sketch = raw, strict = raw;
  for (int round = 0; round < 2; ++round) {
    sketch = nomadSketchDecode(sketch);
    strict = nomadPctDecode(strict, true);
    if (isConfigPath(nomadUntilNul(sketch)) || isConfigPath(nomadUntilNul(strict))) return true;
  }
  return isConfigPath(nomadUntilNul(nomadPctDecode(raw, false)));
}

enum NomadWriteVerdict : uint8_t {
  NW_OK = 0,
  NW_BAD_PATH,     // malformed: control characters, "..", backslashes
  NW_NEVER,        // the card root or /config: nobody, through a file route
  NW_ADMIN,        // allowed, but only for an admin
  NW_GUESTS_OFF,   // the admin has turned visitor uploads off
};

// isAdmin is checkAdminAuth(): true for a logged-in admin, and for everyone on a Nomad
// with no admin password set.
// guestWritesOff is the admin's upload switch for this path (all pages or this page),
// already worked out for this caller. It applies to everyone but a signed-in admin, so
// it has to be checked even when isAdmin is true (no password set). It covers the folders
// people upload to. Admin-only paths have their own rule above it.
static inline NomadWriteVerdict nomadWriteVerdict(const String &raw, bool isAdmin, bool guestWritesOff) {
  if (!isCleanWritePath(raw)) return NW_BAD_PATH;
  if (canonicalPath(raw) == "/") return NW_NEVER;
  if (nomadReadBlocked(raw)) return NW_NEVER;
  const bool community = isCommunityPath(raw);
  const bool adminOnly = isSystemPath(raw) || hasShortNameAlias(raw) || isInterfacePath(raw) || isActiveContent(raw) ||
                         (isRootEntry(raw) && !community);   // /Games, /Movies, new root folders
  if (adminOnly) return isAdmin ? NW_OK : NW_ADMIN;
  if (guestWritesOff && !community) return NW_GUESTS_OFF;
  return NW_OK;
}

static inline int nomadWriteStatus(NomadWriteVerdict v) {
  switch (v) {
    case NW_OK:         return 200;
    case NW_BAD_PATH:   return 400;
    case NW_ADMIN:      return 401;
    default:            return 403;
  }
}

static inline const char *nomadWriteMessage(NomadWriteVerdict v) {
  switch (v) {
    case NW_OK:         return "OK";
    case NW_BAD_PATH:   return "Invalid path";
    case NW_NEVER:      return "That location cannot be changed";
    case NW_ADMIN:      return "Admin login required to change that";
    case NW_GUESTS_OFF: return "Uploads are turned off on Nomad";
  }
  return "Refused";
}

// ESPAsyncWebServer's AsyncCallbackWebHandler::canHandle accepts a route itself or
// anything under it (url.startsWith(route + "/")), so a guard keyed on a route has to
// match the same set: POST /delete/x reaches the /delete handler.
static inline bool nomadRouteMatches(const String &url, const char *route) {
  if (url == route) return true;
  const int n = (int)strlen(route);
  return url.length() > n && url.startsWith(route) && url.charAt(n) == '/';
}

// For text that lands inside HTML the firmware builds itself.
static inline String nomadHtmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  const int n = in.length();
  for (int i = 0; i < n; ++i) {
    const char c = in.charAt(i);
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;
    }
  }
  return out;
}

// Same work whatever the inputs, so a wrong guess at a secret takes as long whether it
// failed on the first character or the last.
static inline bool nomadSecretEquals(const String &a, const String &b) {
  const int la = a.length(), lb = b.length();
  const int n = la > lb ? la : lb;
  unsigned char diff = (unsigned char)(la != lb);
  for (int i = 0; i < n; ++i) {
    const unsigned char x = i < la ? (unsigned char)a.charAt(i) : 0;
    const unsigned char y = i < lb ? (unsigned char)b.charAt(i) : 0;
    diff |= (unsigned char)(x ^ y);
  }
  return diff == 0;
}
