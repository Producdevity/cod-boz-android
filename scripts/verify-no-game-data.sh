#!/usr/bin/env bash
set -euo pipefail

root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "not inside a Git work tree" >&2
  exit 2
}
cd "$root"

status=0
shopt -s nocasematch
while IFS= read -r path; do
  [[ -n "$path" ]] || continue
  case "$path" in
    *.apk|*.aab|*.s3e|*.s3e.unpacked|*.dz)
      echo "FAIL  proprietary game payload in repository: $path" >&2
      status=1
      ;;
  esac
done < <(git ls-files --cached --others --exclude-standard)

if (( status == 0 )); then
  echo "ok    no proprietary game payloads in repository"
fi
exit "$status"
