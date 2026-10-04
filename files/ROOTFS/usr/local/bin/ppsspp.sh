#!/bin/bash

# ROM card follows the selected game, independent of the SD-switch script.
if [[ "$2" =~ ^/(roms2?)/(psp|pspminis)/ ]]; then
  directory="${BASH_REMATCH[1]}"
else
  printf 'PPSSPP: game must be inside the selected ROM card PSP or PSP Minis directory.\n' >&2
  exit 1
fi
# Do not create a second-card profile on the system disk when SD2 is absent.
if [[ "$directory" == "roms2" ]] && ! mountpoint -q /roms2; then
  printf 'PPSSPP: second ROM card is not mounted.\n' >&2
  exit 1
fi
psp_root="/$directory/psp"
profile_std="$psp_root/ppsspp"
profile_2021="$psp_root/ppsspp-2021"

prepare_profile() {
  local profile="$1" template="$2"
  mkdir -p "$profile/PSP/SYSTEM" || return 1
  if [[ ! -f "$profile/PSP/SYSTEM/controls.ini" ]]; then
    cp "$template/PSP/SYSTEM/controls.ini" "$profile/PSP/SYSTEM/controls.ini" || return 1
  fi
  if [[ ! -f "$profile/PSP/SYSTEM/ppsspp.ini.sdl" ]]; then
    # Preserve an existing user INI when only its SDL working copy is missing.
    if [[ -f "$profile/PSP/SYSTEM/ppsspp.ini" ]]; then
      cp "$profile/PSP/SYSTEM/ppsspp.ini" "$profile/PSP/SYSTEM/ppsspp.ini.sdl" || return 1
    else
      cp "$template/PSP/SYSTEM/ppsspp.ini.sdl" "$profile/PSP/SYSTEM/ppsspp.ini.sdl" || return 1
    fi
  fi
  if [[ -e "/home/ark/.config/ppsspp" && ! -L "/home/ark/.config/ppsspp" ]]; then
    printf 'PPSSPP: refusing to replace a real user config directory.\n' >&2
    return 1
  fi
  ln -sfn "$profile/" "/home/ark/.config/ppsspp" || return 1
  cp "$profile/PSP/SYSTEM/ppsspp.ini.sdl" "$profile/PSP/SYSTEM/ppsspp.ini"
}

if [[ "$1" == "standalone" ]]; then
  prepare_profile "$profile_std" "/opt/ppsspp/backupforromsfolder/ppsspp" || exit 1
  xres="$(cat /sys/class/graphics/fb0/modes | grep -o -P '(?<=:).*(?=p-)' | cut -dx -f1)"
  if [[ "$xres" -ge 1280 ]]; then
    HDMI="/usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0.10.0"
  fi
  LD_PRELOAD="$HDMI" "/opt/ppsspp/PPSSPPSDL" --fullscreen "$2"
  result=$?
  cp "$profile_std/PSP/SYSTEM/ppsspp.ini" "$profile_std/PSP/SYSTEM/ppsspp.ini.sdl"
  exit "$result"
elif [[ "$1" == "standalone-2021" ]]; then
  if [[ ! -e "$profile_2021" && ! -L "$profile_2021" && -d "$profile_std" ]]; then
    # Previous R36 launcher shared this tree: retain all existing settings/saves
    # in the new independent profile. Never delete or overwrite the old tree.
    staging="$(mktemp -d "$psp_root/.ppsspp-2021.XXXXXX")" || exit 1
    if ! cp -a "$profile_std/." "$staging/"; then
      rm -rf -- "$staging"
      exit 1
    fi
    if ! mv -T "$staging" "$profile_2021"; then
      rm -rf -- "$staging"
      exit 1
    fi
  fi
  prepare_profile "$profile_2021" "/opt/ppsspp-2021/backupforromsfolder/ppsspp" || exit 1
  export SDL_AUDIODRIVER=alsa
  echo "VAR=PPSSPPSDL" > "/home/ark/.config/KILLIT"
  sudo systemctl restart killer_daemon.service
  "/opt/ppsspp-2021/PPSSPPSDL" --fullscreen "$2"
  result=$?
  cp "$profile_2021/PSP/SYSTEM/ppsspp.ini" "$profile_2021/PSP/SYSTEM/ppsspp.ini.sdl"
  sudo systemctl stop killer_daemon.service
  unset SDL_AUDIODRIVER
  exit "$result"
else
  # The original launcher also selected the standard home profile for libretro.
  if [[ -e "/home/ark/.config/ppsspp" && ! -L "/home/ark/.config/ppsspp" ]]; then
    printf 'PPSSPP: refusing to replace a real user config directory.\n' >&2
    exit 1
  fi
  ln -sfn "$profile_std/" "/home/ark/.config/ppsspp" || exit 1
  if [[ ! -d "/$directory/psp/PSP" ]]; then
    mkdir /$directory/psp/PSP
  fi
  if [[ ! -d "/$directory/psp/PSP/SAVEDATA" ]]; then
    mkdir /$directory/psp/PSP/SAVEDATA
  fi
  if [[ ! -d "/$directory/psp/SAVEDATA" ]]; then
    mkdir /$directory/psp/SAVEDATA
  fi
  /usr/local/bin/watchpsp.sh $directory &
  /usr/local/bin/retroarch -L /home/ark/.config/retroarch/cores/ppsspp_libretro.so "$2"
  sudo kill -9 $(pidof watchpsp.sh)
fi
