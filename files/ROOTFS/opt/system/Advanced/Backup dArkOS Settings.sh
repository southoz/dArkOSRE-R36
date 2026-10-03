#!/bin/bash
clear
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
. /usr/local/bin/buttonmon.sh

printf "\nAre you sure you want to create a backup of your dArkOS settings?\n"
printf "\nPress A to continue.  Press B to exit.\n"
while true
do
    Test_Button_A
    if [ "$?" -eq "10" ]; then
		if [ ! -d "/roms/backup/" ]; then
	      sudo mkdir -v /roms/backup
		fi
		LOG_FILE="/roms/backup/arkosbackup.log"
		BACKUP_FILE="/roms/backup/arkosbackup.tar.gz"
		TEMP_BACKUP="${BACKUP_FILE}.tmp.$$"
		printf "\033[0mCreating a backup.  Please wait...\n"
		sleep 2

		sudo chmod 666 /dev/tty1
		#tail -f $LOG_FILE >> /dev/tty1 &

		# Keep FileBrowser scope/settings when this R36 build has its database.
		EXTRA_BACKUP_FILES=()
		if [ -f "/home/ark/.config/filebrowser.db" ]; then
		  EXTRA_BACKUP_FILES+=("/home/ark/.config/filebrowser.db")
		fi

		# Backup currently set timezone
		/usr/local/bin/timezones current > /dev/shm/TZ
		sudo rm -f "$TEMP_BACKUP"
		if [ -f "/boot/rk3566.dtb" ] || [ -f "/boot/rk3566-OC.dtb" ]; then
		  sudo tar -zchvf "$TEMP_BACKUP" /home/ark/.config/panel_settings.txt /home/ark/.kodi/ /etc/NetworkManager/system-connections /home/ark/.config/retroarch/retroarch.cfg /home/ark/.config/retroarch/config /home/ark/.config/retroarch/retroarch-core-options.cfg /home/ark/.config/retroarch32/retroarch.cfg /home/ark/.config/retroarch32/retroarch-core-options.cfg /home/ark/.config/retroarch32/config /home/ark/.emulationstation/collections /home/ark/.emulationstation/es_settings.cfg /opt/amiberry/savestates /opt/amiberry/whdboot /opt/mupen64plus/InputAutoCfg.ini /opt/drastic/config/drastic.cfg /dev/shm/TZ /home/ark/.bigpemu_userdata/ "${EXTRA_BACKUP_FILES[@]}" | tee -a "$LOG_FILE"
		  TAR_STATUS=${PIPESTATUS[0]}
		else
		  sudo tar -zchvf "$TEMP_BACKUP" /etc/NetworkManager/system-connections /home/ark/.config/retroarch/retroarch.cfg /home/ark/.config/retroarch/config /home/ark/.config/retroarch32/retroarch.cfg /home/ark/.config/retroarch32/config /home/ark/.emulationstation/collections /home/ark/.emulationstation/es_settings.cfg /opt/amiberry/savestates /opt/amiberry/whdboot /opt/mupen64plus/InputAutoCfg.ini /opt/drastic/config/drastic.cfg /dev/shm/TZ "${EXTRA_BACKUP_FILES[@]}" | tee -a "$LOG_FILE"
		  TAR_STATUS=${PIPESTATUS[0]}
		fi

		if [ "$TAR_STATUS" -eq 0 ] && sudo mv -f "$TEMP_BACKUP" "$BACKUP_FILE"; then
		  if [ ! -z "$(cat /etc/fstab | grep roms2 | tr -d '\0')" ]; then
			if [ ! -d "/roms2/backup/" ]; then
	          sudo mkdir -v /roms2/backup
			fi
			if [ -f "/roms2/backup/arkosbackup.tar.gz" ]; then
			  sudo rm /roms2/backup/arkosbackup.tar.gz
			fi
			sudo cp /roms/backup/arkosbackup.* /roms2/backup/.
		  fi
		  printf "\n\n\e[32mThe backup completed successfuly. \nYour settings backup is located in the backup folder on the easyroms partition and is named arkosbackup.tar.gz. \nKeep it somewhere safe! \n"  | tee -a "$LOG_FILE"
		  printf "\033[0m"  | tee -a "$LOG_FILE"
		  sleep 5
		else
		  sudo rm -f "$TEMP_BACKUP"
		  printf "\n\n\e[31mThe backup did NOT complete successfully! \n\e[33mVerify you have at least 1GB of space available on your easyroms partition then try again.\n" | tee -a "$LOG_FILE"
		  printf "\033[0m" | tee -a "$LOG_FILE"
		  sleep 5
		fi
	    exit 0
	fi

    Test_Button_B
    if [ "$?" -eq "10" ]; then
	  printf "\nExiting without creating a backup."
	  sleep 1
      exit 0
	fi
done
