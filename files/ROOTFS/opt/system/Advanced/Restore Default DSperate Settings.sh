#!/bin/bash

. /usr/local/bin/buttonmon.sh

resolve_dsperate_root() {
  local path root
  local -a roots=()
  if [[ ! -r /etc/emulationstation/es_systems.cfg ]]; then
    printf 'EmulationStation system paths are unavailable.\n'
    return 1
  fi
  mapfile -t roots < <(
    grep -o '<path>[^<]*</path>' /etc/emulationstation/es_systems.cfg |
      sed -nE 's#<path>(/roms2?)(/[^<]*)?</path>#\1#p' | sort -u
  )
  if [[ "${#roots[@]}" -ne 1 || ( "${roots[0]}" != /roms && "${roots[0]}" != /roms2 ) ]]; then
    printf 'EmulationStation must select exactly one ROM card (/roms or /roms2).\n'
    return 1
  fi
  root="${roots[0]}"
  if ! mountpoint -q "$root" || [[ -L "$root" ]]; then
    printf 'The selected ROM card %s is not mounted safely.\n' "$root"
    return 1
  fi
  printf '%s' "$root"
}

rom_root="$(resolve_dsperate_root)" || exit 1
config_dir="$rom_root/nds/dsperate"
config="$config_dir/dsperate.ini"
template="/opt/DSperate/config/dsperate.ini"
backup_dir="$rom_root/backup/dsperate-restore"

if [[ ! -r "$template" || -L "$template" ]]; then
  printf 'The reviewed R36S DSperate defaults are unavailable.\n'
  exit 1
fi

printf '\nRestore DSperate settings for %s?\n' "$rom_root"
printf 'This resets emulator settings on the selected ROM card. Saves and save states stay untouched.\n'
printf 'Game-specific settings files are left unchanged.\n\nPress A to continue; press B to cancel.\n'
while true; do
  Test_Button_A
  if [[ "$?" -eq 10 ]]; then
    if [[ -e "$config" && ( -L "$config" || ! -f "$config" ) ]]; then
      printf '\nRefusing to replace an unsafe DSperate config path.\n'
      sleep 4
      exit 1
    fi
    if [[ -f "$config" ]]; then
      mkdir -p "$backup_dir" || exit 1
      if [[ -L "$backup_dir" || ! -d "$backup_dir" ]]; then
        printf '\nRefusing unsafe backup directory.\n'
        sleep 4
        exit 1
      fi
      cp -p -- "$config" "$backup_dir/dsperate.ini.previous" || {
        printf '\nCould not back up the current DSperate settings. Nothing was reset.\n'
        sleep 4
        exit 1
      }
    fi
    mkdir -p "$config_dir" || exit 1
    temporary="$(mktemp "$config_dir/.dsperate.ini.restore.XXXXXX")" || exit 1
    if ! cp -- "$template" "$temporary"; then
      rm -f -- "$temporary"
      exit 1
    fi
    sed -i \
      -e "s|^saves[[:space:]]*=.*$|saves = $config_dir/saves|" \
      -e "s|^states[[:space:]]*=.*$|states = $config_dir/states|" \
      -e "s|^cheats[[:space:]]*=.*$|cheats = $rom_root/nds/cheats|" \
      "$temporary" || { rm -f -- "$temporary"; exit 1; }
    if [[ -f "$config" ]]; then
      chmod --reference="$config" "$temporary" 2>/dev/null || true
      chown --reference="$config" "$temporary" 2>/dev/null || true
    else
      chown ark:ark "$temporary" 2>/dev/null || true
    fi
    if ! mv -f -- "$temporary" "$config"; then
      rm -f -- "$temporary"
      printf '\nCould not install the default settings.\n'
      sleep 4
      exit 1
    fi
    printf '\nRestored the R36S DSperate settings on %s.\n' "$rom_root"
    sleep 4
    exit 0
  fi
  Test_Button_B
  if [[ "$?" -eq 10 ]]; then
    printf '\nCancelled; no settings were changed.\n'
    sleep 1
    exit 0
  fi
done
