#pragma once
#include "WiFi.h"

#ifndef NOMAD_ENABLE_BLE_SCAN
#define NOMAD_ENABLE_BLE_SCAN 0
#endif

#if NOMAD_ENABLE_BLE_SCAN
#include <BLEDevice.h>
#include <BLEScan.h>
#endif

extern bool WIFI_Connection;
extern uint8_t WIFI_NUM;
extern uint8_t BLE_NUM;
extern bool Scan_finish;

// int wifi_scan_number();
// int ble_scan_number();
// void Wireless_Test1();
void Wireless_Test2();
