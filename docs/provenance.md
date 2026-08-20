# Provenance

This is a standalone Android repository. `Producdevity/cod-boz-port` is used as
a read-only behavioral reference and is not a build dependency. The original
port was compared against reference revision `1bce4caf`; the local reference
tree was last compared at `40be02ad`.

- **S3E image and relocation parsing:** `src/s3e_image.c`, rewritten around
  checked C++ readers and 32-bit guest addresses.
- **APK asset and LZMA handling:**
  `tools/apk_extract/codboz_apk_extract.c`, rewritten with hash, ZIP, CRC, path,
  and staged-import validation.
- **Resource archive preparation:** `tools/setup_game.sh`, rewritten as an
  Android foreground service with resumable downloads and atomic completion.
- **DTRZ archives:** `src/dtrz.c`, adapted to the virtual filesystem.
- **Config and file behavior:** `src/s3e_config.c` and `src/s3e_file.c`, adapted
  without host pointers.
- **Timers, sockets, input, and sound:** `src/s3e_timer.c`, `src/s3e_socket.c`,
  `src/s3e_input.c`, and `src/s3e_sound.c`, adapted to queued guest callbacks
  and Android backends.
- **OpenGL behavior:** `src/s3e_gl.c` and the loader call inventory,
  reimplemented as an Android EGL/GLES bridge.
- **Game fault offsets:** recovery handlers in `src/main.c`, reimplemented as
  JIT fault fixups.
- **Player-name patch:** `src/main.c`, reimplemented with supported-image byte
  checks.
- **mDNS and ZeroConf:** `src/mdns_wire.c` and `src/s3e_zeroconf.c`, adapted to
  Android NSD and queued guest callbacks. The wire parser was not copied.

The touchscreen event contract was independently checked against the original
APK's Marmalade Java adapter and ARM32 runtime. That comparison is used only to
verify event ordering and S3E state semantics; no original runtime code is
included.

Third-party code is vendored under `native/third_party`:

- Dynarmic 6.7.0, ISC;
- LZMA SDK decoder, public domain;
- the Dynarmic dependency set with its upstream license files.

The vendored Dynarmic AArch64 backend contains one local correctness fix:
memory-abort checks load the JIT's 32-bit halt flag with a 32-bit acquire load.
The upstream 64-bit load could raise `BUS_ADRALN` when that field was only
four-byte aligned.

The original APK, `boz.s3e`, decompressed images, and resource archives are not
part of the repository or release artifacts. The only supported source is
Android version 1.0.11 with SHA-256
`359ee68b6e0a3a66e921ec9b955b290dedb93135fd3c20904bc1bb6f47b5499d`.
Public identifying metadata for that source, its resource archives, and the
extension interfaces used by the runtime is centralized in
`config/source-profile.json`.
