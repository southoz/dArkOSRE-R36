#!/bin/bash
clear

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

if [ -f "/roms/backup/arkosbackup.tar.gz" ]; then
  BACKUP_FILE="/roms/backup/arkosbackup.tar.gz"
elif [ -f "/roms2/backup/arkosbackup.tar.gz" ]; then
  BACKUP_FILE="/roms2/backup/arkosbackup.tar.gz"
else
  printf "\nNo arkosbackup.tar.gz file was found in the"
  printf "\n/roms/backup or /roms2/backup folder.\n"
  printf "Please place the archive in either folder and try again."
  sleep 7
  printf "\033[0m"
  exit 1
fi

printf "\nAre you sure you want to restore a backup of your dArkOS settings?\n"
printf "\nPress A to continue.  Press B to exit.\n"
while true
do
    Test_Button_A
    if [ "$?" -eq "10" ]; then
		LOG_FILE="$(dirname "$BACKUP_FILE")/lastarkosrestore.log"

		printf "\033[0mRestoring a backup from %s.  Please wait...\n" "$BACKUP_FILE"
		sleep 2

		sudo chmod 666 /dev/tty1

		# Pause network and FileBrowser writers while restoring their settings/database.
		NM_WAS_ACTIVE=0
		if systemctl is-active --quiet NetworkManager; then
		  sudo systemctl stop NetworkManager
		  NM_WAS_ACTIVE=1
		fi
		restart_networkmanager() {
		  if [ "$NM_WAS_ACTIVE" -eq 1 ]; then
		    sudo systemctl start NetworkManager
		    NM_WAS_ACTIVE=0
		  fi
		}
		trap restart_networkmanager EXIT
		sudo pkill -x filebrowser 2>/dev/null || true

		sudo tar --same-owner -zxhvf "$BACKUP_FILE" -C / | tee -a "$LOG_FILE"
		TAR_STATUS=${PIPESTATUS[0]}
		restart_networkmanager
		trap - EXIT
		if [ "$TAR_STATUS" -eq 0 ]; then
			# Properly restore previously set timezone
			[ -f /dev/shm/TZ ] && ln -sf /usr/share/zoneinfo/$(cat /dev/shm/TZ) /etc/localtime && sudo rm /dev/shm/TZ
			# Properly restore preferred Switch button tap to off state
			if [ ! -z $(grep '<bool name="InvertPwrBtn" value="true" />' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0') ]; then
			  [ -z $(find /home/ark/.config/.SWAPPOWERANDSUSPEND 2>/dev/null) ] && touch /home/ark/.config/.SWAPPOWERANDSUSPEND
			fi
			# Properly restore preferred Verbal Battery Warning state
			if [ ! -z $(grep '<string name="VerbalBatteryWarning" value="on" />' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0') ]; then
			  [ ! -z $(find /home/ark/.config/.NOVERBALBATTERYWARNING 2>/dev/null) ] && rm /home/ark/.config/.NOVERBALBATTERYWARNING 2>/dev/null
			  if [ ! -z $(grep '<string name="VerbalBatteryVoice" value="male1" />' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0') ]; then
			    [ ! -z $(find /home/ark/.config/.MBROLA_VOICE* 2>/dev/null) ] && rm /home/ark/.config/.MBROLA_VOICE* 2>/dev/null
			  elif [ ! -z $(grep '<string name="VerbalBatteryVoice" value="male2" />' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0') ]; then
			    [ ! -z $(find /home/ark/.config/.MBROLA_VOICE* 2>/dev/null) ] && rm /home/ark/.config/.MBROLA_VOICE* 2>/dev/null
			    touch /home/ark/.config/.MBROLA_VOICE_MALE3
			  else
			    [ ! -z $(find /home/ark/.config/.MBROLA_VOICE* 2>/dev/null) ] && rm /home/ark/.config/.MBROLA_VOICE* 2>/dev/null
			    touch /home/ark/.config/.MBROLA_VOICE_FEMALE
			  fi
			fi
			# Properly restore preferred Game Loading image state
			if [ ! -z $(grep '<string name="GameLoadingImageMode" value="ascii" />' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0') ]; then
			  [ ! -z $(find /home/ark/.config/.GameLoadingIMode* 2>/dev/null) ] && rm /home/ark/.config/.GameLoadingIMode* 2>/dev/null
			  touch /home/ark/.config/.GameLoadingIModeASCII
			elif [ ! -z $(grep '<string name="GameLoadingImageMode" value="pic" />' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0') ]; then
			  [ ! -z $(find /home/ark/.config/.GameLoadingIMode* 2>/dev/null) ] && rm /home/ark/.config/.GameLoadingIMode* 2>/dev/null
			  touch /home/ark/.config/.GameLoadingIModePIC
			  if [ ! -z $(grep '<string name="GameLoadingImage" value="default" />' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0') ]; then
			    [ ! -z $(find /home/ark/.config/.LOADING_IMAGE* 2>/dev/null) ] && rm /home/ark/.config/.LOADING_IMAGE* 2>/dev/null
			  elif [ ! -z $(grep '<string name="GameLoadingImage" value="marquee" />' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0') ]; then
			    [ ! -z $(find /home/ark/.config/.LOADING_IMAGE* 2>/dev/null) ] && rm /home/ark/.config/.LOADING_IMAGE* 2>/dev/null
				touch /home/ark/.config/.LOADING_IMAGE_MARQUEE
			  elif [ ! -z $(grep '<string name="GameLoadingImage" value="image" />' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0') ]; then
			    [ ! -z $(find /home/ark/.config/.LOADING_IMAGE* 2>/dev/null) ] && rm /home/ark/.config/.LOADING_IMAGE* 2>/dev/null
				touch /home/ark/.config/.LOADING_IMAGE_IMAGE
			  else
			    [ ! -z $(find /home/ark/.config/.LOADING_IMAGE* 2>/dev/null) ] && rm /home/ark/.config/.LOADING_IMAGE* 2>/dev/null
				touch /home/ark/.config/.LOADING_IMAGE_THUMB
			  fi
			else
			  [ ! -z $(find /home/ark/.config/.GameLoadingIMode* 2>/dev/null) ] && rm /home/ark/.config/.GameLoadingIMode* 2>/dev/null
			  touch /home/ark/.config/.GameLoadingIModeNO
			fi
			# Properly restore auto suspend timeout state
			if [ -z $(grep '<string name="AutoSuspendTimeout" value="Off" />' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0') ]; then
			  mins=$(grep '<string name="AutoSuspendTimeout"' /home/ark/.emulationstation/es_settings.cfg | tr -d '\0' | grep -oP '"\K[^"\047]+(?=["\047])' | tail -1)
			  if [ ! -z "$mins" ]; then
			    echo "$mins" > /home/ark/.config/.TIMEOUT
				/usr/local/bin/auto_suspend_update.sh
			  fi
			fi
			printf "\n\n\e[32mThe restore completed successfuly. \nYou will need to reboot your system in order for your restored settings to take effect! \n" | tee -a "$LOG_FILE"
			printf "\033[0m" | tee -a "$LOG_FILE"
			sleep 1
			printf "\nDo you want to delete the backup file?\n"
			printf "\nPress A to delete the backup then exit.  Press B to keep the backup then exit.\n"
			while true
			do
				Test_Button_A
				if [ "$?" -eq "10" ]; then
					sudo rm -fv "$BACKUP_FILE" | tee -a "$LOG_FILE"
					sleep 1
					printf "\n The arkosbackup.tar.gz file has been deleted from %s.  Exiting..." "$(dirname "$BACKUP_FILE")"
					sleep 3
					exit 0
				fi

				Test_Button_B
				if [ "$?" -eq "10" ]; then
					printf "\n Exiting without deleting the arkosbackup.tar.gz file from %s." "$(dirname "$BACKUP_FILE")" | tee -a "$LOG_FILE"
					printf "\033[0m" | tee -a "$LOG_FILE"
					sleep 3
					exit 0
				fi
			done
		else
			printf "\n\n\e[31mThe restore did NOT complete successfully! \n\e[33mVerify a valid arkosbackup.tar.gz exists in \nyour /roms/backup or /roms2/backup folder then try again.\n" | tee -a "$LOG_FILE"
			printf "\033[0m" | tee -a "$LOG_FILE"
			sleep 10
			exit 0
		fi
	fi

    Test_Button_B
    if [ "$?" -eq "10" ]; then
	  printf "\nExiting without restoring a backup."
	  sleep 1
      exit 0
	fi
done
