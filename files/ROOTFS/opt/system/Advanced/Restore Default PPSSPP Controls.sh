#!/bin/bash

DEVICE=""

if [ -f "/boot/rk3326-r36s-linux.dtb" ]; then
  DEVICE="R36S"
elif [ -f "/boot/rk3326-rg351v-linux.dtb" ]; then
  DEVICE="RG351V"
elif [ -f "/boot/rk3326-rg351mp-linux.dtb" ] || [ -f "/boot/rk3326-g350-linux.dtb" ]; then
  DEVICE="RG351MP"
elif [ -f "/boot/rk3326-gameforce-linux.dtb" ]; then
  DEVICE="Gameforce Chi"
elif [ -f "/boot/rk3326-odroidgo2-linux.dtb" ] || [ -f "/boot/rk3326-odroidgo2-linux-v11.dtb" ]; then
  if [[ ! -z $(cat /etc/emulationstation/es_input.cfg | grep "190000004b4800000010000001010000") ]]; then
    DEVICE="RGB10/OGA 1.1 (BE)"
  else
    DEVICE="RK2020/OGA 1.0"
  fi
elif [ -f "/boot/rk3326-odroidgo3-linux.dtb" ]; then
  if [ "$(cat ~/.config/.OS)" == "ArkOS" ] && [ "$(cat ~/.config/.DEVICE)" == "RGB10MAX" ]; then
    DEVICE="RGB10MAX"
  else
    DEVICE="Maybe an OGS"
  fi
elif [ -f "/boot/rk3566.dtb" ] || [ -f "/boot/rk3566-OC.dtb" ]; then
  if [ "$(cat ~/.config/.DEVICE)" == "RG353M" ]; then
    DEVICE="RG353M"
  elif [ "$(cat ~/.config/.DEVICE)" == "RG353V" ]; then
    DEVICE="RG353V/VS"
  elif [ "$(cat ~/.config/.DEVICE)" == "RK2023" ]; then
    DEVICE="RK2023"
  elif [ "$(cat ~/.config/.DEVICE)" == "RGB30" ]; then
    DEVICE="RGB30"
  else
    DEVICE="RG503"
  fi
fi

. /usr/local/bin/buttonmon.sh

if compgen -G "/boot/rk3566*" > /dev/null; then
  if test ! -z "$(cat /home/ark/.config/.DEVICE | grep RGB20PRO | tr -d '\0')"
  then
    sudo setfont /usr/share/consolefonts/Lat7-TerminusBold32x16.psf.gz
  else
    sudo setfont /usr/share/consolefonts/Lat7-TerminusBold28x14.psf.gz
  fi
  height="20"
  width="60"
fi

printf "\nAre you sure you want to default your PPSSPP controls?\n"
printf "\nPress A to continue.  Press B to exit.\n"
while true
do
    Test_Button_A
    if [ "$?" -eq "10" ]; then
      reset_status=0
      # Keep the original two-card reset scope; include each independent version.
      for card in roms roms2; do
        if [[ "$card" == roms2 ]] && ! mountpoint -q "/$card"; then
          continue
        fi
        for profile in ppsspp ppsspp-2021; do
          config="/$card/psp/$profile/PSP/SYSTEM"
          [[ -d "$config" ]] || continue
          rm -f "$config/controls.ini" "$config/ppsspp.ini" "$config/ppsspp.ini.sdl" || reset_status=1
        done
      done
      if [[ "$reset_status" == 0 ]]; then
        printf "\nRestored the default PPSSPP controls for the\n"
        printf "$DEVICE"
        sleep 5
      else
        printf "\nFailed to restore the default PPSSPP controls for $DEVICE"
        sleep 5
      fi
      exit 0
	fi

    Test_Button_B
    if [ "$?" -eq "10" ]; then
	  printf "\nExiting without defaulting the PPSSPP controls."
	  sleep 1
      exit 0
	fi
done
