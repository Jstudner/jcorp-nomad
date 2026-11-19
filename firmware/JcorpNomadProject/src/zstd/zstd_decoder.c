/* zstd_decoder.c - builds the Zstandard decoder (zstddeclib_impl.h, see README.md) for speed.
 * The Arduino build is -Os for the whole sketch; the decoder unpacks every archive page
 * on Nomad, and size-optimised it runs much slower. */
#pragma GCC optimize ("O2")
#include "zstddeclib_impl.h"
