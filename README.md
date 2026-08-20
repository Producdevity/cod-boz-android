# COD BOZ Android

COD BOZ Android runs the original ARM32 Call of Duty: Black Ops Zombies game
inside an `arm64-v8a` Android application. Dynarmic translates the ARMv7/Thumb
guest code to AArch64 while native Android backends provide graphics, audio,
input, storage, and networking.

The application targets Android devices without a 32-bit userspace. Offline
gameplay works on ARM64-only Android 16 emulators with 4 KiB and 16 KiB memory
pages. A release also requires physical-device validation of controllers,
touchscreen input, audio, voice chat, and multiplayer.

## Game data

Game data is not included. Users must import a legally obtained Android 1.0.11
APK with this SHA-256:

```text
359ee68b6e0a3a66e921ec9b955b290dedb93135fd3c20904bc1bb6f47b5499d
```

Other versions are rejected. The importer validates staged data before
replacing an existing import and stores extracted files in app-private storage.

The supported APK contains the loader but not the two full resource archives.
After import, the app downloads about 787 MiB from the Activision URL embedded
in the APK. Downloads are resumable and each archive is checked against its
pinned size and SHA-256 before Play is enabled. The source endpoint uses HTTP,
so the app verifies each completed download rather than relying on transport
security.

## Building

The build requires JDK 17, Android SDK platform 36, NDK 28.2.13676358, and
CMake 3.31.6.

```sh
./gradlew spotlessCheck :app:test :app:lintDebug :app:assembleDebug :app:assembleRelease
```

The debug APK is written to `app/build/outputs/apk/debug/app-debug.apk`.
Release signing is documented in [docs/release.md](docs/release.md).

## Testing

Run the native host suite with:

```sh
cmake -S native -B build/host-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host-tests --parallel
ctest --test-dir build/host-tests --output-on-failure
```

Tests that inspect or execute the proprietary guest are skipped unless the
source APK is supplied explicitly:

```sh
CODBOZ_TEST_APK=/absolute/path/to/game.apk \
  ctest --test-dir build/host-tests --output-on-failure

CODBOZ_TEST_APK=/absolute/path/to/game.apk ./gradlew connectedCheck
```

The second command embeds the source APK only in a generated local
instrumentation build. Packaging checks reject proprietary game data in source
or release artifacts.

For a physical-device release test, select the target explicitly and follow
[docs/device-testing.md](docs/device-testing.md):

```sh
export ANDROID_SERIAL=emulator-5554
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -W -n dev.producdevity.codboz/.MainActivity
```

## Source inspection

The inspection tool validates the source APK without executing game code. It
reports manifest and signing metadata, native ABIs, S3E sections, imports,
relocations, extension hashes, and required resource archives. Build the host
tools first and make the Android SDK's `apkanalyzer` and `apksigner` available.

```sh
scripts/inspect-source-apk.py /absolute/path/to/game.apk \
  --output /tmp/codboz-source-report.json
```

## Packaging checks

```sh
scripts/verify-no-game-data.sh
scripts/verify-apk.sh app/build/outputs/apk/debug/app-debug.apk
scripts/verify-apk.sh --unsigned app/build/outputs/apk/release/app-release-unsigned.apk
```

See [docs/architecture.md](docs/architecture.md) for the runtime design and
[docs/provenance.md](docs/provenance.md) for reused code and third-party sources.

## License

The host application and runtime are licensed under GPL-3.0-or-later. Vendored
dependencies retain their own compatible licenses; see [NOTICE](NOTICE) and the
license files under `native/third_party`.

The original game, its APK, and downloaded assets are not part of this
repository or license. Users must provide their own legally obtained copy.
