# Zstandard decoder

The decoder half of [Zstandard 1.5.7](https://github.com/facebook/zstd/releases/tag/v1.5.7),
for NomadZimCore.h (ZIM clusters are zstd). BSD licence, `LICENSE` here.

- `zstddeclib_impl.h` is the release's `build/single_file_libs/zstddeclib-in.c` with every
  `#include "..."` from `lib/` written in place, each file once, `legacy/zstd_legacy.h`
  left out: what the release's own `combine.py` produces. Nothing in it was edited.
  `zstd_decoder.c` builds it (at -O2: the sketch is -Os); it is a .h so the build does
  not compile it a second time on its own.
- `zstd.h`, `zstd_errors.h` are the release's `lib/` headers, unchanged.
- Release tarball SHA-256 (matches the `.sha256` published beside it):
  `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`
- Built with its own settings: no legacy formats, no assembly, error strings stripped.
  About 100 KB of code.
