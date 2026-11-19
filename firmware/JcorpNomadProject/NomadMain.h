// NomadMain.h - Nomad's main screen: the SquareLine page (storage bar, "Connect to:", the network
// in its pill, "Connected Users:", the tree with the count on it, the WiFi and card switches),
// drawn with the status pages' type and icons, and with a QR code in the tree's place when the
// setting is on (the count then sits between the two icons at the bottom).
//
// LVGL only (no Arduino), so tools/screen/preview can draw it. Call from the task that owns LVGL.
#pragma once
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// After ui_init(): restyle the page and build the QR parts (hidden).
void mainInit(void);
// Phones on the hotspot.
void mainSetCount(int n);
// The card has no room: the storage bar turns red with "Card full" on it.
void mainSetCardFull(int full);
// The QR code's text (a WIFI: join string, or Nomad's address in WiFi Mode), or NULL for the tree.
void mainSetQr(const char *payload);

// "WIFI:T:WPA;S:<name>;P:<password>;;" with the characters the format reserves escaped;
// an empty password gives an open network. Returns false if it doesn't fit.
int mainWifiQr(char *out, unsigned outLen, const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
