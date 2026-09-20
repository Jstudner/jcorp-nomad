// NomadDNS.cpp - see NomadDNS.h for the overview.

#include "NomadDNS.h"
#include <AsyncUDP.h>
#include <Arduino.h>

static AsyncUDP s_dns;
static bool s_started = false;
static bool s_catchAll = false;
static IPAddress s_self;

// Hostnames every major OS/browser/console uses to decide "am I online / is
// this a portal". Matched as whole names or any subdomain of an entry, so
// www./ipv6./etc variants are covered. Deliberately broad: hijacking one of
// these on an offline hotspot costs nothing, missing one means that device
// never shows the sign-in popup.
static const char *CAPTIVE_DOMAINS[] = {
  // android / google (clientsN.google.com handled separately below)
  "connectivitycheck.gstatic.com",
  "gstatic.com",
  "connectivitycheck.android.com",
  "android.clients.google.com",
  "play.googleapis.com",
  // vendor androids
  "connect.rom.miui.com",
  "captive.miui.com",
  "connectivitycheck.platform.hicloud.com",
  "connectivitycheck.cbg-app.huawei.com",
  "wifi.vivo.com.cn",
  "oppomobile.com",
  // apple (current + the old probe host set)
  "captive.apple.com",
  "appleiphonecell.com",
  "airport.us",
  "ibook.info",
  "itools.info",
  "thinkdifferent.us",
  "www.apple.com",
  // windows
  "msftconnecttest.com",
  "msftncsi.com",
  // browsers / linux
  "detectportal.firefox.com",
  "nmcheck.gnome.org",
  "connectivity-check.ubuntu.com",
  "network-test.debian.org",
  "fedoraproject.org",
  "ping.archlinux.org",
  "networkcheck.kde.org",
  // consoles & gadgets
  "conntest.nintendowifi.net",
  "ctest.cdn.nintendo.net",
  "test.steampowered.com",
  "spectrum.s3.amazonaws.com",  // kindle wifistub
};
static const int N_CAPTIVE = sizeof(CAPTIVE_DOMAINS) / sizeof(CAPTIVE_DOMAINS[0]);

static bool nameMatches(const String &name) {
  // nomad's own names (mDNS covers .local for most, this is the unicast fallback)
  if (name == "nomad" || name == "nomad.local" || name == "www.nomad" ||
      name == "nomad.lan" || name == "nomad.home") return true;
  // android validation rotates through clients1..6.google.com
  if (name.startsWith("clients") && name.endsWith(".google.com")) return true;
  for (int i = 0; i < N_CAPTIVE; i++) {
    const char *pat = CAPTIVE_DOMAINS[i];
    size_t pl = strlen(pat), nl = name.length();
    if (nl == pl && name.equals(pat)) return true;
    if (nl > pl && name.charAt(nl - pl - 1) == '.' && name.endsWith(pat)) return true;
  }
  return false;
}

static void onDnsPacket(AsyncUDPPacket pkt) {
  const uint8_t *d = pkt.data();
  size_t len = pkt.length();
  if (len < 17 || len > 576) return;   // header + a minimal question, sane cap
  if (d[2] & 0x80) return;             // QR set = a response, not for us
  if ((d[2] >> 3) & 0x0F) return;      // only standard queries
  uint16_t qd = (d[4] << 8) | d[5];
  if (qd < 1) return;

  // walk the first question's name. compression never appears in a question
  // from a stub resolver, treat it (and junk lengths) as malformed.
  size_t i = 12;
  String name;
  while (i < len) {
    uint8_t l = d[i];
    if (l == 0) { i++; break; }
    if (l & 0xC0) return;
    if (i + 1 + l > len || name.length() + l > 253) return;
    if (name.length()) name += '.';
    for (uint8_t k = 0; k < l; k++) name += (char)tolower(d[i + 1 + k]);
    i += 1 + l;
  }
  if (i + 4 > len) return;
  uint16_t qtype = (d[i] << 8) | d[i + 1];
  uint16_t qclass = (d[i + 2] << 8) | d[i + 3];
  size_t qend = i + 4;

  bool known = s_catchAll || nameMatches(name);
  bool classOk = (qclass == 1 || qclass == 255);  // IN / ANY
  // A record only for known names. Known name + other qtype (AAAA, HTTPS/65...)
  // gets NOERROR with no answers so the client falls back to A. Unknown names
  // get NXDOMAIN - the fast "this network has no internet" signal.
  bool answer = known && classOk && qtype == 1;

  uint8_t resp[576 + 16];
  size_t qsec = qend;  // header + question, echoed back
  if (qsec + 16 > sizeof(resp)) return;
  memcpy(resp, d, qsec);
  resp[2] = 0x80 | (d[2] & 0x01) | 0x04;      // QR=1, AA=1, RD copied
  resp[3] = known ? 0x00 : 0x03;              // RCODE: NOERROR / NXDOMAIN
  resp[4] = 0; resp[5] = 1;                   // QDCOUNT=1 (extras dropped)
  resp[6] = 0; resp[7] = answer ? 1 : 0;      // ANCOUNT
  resp[8] = 0; resp[9] = 0;                   // NSCOUNT
  resp[10] = 0; resp[11] = 0;                 // ARCOUNT (EDNS OPT dropped)
  size_t n = qsec;
  if (answer) {
    const uint8_t a[] = { 0xC0, 0x0C,          // name: pointer to question
                          0x00, 0x01,          // type A
                          0x00, 0x01,          // class IN
                          0x00, 0x00, 0x00, 0x3C,  // TTL 60s
                          0x00, 0x04,
                          s_self[0], s_self[1], s_self[2], s_self[3] };
    memcpy(resp + n, a, sizeof(a));
    n += sizeof(a);
  }
  pkt.write(resp, n);
}

void nomadDnsStart(IPAddress selfIp) {
  s_self = selfIp;
  if (s_started) return;
  if (s_dns.listen(53)) {
    s_dns.onPacket(onDnsPacket);
    s_started = true;
    Serial.printf("[DNS] captive responder up on :53 (%s)\n",
                  s_catchAll ? "catch-all" : "targeted");
  } else {
    Serial.println("[DNS] bind on :53 FAILED");
  }
}

void nomadDnsStop() {
  if (!s_started) return;
  s_dns.close();
  s_started = false;
}

void nomadDnsSetCatchAll(bool on) {
  s_catchAll = on;
}
