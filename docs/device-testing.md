# Physical device testing

Run this matrix against the exact APK intended for release. Record the APK
SHA-256, device model, Android build, and results together. A screenshot of a
menu is not enough evidence for gameplay, controller, audio, or multiplayer.

## Device preflight

For each device, save these values before installing the app:

```sh
adb shell getprop ro.product.manufacturer
adb shell getprop ro.product.model
adb shell getprop ro.build.fingerprint
adb shell getprop ro.product.cpu.abilist
adb shell getprop ro.zygote
adb shell getconf PAGE_SIZE
```

At least one test device must report only `arm64-v8a` and `zygote64`. Cover
Android 15 and 16, one Adreno GPU, one non-Adreno GPU, and both phone and
tablet/foldable-sized windows. Install the release candidate, import the
supported APK through the picker, prepare its resources, and export the app's
diagnostics after entering the game.

## Gameplay and platform checks

On each primary device:

1. Start a new offline game and play for at least ten minutes.
2. Use every original touch control, including simultaneous movement, aiming,
   and firing. Check the edges and corners on non-16:9 displays.
3. Save, force-stop the app, reopen it, and confirm the saved state loads.
4. Background and resume during the menu and during gameplay. Lock and unlock
   the screen, rotate where Android permits it, and disconnect/reconnect the
   display surface without losing the guest session.
5. Verify music and effects through the speaker, Bluetooth output, and one
   wired or USB output. Switch outputs while the game is running.
6. Repeat menu navigation and live gameplay with generic, Xbox-style, and
   PlayStation-style controllers. Exercise both sticks, triggers, shoulders,
   face buttons, D-pad, Start, and Select. Verify L3 sprint, LT aim, RT fire,
   X reload/use, Y weapon change, B crouch/prone, R3 melee, LB tactical grenade,
   and RB lethal grenade. Confirm that A/B/D-pad menu navigation does not invoke
   a gameplay action behind the menu.
7. Copy or save diagnostics and retain the reported host ABI, page size,
   renderer, GL version, GL counters, host APK hash, source hash, and import
   count with the test result.
8. Force one Java crash in a debug build, reopen the launcher, and confirm the
   previous-run warning appears. Save the report, verify it identifies the
   crash, and check that it contains no configured player name, server address,
   URL, email address, IP address, or app-private path.
9. Leave voice chat disabled and confirm the app never requests microphone
   permission. Enable it in settings, grant permission, and complete an online
   match between two devices. Verify transmitted and received speech, the
   in-game mute control, background/resume, and speaker/Bluetooth route changes.

Any host crash, guest fault, stuck input, stretched display, missing sound,
lost save, or guest restart after a normal lifecycle event fails the candidate.

## Local Wi-Fi

Use two physical devices on the same ordinary Wi-Fi network, with client
isolation disabled:

1. Create a Local Wi-Fi lobby on device A.
2. Confirm device B discovers it without entering an address manually.
3. Join from device B, start a match, and play from both devices for at least
   ten minutes.
4. Leave and recreate the lobby with the devices' host/client roles reversed.
5. Repeat once after disabling and re-enabling Wi-Fi on the joining device.

Record both diagnostics reports and a short capture showing both players in the
same live match. Discovery alone does not pass this check.

## Online server

Use only an explicitly approved compatible server:

1. Change the server in the app settings and restart the game.
2. Confirm the title flow reaches that endpoint without modifying `boz.s3e`.
3. Sign in or create the required account, join another real client, and finish
   a match.
4. Restore the default setting, then verify a DNS failure produces a recoverable
   error rather than redirecting unrelated traffic or crashing the guest.

The settings connection test proves DNS, TCP 3074 connectivity, and an RFC 3489
STUN response on UDP 3478. It does not count as game-protocol, account, or match
evidence.
