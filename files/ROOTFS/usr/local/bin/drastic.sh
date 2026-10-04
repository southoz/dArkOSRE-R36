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

# DSperate is optional beside Drastic and Advanced Drastic. Keep its settings,
# saves, states, and cheats on the same ROM card as the selected game.
if [[ $1 == "dsperate" ]]; then
	# The existing SD switchers rewrite literal /roms/ and /roms2/ paths
	# throughout this file. Match both roots without a rewritable literal.
	if [[ "$2" =~ ^/(roms2?)/nds/ ]]; then
		rom_root="${BASH_REMATCH[1]}"
	else
		printf 'DSperate: expected a Nintendo DS ROM in the selected ROM card nds directory.\n' >&2
		exit 1
	fi
	if [[ ! -f "$2" ]]; then
		printf 'DSperate: ROM not found: %s\n' "$2" >&2
		exit 1
	fi

	rom_dir="/$rom_root/nds"
	data_dir="$rom_dir/dsperate"
	config="$data_dir/dsperate.ini"
	mkdir -p "$data_dir/saves" "$data_dir/states" "$rom_dir/cheats"
	if [[ ! -s "$config" ]]; then
		cp /opt/DSperate/config/dsperate.ini "$config"
	fi
	sed -i -E \
		-e "s|^saves[[:space:]]*=.*$|saves = $data_dir/saves|" \
		-e "s|^states[[:space:]]*=.*$|states = $data_dir/states|" \
		-e "s|^cheats[[:space:]]*=.*$|cheats = $rom_dir/cheats|" \
		"$config"

	game="$2"
	workdir=""
	cleanup_dsperate() {
		if [[ -n "$workdir" && -d "$workdir" ]]; then
			rm -rf -- "$workdir"
		fi
		sudo systemctl stop killer_daemon.service
		sudo systemctl restart ogage &
	}
	trap cleanup_dsperate EXIT
	case "${2##*.}" in
		7z|7Z)
			sevenzip="$(command -v 7zzs || command -v 7z || true)"
			if [[ -z "$sevenzip" && -x /tmp/7zzs.aarch64 ]]; then
				sevenzip=/tmp/7zzs.aarch64
			fi
			if [[ -z "$sevenzip" ]]; then
				printf 'DSperate: 7z support needs the 7zzs or 7z command.\n' >&2
				exit 1
			fi
			workdir="$(mktemp -d "$rom_dir/.dsperate-cache.XXXXXX")"
			if ! "$sevenzip" e "$2" -bd -y "-o$workdir" '*.nds' '*.NDS'; then
				printf 'DSperate: could not extract an NDS ROM from %s.\n' "$2" >&2
				exit 1
			fi
			game="$(find "$workdir" -type f -iname '*.nds' -print -quit)"
			if [[ -z "$game" ]]; then
				printf 'DSperate: no .nds file found in %s.\n' "$2" >&2
				exit 1
			fi
			;;
		nds|NDS|zip|ZIP) ;;
		*) printf 'DSperate: unsupported Nintendo DS ROM format: %s\n' "${2##*.}" >&2; exit 1 ;;
	esac

	echo 'VAR=drastic' > /home/ark/.config/KILLIT
	sudo systemctl restart killer_daemon.service
	SDL_GAMECONTROLLERCONFIG='190000004b4800000011000000010000,GO-Super Gamepad,x:b3,a:b0,b:b1,y:b2,back:b12,start:b13,dpleft:b10,dpdown:b9,dpright:b11,dpup:b8,leftshoulder:b4,lefttrigger:b6,rightshoulder:b5,righttrigger:b7,leftstick:b14,rightstick:b15,guide:b16,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,' \
		/opt/DSperate/dsperate "$game" --config "$config"
fi
