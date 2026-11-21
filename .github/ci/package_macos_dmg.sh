#!/usr/bin/env bash
set -euo pipefail

app="$1"
output="$2"
script_dir="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d "$(dirname "$output")/sleepygram-dmg.XXXXXX")"
mounted=false

cleanup() {
  if "$mounted" && ! hdiutil detach "$work/mount"; then
    return 1
  fi
  rm -rf "$work"
}
trap cleanup EXIT

mkdir "$work/staging" "$work/mount"
ditto "$app" "$work/staging/SleepyGram.app"
ln -s /Applications "$work/staging/Applications"

hdiutil create -ov -format UDZO -fs HFS+ -volname SleepyGram \
  -srcfolder "$work/staging" "$work/SleepyGram.dmg"
hdiutil verify "$work/SleepyGram.dmg"
hdiutil attach "$work/SleepyGram.dmg" -readonly -nobrowse -noautoopen \
  -mountpoint "$work/mount"
mounted=true

test "$(readlink "$work/mount/Applications")" = /Applications
cmp "$app/Contents/Info.plist" "$work/mount/SleepyGram.app/Contents/Info.plist"
resources="Contents/Resources"
cmp "$app/$resources/BuildManifest.json" \
  "$work/mount/SleepyGram.app/$resources/BuildManifest.json"

mkdir "$work/installed"
ditto "$work/mount/SleepyGram.app" "$work/installed/SleepyGram.app"
hdiutil detach "$work/mount"
mounted=false
codesign --verify --deep --strict "$work/installed/SleepyGram.app"

if [[ ${3:-} == --smoke ]]; then
  python3 "$script_dir/smoke.py" --platform macos-arm64 \
    --directory "$work/installed" --qpa-platform cocoa --require-startup
fi

shasum -a 256 < "$work/SleepyGram.dmg" | cut -d ' ' -f 1 > "$work/sha256"
mv "$work/SleepyGram.dmg" "$output"
mv "$work/sha256" "$output.sha256"
