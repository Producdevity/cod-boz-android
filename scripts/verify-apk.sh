#!/usr/bin/env bash
set -euo pipefail

unsigned=0
if [[ "${1:-}" == "--unsigned" ]]; then
  unsigned=1
  shift
fi

apk="${1:?usage: $0 [--unsigned] <app.apk>}"
[[ -f "$apk" ]] || { echo "apk not found: $apk" >&2; exit 2; }
command -v unzip >/dev/null || { echo "unzip not found" >&2; exit 2; }

sdk_roots=()
[[ -n "${ANDROID_SDK_ROOT:-}" ]] && sdk_roots+=("$ANDROID_SDK_ROOT")
[[ -n "${ANDROID_HOME:-}" ]] && sdk_roots+=("$ANDROID_HOME")
sdk_roots+=("$HOME/Library/Android/sdk")

build_tools=""
build_tools_revision=""
readelf=""
ndk_revision=""

version_is_newer() {
  local candidate="$1"
  local current="$2"
  local candidate_parts=()
  local current_parts=()
  local index candidate_part current_part
  IFS=. read -r -a candidate_parts <<<"$candidate"
  IFS=. read -r -a current_parts <<<"$current"
  for index in 0 1 2 3; do
    candidate_part="${candidate_parts[$index]:-0}"
    current_part="${current_parts[$index]:-0}"
    (( 10#$candidate_part > 10#$current_part )) && return 0
    (( 10#$candidate_part < 10#$current_part )) && return 1
  done
  return 1
}

for sdk_root in "${sdk_roots[@]}"; do
  for candidate in "$sdk_root"/build-tools/*; do
    revision="$(awk -F= '/^Pkg.Revision[[:space:]]*=/{gsub(/[[:space:]]/, "", $2); print $2}' "$candidate/source.properties" 2>/dev/null || true)"
    if [[ -x "$candidate/zipalign" && -x "$candidate/apksigner" &&
          "$revision" =~ ^[0-9]+(\.[0-9]+){0,3}$ ]] &&
       { [[ -z "$build_tools_revision" ]] || version_is_newer "$revision" "$build_tools_revision"; }; then
      build_tools="$candidate"
      build_tools_revision="$revision"
    fi
  done
  for candidate in "$sdk_root"/ndk/*/toolchains/llvm/prebuilt/*/bin/llvm-readelf; do
    revision="$(awk -F= '/^Pkg.Revision[[:space:]]*=/{gsub(/[[:space:]]/, "", $2); print $2}' "${candidate%%/toolchains/*}/source.properties" 2>/dev/null || true)"
    if [[ -x "$candidate" && "$revision" =~ ^[0-9]+(\.[0-9]+){0,3}$ ]] &&
       { [[ -z "$ndk_revision" ]] || version_is_newer "$revision" "$ndk_revision"; }; then
      readelf="$candidate"
      ndk_revision="$revision"
    fi
  done
done

[[ -n "$build_tools" ]] || {
  echo "Android build-tools not found; set ANDROID_SDK_ROOT" >&2
  exit 2
}
if [[ -z "$readelf" ]]; then
  readelf="$(command -v llvm-readelf || command -v readelf || true)"
fi
[[ -n "$readelf" ]] || { echo "llvm-readelf or readelf not found" >&2; exit 2; }

entries="$(unzip -Z1 "$apk")"
status=0

abi_list="$(awk -F/ '$1 == "lib" && NF == 3 && $3 ~ /\.so$/ {print $2}' <<<"$entries" | sort -u)"
if [[ -z "$abi_list" ]]; then
  echo "FAIL  no native ABIs in APK" >&2
  status=1
fi
while IFS= read -r abi; do
  [[ -z "$abi" ]] && continue
  if [[ "$abi" == "arm64-v8a" ]]; then
    echo "ok    ABI $abi"
  else
    echo "FAIL  unexpected ABI $abi" >&2
    status=1
  fi
done <<<"$abi_list"

proprietary_entries="$(
  awk '
    {
      path = tolower($0)
      if (path ~ /\.apk$/ || path ~ /\.aab$/ || path ~ /\.s3e(\.unpacked)?$/ || path ~ /\.dz$/) {
        print $0
      }
    }
  ' <<<"$entries"
)"
if [[ -n "$proprietary_entries" ]]; then
  echo "FAIL  proprietary game data in APK:" >&2
  printf '  %s\n' "$proprietary_entries" >&2
  status=1
else
  echo "ok    no proprietary game data"
fi

tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT

library_count=0
while IFS= read -r entry; do
  [[ "$entry" == lib/*/*.so ]] || continue
  library_count=$((library_count + 1))
  library="$tmp/${entry//\//_}"
  unzip -p "$apk" "$entry" >"$library"

  elf_header="$($readelf -hW "$library")"
  program_headers="$($readelf -lW "$library")"
  dynamic="$($readelf -dW "$library")"
  failed=0

  if ! grep -Eq 'Class:[[:space:]]+ELF64' <<<"$elf_header" ||
     ! grep -Eq 'Machine:[[:space:]]+AArch64' <<<"$elf_header"; then
    echo "FAIL  $entry is not an ELF64 AArch64 library" >&2
    failed=1
  fi

  while IFS= read -r alignment; do
    if [[ ! "$alignment" =~ ^0x[0-9a-fA-F]+$ ]]; then
      echo "FAIL  $entry has an unreadable LOAD alignment: $alignment" >&2
      failed=1
      continue
    fi
    if (( alignment < 0x4000 || alignment % 0x4000 != 0 )); then
      echo "FAIL  $entry has a LOAD alignment below 16 KiB: $alignment" >&2
      failed=1
    fi
  done < <(awk '/^[[:space:]]*LOAD / {print $NF}' <<<"$program_headers")

  if ! grep -q 'GNU_RELRO' <<<"$program_headers"; then
    echo "FAIL  $entry has no GNU_RELRO segment" >&2
    failed=1
  fi
  if ! grep -Eq '\(BIND_NOW\)|FLAGS.*NOW' <<<"$dynamic"; then
    echo "FAIL  $entry does not use immediate binding" >&2
    failed=1
  fi
  if awk '/GNU_STACK/ && /E/ {found=1} END {exit !found}' <<<"$program_headers"; then
    echo "FAIL  $entry has an executable stack" >&2
    failed=1
  fi
  if awk '/^[[:space:]]*LOAD / && /W/ && /E/ {found=1} END {exit !found}' \
      <<<"$program_headers"; then
    echo "FAIL  $entry has a writable-executable LOAD segment" >&2
    failed=1
  fi

  if (( failed )); then
    status=1
  else
    echo "ok    $entry (ELF64, AArch64, 16 KiB, RELRO, NOW, NX)"
  fi
done <<<"$entries"

if (( library_count == 0 )); then
  echo "FAIL  no native libraries in APK" >&2
  status=1
fi

if "$build_tools/zipalign" -c -P 16 -v 4 "$apk" >/dev/null 2>&1; then
  echo "ok    16 KiB ZIP alignment"
else
  echo "FAIL  16 KiB ZIP alignment" >&2
  status=1
fi

if (( unsigned )); then
  if "$build_tools/apksigner" verify "$apk" >/dev/null 2>&1; then
    echo "FAIL  unsigned APK unexpectedly has a valid signature" >&2
    status=1
  else
    echo "ok    APK is unsigned"
  fi
elif "$build_tools/apksigner" verify -Werr --print-certs "$apk" >/dev/null 2>&1; then
  echo "ok    APK signature"
else
  echo "FAIL  APK signature" >&2
  status=1
fi

exit "$status"
