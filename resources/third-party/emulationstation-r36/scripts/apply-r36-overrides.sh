#!/usr/bin/env bash
set -euo pipefail

readonly script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly patch_root=$(cd -- "$script_dir/.." && pwd)
readonly pin='74498be31cd016af6a42d00310f876d7256eff52'
readonly base_volume_sha='19938ff199fbee29cc95bccf313baf82b3d562c6185d3c6c0dac069abc02b761'
readonly r36_volume_sha='763dd7c2608799b437a5a18ba2c5ec3005e892a46b032e3b8e5f99f63359252a'
readonly fontawesome_sha='aa58f33f239a0fb02f5c7a6c45c043d7a9ac9a093335806694ecd6d4edc0d6a8'

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 FCAMOD_SOURCE_DIR" >&2
  exit 2
fi
src=$(cd -- "$1" && pwd)
actual=$(git -C "$src" rev-parse HEAD)
if [[ "$actual" != "$pin" ]]; then
  echo "Expected pinned FCAMOD revision $pin; found $actual" >&2
  exit 1
fi

# Confirm the selected RG351MP branch already implements both requested UI behaviors.
gui="$src/es-app/src/guis/GuiMenu.cpp"
gamesdb="$src/es-app/src/scrapers/GamesDBJSONScraperResources.cpp"
settings="$src/es-core/src/Settings.cpp"
grep -Fq 'SHOW NETWORK ICON' "$gui" || { echo 'Missing Wi-Fi indicator setting UI in pinned source' >&2; exit 1; }
grep -Fq 'ShowNetworkIndicator' "$settings" || { echo 'Missing Wi-Fi indicator setting default in pinned source' >&2; exit 1; }
grep -Fq 'GamesDBApiKey' "$gui" || { echo 'Missing TheGamesDB API key UI in pinned source' >&2; exit 1; }
grep -Fq 'Settings::getInstance()->getString("GamesDBApiKey")' "$gamesdb" || { echo 'Missing user API key lookup in pinned source' >&2; exit 1; }

volume="$src/es-core/src/VolumeControl.cpp"
current_volume_sha=$(sha256sum "$volume" | cut -d' ' -f1)
if [[ "$current_volume_sha" == "$r36_volume_sha" ]]; then
  echo 'R36 VolumeControl override already present'
elif [[ "$current_volume_sha" == "$base_volume_sha" ]]; then
  git -C "$src" apply --check "$patch_root/patches/0001-r36-volume-control.patch"
  git -C "$src" apply "$patch_root/patches/0001-r36-volume-control.patch"
else
  echo "Refusing to replace unexpected VolumeControl.cpp (SHA-256 $current_volume_sha)" >&2
  exit 1
fi

result_volume_sha=$(sha256sum "$volume" | cut -d' ' -f1)
[[ "$result_volume_sha" == "$r36_volume_sha" ]] || { echo 'R36 VolumeControl hash check failed' >&2; exit 1; }
printf 'R36 override applied; source base=%s, VolumeControl SHA-256=%s\n' "$actual" "$result_volume_sha"

# Font.cpp explicitly uses this Font Awesome font for U+F07B/U+F013 menu glyphs.
# The pinned FCAMOD resource tree does not ship it; preserve the expected fallback.
fontawesome_asset="$patch_root/assets/fontawesome-webfont.ttf"
fontawesome_dest="$src/resources/fontawesome-webfont.ttf"
actual_fontawesome_sha=$(sha256sum "$fontawesome_asset" | cut -d' ' -f1)
[[ "$actual_fontawesome_sha" == "$fontawesome_sha" ]] || {
  echo "Unexpected pinned Font Awesome asset hash: $actual_fontawesome_sha" >&2
  exit 1
}
if [[ -e "$fontawesome_dest" || -L "$fontawesome_dest" ]]; then
  [[ -f "$fontawesome_dest" && ! -L "$fontawesome_dest" ]] || {
    echo "Refusing non-regular Font Awesome destination: $fontawesome_dest" >&2
    exit 1
  }
  existing_fontawesome_sha=$(sha256sum "$fontawesome_dest" | cut -d' ' -f1)
  [[ "$existing_fontawesome_sha" == "$fontawesome_sha" ]] || {
    echo "Refusing to replace unexpected Font Awesome resource (SHA-256 $existing_fontawesome_sha)" >&2
    exit 1
  }
  echo 'Pinned Font Awesome resource already present'
else
  install -m 0644 "$fontawesome_asset" "$fontawesome_dest"
  echo 'Installed pinned Font Awesome resource into source resource tree'
fi

# The maintained R36 Colorful v2 themes put networkIcon on batteryIndicator.
# FCAMOD's inherited applyTheme() already consumes PATH properties; allow that
# specific element to accept the path while leaving theme files untouched.
theme_data="$src/es-core/src/ThemeData.cpp"
theme_patch="$patch_root/patches/0003-battery-indicator-network-icon.patch"
if git -C "$src" apply --reverse --check "$theme_patch" 2>/dev/null; then
  echo 'batteryIndicator networkIcon theme property already present'
else
  git -C "$src" apply --check "$theme_patch"
  git -C "$src" apply "$theme_patch"
fi
battery_properties=$(awk '/\{ "batteryIndicator", \{/{capture=1} capture {print} capture && /\} \},/ {exit}' "$theme_data")
grep -Fq '{ "networkIcon", PATH }' <<< "$battery_properties" || {
  echo 'batteryIndicator networkIcon PATH property is missing after patch' >&2
  exit 1
}
printf 'R36 batteryIndicator networkIcon PATH property is present\n'

# Standards-compliant dependent-base type declarations; two lines only.
portability_patch="$patch_root/patches/0002-dependent-entry-type-portability.patch"
if git -C "$src" apply --reverse --check "$portability_patch" 2>/dev/null; then
  echo 'Dependent-type portability patch already present'
else
  git -C "$src" apply --check "$portability_patch"
  git -C "$src" apply "$portability_patch"
fi
