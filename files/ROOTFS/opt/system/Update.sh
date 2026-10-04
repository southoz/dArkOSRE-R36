#!/bin/bash

if [[ "$(stat -c "%U" /home/ark)" != "ark" ]]; then
  printf "Fixing home folder permissions.  Please wait..."
  sudo chown -R ark:ark /home/ark
  sudo chmod -R 755 /home/ark
fi

printf "\nChecking for updates.  Please wait..."

LOG_FILE="/home/ark/esupdate.log"

if [ -f "$LOG_FILE" ]; then
  sudo rm "$LOG_FILE"
fi

sudo timedatectl set-ntp 1

LOCATION="https://raw.githubusercontent.com/dixtuel/darkos-updates/main"

wget -t 3 -T 60 --no-check-certificate "$LOCATION"/LICENSE -O /dev/shm/LICENSE -a "$LOG_FILE"
if [ $? -ne 0 ]; then
  sudo msgbox "Looks like OTA updating is currently down or your wifi or internet connection is not functioning correctly."
  printf "There was an error with attempting this update." | tee -a "$LOG_FILE"
  exit 1
fi

wget -t 3 -T 60 --no-check-certificate "$LOCATION"/dArkOSUpdate.sh -O /home/ark/dArkOSUpdate.sh -a "$LOG_FILE" || sudo rm -f /home/ark/dArkOSUpdate.sh | tee -a "$LOG_FILE"
if [ $? -ne 0 ]; then
  sudo msgbox "Looks like OTA updating is currently down or your wifi or internet connection is not functioning correctly."
  printf "There was an error with attempting this update." | tee -a "$LOG_FILE"
  exit 1
fi

sudo chmod -v 777 /home/ark/dArkOSUpdate.sh | tee -a "$LOG_FILE"
/home/ark/dArkOSUpdate.sh

if [ $? -ne 187 ]; then
  sudo msgbox "There was an error with attempting this update.  Did you make sure to enable your wifi and connect to a wifi network?  If so, enable remote services in options and try to update again."
  printf "There was an error with attempting this update." | tee -a "$LOG_FILE"
  if [ -f /home/ark/dArkOSUpdate.sh ]; then
    rm /home/ark/dArkOSUpdate.sh
  fi
fi

if [ ! -z $(pidof rg351p-js2xbox) ]; then
  sudo kill -9 $(pidof rg351p-js2xbox)
  sudo rm /dev/input/by-path/platform-odroidgo2-joypad-event-joystick
fi
