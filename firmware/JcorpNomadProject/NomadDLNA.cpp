// NomadDLNA.cpp - see NomadDLNA.h for the overview.

#include "NomadDLNA.h"
#include <WiFi.h>
#include <AsyncUDP.h>
#include <ArduinoJson.h>
#include "NomadSD.h"

// the sketch aliases SD_MMC to the SdFat shim; do the same here
#define SD_MMC NomadSD

// helpers that live in the sketch
extern SemaphoreHandle_t sdMutex;
extern const char *INDEX_DIR;
extern String urlencode(String str);
extern String encodeIndexName(const String &path_in);
extern String normalizePath(const String &p_in);
extern String xmlEscape(const String &in);
extern void webLogf(const String &type, const char *format, ...);
extern IPAddress nomadLocalIP();  // STA address in WiFi Mode, AP address on the hotspot

static AsyncUDP s_ssdp;
static bool s_enabled = true;
static bool s_started = false;
static unsigned long s_lastNotify = 0;
static char s_uuid[48] = "";

static const char *ST_ROOT = "upnp:rootdevice";
static const char *ST_MS   = "urn:schemas-upnp-org:device:MediaServer:1";
static const char *ST_CDS  = "urn:schemas-upnp-org:service:ContentDirectory:1";
static const char *SSDP_SERVER = "FreeRTOS/10 UPnP/1.0 JcorpNomad/4.6";

// one flags string everywhere. OP=01 = byte seek, and the LG-required
// realTimeInfo header is added by handleRangeRequest on the stream itself.
static const char *DLNA_VIDEO_PI = "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000";

static String apBase() {
  return "http://" + nomadLocalIP().toString();
}

static const char *deviceUUID() {
  if (!s_uuid[0]) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(s_uuid, sizeof(s_uuid), "uuid:4a4e4d44-6d6b-3400-8000-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }
  return s_uuid;
}

/* ---------------- SSDP ---------------- */

static void ssdpSendTo(const String &msg, IPAddress ip, uint16_t port) {
  s_ssdp.writeTo((const uint8_t *)msg.c_str(), msg.length(), ip, port);
}

static String ssdpResponse(const char *st, bool rootUsn) {
  String usn = String(deviceUUID());
  if (!rootUsn) usn += String("::") + st;
  return String("HTTP/1.1 200 OK\r\n") +
         "CACHE-CONTROL: max-age=1800\r\n" +
         "EXT: \r\n" +
         "LOCATION: " + apBase() + "/dlna/device.xml\r\n" +
         "SERVER: " + SSDP_SERVER + "\r\n" +
         "ST: " + st + "\r\n" +
         "USN: " + usn + "\r\n\r\n";
}

static String ssdpNotify(const char *nt, bool rootUsn, bool alive) {
  String usn = String(deviceUUID());
  if (!rootUsn) usn += String("::") + nt;
  return String("NOTIFY * HTTP/1.1\r\n") +
         "HOST: 239.255.255.250:1900\r\n" +
         "CACHE-CONTROL: max-age=1800\r\n" +
         "LOCATION: " + apBase() + "/dlna/device.xml\r\n" +
         "NT: " + nt + "\r\n" +
         "NTS: " + (alive ? "ssdp:alive" : "ssdp:byebye") + "\r\n" +
         "SERVER: " + SSDP_SERVER + "\r\n" +
         "USN: " + usn + "\r\n\r\n";
}

static void ssdpAnnounce(bool alive) {
  // uuid announcement uses the bare uuid as both NT and USN
  s_ssdp.print(ssdpNotify(ST_ROOT, false, alive));
  s_ssdp.print(ssdpNotify(deviceUUID(), true, alive));
  s_ssdp.print(ssdpNotify(ST_MS, false, alive));
  s_ssdp.print(ssdpNotify(ST_CDS, false, alive));
}

// case-insensitive header pull out of a raw SSDP packet. must anchor to line
// starts: every M-SEARCH has a HOST: line that ends in "st:", so an unanchored
// search for ST: grabs the host address and no discovery request ever matches
static String ssdpHeader(const String &pkt, const char *name) {
  String low = pkt;
  low.toLowerCase();
  String key = String(name) + ":";
  key.toLowerCase();
  int i = -1;
  int from = 0;
  while ((i = low.indexOf(key, from)) >= 0) {
    if (i == 0 || low.charAt(i - 1) == '\n') break;  // header names start a line
    from = i + 1;
  }
  if (i < 0) return "";
  int e = pkt.indexOf('\r', i);
  if (e < 0) e = pkt.length();
  String v = pkt.substring(i + key.length(), e);
  v.trim();
  return v;
}

static void onSSDPPacket(AsyncUDPPacket packet) {
  if (!s_enabled) return;
  if (packet.length() < 20 || packet.length() > 1024) return;
  String pkt;
  pkt.reserve(packet.length());
  for (size_t i = 0; i < packet.length(); i++) pkt += (char)packet.data()[i];
  if (!pkt.startsWith("M-SEARCH")) return;

  String st = ssdpHeader(pkt, "ST");
  IPAddress ip = packet.remoteIP();
  uint16_t port = packet.remotePort();

  if (st.equalsIgnoreCase("ssdp:all")) {
    ssdpSendTo(ssdpResponse(ST_ROOT, false), ip, port);
    ssdpSendTo(ssdpResponse(ST_MS, false), ip, port);
    ssdpSendTo(ssdpResponse(ST_CDS, false), ip, port);
  } else if (st.equalsIgnoreCase(ST_ROOT)) {
    ssdpSendTo(ssdpResponse(ST_ROOT, false), ip, port);
  } else if (st.equalsIgnoreCase(ST_MS)) {
    ssdpSendTo(ssdpResponse(ST_MS, false), ip, port);
  } else if (st.equalsIgnoreCase(ST_CDS)) {
    ssdpSendTo(ssdpResponse(ST_CDS, false), ip, port);
  }
}

void dlnaStart() {
  if (s_started) return;
  if (s_ssdp.listenMulticast(IPAddress(239, 255, 255, 250), 1900, 2)) {
    s_ssdp.onPacket(onSSDPPacket);
    s_started = true;
    Serial.println("[DLNA] SSDP listener up on 239.255.255.250:1900");
    if (s_enabled) ssdpAnnounce(true);
  } else {
    Serial.println("[DLNA] SSDP multicast bind FAILED");
  }
}

void dlnaRestart() {
  if (s_started) {
    s_ssdp.close();
    s_started = false;
  }
  dlnaStart();
}

void dlnaTick() {
  if (!s_started || !s_enabled) return;
  if (millis() - s_lastNotify < 90000UL) return;
  s_lastNotify = millis();
  ssdpAnnounce(true);
}

void dlnaSetEnabled(bool en) {
  if (en == s_enabled) return;
  s_enabled = en;
  if (!s_started) return;
  // alive when turning on, byebye when turning off so TVs drop the entry
  ssdpAnnounce(en);
}

bool dlnaIsEnabled() {
  return s_enabled;
}

/* ---------------- device + service descriptions ---------------- */

static void sendDeviceXml(AsyncWebServerRequest *request) {
  String xml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
    "<specVersion><major>1</major><minor>0</minor></specVersion>"
    "<device>"
    "<deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>"
    "<friendlyName>Nomad</friendlyName>"
    "<manufacturer>Jcorp</manufacturer>"
    "<manufacturerURL>http://jcorp.local</manufacturerURL>"
    "<modelName>Nomad Media Server</modelName>"
    "<modelDescription>Offline SD card media server</modelDescription>"
    "<modelNumber>Mk4</modelNumber>"
    "<UDN>" + String(deviceUUID()) + "</UDN>"
    "<dlna:X_DLNADOC xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">DMS-1.50</dlna:X_DLNADOC>"
    "<serviceList>"
    "<service>"
    "<serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>"
    "<serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId>"
    "<SCPDURL>/dlna/cds.xml</SCPDURL>"
    "<controlURL>/dlna/control/cds</controlURL>"
    "<eventSubURL>/dlna/event/cds</eventSubURL>"
    "</service>"
    "<service>"
    "<serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
    "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
    "<SCPDURL>/dlna/cms.xml</SCPDURL>"
    "<controlURL>/dlna/control/cms</controlURL>"
    "<eventSubURL>/dlna/event/cms</eventSubURL>"
    "</service>"
    "</serviceList>"
    "</device>"
    "</root>";
  request->send(200, "text/xml", xml);
}

// minimal SCPDs. TVs fetch these but rarely validate deeply.
static const char CDS_SCPD[] PROGMEM =
  "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
  "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
  "<specVersion><major>1</major><minor>0</minor></specVersion>"
  "<actionList>"
  "<action><name>Browse</name><argumentList>"
  "<argument><name>ObjectID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>"
  "<argument><name>BrowseFlag</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_BrowseFlag</relatedStateVariable></argument>"
  "<argument><name>Filter</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument>"
  "<argument><name>StartingIndex</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument>"
  "<argument><name>RequestedCount</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>"
  "<argument><name>SortCriteria</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument>"
  "<argument><name>Result</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>"
  "<argument><name>NumberReturned</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>"
  "<argument><name>TotalMatches</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>"
  "<argument><name>UpdateID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument>"
  "</argumentList></action>"
  "<action><name>GetSystemUpdateID</name><argumentList>"
  "<argument><name>Id</name><direction>out</direction><relatedStateVariable>SystemUpdateID</relatedStateVariable></argument>"
  "</argumentList></action>"
  "<action><name>GetSearchCapabilities</name><argumentList>"
  "<argument><name>SearchCaps</name><direction>out</direction><relatedStateVariable>SearchCapabilities</relatedStateVariable></argument>"
  "</argumentList></action>"
  "<action><name>GetSortCapabilities</name><argumentList>"
  "<argument><name>SortCaps</name><direction>out</direction><relatedStateVariable>SortCapabilities</relatedStateVariable></argument>"
  "</argumentList></action>"
  "</actionList>"
  "<serviceStateTable>"
  "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_ObjectID</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_BrowseFlag</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Filter</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Index</name><dataType>ui4</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Count</name><dataType>ui4</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_SortCriteria</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_Result</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>A_ARG_TYPE_UpdateID</name><dataType>ui4</dataType></stateVariable>"
  "<stateVariable sendEvents=\"yes\"><name>SystemUpdateID</name><dataType>ui4</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>SearchCapabilities</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"no\"><name>SortCapabilities</name><dataType>string</dataType></stateVariable>"
  "</serviceStateTable>"
  "</scpd>";

static const char CMS_SCPD[] PROGMEM =
  "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
  "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
  "<specVersion><major>1</major><minor>0</minor></specVersion>"
  "<actionList>"
  "<action><name>GetProtocolInfo</name><argumentList>"
  "<argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>"
  "<argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>"
  "</argumentList></action>"
  "</actionList>"
  "<serviceStateTable>"
  "<stateVariable sendEvents=\"yes\"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>"
  "<stateVariable sendEvents=\"yes\"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>"
  "</serviceStateTable>"
  "</scpd>";

/* ---------------- ContentDirectory Browse ---------------- */

struct BucketDef { const char *id; const char *title; bool video; bool audio; bool image; };
static const BucketDef BUCKETS[] = {
  { "/Movies",  "Movies",  true,  false, false },
  { "/Shows",   "Shows",   true,  false, false },
  { "/Music",   "Music",   false, true,  false },
  { "/Gallery", "Gallery", false, false, true  },
};
static const int NBUCKETS = sizeof(BUCKETS) / sizeof(BUCKETS[0]);

static String mimeForName(const String &lower) {
  if (lower.endsWith(".mp4"))  return "video/mp4";
  if (lower.endsWith(".m4v"))  return "video/x-m4v";
  if (lower.endsWith(".mkv"))  return "video/x-matroska";
  if (lower.endsWith(".webm")) return "video/webm";
  if (lower.endsWith(".avi"))  return "video/x-msvideo";
  if (lower.endsWith(".mp3"))  return "audio/mpeg";
  if (lower.endsWith(".flac")) return "audio/flac";
  if (lower.endsWith(".m4a"))  return "audio/mp4";
  if (lower.endsWith(".aac"))  return "audio/aac";
  if (lower.endsWith(".wav"))  return "audio/x-wav";
  if (lower.endsWith(".ogg"))  return "audio/ogg";
  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
  if (lower.endsWith(".png"))  return "image/png";
  if (lower.endsWith(".webp")) return "image/webp";
  return "";
}

static bool typeAllowed(const String &mime, const BucketDef *b) {
  if (mime.startsWith("video/")) return b->video;
  if (mime.startsWith("audio/")) return b->audio;
  if (mime.startsWith("image/")) return b->image;
  return false;
}

static const BucketDef *bucketFor(const String &path) {
  for (int i = 0; i < NBUCKETS; i++) {
    if (path == BUCKETS[i].id || path.startsWith(String(BUCKETS[i].id) + "/")) return &BUCKETS[i];
  }
  return nullptr;
}

static String upnpClassFor(const String &mime) {
  if (mime.startsWith("video/")) return "object.item.videoItem";
  if (mime.startsWith("audio/")) return "object.item.audioItem.musicTrack";
  return "object.item.imageItem.photo";
}

static String didlContainer(const String &id, const String &parent, const String &title) {
  return "<container id=\"" + xmlEscape(id) + "\" parentID=\"" + xmlEscape(parent) +
         "\" restricted=\"1\" searchable=\"0\"><dc:title>" + xmlEscape(title) +
         "</dc:title><upnp:class>object.container.storageFolder</upnp:class></container>";
}

static String didlItem(const String &path, const String &name, const String &parent,
                       const String &mime, uint64_t sz) {
  String pi;
  if (mime.startsWith("image/")) {
    pi = "http-get:*:" + mime + ":*";
  } else {
    pi = "http-get:*:" + mime + ":" + DLNA_VIDEO_PI;
  }
  String title = name;
  int dot = title.lastIndexOf('.');
  if (dot > 0) title = title.substring(0, dot);
  String out = "<item id=\"i:" + xmlEscape(path) + "\" parentID=\"" + xmlEscape(parent) +
               "\" restricted=\"1\"><dc:title>" + xmlEscape(title) +
               "</dc:title><upnp:class>" + upnpClassFor(mime) + "</upnp:class>";
  // same-named .jpg sidecar poster (web UI convention). emitted without an
  // exists() so browsing costs no extra SD stats - a 404'd art fetch just
  // leaves the client's generic icon
  if (mime.startsWith("video/")) {
    int adot = path.lastIndexOf('.');
    if (adot > 0) {
      String art = path.substring(0, adot) + ".jpg";
      out += "<upnp:albumArtURI>" + apBase() + "/media?file=" + urlencode(art) + "</upnp:albumArtURI>";
    }
  }
  out += "<res protocolInfo=\"" + pi + "\"";
  if (sz > 0) out += " size=\"" + String((unsigned long long)sz) + "\"";
  out += ">" + apBase() + "/media?file=" + urlencode(path) + "</res></item>";
  return out;
}

// pull <Tag>value</Tag> out of a SOAP body, prefix-insensitive
static String soapTag(const String &body, const char *tag) {
  String open = String("<") + tag;
  int i = body.indexOf(open);
  if (i < 0) return "";
  int gt = body.indexOf('>', i);
  if (gt < 0) return "";
  String close = String("</") + tag + ">";
  int e = body.indexOf(close, gt);
  if (e < 0) return "";
  return body.substring(gt + 1, e);
}

static String xmlUnescape(String s) {
  s.replace("&lt;", "<");
  s.replace("&gt;", ">");
  s.replace("&quot;", "\"");
  s.replace("&apos;", "'");
  s.replace("&amp;", "&");
  return s;
}

static void soapFault(AsyncWebServerRequest *request, int code, const char *msg) {
  String body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>"
    "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
    "<errorCode>" + String(code) + "</errorCode><errorDescription>" + msg + "</errorDescription>"
    "</UPnPError></detail></s:Fault></s:Body></s:Envelope>";
  request->send(500, "text/xml; charset=\"utf-8\"", body);
}

static void soapOk(AsyncWebServerRequest *request, const char *service, const char *action, const String &args) {
  String body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body><u:" + String(action) + "Response xmlns:u=\"" + service + "\">" +
    args + "</u:" + action + "Response></s:Body></s:Envelope>";
  AsyncWebServerResponse *r = request->beginResponse(200, "text/xml; charset=\"utf-8\"", body);
  r->addHeader("EXT", "");
  request->send(r);
}

// Browse one directory out of the on card indexes. Emits DIDL for entries in
// [start, start+want), counts everything for TotalMatches.
static bool browseDir(const String &dirPath, const BucketDef *b, const String &containerId,
                      uint32_t start, uint32_t want,
                      String &didl, uint32_t &returned, uint32_t &total) {
  bool bucketRoot = (dirPath.indexOf('/', 1) < 0);
  String indexFile;
  if (bucketRoot) {
    indexFile = String(INDEX_DIR) + "/" + dirPath.substring(1) + ".index.ndjson";
  } else {
    indexFile = String(INDEX_DIR) + "/" + encodeIndexName(dirPath) + ".nested.ndjson";
  }

  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1500)) != pdTRUE) return false;

  File f = SD_MMC.open(indexFile, FILE_READ);
  if (!f) {
    if (sdMutex) xSemaphoreGive(sdMutex);
    // no index yet reads as an empty folder, not an error
    returned = 0;
    total = 0;
    return true;
  }

  bool headerSkipped = false;
  uint32_t match = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (!headerSkipped) { headerSkipped = true; continue; }

    // 1KB: a long nested path overflows 512B and the item silently vanishes
    // from the TV's listing (and its paging count)
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, line)) continue;
    const char *t = doc["t"] | "";
    const char *n = doc["n"] | "";
    const char *p = doc["p"] | "";
    if (!n[0]) continue;
    String full = p[0] ? String(p) : dirPath + "/" + n;

    if (t[0] == 'd') {
      if (match >= start && returned < want) {
        didl += didlContainer("c:" + full, containerId, String(n));
        returned++;
      }
      match++;
    } else if (t[0] == 'f') {
      String lower = String(n);
      lower.toLowerCase();
      String mime = mimeForName(lower);
      if (mime.length() == 0 || !typeAllowed(mime, b)) continue;
      if (match >= start && returned < want) {
        uint64_t sz = doc["sz"] | (uint64_t)0;
        didl += didlItem(full, String(n), containerId, mime, sz);
        returned++;
      }
      match++;
    }
    yield();
  }
  f.close();
  if (sdMutex) xSemaphoreGive(sdMutex);
  total = match;
  return true;
}

static void handleBrowse(AsyncWebServerRequest *request, const String &body) {
  String objectId = xmlUnescape(soapTag(body, "ObjectID"));
  String flag = soapTag(body, "BrowseFlag");
  uint32_t start = (uint32_t)strtoul(soapTag(body, "StartingIndex").c_str(), nullptr, 10);
  uint32_t want = (uint32_t)strtoul(soapTag(body, "RequestedCount").c_str(), nullptr, 10);
  // 0 means "everything"; cap so one response can't eat the heap. TVs page.
  if (want == 0 || want > 30) want = 30;
  bool metadata = flag.equals("BrowseMetadata");

  String didl;
  didl.reserve(4096);
  uint32_t returned = 0, total = 0;

  if (objectId == "0" || objectId == "") {
    if (metadata) {
      didl += "<container id=\"0\" parentID=\"-1\" restricted=\"1\" searchable=\"0\">"
              "<dc:title>Nomad</dc:title><upnp:class>object.container.storageFolder</upnp:class></container>";
      returned = 1; total = 1;
    } else {
      for (int i = 0; i < NBUCKETS; i++) {
        if ((uint32_t)i >= start && returned < want) {
          didl += didlContainer(String("c:") + BUCKETS[i].id, "0", BUCKETS[i].title);
          returned++;
        }
      }
      total = NBUCKETS;
    }
  } else if (objectId.startsWith("c:")) {
    String path = normalizePath(objectId.substring(2));
    const BucketDef *b = bucketFor(path);
    if (!b) { soapFault(request, 701, "No such object"); return; }
    if (metadata) {
      String title = path.substring(path.lastIndexOf('/') + 1);
      String parent = (path.indexOf('/', 1) < 0) ? "0" : ("c:" + path.substring(0, path.lastIndexOf('/')));
      didl += didlContainer(objectId, parent, title);
      returned = 1; total = 1;
    } else {
      if (!browseDir(path, b, objectId, start, want, didl, returned, total)) {
        soapFault(request, 501, "SD busy");
        return;
      }
    }
  } else {
    soapFault(request, 701, "No such object");
    return;
  }

  String didlDoc =
    "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
    "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">" + didl + "</DIDL-Lite>";

  String args;
  args.reserve(didlDoc.length() + didlDoc.length() / 2 + 200);
  args += "<Result>";
  args += xmlEscape(didlDoc);
  args += "</Result><NumberReturned>" + String(returned) +
          "</NumberReturned><TotalMatches>" + String(total) +
          "</TotalMatches><UpdateID>1</UpdateID>";
  soapOk(request, ST_CDS, "Browse", args);
}

/* ---------------- HTTP routes ---------------- */

// SOAP bodies arrive through the body callback; keep them bounded. Uses a
// malloc'd buffer because the library free()s _tempObject if the client
// disconnects mid request.
static void collectBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (total == 0 || total > 8192) return;
  if (index == 0) request->_tempObject = calloc(1, total + 1);
  char *buf = (char *)request->_tempObject;
  if (!buf) return;
  memcpy(buf + index, data, len);
}

static String takeBody(AsyncWebServerRequest *request) {
  char *buf = (char *)request->_tempObject;
  if (!buf) return "";
  String out(buf);
  free(buf);
  request->_tempObject = nullptr;
  return out;
}

void dlnaRegisterRoutes(AsyncWebServer &server) {
  server.on("/dlna/device.xml", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!s_enabled) { request->send(404, "text/plain", "DLNA disabled"); return; }
    sendDeviceXml(request);
  });

  // some players probe /description.xml before reading SSDP LOCATION
  server.on("/description.xml", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!s_enabled) { request->send(404, "text/plain", "DLNA disabled"); return; }
    sendDeviceXml(request);
  });

  server.on("/dlna/cds.xml", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!s_enabled) { request->send(404, "text/plain", "DLNA disabled"); return; }
    request->send(200, "text/xml", String(CDS_SCPD));
  });

  server.on("/dlna/cms.xml", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!s_enabled) { request->send(404, "text/plain", "DLNA disabled"); return; }
    request->send(200, "text/xml", String(CMS_SCPD));
  });

  server.on("/dlna/control/cds", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!s_enabled) { request->send(404, "text/plain", "DLNA disabled"); return; }
      String body = takeBody(request);
      String action = "";
      if (request->hasHeader("SOAPACTION")) action = request->getHeader("SOAPACTION")->value();
      else if (request->hasHeader("SOAPAction")) action = request->getHeader("SOAPAction")->value();

      if (action.indexOf("#Browse") >= 0) {
        handleBrowse(request, body);
      } else if (action.indexOf("#GetSystemUpdateID") >= 0) {
        soapOk(request, ST_CDS, "GetSystemUpdateID", "<Id>1</Id>");
      } else if (action.indexOf("#GetSearchCapabilities") >= 0) {
        soapOk(request, ST_CDS, "GetSearchCapabilities", "<SearchCaps></SearchCaps>");
      } else if (action.indexOf("#GetSortCapabilities") >= 0) {
        soapOk(request, ST_CDS, "GetSortCapabilities", "<SortCaps></SortCaps>");
      } else {
        soapFault(request, 401, "Invalid Action");
      }
    }, NULL, collectBody);

  server.on("/dlna/control/cms", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!s_enabled) { request->send(404, "text/plain", "DLNA disabled"); return; }
      takeBody(request);
      String action = "";
      if (request->hasHeader("SOAPACTION")) action = request->getHeader("SOAPACTION")->value();
      else if (request->hasHeader("SOAPAction")) action = request->getHeader("SOAPAction")->value();

      if (action.indexOf("#GetProtocolInfo") >= 0) {
        soapOk(request, "urn:schemas-upnp-org:service:ConnectionManager:1", "GetProtocolInfo",
          "<Source>http-get:*:video/mp4:*,http-get:*:video/x-matroska:*,http-get:*:video/webm:*,"
          "http-get:*:video/x-m4v:*,http-get:*:audio/mpeg:*,http-get:*:audio/flac:*,"
          "http-get:*:audio/mp4:*,http-get:*:audio/x-wav:*,http-get:*:audio/ogg:*,"
          "http-get:*:image/jpeg:*,http-get:*:image/png:*</Source><Sink></Sink>");
      } else if (action.indexOf("#GetCurrentConnectionIDs") >= 0) {
        soapOk(request, "urn:schemas-upnp-org:service:ConnectionManager:1", "GetCurrentConnectionIDs",
          "<ConnectionIDs>0</ConnectionIDs>");
      } else {
        soapFault(request, 401, "Invalid Action");
      }
    }, NULL, collectBody);

  // No eventing. Answer politely so a TV's SUBSCRIBE attempt does not hang;
  // players work fine without change notifications from a read only server.
  server.on("/dlna/event/cds", HTTP_ANY, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *r = request->beginResponse(200, "text/plain", "");
    r->addHeader("SID", "uuid:nomad-noevents");
    r->addHeader("TIMEOUT", "Second-1800");
    request->send(r);
  });
  server.on("/dlna/event/cms", HTTP_ANY, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *r = request->beginResponse(200, "text/plain", "");
    r->addHeader("SID", "uuid:nomad-noevents");
    r->addHeader("TIMEOUT", "Second-1800");
    request->send(r);
  });
}
