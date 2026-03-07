#!/bin/bash

directory=$(dirname "$1" | cut -d "/" -f2)
if [[ ! -d "/$directory/atarijaguar/.bigpemu_userdata" ]]; then
  mkdir -p /$directory/atarijaguar/.bigpemu_userdata
  if [ -f "/boot/rk3326-rg351mp-linux.dtb" ] || [ -f "/boot/rk3326-g350-linux.dtb" ] || [ -f "/boot/rk3326-r36s-linux.dtb" ]; then
    CONFIG="BigPEmuConfig.bigpcfg.rg351mp"
  elif [ -f "/boot/rk3326-odroidgo2-linux.dtb" ] || [ -f "/boot/rk3326-odroidgo2-linux-v11.dtb" ]; then
    if [[ ! -z $(cat /etc/emulationstation/es_input.cfg | grep "190000004b4800000010000001010000") ]]; then
      CONFIG="BigPEmuConfig.bigpcfg.rgb10"
    else
      CONFIG="BigPEmuConfig.bigpcfg.rk2020"
    fi
  else
    CONFIG="BigPEmuConfig.bigpcfg.rk3566"
  fi
  cp -f /opt/bigpemu/defaultconfigs/${CONFIG} /$directory/atarijaguar/.bigpemu_userdata/BigPEmuConfig.bigpcfg
fi

rm -rf /home/ark/.bigpemu_userdata
ln -s /$directory/atarijaguar/.bigpemu_userdata

echo "VAR=bigpemu" > /home/ark/.config/KILLIT
sudo systemctl restart killer_daemon.service

cd /opt/bigpemu

export LD_LIBRARY_PATH="/opt/bigpemu"
LD_PRELOAD=./libOpenGL.so ./bigpemu "$1"

sudo systemctl stop killer_daemon.service

sudo systemctl restart ogage &
