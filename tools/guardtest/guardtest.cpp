// guardtest: run the firmware's path rules (firmware/JcorpNomadProject/NomadGuard.h)
// on a PC. The header is compiled unchanged against the small String below, which
// implements only the Arduino String calls the header makes.
//
//   tools/guardtest/run.sh
//
// Every case here is a request someone could send, the comment says why it matters.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class String {
 public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  String(const char *s, size_t n) : s_(s, n) {}             // for embedded NULs
  int length() const { return (int)s_.size(); }
  char charAt(int i) const { return (i >= 0 && i < length()) ? s_[i] : 0; }
  void reserve(int n) { s_.reserve(n); }
  String &operator+=(char c) { s_ += c; return *this; }
  String &operator+=(const char *c) { s_ += c; return *this; }
  String &operator+=(const String &o) { s_ += o.s_; return *this; }
  bool startsWith(const char *p) const { return s_.compare(0, strlen(p), p) == 0; }
  int indexOf(char c, int from = 0) const {
    size_t r = s_.find(c, from); return r == std::string::npos ? -1 : (int)r;
  }
  int lastIndexOf(char c) const {
    size_t r = s_.rfind(c); return r == std::string::npos ? -1 : (int)r;
  }
  String substring(int b) const { return String(s_.substr(b)); }
  String substring(int b, int e) const { return String(s_.substr(b, e - b)); }
  bool operator==(const char *o) const { return s_ == o; }
  bool operator==(const String &o) const { return s_ == o.s_; }
  const char *c_str() const { return s_.c_str(); }
 private:
  std::string s_;
};

#define NOMAD_GUARD_HOST_TEST 1
#include "../../firmware/JcorpNomadProject/NomadGuard.h"

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char *what) {
  if (ok) { g_pass++; return; }
  g_fail++;
  printf("FAIL  %s\n", what);
}
#define T(expr) check((expr), #expr)

static NomadWriteVerdict guest(const char *p) { return nomadWriteVerdict(p, false, false); }
static NomadWriteVerdict guestOff(const char *p) { return nomadWriteVerdict(p, false, true); }
static NomadWriteVerdict admin(const char *p) { return nomadWriteVerdict(p, true, false); }

int main() {
  // --- folding matches what SdFat opens
  T(canonicalPath("/CONFIG/settings.json") == "/config/settings.json");
  T(canonicalPath("//config//settings.json") == "/config/settings.json");
  T(canonicalPath("/config./settings.json") == "/config/settings.json");
  T(canonicalPath("/config /settings.json") == "/config/settings.json");
  T(canonicalPath("/ config/settings.json") == "/config/settings.json");
  T(canonicalPath("config/settings.json") == "/config/settings.json");
  T(canonicalPath("") == "/");
  T(canonicalPath("///") == "/");

  // --- /config is never readable, in any spelling a handler might open
  T(nomadReadBlocked("/config/settings.json"));
  T(nomadReadBlocked("/Config/Settings.json"));
  T(nomadReadBlocked("//config/settings.json"));
  T(nomadReadBlocked("/config./settings.json"));
  T(nomadReadBlocked("/config"));
  T(nomadReadBlocked("/config/"));
  T(nomadReadBlocked("config/settings.json"));             // handlers prepend the '/'
  T(nomadReadBlocked("/%63onfig/settings.json"));          // decoded again by urlDecode()
  T(nomadReadBlocked("/%2563onfig/settings.json"));        // and again
  T(nomadReadBlocked("/config+/settings.json"));           // '+' becomes a trailing space
  T(nomadReadBlocked("/config%20/settings.json"));
  T(nomadReadBlocked("/CONFIG/SETTIN~1.JSO"));             // FAT32 alias, still under /config
  T(!nomadReadBlocked("/Movies/Config Wars.mkv"));
  T(!nomadReadBlocked("/configs/readme.txt"));
  T(!nomadReadBlocked("/Files/config"));
  T(!nomadReadBlocked("/.system-theme.json"));             // pages read these
  T(!nomadReadBlocked("/index.html"));
  T(!nomadReadBlocked("/"));
  T(nomadReadBlocked("/config%"));                         // urlDecode() drops a trailing '%'
  T(nomadReadBlocked("/config%/settings.json"));           // urlDecode() reads "%/s" as a NUL
  T(nomadReadBlocked("/c%6Fnfig/settings.json"));
  T(nomadReadBlocked("/config.%00x"));                     // NUL ends the name open() sees
  T(nomadReadBlocked("/config.%zzx"));                     // strtol turns %zz into NUL
  T(nomadReadBlocked(String("/config.\0/settings.json", 24)));  // a NUL already in the value
  T(nomadReadBlocked("/Files/a\tb"));                      // control characters, any
  T(!nomadReadBlocked("/Music/50%off.mp3"));               // a real name with a '%' in it
  T(!nomadReadBlocked("/Files/100%25 sure.txt"));

  // --- the card root and /config are never changed through a file route
  T(guest("/") == NW_NEVER);
  T(admin("/") == NW_NEVER);                               // POST /delete filename=/
  T(admin("//") == NW_NEVER);
  T(admin("/.") == NW_BAD_PATH);
  T(admin("/config/settings.json") == NW_NEVER);
  T(admin("/CONFIG") == NW_NEVER);
  T(admin("/%63onfig/settings.json") == NW_NEVER);

  // --- malformed paths
  T(guest("") == NW_BAD_PATH);
  T(guest("/Files/../config/settings.json") == NW_BAD_PATH);
  T(guest("/Files/..") == NW_BAD_PATH);
  T(guest("/Files/. . /x") == NW_BAD_PATH);
  T(guest("/Files\\x.txt") == NW_BAD_PATH);
  T(guest("/Files/a\nb.txt") == NW_BAD_PATH);
  T(guest("/Files/notes..txt") == NW_OK);                  // dots inside a name are fine
  T(guest("/Files/.hidden") == NW_OK);

  // --- what only an admin may change
  T(guest("/.system-theme.json") == NW_ADMIN);
  T(guest("/.SYSTEM-ui.json") == NW_ADMIN);
  T(guest("//.system-ui.json") == NW_ADMIN);
  T(guest("/.system-index/Movies.json") == NW_ADMIN);
  T(guest("/SYSTEM~1.JSO") == NW_ADMIN);                   // alias of a system dotfile
  T(guest("/EMULAT~1/data/loader.js") == NW_ADMIN);        // alias of /EmulatorJS
  T(guest("/Files/REPORT~1.PDF") == NW_ADMIN);             // looks like an alias, admin only
  T(guest("/index.html") == NW_ADMIN);
  T(guest("/admin.js") == NW_ADMIN);
  T(guest("/master.css") == NW_ADMIN);
  T(guest("/Logo.png") == NW_ADMIN);                       // root files are the interface
  T(guest("/assets/epub.min.js") == NW_ADMIN);
  T(guest("/assets") == NW_ADMIN);
  T(guest("/EmulatorJS/data/loader.js") == NW_ADMIN);
  T(guest("/Files/page.html") == NW_ADMIN);                // stored script, any folder
  T(guest("/Files/page.HTM") == NW_ADMIN);
  T(guest("/Gallery/pic.svg") == NW_ADMIN);
  T(guest("/Games/evil.js") == NW_ADMIN);
  T(guest("/Games/Whiteboard.html") == NW_ADMIN);          // deleting a shipped game too
  T(guest("/Files/x.xhtml") == NW_ADMIN);
  T(guest("/Gallery/x.html.gz") == NW_ADMIN);              // served as text/html, gzip-encoded
  T(guest("/Files/x.svg.br") == NW_ADMIN);
  T(guest("/Files/x.js.gz.gz") == NW_ADMIN);
  T(guest("/Files/backup.tar.gz") == NW_OK);
  T(guest("/Files/notes.txt.gz") == NW_OK);
  T(admin("/index.html") == NW_OK);
  T(admin("/.system-theme.json") == NW_OK);
  T(admin("/Files/page.html") == NW_OK);

  // --- what a visitor may change while uploads are on
  T(guest("/Files/notes.txt") == NW_OK);
  T(guest("/Gallery/IMG_0001.jpg") == NW_OK);
  T(guest("/Movies/Film (2001).mp4") == NW_OK);
  T(guest("/Movies") == NW_ADMIN);                         // the root's layout is the admin's
  T(guest("/Games") == NW_ADMIN);                          // recursive delete of shipped games
  T(guest("/Workshop") == NW_ADMIN);
  T(guest("/New Folder") == NW_ADMIN);
  T(guest("/notes.txt") == NW_ADMIN);
  T(guest("/Movies/Some Show") == NW_OK);                  // inside a bucket is fine
  T(guest("/Files/My~Notes.txt") == NW_OK);                // '~' alone is not an alias
  T(guest("/Files/~1.txt") == NW_OK);                      // nothing before the '~'
  T(guest("/Files/LONGNAME~1.TXT") == NW_OK);              // nine characters: not 8.3
  T(guest("/Cookbook/Soup.md") == NW_OK);
  T(guest("/.search-index.json") == NW_OK);
  T(guest("/.chat/room.json") == NW_OK);
  T(guest("/.whiteboard/layer-1.json") == NW_OK);

  // --- and with the admin's "Allow uploads" switched off
  T(guestOff("/Files/notes.txt") == NW_GUESTS_OFF);
  T(guestOff("/Movies/Some Show") == NW_GUESTS_OFF);
  T(guestOff("/Cookbook/Soup.md") == NW_GUESTS_OFF);
  T(guestOff("/.chat/room.json") == NW_OK);                // chat is its own page
  T(guestOff("/.whiteboard/layer-1.json") == NW_OK);
  T(guestOff("/.search-index.json") == NW_OK);
  // the caller exempts a signed-in admin (it passes guestWritesOff false for one). With the
  // switch on, "admin" here is an owner with no password, and the switch binds them too
  T(nomadWriteVerdict("/Files/notes.txt", true, false) == NW_OK);
  T(nomadWriteVerdict("/Files/notes.txt", true, true) == NW_GUESTS_OFF);
  T(nomadWriteVerdict("/.system-theme.json", true, true) == NW_OK);    // admin-only paths keep their own rule
  T(nomadWriteVerdict("/index.html", true, true) == NW_OK);
  T(nomadWriteVerdict("/.chat/x.json", false, true) == NW_OK);         // community paths stay open

  // --- routes are matched the way the library matches them
  T(nomadRouteMatches("/delete", "/delete"));
  T(nomadRouteMatches("/delete/", "/delete"));
  T(nomadRouteMatches("/delete/anything", "/delete"));     // reaches the /delete handler
  T(!nomadRouteMatches("/deletex", "/delete"));
  T(!nomadRouteMatches("/Delete", "/delete"));             // the library compares exactly too

  // --- status codes the pages see
  T(nomadWriteStatus(NW_ADMIN) == 401);
  T(nomadWriteStatus(NW_NEVER) == 403);
  T(nomadWriteStatus(NW_GUESTS_OFF) == 403);
  T(nomadWriteStatus(NW_BAD_PATH) == 400);

  // --- helpers
  T(nomadHtmlEscape("a<b>&\"'") == "a&lt;b&gt;&amp;&quot;&#39;");
  T(nomadSecretEquals("abc", "abc"));
  T(!nomadSecretEquals("abc", "abd"));
  T(!nomadSecretEquals("abc", "abcd"));
  T(!nomadSecretEquals("", "a"));
  T(nomadSecretEquals("", ""));

  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
