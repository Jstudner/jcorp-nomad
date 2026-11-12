// NomadFirmwareKey.h - the public half of the key nomadflash signs firmware with.
// Over-the-air updates (NomadFirmware.h) install only firmware signed with the matching private
// key, which lives on the build machine and nowhere else. Until a key is made this is empty and
// every over-the-air update is refused. USB flashing still works.
// Commit it, it's public.
#ifndef NOMAD_FIRMWARE_KEY_H
#define NOMAD_FIRMWARE_KEY_H
#include <stddef.h>
static const unsigned char NOMAD_FW_PUBKEY[] = { 0 };
static const size_t NOMAD_FW_PUBKEY_LEN = 0;
#endif
