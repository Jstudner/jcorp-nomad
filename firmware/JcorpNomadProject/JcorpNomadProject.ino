// Core ESP32 + WiFi Libraries
#include "Arduino.h"
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "FS.h"
#include <SD.h>
#include "DNSServer.h"
#include <ArduinoJson.h>


// Waveshare Display and UI Libraries
#include "Display_ST7789.h"
#include "LVGL_Driver.h"
#include "ui.h"
#include "RGB_lamp.h"

// SPIFFS support and ESP WiFi control
#include <SPIFFS.h>
#include "esp_wifi.h"

// ==================== CONFIGURATION ====================

// WiFi network name and password (this is the hotspot the ESP32 creates)
#define WIFI_SSID "Jcorp_Nomad"
// Password MUST be at least 8 characters, must not be more than 63 characters
#define WIFI_PASSWORD "password"

// Max number of devices that can connect at once
#define MAX_CLIENTS 8

// SD card pinout for Waveshare ESP32-S3
#define SD_MISO 19
#define SD_MOSI 23
#define SD_SCK  18
#define SD_CS   5

// Captive portal DNS setup
const byte DNS_PORT = 53;
DNSServer dnsServer;
AsyncWebServer server(80); // Web server on port 80

int connectedClients = 0;

// Update the UI with the number of connected users
void updateUI(int userCount) {
    char buffer[10];
    snprintf(buffer, sizeof(buffer), "%d", userCount);
    lv_label_set_text(ui_uiuserlabel, buffer);
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


// ==================== MEDIA STREAM HANDLER ====================

// Handles video/audio streaming via range requests
void handleRangeRequest(AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
        request->send(400, "text/plain", "File parameter missing");
        return;
    }

    String filePath = request->getParam("file")->value();
    if (!SD_MMC.exists(filePath)) {
        request->send(404, "text/plain", "File not found");
        return;
    }

    File file = SD_MMC.open(filePath, "r");
    size_t fileSize = file.size();

    Serial.println("=== Range Request Debug ===");
    Serial.println("File requested: " + filePath);
    Serial.println("File size: " + String(fileSize));

    if (request->method() == HTTP_HEAD) {
        Serial.println("HEAD request received");
        AsyncWebServerResponse *headResponse = request->beginResponse(200, "application/octet-stream", "");
        headResponse->addHeader("Content-Length", String(fileSize));
        request->send(headResponse);
        file.close();
        return;
    }

    // Handle "Range" header to support seeking and chunked media playback
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

    AsyncWebServerResponse *response = request->beginResponse("video/mp4", contentLength,
        [file, startByte, endByte, contentLength](uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t {
            file.seek(startByte + index);
            size_t bytesToRead = min(maxLen, endByte - (startByte + index) + 1);
            size_t bytesRead = file.read(buffer, bytesToRead);
            if (index + bytesRead >= contentLength) {
                file.close();
            }
            return bytesRead;
        }
    );

    response->addHeader("Content-Type", "video/mp4");
    response->addHeader("Accept-Ranges", "bytes");
    response->addHeader("Content-Range", "bytes " + String(startByte) + "-" + String(endByte) + "/" + String(fileSize));
    response->addHeader("Cache-Control", "no-cache");
    response->addHeader("Pragma", "no-cache");
    response->setCode(206); // Partial Content
    request->send(response);
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

    // Initialize VSPI for SD card
    Serial.println("Initializing SD Card with VSPI...");
    SPIClass spiSD(VSPI);
    spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    
    if (!SD.begin(SD_CS, spiSD)) {
        Serial.println("ERROR: SD Card initialization failed.");
        return;
    }
    Serial.println("SD Card initialized successfully!");

    delay(2000); 

    generateMediaJSON();  

    // Start Captive DNS redirection
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

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
        Serial.println("Non-Apple device. Serving index.html");
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
      File root = SD.open("/Movies");
      if (root && root.isDirectory()) {
        if (root) {
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


    // Static HTML routes
    server.serveStatic("/movies.html", SD_MMC, "/movies.html");
    server.serveStatic("/music.html", SD_MMC, "/music.html");
    server.serveStatic("/books.html", SD_MMC, "/books.html");
    server.serveStatic("/shows.html", SD_MMC, "/shows.html");

    // Serve root directory and default to index.html
    server.serveStatic("/", SD, "/").setDefaultFile("index.html");

    // Endpoint for video/audio streaming
    server.on("/media", HTTP_GET, [](AsyncWebServerRequest *request) {
        String path = request->url();
        if (path.startsWith("/media")) {
            path = path.substring(6); // Remove "/media"
        }
        
        if (SD.exists(path)) {
            File file = SD.open(path);
            if (file) {
                request->send(SD, path);
                file.close();
            } else {
                request->send(404);
            }
        } else {
            request->send(404);
        }
    });

    // Start the web server
    server.begin();
    Serial.println("Web Server started!");
}

// ==================== MAIN LOOP ====================

void loop() {
    dnsServer.processNextRequest(); // Keeps captive DNS alive
    Timer_Loop();                   // Handles UI timing events
    RGB_Lamp_Loop(2);               // Optional LED effect loop
    delay(5);                       // Slight delay

    updateClientCount();           // Refresh user count
}
