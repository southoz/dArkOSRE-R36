#!/bin/bash

# dArkOSRE uses the active EmulationStation ROM root (/roms or /roms2).
# Keep the launcher rooted at the ROM shortcut passed by EmulationStation so
# Singe game files and frame data stay on the selected card.
# Adapted from vanilla dArkOS's Singe/ZLua launcher and fixes:
# https://github.com/christianhaitian/dArkOS/commit/394c1d23b6c40a604c6e2fc51e5a5a9c6031790c
# https://github.com/christianhaitian/dArkOS/commit/2651cbff95ae862599baa0f3bd1bdb48fc47c270
# https://github.com/christianhaitian/dArkOS/commit/ea0c56365456ab52074f543af4b505aab2a5a203
ROM_PATH="$1"
case "$ROM_PATH" in
  /roms/alg/*) ROM_ROOT="/roms" ;;
  /roms2/alg/*) ROM_ROOT="/roms2" ;;
  *)
    echo "Singe ROM must be inside /roms/alg or /roms2/alg: $ROM_PATH" >&2
    exit 1
    ;;
esac

HYPSEUS_BIN=/opt/hypseus-singe/hypseus-singe
HYPSEUS_HOME=/opt/hypseus-singe
HYPSEUS_SHARE="$ROM_ROOT/alg"
basedir=$(basename -- "$ROM_PATH")
SINGEGAME=${basedir%.*}

if [ "$SINGEGAME" = "$basedir" ] || [ -z "$SINGEGAME" ]; then
  echo "Specify a Singe game shortcut: $0 /roms[2]/alg/<game>.alg" >&2
  exit 1
fi

# Hypseus expects both roots in its shared directory. These links follow the
# selected card instead of hard-coding vanilla's /roms layout or R36's old
# erroneous alg/roms2 subdirectory.
ln -sfn "$HYPSEUS_SHARE/roms" /opt/hypseus-singe/roms || exit 1
ln -sfn "$HYPSEUS_SHARE" /opt/hypseus-singe/singe || exit 1

if [ "$ROM_PATH" = "$HYPSEUS_SHARE/Scan_for_new_games.alg" ]; then
  printf "\033c" >> /dev/tty1
  cd "$HYPSEUS_SHARE" || exit 1
  ./Scan_for_new_games.alg
  printf "\n\nFinished scanning the alg folder for games." >> /dev/tty1
  printf "\nPlease restart emulationstation to find the new shortcuts" >> /dev/tty1
  printf "\ncreated if any.\n" >> /dev/tty1
  sleep 5
  printf "\033c" >> /dev/tty1
  exit 1
fi

GAME_DIR="$HYPSEUS_SHARE/$SINGEGAME"
FRAMEFILE="$GAME_DIR/$SINGEGAME.txt"
SINGE_SCRIPT="$GAME_DIR/$SINGEGAME.singe"
ZLUA_ZIP="$GAME_DIR/$SINGEGAME.zip"

if [ ! -f "$FRAMEFILE" ]; then
  echo "Missing framefile: $FRAMEFILE" >&2
  exit 1
fi

if [ -f "$ZLUA_ZIP" ]; then
  LUA_ARGS=(-zlua "$ZLUA_ZIP")
elif [ -f "$SINGE_SCRIPT" ]; then
  LUA_ARGS=(-script "$SINGE_SCRIPT")
else
  echo "Missing Singe script or ZLua archive:" >&2
  echo "  $SINGE_SCRIPT" >&2
  echo "  $ZLUA_ZIP" >&2
  exit 1
fi

echo "VAR=hypseus-singe" > /home/ark/.config/KILLIT
sudo systemctl restart killer_daemon.service
trap 'sudo systemctl stop killer_daemon.service' EXIT

EXTRAPARAMS=()
if [ -f "$GAME_DIR/$SINGEGAME.commands" ]; then
  read -r -a EXTRAPARAMS < "$GAME_DIR/$SINGEGAME.commands"
fi

RES=()
if grep -q '720x720' /sys/class/graphics/fb0/modes 2>/dev/null; then
  RES=(-x 720 -y 600)
fi

"$HYPSEUS_BIN" singe vldp \
  -gamepad \
  -texturestream \
  "${RES[@]}" \
  -framefile "$FRAMEFILE" \
  "${LUA_ARGS[@]}" \
  -homedir "$HYPSEUS_HOME" \
  -datadir "$HYPSEUS_HOME" \
  -fullscreen \
  "${EXTRAPARAMS[@]}"
EXIT_CODE=$?

if [ "$EXIT_CODE" -ne 0 ]; then
  if [ "$EXIT_CODE" -eq 127 ]; then
    echo "Hypseus-Singe could not start; check its shared-library dependencies." >&2
  else
    echo "Hypseus-Singe exited with code $EXIT_CODE." >&2
  fi
fi
exit "$EXIT_CODE"
