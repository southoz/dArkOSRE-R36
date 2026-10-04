#!/bin/bash

export SDL_ASSERT="always_ignore"

. /usr/local/bin/buttonmon.sh

boot_controls() {
  export DIALOGRC=/opt/inttools/noshadows.dialogrc
  printf "\033c" > /dev/tty1
  if [[ ! -e "/dev/input/by-path/platform-odroidgo2-joypad-event-joystick" ]]; then
    if test ! -z "$(cat /home/ark/.config/.DEVICE | grep RGB20PRO | tr -d '\0')"
    then
      sudo setfont /usr/share/consolefonts/Lat7-TerminusBold32x16.psf.gz
    else
      sudo setfont /usr/share/consolefonts/Lat7-TerminusBold28x14.psf.gz
    fi
  else
    sudo setfont /usr/share/consolefonts/Lat7-Terminus16.psf.gz
  fi

  if [[ -z $(pgrep -f gptokeyb) ]] && [[ -z $(pgrep -f oga_controls) ]]; then
    sudo chmod 666 /dev/uinput
    export SDL_GAMECONTROLLERCONFIG_FILE="/opt/inttools/gamecontrollerdb.txt"
    /opt/inttools/gptokeyb -c "/opt/inttools/keys.gptk" > /dev/null &
    disown
    set_gptokeyb="Y"
  fi
}

kill_boot_controls() {
  if [[ ! -z "$set_gptokeyb" ]]; then
    pgrep -f gptokeyb | sudo xargs kill -9
    unset SDL_GAMECONTROLLERCONFIG_FILE
  fi
}

sudo chmod 666 /dev/tty1
export TERM=linux
export XDG_RUNTIME_DIR=/run/user/$UID/


isitrgui=$(cat ~/.config/retroarch/retroarch.cfg | grep 'menu_driver = "rgui"')

Test_Button_B
if [ "$?" -eq "10" ]; then
  printf "\033c" > /dev/tty1
  boot_controls
  if [[ ! -e "/dev/input/by-path/platform-odroidgo2-joypad-event-joystick" ]]; then
    if test ! -z "$(cat /home/ark/.config/.DEVICE | grep RGB20PRO | tr -d '\0')"
    then
      sudo setfont /usr/share/consolefonts/Lat7-TerminusBold32x16.psf.gz
    else
      sudo setfont /usr/share/consolefonts/Lat7-TerminusBold28x14.psf.gz
    fi
  else
    sudo setfont /usr/share/consolefonts/Lat7-Terminus16.psf.gz
  fi
  cd /usr/bin/emulationstation

  while true; do

          selection=(dialog \
          --backtitle "Boot and Recovery Tools" \
          --title "BaRT" \
          --no-collapse \
          --clear \
          --cancel-label "You must select one" \
          --menu "Distro: $(cat /usr/share/plymouth/themes/text.plymouth | grep ArkOS | cut -c 7-50)        Batt: $(cat /sys/class/power_supply/battery/capacity)%" 14 60 10)

          options=(
                  "1)" "Emulationstation"
                  "2)" "Retroarch"
                  "3)" "Wifi"
                  "4)" "Enable Remote Services"
                  "5)" "351Files"
                  "6)" "Backup dArkOS Settings"
                  "7)" "Restore dArkOS Settings"
                  "8)" "Reboot"
                  "9)" "Power Off"
          )

          choices=$("${selection[@]}" "${options[@]}" 2>&1 > /dev/tty1)

          for choice in $choices; do
                  case $choice in
                          "1)") kill_boot_controls
                                printf "\033c" > /dev/tty1
                                sudo systemctl restart ogage &
                                [ -f "/home/ark/.config/.BOOT_TO_RETROARCH" ] && rm -f /home/ark/.config/.BOOT_TO_RETROARCH
                                /usr/bin/emulationstation/emulationstation.sh
                                exit
                                ;;
                          "2)") if [ ! -z "$isitrgui" ]; then
                                  if [ ! -f "~/.config/.setxmb" ]; then
                                    sed -i '/menu_driver = \"rgui\"/s//menu_driver = \"xmb\"/g' /home/ark/.config/retroarch/retroarch.cfg
                                    sed -i '/xmb_menu_color_theme = \"4\"/s//xmb_menu_color_theme = \"8\"/g' /home/ark/.config/retroarch/retroarch.cfg
                                    touch ~/.config/.setxmb
                                  fi
                                fi
                                touch /home/ark/.config/.BOOT_TO_RETROARCH
                                kill_boot_controls
                                sed -i '/input_exit_emulator_btn/s//;input_exit_emulator_btn/g' /home/ark/.config/retroarch/retroarch.cfg
                                printf "\033c" > /dev/tty1
                                sudo systemctl restart ogage &
                                /usr/bin/emulationstation/emulationstation.sh
                                exit
                                ;;
                          "3)") kill_boot_controls
                                /opt/system/Wifi.sh
                                boot_controls
                                ;;
                          "4)") /opt/system/Enable\ Remote\ Services.sh 2>&1 > /dev/tty1
                                ;;
                          "5)") /opt/system/351Files.sh 2>&1 > /dev/tty1
                                ;;
                          "6)") kill_boot_controls
                                /opt/system/Advanced/"Backup dArkOS Settings.sh" 2>&1 > /dev/tty1
                                boot_controls
                                ;;
                          "7)") kill_boot_controls
                                /opt/system/Advanced/"Restore dArkOS Settings.sh" 2>&1 > /dev/tty1
                                boot_controls
                                ;;
                          "8)") sudo reboot
                                ;;
                          "9)") sudo shutdown now
                                ;;
                  esac
          done
  done
else
  if [ ! -f "/home/ark/.config/.BOOT_TO_RETROARCH" ]; then
    esdir="$(dirname $0)"
    while true; do
        rm -f /tmp/es-restart /tmp/es-sysrestart /tmp/es-shutdown
        "$esdir/emulationstation" "$@"
        ret=$?
        [ -f /tmp/es-restart ] && continue
        if [ -f /tmp/es-sysrestart ]; then
            rm -f /tmp/es-sysrestart
            systemctl reboot
            break
        fi
        if [ -f /tmp/es-shutdown ]; then
            rm -f /tmp/es-shutdown
            systemctl poweroff
            break
        fi
        break
    done
    exit $ret
  else
    if [[ "$(tr -d '\0' < /proc/device-tree/compatible)" == *"rk3326"* ]]; then
      gpu="ff400000"
    else
      gpu="fde60000"
    fi
    echo performance | sudo tee /sys/devices/platform/${gpu}.gpu/devfreq/${gpu}.gpu/governor
    echo performance | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
    echo performance | sudo tee /sys/devices/platform/dmc/devfreq/dmc/governor
    while true; do
      retroarch "$@"
    done
  fi
fi
