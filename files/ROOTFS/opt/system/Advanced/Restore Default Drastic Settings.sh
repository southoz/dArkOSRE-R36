#!/bin/bash

DEVICE=""
CONFIG=""
JSON=""

if [ -f "/boot/rk3326-rg351v-linux.dtb" ]; then
  DEVICE="RG351V"
  CONFIG="drastic.cfg.351v"
elif [ -f "/boot/rk3326-rg351mp-linux.dtb" ] || [ -f "/boot/rk3326-g350-linux.dtb" ] || [ -f "/boot/rk3326-r36s-linux.dtb" ]; then
  DEVICE="RG351MP"
  CONFIG="drastic.cfg.rg351mp"
  JSON="settings.json.rg351mp"
elif [ -f "/boot/rk3326-gameforce-linux.dtb" ]; then
  DEVICE="Gameforce Chi"
  CONFIG="drastic.cfg.chi"
elif [ -f "/boot/rk3326-odroidgo2-linux.dtb" ] || [ -f "/boot/rk3326-odroidgo2-linux-v11.dtb" ]; then
  if [[ ! -z $(cat /etc/emulationstation/es_input.cfg | grep "190000004b4800000010000001010000") ]]; then
    DEVICE="RGB10/OGA 1.1 (BE)"
    CONFIG="drastic.cfg.rgb10"
  else
    DEVICE="RK2020/OGA 1.0"
    CONFIG="drastic.cfg.rk2020"
  fi
elif [ -f "/boot/rk3326-odroidgo3-linux.dtb" ]; then
  if [ "$(cat ~/.config/.OS)" == "ArkOS" ] && [ "$(cat ~/.config/.DEVICE)" == "RGB10MAX" ]; then
    DEVICE="RGB10MAX"
  else
    DEVICE="Maybe an OGS"
  fi
  CONFIG="drastic.cfg.rgb10max"
elif [ -f "/boot/rk3566.dtb" ] || [ -f "/boot/rk3566-OC.dtb" ]; then
  if [ "$(cat ~/.config/.DEVICE)" == "RG353M" ]; then
    DEVICE="RG353M"
    CONFIG="drastic.cfg.353m"
  elif [ "$(cat ~/.config/.DEVICE)" == "RG353V" ]; then
    DEVICE="RG353V/VS"
    CONFIG="drastic.cfg.353v"
  elif [ "$(cat ~/.config/.DEVICE)" == "RK2023" ]; then
    DEVICE="RK2023"
    CONFIG="drastic.cfg.rk2023"
  elif [ "$(cat ~/.config/.DEVICE)" == "RGB30" ]; then
    DEVICE="RGB30"
    CONFIG="drastic.cfg.rk2023"
  else
    DEVICE="RG503"
    CONFIG="drastic.cfg.503"
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

printf "\nAre you sure you want to default your drastic configuration?\n"
printf "\nPress A to continue.  Press B to exit.\n"
while true
do
    Test_Button_A
    if [ "$?" -eq "10" ]; then
      cp -f /opt/drastic/config/backup/${CONFIG} /opt/drastic/config/drastic.cfg
      cp -f /opt/drastic/config/backup/${JSON} /opt/drastic/resources/settings.json
      if [ $? == 0 ]; then
        printf "\nRestored the default drastic emulator configuration for the\n"
        printf "$DEVICE"
        sleep 5
      else
        printf "\nFailed to restore the default drastic emulator configuration for $DEVICE"
        sleep 5
      fi
      exit 0
	fi

    Test_Button_B
    if [ "$?" -eq "10" ]; then
	  printf "\nExiting without defaulting the drastic emulator configuration."
	  sleep 1
      exit 0
	fi
done
