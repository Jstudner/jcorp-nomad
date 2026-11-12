// NomadDNS.h - captive DNS for the hotspot.
//
// Answering every lookup with Nomad's address makes every app on a connected phone
// "resolve" fine and then hang on a dead TCP connect, so messages and searches that
// could go over mobile data just time out. This only answers for the OS captive-portal
// check hostnames (so the sign-in popup still appears) and Nomad's own names.
// Everything else gets an instant NXDOMAIN, which makes phones flag the network
// "no internet" and keep routing real traffic over cellular.
//
// Never started in WiFi Mode (station), the router owns DNS there.

#ifndef NOMAD_DNS_H
#define NOMAD_DNS_H

#include <IPAddress.h>
#include <WString.h>

// bind :53 and answer per policy; call when the AP is up
void nomadDnsStart(IPAddress selfIp);
void nomadDnsStop();

// wildcard mode: answer every name with Nomad's address.
// escape hatch for a device whose captive check we don't know about.
void nomadDnsSetCatchAll(bool on);

// How many queries have reached us, and the last name asked for. A responder that
// never gets a query looks exactly like one that's answering wrong, so this tells
// them apart.
void nomadDnsStats(uint32_t &queries, uint32_t &answered, uint32_t &nxdomain, String &lastName);

#endif  // NOMAD_DNS_H
