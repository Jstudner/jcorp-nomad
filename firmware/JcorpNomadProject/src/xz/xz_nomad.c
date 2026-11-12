/* xz_nomad.c - builds XZ Embedded (see README.md) for the archive server: archives made
 * before 2021 compress their clusters with xz. Single-call mode only (the whole cluster in
 * one go, the output buffer doubling as the dictionary, so no 64 MB dictionary to hold).
 *
 * Internal RAM: XZ Embedded's own CRC tables are 3 KB of .bss, which kept the Nomad's
 * internal DMA memory under the media handler's 48 KB floor and every video was refused.
 * CRC32 is the ESP32's ROM routine instead (same contract: 0 to start, the last result to
 * go on), and CRC64-checked streams are decoded without checking (XZ_DEC_ANY_CHECK). */
#pragma GCC optimize ("O2")
#define XZ_DEC_SINGLE
#define XZ_DEC_ANY_CHECK
#define XZ_INTERNAL_CRC32 0
#include "xz_config.h"
#include "esp_rom_crc.h"

uint32_t xz_crc32(const uint8_t *buf, size_t size, uint32_t crc) {
  return esp_rom_crc32_le(crc, buf, (uint32_t)size);
}

#include "xz_dec_lzma2_impl.h"
#include "xz_dec_stream_impl.h"
