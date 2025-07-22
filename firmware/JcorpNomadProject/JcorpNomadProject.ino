//Jcorp Nomad Project
#include "Arduino.h"
#define FF_USE_FASTSEEK 1
#define SD_FREQ_KHZ 10000         // ✱✱ VERY IMPORTANT SETTING ✱✱
                                  // This controls how fast reads from your SD Card can go, If you have a name brand fancy card you can go faster with better results. Check what your card recomends. 
                                  // 10 000 kHz (10 MHz) = safest 
                                  // 12000 kHz (12 MHz) = good 
                                  // 20000 kHz (20 MHz) = fastest 
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "FS.h"
#include "SD_MMC.h"
#include "DNSServer.h"
#include <ArduinoJson.h>
#include <map>
#include "Display_ST7789.h"
#include "LVGL_Driver.h"
#include "ui.h"
#include "RGB_lamp.h"
#include <SPIFFS.h>
#include "esp_wifi.h"

// ==================== CONFIGURATION ====================

// WiFi network name and password (this is the hotspot the ESP32 creates)
#define WIFI_SSID "Jcorp_Nomad"
#define WIFI_PASSWORD "Password"
//Auto Generate Media.json?
//generate media.json every time the device boots?
bool AUTO_GENERATE_MEDIA_JSON = false; //True generates on boot, False only generate when you request it in the admin panel
// Max number of devices that can connect at once
#define MAX_CLIENTS 4 // I recomend 4, If you want to try more knock yourself out!

// =================== scary config ====================
// SD card pinout for Waveshare ESP32-S3
#define SD_CLK_PIN 14
#define SD_CMD_PIN 15
#define SD_D0_PIN 16
#define SD_D1_PIN 18
#define SD_D2_PIN 17
#define SD_D3_PIN 21

// ───────────────── SD‑recovery globals ───────────────
volatile bool sdErrorFlag            = false;      
unsigned long sdErrorCooldownUntil   = 0;          

bool tryRecoverSDCard() {                          
    Serial.println("[SD] Attempting recovery…");
    SD_MMC.end();          // unmount
    delay(1000);           // give hardware a breather

    if (!SD_MMC.begin("/sdcard", true, false, SD_FREQ_KHZ, 12)) {
        Serial.println("[SD] Recovery failed.");
        return false;
    }
    Serial.println("[SD] Recovery OK.");
    return true;
}

String rfc3339Now() {
  return "2025-07-12T12:00:00Z";  // Hard-coded UTC timestamp, or it gets mad
}

// Captive portal DNS setup
const byte DNS_PORT = 53;
DNSServer dnsServer;
AsyncWebServer server(80); // Web server on port 80
std::map<AsyncWebServerRequest*, File> activeUploads;
int connectedClients = 0;
// LED Mode and Color Helper Wrappers
uint8_t currentLEDMode = 0;  // 0=off, 1=rainbow, 2=solid color
uint8_t solidR = 0, solidG = 0, solidB = 0;
void RGB_SetColor(uint8_t r, uint8_t g, uint8_t b) {
    solidR = r;
    solidG = g;
    solidB = b;
    currentLEDMode = 2; 
    Set_Color(g, r, b);
}
extern lv_obj_t *ui_wifi;
extern lv_obj_t *ui_SDcard;
bool lastWifiStatus = false;
bool lastSDStatus = false;
//Globals for SD scan
unsigned long lastUpdateTime = 0;
unsigned long lastSDScanTime = 0;
const unsigned long SD_SCAN_INTERVAL = 60000; // 60 seconds
uint64_t cachedUsedBytes = 0;
uint64_t cachedTotalBytes = 0;
unsigned long lastScanTime = 0;
// Update the UI with the number of connected users
void updateUI(int userCount) {
    char buffer[10];
    snprintf(buffer, sizeof(buffer), "%d", userCount);
    lv_label_set_text(ui_userlabel, buffer);
}
void updateToggleStatus() {
    bool currentWifiStatus = WiFi.softAPIP();
    if (currentWifiStatus != lastWifiStatus) {
        if (currentWifiStatus) {
            lv_obj_add_state(ui_wifi, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(ui_wifi, LV_STATE_CHECKED);
            Serial.println("[Status] WiFi AP failure detected.");
        }
        lastWifiStatus = currentWifiStatus;
    }

bool currentSDStatus = SD_MMC.cardType() != CARD_NONE;   // ✱✱ NEW / CHANGED ✱✱
    if (currentSDStatus != lastSDStatus) {
        if (currentSDStatus) {
            lv_obj_add_state(ui_SDcard, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(ui_SDcard, LV_STATE_CHECKED);
            Serial.println("[Status] SD card failure detected.");
        }
        lastSDStatus = currentSDStatus;
    }
}

// Stream a chunk of text to save RAM
void opdsWrite(AsyncResponseStream *s, const String &chunk) {
    s->print(chunk);
}
//(OPDS thing)
String xmlEscape(const String &in) {        // we already added this
  String out;
  for (char c : in) {
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      default:   out += c;        break;
    }
  }
  return out;
}

String slugify(const String &in) {          // new helper
  String out;
  for (char c : in) {
    if (isalnum(c))       out += (char)tolower(c);
    else if (c==' ' || c=='_' || c=='-') out += '-';
    // every other char is dropped
  }
  return out;
}

// Get a count of currently connected WiFi clients
void updateClientCount() {
    wifi_sta_list_t wifi_sta_list;
    if (esp_wifi_ap_get_sta_list(&wifi_sta_list) == ESP_OK) {
        connectedClients = wifi_sta_list.num;
        updateUI(connectedClients);
    }
}
void generateMediaJSON() {
    Serial.println("[MediaGen] Called generateMediaJSON()");

    if (SD_MMC.exists("/media.json")) {
        SD_MMC.remove("/media.json");
        Serial.println("[MediaGen] Old media.json removed.");
    }

    StaticJsonDocument<32768> doc;

    // Movies
    JsonArray movies = doc.createNestedArray("movies");
    File moviesDir = SD_MMC.open("/Movies");
    while (File file = moviesDir.openNextFile()) {
        if (!file.isDirectory()) {
            String name = file.name();
            if (name.endsWith(".mp4") || name.endsWith(".mkv")) {
                Serial.println("[MediaGen] Found movie: " + name);
                String base = name.substring(name.lastIndexOf('/') + 1);
                base = base.substring(0, base.lastIndexOf('.'));

                String cover = "movies/" + base + ".jpg";
                if (!SD_MMC.exists("/Movies/" + base + ".jpg")) {
                    cover = "placeholder.jpg";
                }

                JsonObject item = movies.createNestedObject();
                item["name"] = base;
                item["cover"] = cover;
                item["file"] = "Movies/" + base + name.substring(name.lastIndexOf('.'));  // Ensure full filename
            }
        }
    }

    // Shows
    JsonArray shows = doc.createNestedArray("shows");
    File showsRoot = SD_MMC.open("/Shows");

    while (File folder = showsRoot.openNextFile()) {
        if (folder.isDirectory()) {
            String folderName = String(folder.name()); // just "GravityFalls"
            String fullPath = "/Shows/" + folderName;

            Serial.println("[MediaGen] Found show folder: " + folderName);

            JsonObject show = shows.createNestedObject();
            show["name"] = folderName;

            String coverPath = "shows/" + folderName + ".jpg";
            if (!SD_MMC.exists("/Shows/" + folderName + ".jpg")) {
                coverPath = "placeholder.jpg";
            }
            show["cover"] = coverPath;

            JsonArray episodes = show.createNestedArray("episodes");

            File epFolder = SD_MMC.open(fullPath);
            while (File ep = epFolder.openNextFile()) {
                if (!ep.isDirectory()) {
                    String epName = String(ep.name());
                    if (epName.endsWith(".mp4") || epName.endsWith(".mkv")) {
                        String epBase = epName.substring(epName.lastIndexOf('/') + 1);

                        JsonObject epItem = episodes.createNestedObject();
                        epItem["name"] = epBase;
                        epItem["file"] = "Shows/" + folderName + "/" + epBase;

                        Serial.println("  [MediaGen] Episode: " + epBase);
                    }
                }
            }
        }
    }

    // Books
    JsonArray books = doc.createNestedArray("books");
    File booksDir = SD_MMC.open("/Books");
    while (File file = booksDir.openNextFile()) {
        if (!file.isDirectory()) {
            String name = file.name();
            if (name.endsWith(".pdf") || name.endsWith(".epub")) {
                Serial.println("[MediaGen] Found book: " + name);
                String base = name.substring(name.lastIndexOf('/') + 1);
                base = base.substring(0, base.lastIndexOf('.'));

                String cover = "books/" + base + ".jpg";
                if (!SD_MMC.exists("/Books/" + base + ".jpg")) {
                    cover = "placeholder.jpg";
                }

                JsonObject item = books.createNestedObject();
                item["name"] = base;
                item["cover"] = cover;
                item["file"] = "Books/" + base + name.substring(name.lastIndexOf('.'));
            }
        }
    }

    // Music
    JsonArray music = doc.createNestedArray("music");
    File musicDir = SD_MMC.open("/Music");
    while (File file = musicDir.openNextFile()) {
        if (!file.isDirectory()) {
            String name = file.name();
            if (name.endsWith(".mp3")) {
                Serial.println("[MediaGen] Found music: " + name);
                String base = name.substring(name.lastIndexOf('/') + 1);
                base = base.substring(0, base.lastIndexOf('.'));

                String cover = "music/" + base + ".jpg";
                if (!SD_MMC.exists("/Music/" + base + ".jpg")) {
                    cover = "placeholder.jpg";
                }

                JsonObject item = music.createNestedObject();
                item["name"] = base;
                item["cover"] = cover;
                item["file"] = "Music/" + base + ".mp3";
            }
        }
    }

    // Save to media.json
    File jsonFile = SD_MMC.open("/media.json", FILE_WRITE);
    if (!jsonFile) {
        Serial.println("[MediaGen] Error opening media.json for writing.");
        return;
    }

    serializeJsonPretty(doc, jsonFile);
    jsonFile.close();
    Serial.println("[MediaGen] media.json created successfully.");
}

String absURL(const String &path) {
  return "http://" + WiFi.softAPIP().toString() + path;
}

void handleOPDSRoot(AsyncWebServerRequest *request) {
    AsyncResponseStream *res = request->beginResponseStream(
        "application/atom+xml;profile=opds-catalog;kind=navigation");

    opdsWrite(res, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<feed xmlns=\"http://www.w3.org/2005/Atom\" "
                   "xmlns:opds=\"http://opds-spec.org/2010/catalog\">\n");

    opdsWrite(res, "  <id>urn:uuid:nomad-opds-root</id>\n"
                   "  <title>Nomad OPDS Catalog</title>\n"
                   "  <updated>2025-07-12T12:00:00Z</updated>\n"
                   "  <author><name>Nomad Server</name></author>\n");

    // Add required navigation links
    opdsWrite(res, "  <link rel=\"self\" href=\"" + absURL("/opds/root.xml") + "\" "
                   "type=\"application/atom+xml;profile=opds-catalog;kind=navigation\"/>\n");
    opdsWrite(res, "  <link rel=\"start\" href=\"" + absURL("/opds/root.xml") + "\" "
                   "type=\"application/atom+xml;profile=opds-catalog;kind=navigation\"/>\n");

    opdsWrite(res, "  <entry>\n"
                   "    <title>All Books</title>\n"
                   "    <id>urn:uuid:nomad-opds-books</id>\n"
                   "    <updated>2025-07-12T12:00:00Z</updated>\n"
                   "    <link rel=\"http://opds-spec.org/catalog\" "
                   "type=\"application/atom+xml;profile=opds-catalog;kind=acquisition\" "
                   "href=\"" + absURL("/opds/books.xml") + "\"/>\n"
                   "  </entry>\n");

    opdsWrite(res, "</feed>");
    request->send(res);
}


void handleOPDSBooks(AsyncWebServerRequest *request) {
    Serial.println("[OPDS] === handleOPDSBooks() called ===");
    AsyncResponseStream *res =
        request->beginResponseStream(
            "application/atom+xml;profile=opds-catalog;kind=acquisition");

    opdsWrite(res,"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                  "<feed xmlns=\"http://www.w3.org/2005/Atom\" "
                  "xmlns:opds=\"http://opds-spec.org/2010/catalog\">\n");
    opdsWrite(res,
      "  <id>urn:uuid:nomad-opds-books</id>\n"
      "  <title>All Books</title>\n"
      "  <updated>"+rfc3339Now()+"</updated>\n"
      "  <link rel=\"self\"  href=\""+absURL("/opds/books.xml")+"\" "
      "type=\"application/atom+xml;profile=opds-catalog;kind=acquisition\"/>\n"
      "  <link rel=\"start\" href=\""+absURL("/opds/root.xml")+"\" "
      "type=\"application/atom+xml;profile=opds-catalog;kind=navigation\"/>\n");

    File dir = SD_MMC.open("/Books");
    Serial.println("[OPDS] Opened /Books directory");

    if (!dir || !dir.isDirectory()) {
        Serial.println("[OPDS] /Books directory not found or is not a directory!");
    }
    while (dir && dir.isDirectory()) {
        File file = dir.openNextFile();
        if (!file) {
            Serial.println("[OPDS] No more files in /Books");
            break;
        }

        Serial.println(String("[OPDS] Found file: ") + file.name());

        if (file.isDirectory()) {
            Serial.println("[OPDS] Skipping directory");
            continue;
        }

        String fn = file.name();
        String fnLower = fn;
        fnLower.toLowerCase();

        if (!(fnLower.endsWith(".epub") || fnLower.endsWith(".pdf"))) {
            Serial.println("[OPDS] Skipping non-ebook file: " + fn);
            continue;
        }

        Serial.println("[OPDS] Processing book file: " + fn);


        String base = fn.substring(fn.lastIndexOf('/')+1);
        base = base.substring(0, base.lastIndexOf('.'));  
        String safeTitle = xmlEscape(base);                 
        String safeId    = "urn:uuid:nomad-book-" + slugify(base);  
        String mime = fn.endsWith(".epub") ?
                      "application/epub+zip" : "application/pdf";

        // Cover (falls back to placeholder)
        String coverPath = SD_MMC.exists("/Books/"+base+".jpg") ?
                           "Books/"+base+".jpg" : "placeholder.jpg";

        // Compute real download path
        String ext    = fnLower.endsWith(".pdf") ? ".pdf" : ".epub";
        String dlPath = "/Books/" + urlencode(base) + ext;

        opdsWrite(res,"  <entry>\n"
                      "    <title>"+safeTitle+"</title>\n"
                      "    <id>"+safeId+"</id>\n"
                      "    <updated>"+rfc3339Now()+"</updated>\n"
                      "    <link rel=\"http://opds-spec.org/image/thumbnail\" "
                      "type=\"image/jpeg\" href=\""+absURL("/"+urlencode(coverPath))+"\"/>\n"
                      "    <link rel=\"http://opds-spec.org/acquisition\" "
                      "type=\""+mime+"\" "
                      "href=\"" + absURL(dlPath) + "\"/>\n"
                      "  </entry>\n");

    }
    if (dir) {
        dir.close();
        Serial.println("[OPDS] Closed /Books directory");
    }


    opdsWrite(res,"</feed>");
    request->send(res);
}

// ==================== MEDIA STREAM HANDLER ====================
// Handles video/audio streaming via range requests
void handleRangeRequest(AsyncWebServerRequest *request) {

    /* ----- validate path ----- */
    if (!request->hasParam("file")) {
        request->send(400, "text/plain", "File parameter missing");
        return;
    }

    String filePath = request->getParam("file")->value();
    if (!SD_MMC.exists(filePath)) {
        request->send(404, "text/plain", "File not found");
        return;
    }

    /* ----- open file with recovery guard ----- */
    File file = SD_MMC.open(filePath, "r");
    if (!file) {                                              // ✱✱ NEW / RECOVERY ✱✱
        Serial.println("[SD] open() failed — trigger recovery");
        sdErrorFlag            = true;                        // flag main loop
        sdErrorCooldownUntil   = millis() + 5000;            // pause 5 s
        request->send(503, "text/plain",
                      "SD error — retrying shortly");        // browser will auto‑retry
        return;
    }

    size_t fileSize = file.size();

    /* ----- debug ----- */
    Serial.println("=== Range Request Debug ===");
    Serial.println("File requested: " + filePath);
    Serial.println("File size: " + String(fileSize));

    /* ----- HEAD support ----- */
    if (request->method() == HTTP_HEAD) {
        Serial.println("HEAD request received");
        AsyncWebServerResponse *headResponse =
            request->beginResponse(200, "application/octet-stream", "");
        headResponse->addHeader("Content-Length", String(fileSize));
        request->send(headResponse);
        file.close();
        return;
    }

    /* ----- parse Range header ----- */
    String rangeHeader = request->header("Range");
    Serial.println("Raw Range header: " + rangeHeader);

    size_t startByte = 0, endByte = fileSize - 1;
    if (rangeHeader.startsWith("bytes=")) {
        int dashIndex = rangeHeader.indexOf('-');
        startByte = rangeHeader.substring(6, dashIndex).toInt();
        if (dashIndex + 1 < rangeHeader.length()) {
            endByte = rangeHeader.substring(dashIndex + 1).toInt();
        }
    }
    if (endByte >= fileSize) endByte = fileSize - 1;
    size_t contentLength = endByte - startByte + 1;

    Serial.println("Computed startByte: " + String(startByte));
    Serial.println("Computed endByte: " + String(endByte));
    Serial.println("Content length: " + String(contentLength));
    Serial.println("============================");

    /* ----- create async response ----- */
    AsyncWebServerResponse *response =
        request->beginResponse("video/mp4", contentLength,
        /* capture globals by reference so we can set the flag inside */
        [&, file, startByte, endByte, contentLength]
        (uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t {

            /* seek & read */
            file.seek(startByte + index);
            size_t bytesToRead = min(maxLen, endByte - (startByte + index) + 1);
            size_t bytesRead   = file.read(buffer, bytesToRead);

            /* ----- detect read failure ----- */
            if (bytesRead == 0) {                               
                Serial.println("[SD] read() failed — recovery");
                file.close();
                sdErrorFlag          = true;
                sdErrorCooldownUntil = millis() + 5000;
                return 0;               // aborts this chunk; client retries
            }

            /* close when finished */
            if (index + bytesRead >= contentLength) {
                file.close();
            }
            return bytesRead;
        });

 
    response->addHeader("Accept-Ranges", "bytes");
    response->addHeader("Content-Range", "bytes " + String(startByte) + "-" + String(endByte) + "/" + String(fileSize));
    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Pragma", "no-cache");
    response->setCode(206); // Partial Content
    request->send(response);
}
void handleListFiles(AsyncWebServerRequest *request) {
    if (!request->hasParam("dir")) {
        request->send(400, "application/json", "{\"error\":\"Missing 'dir' parameter\"}");
        return;
    }
    String dir = request->getParam("dir")->value();

    if (!SD_MMC.exists(dir)) {
        request->send(404, "application/json", "{\"error\":\"Directory not found\"}");
        return;
    }

    File directory = SD_MMC.open(dir);
    if (!directory || !directory.isDirectory()) {
        request->send(404, "application/json", "{\"error\":\"Not a directory\"}");
        return;
    }

    DynamicJsonDocument doc(8192);
    JsonArray arr = doc.to<JsonArray>();

    File file = directory.openNextFile();
    while (file) {
        JsonObject f = arr.createNestedObject();

        String filename = String(file.name());
        // Strip dir prefix
        if (filename.startsWith(dir)) {
            filename = filename.substring(dir.length());
            if (filename.startsWith("/")) filename = filename.substring(1);
        }

        if (file.isDirectory()) {
            // Append slash to indicate directory
            f["name"] = filename + "/";
            f["size"] = 0;
            f["isDir"] = true;
        } else {
            f["name"] = filename;
            f["size"] = file.size();
            f["isDir"] = false;
        }
        file = directory.openNextFile();
    }
    directory.close();

    String response;
    serializeJson(arr, response);
    request->send(200, "application/json", response);
}

void handleFileUpload(AsyncWebServerRequest *request) {
    if (!request->hasParam("dir", true)) {
        request->send(400, "application/json", "{\"error\":\"Missing 'dir' form field\"}");
        return;
    }
    String dir = request->getParam("dir", true)->value();

    if (!SD_MMC.exists(dir) || !SD_MMC.open(dir).isDirectory()) {
        request->send(404, "application/json", "{\"error\":\"Directory not found\"}");
        return;
    }

    request->send(200, "application/json", "{\"status\":\"Ready to upload\"}");
}
void handleRename(AsyncWebServerRequest *request) {
    if (!request->hasParam("oldname", true) || !request->hasParam("newname", true)) {
        request->send(400, "application/json", "{\"error\":\"Missing parameters\"}");
        return;
    }

    String oldName = request->getParam("oldname", true)->value();
    String newName = request->getParam("newname", true)->value();

    if (!SD_MMC.exists(oldName)) {
        request->send(404, "application/json", "{\"error\":\"Original file not found\"}");
        return;
    }

    if (SD_MMC.exists(newName)) {
        request->send(409, "application/json", "{\"error\":\"Target file already exists\"}");
        return;
    }

    if (SD_MMC.rename(oldName, newName)) {
        request->send(200, "application/json", "{\"status\":\"Rename successful\"}");
    } else {
        request->send(500, "application/json", "{\"error\":\"Rename failed\"}");
    }
}
void handleDelete(AsyncWebServerRequest *request) {
    if (!request->hasParam("filename", true)) {
        request->send(400, "application/json", "{\"error\":\"Missing 'filename' parameter\"}");
        return;
    }

    String filename = request->getParam("filename", true)->value();

    if (!SD_MMC.exists(filename)) {
        request->send(404, "application/json", "{\"error\":\"File not found\"}");
        return;
    }

    if (SD_MMC.remove(filename)) {
        request->send(200, "application/json", "{\"status\":\"Delete successful\"}");
    } else {
        request->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    }
}

std::map<AsyncWebServerRequest*, String> uploadPaths;
File uploadFile;

void onUploadHandler(AsyncWebServerRequest *request, const String& filename, size_t index,
                     uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        // Read subdir from URL query (e.g. /upload-show?subdir=MyShow)
        String subdir = "/";
        if (request->hasParam("subdir")) {
            subdir = request->getParam("subdir")->value();
        }

        // Sanitize subdir path
        if (!subdir.startsWith("/")) subdir = "/" + subdir;
        if (subdir.endsWith("/"))  subdir.remove(subdir.length() - 1);

        // Build the full SD path under /Shows
        String fullPath = "/Shows" + subdir + "/" + filename;

        // Ensure parent directory exists
        int slashPos = fullPath.lastIndexOf('/');
        if (slashPos != -1) {
            String parent = fullPath.substring(0, slashPos);
            SD_MMC.mkdir(parent);
        }

        Serial.println("[Upload] Starting upload to: " + fullPath);
        uploadFile = SD_MMC.open(fullPath, FILE_WRITE);
        if (!uploadFile) {
            Serial.println("[Upload] Failed to open: " + fullPath);
            return;
        }
        uploadPaths[request] = fullPath;
    }

    // Write chunk
    if (uploadFile) {
        uploadFile.write(data, len);
    }

    // Finalize
    if (final) {
        Serial.println("[Upload] Complete: " + uploadPaths[request]);
        uploadFile.close();
        uploadPaths.erase(request);
    }
}


void createSimpleUploadHandler(const String& mediaFolder, const char* endpoint) {
    server.on(endpoint, HTTP_POST,
        [](AsyncWebServerRequest *request) {
            request->send(200, "application/json", "{\"status\":\"Upload finished\"}");
        },
        [mediaFolder](AsyncWebServerRequest *request, const String& filename, size_t index,
                      uint8_t *data, size_t len, bool final) {

            if (index == 0) {
                String fullPath = "/" + mediaFolder + "/" + filename;
                Serial.println("[Upload] Starting upload to: " + fullPath);
                File f = SD_MMC.open(fullPath, FILE_WRITE);
                if (!f) {
                    Serial.println("[Upload] Failed to open file for writing");
                    return;
                }
                activeUploads[request] = f;
            }

            if (activeUploads.count(request)) {
                activeUploads[request].write(data, len);
                Serial.printf("[Upload] Written %u bytes to %s\n", len, filename.c_str());
            }

            if (final && activeUploads.count(request)) {
                Serial.println("[Upload] Upload complete for: " + filename);
                activeUploads[request].close();
                activeUploads.erase(request);
            }
        }
    );
}
void scanSDCardUsage() {
    cachedUsedBytes = 0;
    cachedTotalBytes = SD_MMC.cardSize();

    std::function<void(File)> sumDirectory = [&](File dir) {
        while (true) {
            File entry = dir.openNextFile();
            if (!entry) break;

            if (entry.isDirectory()) {
                sumDirectory(entry);
            } else {
                cachedUsedBytes += entry.size();
            }
            entry.close();
        }
    };

    File root = SD_MMC.open("/");
    if (root && root.isDirectory()) {
        sumDirectory(root);
        root.close();
    }

    lastScanTime = millis();
}

void handleSDInfo(AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(256);

    // Example: Get total and used bytes from SD_MMC or cached values
    uint64_t totalBytes = cachedTotalBytes > 0 ? cachedTotalBytes : (64ULL * 1024 * 1024 * 1024);
    uint64_t usedBytes = cachedUsedBytes; // You should update this periodically

    doc["total"] = totalBytes;
    doc["used"] = usedBytes;

    String output;
    serializeJson(doc, output);

    request->send(200, "application/json", output);
}
String urlencode(String str) {
    String encoded = "";
    char c;
    char bufHex[4];
    for (int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/') {
            encoded += c;
        } else if (c == ' ') {
            encoded += "%20";
        } else {
            sprintf(bufHex, "%%%02X", c);
            encoded += bufHex;
        }
    }
    return encoded;
}
void updateSDBAR() {
    uint64_t totalBytes = SD_MMC.totalBytes();
    uint64_t usedBytes = SD_MMC.usedBytes();

  if (totalBytes > 0) {
    int usage = (usedBytes * 100) / totalBytes;
    lv_bar_set_value(ui_sdbar, usage, LV_ANIM_OFF);
  } else {
    lv_bar_set_value(ui_sdbar, 0, LV_ANIM_OFF);
  }
}

// ==================== SETUP ====================

void setup() {
    LCD_Init();
    Lvgl_Init();
    Set_Backlight(90); // Set display brightness
    ui_init();         // Load the GUI
    
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== ESP32-S3 Captive Portal & SDMMC Server ===");
    // Start WiFi Access Point
    WiFi.softAP(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("WiFi AP started...");
    lv_label_set_text(ui_ssidlabel, WIFI_SSID);
    // Initialize SD card
    Serial.println("Initializing SD Card...");
    if (!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN)) {
        Serial.println("ERROR: SDMMC Pin configuration failed!");
        return;
    }
    if (!SD_MMC.begin("/sdcard",          // mount point
                      true,               // 1-bit mode
                      false,              // no format if fail
                      SDMMC_FREQ_DEFAULT, // bus frequency
                      12)) {              // ✱✱ NEW: raise file descriptor limit
        Serial.println("ERROR: SDMMC Card initialization failed.");
        return;
    }
    Serial.println("SD Card initialized successfully!");
    updateSDBAR();
    createSimpleUploadHandler("Movies", "/upload-movie");
    createSimpleUploadHandler("Music", "/upload-music");
    createSimpleUploadHandler("Books", "/upload-book");

    delay(2000); 

    if (AUTO_GENERATE_MEDIA_JSON) { //Only run if autogen is set to true.
        generateMediaJSON();
    } 

    // Start Captive DNS redirection
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    //OPDS Endpoint
    server.on("/opds/root.xml", HTTP_GET, handleOPDSRoot);
    server.on("/opds/books.xml", HTTP_GET, handleOPDSBooks);
    //.m3u playlist endpoint
    server.on("/playlist.m3u", HTTP_GET, [](AsyncWebServerRequest *request){
        String playlist = "#EXTM3U\n";
    server.on("/nomad.m3u", HTTP_GET, [](AsyncWebServerRequest *request) {
        // internally call the same code or just redirect
        request->redirect("/playlist.m3u");
    });

        // === MOVIES ===
        playlist += "# === MOVIES ===\n";
        File movieDir = SD_MMC.open("/Movies");
        if (movieDir && movieDir.isDirectory()) {
            File file = movieDir.openNextFile();
            while (file) {
                String name = file.name();
                if (!file.isDirectory() && (name.endsWith(".mp4") || name.endsWith(".mkv"))) {
                    String fullPath = String("/Movies/") + file.name();
                    playlist += "#EXTINF:-1," + String(file.name()) + "\n";
                    playlist += "http://" + WiFi.softAPIP().toString() + "/media?file=" + urlencode(fullPath) + "\n";
                }
                file = movieDir.openNextFile();
            }
        }

        // === SHOWS ===
        playlist += "# === SHOWS ===\n";
        File showsRoot = SD_MMC.open("/Shows");
        if (showsRoot && showsRoot.isDirectory()) {
            File showFolder = showsRoot.openNextFile();
            while (showFolder) {
                if (showFolder.isDirectory()) {
                    String showFolderName = String(showFolder.name());  // e.g. "GravityFalls"
                    String fullShowPath = "/Shows/" + showFolderName;

                    File episodeDir = SD_MMC.open(fullShowPath);
                    if (episodeDir && episodeDir.isDirectory()) {
                        File ep = episodeDir.openNextFile();
                        while (ep) {
                            String epName = ep.name();
                            if (!ep.isDirectory() && (epName.endsWith(".mp4") || epName.endsWith(".mkv"))) {
                                String fullPath = fullShowPath + "/" + epName;
                                playlist += "#EXTINF:-1," + epName + "\n";
                                playlist += "http://" + WiFi.softAPIP().toString() + "/media?file=" + urlencode(fullPath) + "\n";
                            }
                            ep = episodeDir.openNextFile();
                        }
                    }
                }
                showFolder = showsRoot.openNextFile();
            }
        }

    //.m3u playlist endpoint
    server.on("/playlist.m3u", HTTP_GET, [](AsyncWebServerRequest *request){
        String playlist = "#EXTM3U\n";
    server.on("/nomad.m3u", HTTP_GET, [](AsyncWebServerRequest *request) {
        // internally call the same code or just redirect
        request->redirect("/playlist.m3u");
    });

        // === MOVIES ===
        playlist += "# === MOVIES ===\n";
        File movieDir = SD_MMC.open("/Movies");
        if (movieDir && movieDir.isDirectory()) {
            File file = movieDir.openNextFile();
            while (file) {
                String name = file.name();
                if (!file.isDirectory() && (name.endsWith(".mp4") || name.endsWith(".mkv"))) {
                    String fullPath = String("/Movies/") + file.name();
                    playlist += "#EXTINF:-1," + String(file.name()) + "\n";
                    playlist += "http://" + WiFi.softAPIP().toString() + "/media?file=" + urlencode(fullPath) + "\n";
                }
                file = movieDir.openNextFile();
            }
        }

        // === SHOWS ===
        playlist += "# === SHOWS ===\n";
        File showsRoot = SD_MMC.open("/Shows");
        if (showsRoot && showsRoot.isDirectory()) {
            File showFolder = showsRoot.openNextFile();
            while (showFolder) {
                if (showFolder.isDirectory()) {
                    String showFolderName = String(showFolder.name());  // e.g. "GravityFalls"
                    String fullShowPath = "/Shows/" + showFolderName;

                    File episodeDir = SD_MMC.open(fullShowPath);
                    if (episodeDir && episodeDir.isDirectory()) {
                        File ep = episodeDir.openNextFile();
                        while (ep) {
                            String epName = ep.name();
                            if (!ep.isDirectory() && (epName.endsWith(".mp4") || epName.endsWith(".mkv"))) {
                                String fullPath = fullShowPath + "/" + epName;
                                playlist += "#EXTINF:-1," + epName + "\n";
                                playlist += "http://" + WiFi.softAPIP().toString() + "/media?file=" + urlencode(fullPath) + "\n";
                            }
                            ep = episodeDir.openNextFile();
                        }
                    }
                }
                showFolder = showsRoot.openNextFile();
            }
        }


        // === MUSIC ===
        playlist += "# === MUSIC ===\n";
        File musicDir = SD_MMC.open("/Music");
        if (musicDir && musicDir.isDirectory()) {
            File file = musicDir.openNextFile();
            while (file) {
                String name = file.name();
                if (!file.isDirectory() && name.endsWith(".mp3")) {
                    String fullPath = String("/Music/") + file.name();
                    playlist += "#EXTINF:-1," + String(file.name()) + "\n";
                    playlist += "http://" + WiFi.softAPIP().toString() + "/media?file=" + urlencode(fullPath) + "\n";
                }
                file = musicDir.openNextFile();
            }
        }

        request->send(200, "audio/x-mpegurl", playlist);
    });

    //fAKE dlna dISCOVERY
    server.on("/dlna/device.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media Server</friendlyName>
            <manufacturer>Jcorp</manufacturer>
            <modelName>Nomad DLNA</modelName>
            <UDN>uuid:ESP32-DLNA-NOMAD</UDN>
          </device>
        </root>
      )rawliteral");
    });
    server.on("/ssdp/device-desc.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media Server</friendlyName>
            <manufacturer>Jcorp</manufacturer>
            <modelName>Nomad</modelName>
            <modelNumber>1</modelNumber>
            <UDN>uuid:ESP32-DLNA-FAKE-1234</UDN>
          </device>
        </root>
      )rawliteral");
    });
    server.on("/dlna/description.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media</friendlyName>
            <manufacturer>JCorp</manufacturer>
            <modelName>ESP32-Nomad</modelName>
            <UDN>uuid:nomad-dlna-esp32</UDN>
          </device>
        </root>
      )rawliteral");
    });
    server.on("/description.xml", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
        response->addHeader("Application-URL", "http://" + WiFi.softAPIP().toString() + "/dlna/");
        response->addHeader("Location", "/dlna/desc.xml");  // HTTP redirect target
        request->send(response);
    });

        // === MUSIC ===
        playlist += "# === MUSIC ===\n";
        File musicDir = SD_MMC.open("/Music");
        if (musicDir && musicDir.isDirectory()) {
            File file = musicDir.openNextFile();
            while (file) {
                String name = file.name();
                if (!file.isDirectory() && name.endsWith(".mp3")) {
                    String fullPath = String("/Music/") + file.name();
                    playlist += "#EXTINF:-1," + String(file.name()) + "\n";
                    playlist += "http://" + WiFi.softAPIP().toString() + "/media?file=" + urlencode(fullPath) + "\n";
                }
                file = musicDir.openNextFile();
            }
        }

        request->send(200, "audio/x-mpegurl", playlist);
    });

    //fAKE dlna dISCOVERY
    server.on("/dlna/device.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media Server</friendlyName>
            <manufacturer>Jcorp</manufacturer>
            <modelName>Nomad DLNA</modelName>
            <UDN>uuid:ESP32-DLNA-NOMAD</UDN>
          </device>
        </root>
      )rawliteral");
    });
    server.on("/ssdp/device-desc.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media Server</friendlyName>
            <manufacturer>Jcorp</manufacturer>
            <modelName>Nomad</modelName>
            <modelNumber>1</modelNumber>
            <UDN>uuid:ESP32-DLNA-FAKE-1234</UDN>
          </device>
        </root>
      )rawliteral");
    });
    server.on("/dlna/description.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media</friendlyName>
            <manufacturer>Jcorp</manufacturer>
            <modelName>ESP32-Nomad</modelName>
            <UDN>uuid:nomad-dlna-esp32</UDN>
          </device>
        </root>
      )rawliteral");
    });
    server.on("/description.xml", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
        response->addHeader("Application-URL", "http://" + WiFi.softAPIP().toString() + "/dlna/");
        response->addHeader("Location", "/dlna/desc.xml");  // HTTP redirect target
        request->send(response);
    });

    
    // Set LED mode: solid (0), rainbow (1), etc.
    server.on("/led/onoff", HTTP_POST, [](AsyncWebServerRequest *request){},
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        StaticJsonDocument<64> doc;
        DeserializationError err = deserializeJson(doc, data);
        if (err) {
          request->send(400, "text/plain", "Invalid JSON");
          return;
        }
        bool enabled = doc["enabled"];
        RGB_SetMode(enabled ? 1 : 0);
        request->send(200, "text/plain", "LED toggled");
      }
    );

    // /led/rainbow - Start rainbow loop
    server.on("/led/rainbow", HTTP_POST, [](AsyncWebServerRequest *request) {
      RGB_SetMode(1);  // Rainbow loop
      request->send(200, "text/plain", "Rainbow mode activated");
    });

    // /led/color - Set solid color from JSON { color: "#rrggbb" }
    server.on("/led/color", HTTP_POST, [](AsyncWebServerRequest *request){},
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        StaticJsonDocument<64> doc;
        DeserializationError err = deserializeJson(doc, data);
        if (err) {
          request->send(400, "text/plain", "Invalid JSON");
          return;
        }

        const char* hex = doc["color"];
        if (!hex || strlen(hex) != 7 || hex[0] != '#') {
          request->send(400, "text/plain", "Invalid color format");
          return;
        }
        // Parse "#rrggbb"
        char rs[3] = { hex[1], hex[2], 0 };
        char gs[3] = { hex[3], hex[4], 0 };
        char bs[3] = { hex[5], hex[6], 0 };

        uint8_t r = strtol(rs, NULL, 16);
        uint8_t g = strtol(gs, NULL, 16);
        uint8_t b = strtol(bs, NULL, 16);
        RGB_SetColor(r, g, b);
        request->send(200, "text/plain", "Color set");
      }
    );

    server.on("/sdinfo", HTTP_GET, handleSDInfo);
    server.on("/generate-media", HTTP_POST, [](AsyncWebServerRequest *request){
      generateMediaJSON(); 
      request->send(200, "text/plain", "Done");
    });

    // Captive Portal: Redirect all unknown requests
    server.onNotFound([](AsyncWebServerRequest *request) {
        if (request->hasHeader("User-Agent")) {
            String userAgent = request->header("User-Agent");
            Serial.println("User-Agent: " + userAgent);

            if (userAgent.indexOf("iPhone") >= 0 || userAgent.indexOf("iPad") >= 0 || userAgent.indexOf("Macintosh") >= 0) {
                Serial.println("Apple device detected. Serving appleindex.html");
                request->send(SD_MMC, "/appleindex.html", "text/html");
                return;
            }
        }
        Serial.println("Android device. Serving index.html");
        request->send(SD_MMC, "/index.html", "text/html");
    });
    // Captive triggers for Apple & Android devices
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Apple captive portal request detected, serving appleindex.html");
        request->send(SD_MMC, "/appleindex.html", "text/html");
    });
    
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Android/NORMAL captive portal request detected, serving index.html");
        request->send(SD_MMC, "/index.html", "text/html");
    });
    server.on("/dlna/desc.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media Server</friendlyName>
            <manufacturer>JCorp</manufacturer>
            <modelName>Nomad</modelName>
            <UDN>uuid:ESP32-DLNA-FAKE-1234</UDN>
          </device>
        </root>
      )rawliteral");
    });

    server.on("/dlna/contentdir.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      String xml = "<?xml version=\"1.0\"?><ContentDirectory>";
      File root = SD_MMC.open("/Movies");
      if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
          if (!file.isDirectory()) {
            String name = file.name();
            xml += "<item>";
            xml += "<title>" + name + "</title>";
            xml += "<res protocolInfo=\"http-get:*:video/mp4:*\">";
            xml += "http://192.168.4.1/Movies/" + name + "</res>";
            xml += "</item>";
          }
          file = root.openNextFile();
        }
      }
      xml += "</ContentDirectory>";
      request->send(200, "text/xml", xml);
    });
    server.on("/listfiles", HTTP_GET, handleListFiles);


    // Static HTML routes
    server.serveStatic("/movies.html", SD_MMC, "/movies.html");
    server.serveStatic("/music.html", SD_MMC, "/music.html");
    server.serveStatic("/books.html", SD_MMC, "/books.html");
    server.serveStatic("/shows.html", SD_MMC, "/shows.html");
    server.serveStatic("/admin.html", SD_MMC, "/admin.html");
    server.serveStatic("/games.html", SD_MMC, "/games.html");
    server.serveStatic("/maps.html", SD_MMC, "/maps.html");
    server.serveStatic("/menu.html", SD_MMC, "/menu.html");
    server.serveStatic("/movies", SD_MMC, "/movies.html");
    server.serveStatic("/music",  SD_MMC, "/music.html");
    server.serveStatic("/books",  SD_MMC, "/books.html");
    server.serveStatic("/shows",  SD_MMC, "/shows.html");
    server.serveStatic("/admin",  SD_MMC, "/admin.html");
    server.serveStatic("/games",  SD_MMC, "/games.html");
    server.serveStatic("/maps",   SD_MMC, "/maps.html");
    server.serveStatic("/menu",   SD_MMC, "/menu.html");
    // Serve root directory and default to index.html
    server.serveStatic("/", SD_MMC, "/").setDefaultFile("index.html");

    // Endpoint for video/audio streaming
    server.on("/media", HTTP_GET, handleRangeRequest);

    // convenience redirect:  /opds  →  /opds/root.xml
    server.on("/opds", HTTP_GET, [](AsyncWebServerRequest *req){
        req->redirect("/opds/root.xml");
    });

    server.on("/upload-show", HTTP_POST,
      // 1) immediate responder:
      [](AsyncWebServerRequest *request) {
          request->send(200, "application/json", "{\"status\":\"Upload finished\"}");
      },
      // 2) upload handler:
      [](AsyncWebServerRequest *request, const String& filename, size_t index,
        uint8_t *data, size_t len, bool final) {
          static std::map<AsyncWebServerRequest*, File> showUploads;

          if (index == 0) {
              // Read subdir from query string:
              String subdir = "/";
              if (request->hasParam("subdir")) {
                  subdir = request->getParam("subdir")->value();
              }
              // Sanitize:
              if (!subdir.startsWith("/")) subdir = "/" + subdir;
              if (subdir.endsWith("/"))  subdir.remove(subdir.length() - 1);

              String fullPath = "/Shows" + subdir + "/" + filename;
              int slashPos = fullPath.lastIndexOf('/');
              if (slashPos != -1) {
                  SD_MMC.mkdir(fullPath.substring(0, slashPos));
              }

              Serial.println("[Upload] Starting upload to: " + fullPath);
              File f = SD_MMC.open(fullPath, FILE_WRITE);
              if (!f) {
                  Serial.println("[Upload] Failed to open file for writing");
                  return;
              }
              showUploads[request] = f;
          }

          if (showUploads.count(request)) {
              showUploads[request].write(data, len);
          }

          if (final && showUploads.count(request)) {
              Serial.println("[Upload] Upload complete for show episode.");
              showUploads[request].close();
              showUploads.erase(request);
          }
      }
    );
server.on("/mkdir", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("dirname", true)) {
        request->send(400, "text/plain", "Missing dirname");
        return;
    }

    String dirName = request->getParam("dirname", true)->value();

    // Sanitize directory name: prevent absolute paths
    if (dirName.startsWith("/")) dirName = dirName.substring(1);

    // Build full path under /Shows
    String fullPath = "/Shows/" + dirName;

    if (SD_MMC.exists(fullPath)) {
        request->send(409, "text/plain", "Directory already exists");
        return;
    }

    if (SD_MMC.mkdir(fullPath)) {
        request->send(200, "text/plain", "OK");
    } else {
        request->send(500, "text/plain", "Failed to create directory");
    }
    });

static std::map<AsyncWebServerRequest*, String> uploadPaths;
server.on("/rename", HTTP_POST, handleRename);
server.on("/delete", HTTP_POST, handleDelete);
// Start the web server
server.begin();
updateToggleStatus(); // Reflect initial WiFi and SD status
Serial.println("Web Server started!");
}

// ==================== MAIN LOOP ====================

void loop() {
    dnsServer.processNextRequest();
    Timer_Loop();

    if (currentLEDMode == 1) {
        RGB_Lamp_Loop(2);
    }
    if (sdErrorFlag) {                                             // ✱✱ NEW / RECOVERY ✱✱
        if (millis() > sdErrorCooldownUntil && tryRecoverSDCard()) {
            sdErrorFlag = false;  // we’re back in business
        } else {
            delay(5);  // keep feeding WDT while waiting
            return;    // skip rest of loop until recovery works
        }
    }
    // Update UI toggle status every second
    if (millis() - lastUpdateTime > 1000) {
        updateToggleStatus();
        lastUpdateTime = millis();
    }

    // Scan SD usage in background every 60 seconds
    if (millis() - lastSDScanTime > SD_SCAN_INTERVAL) {
        scanSDCardUsage();  // Safe, non-blocking recursive function
        lastSDScanTime = millis();
        
    }

    delay(5); // Prevent watchdog starvation
    updateClientCount();
}


void RGB_SetMode(uint8_t mode) {
    currentLEDMode = mode;

    if (mode == 0) {
        // Turn off LED immediately
        Set_Color(0, 0, 0);
    } else if (mode == 2) {
        Set_Color(solidG, solidR, solidB);
    }
}