#!/bin/bash

if [[ $1 == *"advanceddrastic"* ]]; then
	directory="$(dirname "$2" | cut -d "/" -f2)"

	for d in backup cheats savestates slot2; do
	  if [[ ! -d "/${directory}/nds/$d" ]]; then
		mkdir /${directory}/nds/${d}
	  fi
	  if [[ -d "/opt/advanceddrastic/$d" && ! -L "/opt/advanceddrastic/$d" ]]; then
		cp -n /opt/advanceddrastic/${d}/* /${directory}/nds/${d}/
		rm -rf /opt/advanceddrastic/${d}/
	  fi
	  ln -sf /${directory}/nds/${d} /opt/advanceddrastic/
	done

	echo "VAR=drastic" > /home/ark/.config/KILLIT
	sudo systemctl restart killer_daemon.service

	cd /opt/advanceddrastic

	export LD_LIBRARY_PATH=./libs:$LD_LIBRARY_PATH

	./drastic "$2" > /opt/advanceddrastic/drastic.log 2>&1

	sudo systemctl stop killer_daemon.service

	sudo systemctl restart ogage &
fi

if [[ $1 == "drastic" ]]; then
	directory="$(dirname "$2" | cut -d "/" -f2)"

	for d in backup cheats savestates slot2; do
	  if [[ ! -d "/${directory}/nds/$d" ]]; then
		mkdir /${directory}/nds/${d}
	  fi
	  if [[ -d "/opt/drastic/$d" && ! -L "/opt/drastic/$d" ]]; then
		cp -n /opt/drastic/${d}/* /${directory}/nds/${d}/
		rm -rf /opt/drastic/${d}/
	  fi
	  ln -sf /${directory}/nds/${d} /opt/drastic/
	done

	echo "VAR=drastic" > /home/ark/.config/KILLIT
	sudo systemctl restart killer_daemon.service

	cd /opt/drastic
	./drastic "$2"

	sudo systemctl stop killer_daemon.service

	sudo systemctl restart ogage &
fi
