# Release

## Signing

The repository does not contain a release keystore or signing credentials.
Create and store them outside the checkout. Back up the production keystore;
losing it prevents compatible updates to an installed app.

Without signing variables, Gradle produces an unsigned release APK:

```sh
./gradlew :app:assembleRelease
```

To produce a signed APK, provide all four variables:

```sh
CODBOZ_RELEASE_KEYSTORE=/absolute/path/to/release.jks \
CODBOZ_RELEASE_STORE_PASSWORD='...' \
CODBOZ_RELEASE_KEY_ALIAS='...' \
CODBOZ_RELEASE_KEY_PASSWORD='...' \
  ./gradlew :app:assembleRelease
```

The signed output is `app/build/outputs/apk/release/app-release.apk`. Gradle
rejects a partial signing configuration instead of silently producing a
differently signed package.

Both forms write full native debug symbols to
`app/build/outputs/native-debug-symbols/release/native-debug-symbols.zip`.
Archive that file with the exact release APK, but do not distribute it as part
of the APK.

Run the same package checks used for debug builds:

```sh
scripts/verify-no-game-data.sh
scripts/verify-apk.sh /absolute/path/to/signed-release.apk
```

`verify-apk.sh` requires an `arm64-v8a`-only native inventory, 16 KiB ZIP
and ELF alignment, hardened native libraries, a valid APK signature, and no
embedded game payload. Keep the unmodified source APK and all downloaded game
data outside release artifacts.

## GitHub Actions

Pull requests and pushes to `master` run formatting, unit tests, Android lint,
debug, unsigned release, and instrumentation test APK builds, package checks,
host C++ tests, sanitizers, and CodeQL. The Android workflow retains the debug
APK as a short-lived test artifact.

Pushes to `master` also build and retain a signed release APK. Configure these
repository secrets before merging:

- `CODBOZ_RELEASE_KEYSTORE_BASE64`: base64 encoding of the production keystore;
- `CODBOZ_RELEASE_STORE_PASSWORD`;
- `CODBOZ_RELEASE_KEY_ALIAS`;
- `CODBOZ_RELEASE_KEY_PASSWORD`.

The keystore is decoded only into the runner's temporary directory. The signed
APK and matching native symbols are uploaded as workflow artifacts. GitHub
artifacts are for testing; publishing a tagged release remains a deliberate
manual step.

## Release checklist

Before publishing a release, test Android 15 and 16 devices with at least one
Adreno GPU and one non-Adreno GPU. Cover touchscreen gameplay, generic, Xbox,
and PlayStation controllers, speakers and Bluetooth audio, screen lock, network
changes, local match joining, and a complete online match against an explicitly
approved compatible server.

The exact procedure and required evidence are in
[device-testing.md](device-testing.md). Emulator discovery, socket reachability,
and synthetic controller events do not replace physical-device testing.
