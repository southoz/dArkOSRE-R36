#!/bin/bash

if  [[ $1 == "standalone" ]]; then

  directory=$(dirname "$2" | cut -d "/" -f2)

  unlink /opt/hypseus-singe/roms
  if [ $? != 0 ]; then
    sudo rm -rf /opt/hypseus-singe/roms
  fi
  ln -sfv /$directory/daphne/roms/ /opt/hypseus-singe/roms

  dir="$2"
  basedir=$(basename -- $dir)
  basefilename=${basedir%.*}

  if [ -f "$dir/$basefilename.commands" ]; then
     extraparams=$(<"$dir/$basefilename.commands")
  fi

  echo "VAR=hypseus-singe" > /home/ark/.config/KILLIT
  sudo systemctl restart killer_daemon.service

  cd /opt/hypseus-singe

  if [[ $(cat /sys/class/graphics/fb0/modes | grep -o -P '(?<=:).*(?=p-)') == "720x720" ]]; then
    RES="-x 720 -y 600"
  fi

  #./hypseus-singe "$basefilename" vldp -gamepad ${RES} -framefile "$dir/$basefilename.txt" -fullscreen -useoverlaysb 2 $extraparams
  ./hypseus-singe "$basefilename" vldp -gamepad -texturestream ${RES} -framefile "$dir/$basefilename.txt" -fullscreen $extraparams

  rm *.csv

  sudo systemctl stop killer_daemon.service
else
  /usr/local/bin/"$1" -L /home/ark/.config/"$1"/cores/"$2"_libretro.so "$3"
fi
