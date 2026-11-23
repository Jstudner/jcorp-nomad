# XZ Embedded

[XZ Embedded](https://github.com/tukaani-project/xz-embedded) v2024-12-30, the small xz
decoder the Linux kernel uses, for archives whose clusters are xz (ZIMs made before 2021).
0BSD licence, `COPYING` here. Tarball SHA-256
`ee12fa8c49c9c0ef4a144af4234d2530d786c1ce14247a7d5fc92a946628977d`.

- `xz.h` is `linux/include/linux/xz.h`; `xz_config.h` is `userspace/xz_config.h`;
  `xz_private.h`, `xz_stream.h`, `xz_lzma2.h` are from `linux/lib/xz/`. Unchanged.
- `xz_crc32_impl.h`, `xz_crc64_impl.h`, `xz_dec_lzma2_impl.h`, `xz_dec_stream_impl.h` are
  the `.c` files of those names, unchanged, renamed so the build compiles them once, through
  `xz_nomad.c`, which sets the options (single-call mode, any check, -O2) and supplies
  CRC32 from the ESP32's ROM (the tables are 3 KB of internal RAM Nomad can't spare).
  `xz_crc32_impl.h` and `xz_crc64_impl.h` are kept for reference but not built.
