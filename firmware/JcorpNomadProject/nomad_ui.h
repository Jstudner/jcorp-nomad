// nomad_ui.h - board independent on-device UI API.
//
// The firmware talks to the screen through these calls instead of poking LVGL
// objects directly, so the 172x320 SquareLine layout and the 160x80 dongle
// layout can look completely different without the main sketch caring.

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// LCD + LVGL + screen construction. Safe to call repeatedly.
void NomadUI_Init(void);

// Full-screen status message overlay ("Booting...", "USB Mass-Storage Mode",
// "Shutting Down...", indexing progress, and so on).
void NomadUI_Message(const char *text);
void NomadUI_ClearMessage(void);

// Live values.
void NomadUI_SetSSID(const char *ssid);
void NomadUI_SetIP(const char *ip);
void NomadUI_SetUsers(int count);
void NomadUI_SetSdUsage(int percent, uint64_t usedBytes, uint64_t totalBytes);
void NomadUI_SetWifiOk(bool ok);
void NomadUI_SetSdOk(bool ok);
void NomadUI_SetSysStats(uint32_t freeHeapKB, uint32_t freePsramKB, float tempC, uint32_t uptimeSec);

// Page cycling (short press on the boot button). The 172x320 layout only has
// one page, so these are no-ops there.
void NomadUI_NextPage(void);
int  NomadUI_GetPage(void);
int  NomadUI_PageCount(void);

// Push one LVGL frame out immediately - used on the paths that are about to
// reboot or block, where the normal LVGL task will not get another turn.
void NomadUI_Flush(void);

#ifdef __cplusplus
}
#endif
