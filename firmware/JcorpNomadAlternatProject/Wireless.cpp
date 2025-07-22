#include "Wireless.h"


bool WIFI_Connection = 0;
uint8_t WIFI_NUM = 0;
uint8_t BLE_NUM = 0;
bool Scan_finish = 0;
int wifi_scan_number()
{
  printf("/**********WiFi Test**********/\r\n");
  // Set WiFi to station mode and disconnect from an AP if it was previously connected.
  WiFi.mode(WIFI_STA);                           
  WiFi.setSleep(true);     
  // WiFi.scanNetworks will return the number of networks found.
  int count = WiFi.scanNetworks();
  if (count == 0)
  {
    printf("No WIFI device was scanned\r\n");
  }
  else{
    printf("Scanned %d Wi-Fi devices\r\n",count);
  }
  
  // Delete the scan result to free memory for code below.
  WiFi.disconnect(true);
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);  
  vTaskDelay(100);           
  printf("/*******WiFi Test Over********/\r\n\r\n");
  return count;
}
int ble_scan_number()
{
  printf("/**********BLE Test**********/\r\n"); 
  BLEDevice::init("ESP32");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);

  int count = 0;
#if ((ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)) && defined(CONFIG_SENSORLIB_ESP_IDF_NEW_API))       // * Using the new API of esp-idf 5.x
  BLEScanResults foundDevices = pBLEScan->start(5);  
  count = foundDevices.getCount(); 
#else //ESP 4.X
  BLEScanResults *foundDevices = pBLEScan->start(5);  
  count = foundDevices->getCount();
#endif //ESP 5.X
  if (count == 0)
  {
    printf("No Bluetooth device was scanned\r\n");
  }
  else{
    printf("Scanned %d Bluetooth devices\r\n",count);
  }
  pBLEScan->stop(); 
  pBLEScan->clearResults(); 
  BLEDevice::deinit(true); 
  vTaskDelay(100);         
  printf("/**********BLE Test Over**********/\r\n\r\n");
  return count;
}
extern char buffer[128];    /* Make sure buffer is enough for `sprintf` */
void Wireless_Test1(){
  // BLE_NUM = ble_scan_number();                       // !!! Please note that continuing to use Bluetooth will result in allocation failure due to RAM usage, so pay attention to RAM usage when Bluetooth is turned on
  WIFI_NUM = wifi_scan_number();
  Scan_finish = 1;
}

void WirelessScanTask(void *parameter) {
  BLE_NUM = ble_scan_number();                       // !!! Please note that continuing to use Bluetooth will result in allocation failure due to RAM usage, so pay attention to RAM usage when Bluetooth is turned on
  WIFI_NUM = wifi_scan_number();
  Scan_finish = 1;
  vTaskDelay(pdMS_TO_TICKS(1000));
  vTaskDelete(NULL);
}
void Wireless_Test2(){
  xTaskCreatePinnedToCore(
    WirelessScanTask,    
    "WirelessScanTask",   
    4096,                
    NULL,                 
    2,                   
    NULL,                 
    0                   
  );
}
