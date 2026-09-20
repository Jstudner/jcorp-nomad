// NomadDNS.h - captive DNS for the hotspot, replacing the stock wildcard hijack.
//
// The old DNSServer answered EVERY lookup with Nomad's address, so every app on a
// connected phone "resolved" fine and then hung on a dead TCP connect. Messages
// and searches that could have gone over mobile data just timed out. This
// responder only answers for the OS captive-portal check hostnames (so the
// sign-in popup still appears) and Nomad's own names; everything else gets an
// instant NXDOMAIN, which is what makes phones flag the network "no internet"
// and keep routing real traffic over cellular.
//
// Never started in WiFi Mode (station) - the router owns DNS there.

#ifndef NOMAD_DNS_H
#define NOMAD_DNS_H

#include <IPAddress.h>

// bind :53 and answer per policy; call when the AP is up
void nomadDnsStart(IPAddress selfIp);
void nomadDnsStop();

// legacy behavior: answer every name with Nomad's address (old DNSServer hijack).
// escape hatch for a device whose captive check we don't know about.
void nomadDnsSetCatchAll(bool on);

#endif  // NOMAD_DNS_H
