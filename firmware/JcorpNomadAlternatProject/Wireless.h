#pragma once
#include "WiFi.h"
#include <BLEDevice.h>
#include <BLEScan.h>

extern bool WIFI_Connection;
extern uint8_t WIFI_NUM;
extern uint8_t BLE_NUM;
extern bool Scan_finish;

// int wifi_scan_number();
// int ble_scan_number();
// void Wireless_Test1();
void Wireless_Test2();