// NomadEth.cpp - see NomadEth.h.

#include "NomadEth.h"
#include "nomad_board.h"

bool nomadEthSupported() {
#if NOMAD_HAS_ETHERNET
  return true;
#else
  return false;
#endif
}

#if !NOMAD_HAS_ETHERNET

bool nomadEthBegin() { return false; }
bool nomadEthLinkUp() { return false; }
bool nomadEthHasIP() { return false; }
IPAddress nomadEthIP() { return IPAddress((uint32_t)0); }
const char *nomadEthState() { return "absent"; }
uint16_t nomadEthSpeed() { return 0; }
bool nomadEthFullDuplex() { return false; }
uint32_t nomadEthLinkUps() { return 0; }
uint32_t nomadEthLinkDowns() { return 0; }
void nomadEthReport(const char *) {}

#else  // NOMAD_HAS_ETHERNET

#include <ETH.h>
#include <Network.h>

static bool     s_started  = false;
static volatile bool s_link = false;
static volatile bool s_gotIp = false;
static volatile uint32_t s_ups = 0;
static volatile uint32_t s_downs = 0;

// Link state comes from events. Don't poll the PHY over SMI from a web handler: it can
// block, and polling the C6 for its client list that way crashed the host.
static void ethEvent(arduino_event_id_t event, arduino_event_info_t info) {
  (void)info;
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("[ETH] MAC and PHY started");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      s_link = true;
      s_ups++;
      Serial.println("[ETH] link up");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      s_gotIp = true;
      // Whichever interface has a real upstream becomes the default route. The AP keeps
      // serving its own subnet either way, and a pulled cable hands the default back.
      ETH.setDefault();
      Serial.printf("[ETH] %u Mbps %s duplex, address %s - now the default route\n",
                    (unsigned)ETH.linkSpeed(),
                    ETH.fullDuplex() ? "full" : "half",
                    ETH.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      s_gotIp = false;
      Serial.println("[ETH] lease lost");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      if (s_link) s_downs++;
      s_link = false;
      s_gotIp = false;
      Serial.println("[ETH] link down (cable out, or the switch port went away)");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      s_link = false;
      s_gotIp = false;
      Serial.println("[ETH] stopped");
      break;
    default:
      break;
  }
}

bool nomadEthBegin() {
  if (s_started) return true;

  Network.onEvent(ethEvent);

  // Pins come from nomad_board.h and match the core's esp32p4 variant (the RMII data pins
  // are IOMUX-fixed). ETH_PHY_IP101 is an alias of ETH_PHY_TLK110, same driver.
  if (!ETH.begin(ETH_PHY_IP101, NOMAD_ETH_PHY_ADDR, NOMAD_ETH_MDC, NOMAD_ETH_MDIO,
                 NOMAD_ETH_POWER, EMAC_CLK_EXT_IN)) {
    Serial.println("[ETH] the wired NIC would not start; carrying on without it");
    return false;
  }

  s_started = true;
  Serial.println("[ETH] waiting for a cable (the hotspot does not depend on this)");
  return true;
}

bool nomadEthLinkUp() { return s_link; }

bool nomadEthHasIP() {
  if (!s_gotIp) return false;
  return (uint32_t)ETH.localIP() != 0;
}

IPAddress nomadEthIP() {
  if (!s_started) return IPAddress((uint32_t)0);
  return ETH.localIP();
}

const char *nomadEthState() {
  if (!s_started) return "absent";
  if (nomadEthHasIP()) return "ready";
  if (s_link) return "link";
  return "down";
}

uint16_t nomadEthSpeed() { return s_link ? ETH.linkSpeed() : 0; }
bool nomadEthFullDuplex() { return s_link ? ETH.fullDuplex() : false; }
uint32_t nomadEthLinkUps() { return s_ups; }
uint32_t nomadEthLinkDowns() { return s_downs; }

void nomadEthReport(const char *when) {
  if (!s_started) {
    Serial.printf("[ETH] (%s) not started\n", when);
    return;
  }
  if (nomadEthHasIP()) {
    Serial.printf("[ETH] (%s) on the LAN at %s, %u Mbps %s duplex\n", when,
                  ETH.localIP().toString().c_str(), (unsigned)ETH.linkSpeed(),
                  ETH.fullDuplex() ? "full" : "half");
  } else if (s_link) {
    Serial.printf("[ETH] (%s) link up, still waiting on DHCP\n", when);
  } else {
    Serial.printf("[ETH] (%s) no cable\n", when);
  }
}

#endif  // NOMAD_HAS_ETHERNET
