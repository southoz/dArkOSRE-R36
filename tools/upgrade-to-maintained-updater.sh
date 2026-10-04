#!/bin/bash
# dArkOSRE-R36 migration helper. Copy this file to /roms/tools and launch it
# from EmulationStation's Tools system. It changes only the update entrypoint.

set -euo pipefail

readonly EXPECTED_FIRMWARE_COMMIT="e71f28170e51aafccdefd3183e1cf3124c2771d2"
readonly UPDATE_ENTRY_URL="https://raw.githubusercontent.com/dixtuel/dArkOSRE-R36/${EXPECTED_FIRMWARE_COMMIT}/files/ROOTFS/opt/system/Update.sh"
readonly UPDATE_ENTRY_SHA256="23be2b2811c4aebfff0a9a6ed2ab1fe1a1c933a181c9117842fc11ab339b6276"
readonly MAINTAINED_FEED="https://raw.githubusercontent.com/dixtuel/darkos-updates/main"
readonly UPDATE_ENTRY="/opt/system/Update.sh"
readonly VERSION_FILE="/home/ark/.config/.VERSION"
readonly ES_SYSTEMS="/etc/emulationstation/es_systems.cfg"

show_message() {
	local message="$1"
	if command -v msgbox >/dev/null 2>&1; then
		msgbox "$message" || true
	else
		printf '%s\n' "$message"
	fi
}

fail() {
	show_message "$1"
	exit 1
}

[[ "$(id -un)" == "ark" ]] || fail "Run this from the dArkOSRE-R36 Tools menu as the ark user."
sudo -n true 2>/dev/null || fail "Password-free system update access is unavailable. No files were changed."

DEVICE_COMPAT="$(tr -d '\0' < /proc/device-tree/compatible 2>/dev/null || true)"
[[ "$DEVICE_COMPAT" == *"rk3326"* ]] || fail "This tool is only for the R36S/RK3326 dArkOSRE-R36 device family. No files were changed."
[[ -r "$VERSION_FILE" && -f "$UPDATE_ENTRY" && ! -L "$UPDATE_ENTRY" ]] || fail "Required dArkOSRE-R36 version or update entrypoint is missing. No files were changed."

CURRENT_VERSION="$(tr -d '\r\n' < "$VERSION_FILE")"
case "$CURRENT_VERSION" in
	03082026|10032026|10032026-r1|10032026-r2|10032026-r3|10032026-r4|10032026-r5) ;;
	*) fail "Installed version '$CURRENT_VERSION' is outside the supported OTA path. Install the official latest dArkOSRE-R36 image (03082026) first. This tool will not flash an image or repartition either card." ;;
esac

mountpoint -q /roms || fail "The first ROM/tools card is not mounted at /roms. No files were changed."
if [[ ! -r "$ES_SYSTEMS" ]]; then fail "EmulationStation's active system list is unavailable. No files were changed."; fi
HAS_ROM1_PATH=0
HAS_ROM2_PATH=0
grep -Fq '<path>/roms/' "$ES_SYSTEMS" && HAS_ROM1_PATH=1 || true
grep -Fq '<path>/roms2/' "$ES_SYSTEMS" && HAS_ROM2_PATH=1 || true
if [[ "$HAS_ROM1_PATH" -eq "$HAS_ROM2_PATH" ]]; then
	fail "EmulationStation must select exactly one ROM root (/roms or /roms2). No files were changed."
fi
if [[ "$HAS_ROM2_PATH" -eq 1 ]]; then
	ROM_ROOT="roms2"
	mountpoint -q /roms2 || fail "EmulationStation selects /roms2, but the second ROM card is not mounted. No files were changed."
else
	ROM_ROOT="roms"
fi

command -v wget >/dev/null 2>&1 || fail "wget is missing. No files were changed."
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is missing. No files were changed."
command -v python3 >/dev/null 2>&1 || fail "python3 is missing. No files were changed."

WORK="$(mktemp -d /dev/shm/darkosre-migration.XXXXXX)" || fail "Could not create a temporary download directory."
cleanup() { rm -rf -- "$WORK"; }
trap cleanup EXIT
DOWNLOADED_ENTRY="$WORK/Update.sh"

show_message "Checking the maintained dArkOSRE-R36 updater…"
if ! wget -q --tries=3 --timeout=30 "$UPDATE_ENTRY_URL" -O "$DOWNLOADED_ENTRY"; then
	fail "Could not download the pinned updater entrypoint over verified HTTPS. Check Wi-Fi, time, and internet access; no files were changed."
fi
printf '%s  %s\n' "$UPDATE_ENTRY_SHA256" "$DOWNLOADED_ENTRY" | sha256sum -c - >/dev/null ||
	fail "The downloaded updater entrypoint did not match the pinned SHA-256. No files were changed."
grep -Fqx "LOCATION=\"$MAINTAINED_FEED\"" "$DOWNLOADED_ENTRY" ||
	fail "The pinned entrypoint does not select the maintained update fork. No files were changed."
bash -n "$DOWNLOADED_ENTRY" || fail "The downloaded updater entrypoint has invalid shell syntax. No files were changed."

CURRENT_ENTRY_SHA256="$(sha256sum "$UPDATE_ENTRY" | awk '{print $1}')"
if [[ "$CURRENT_ENTRY_SHA256" != "$UPDATE_ENTRY_SHA256" ]]; then
	# Accept only the official R36 entrypoint with its one known update-source
	# line changed. This avoids overwriting unrelated local updater edits.
	python3 - "$UPDATE_ENTRY" "$DOWNLOADED_ENTRY" "$WORK/normalized-current" <<'PY'
import pathlib, sys
current, expected, output = map(pathlib.Path, sys.argv[1:])
data = current.read_bytes()
source_lines = (
    b'LOCATION="https://raw.githubusercontent.com/southoz/darkos-updates/master"',
    b'LOCATION="https://raw.githubusercontent.com/southoz/darkos-updates/main"',
    b'LOCATION="https://raw.githubusercontent.com/dixtuel/darkos-updates/main"',
)
target = b'LOCATION="https://raw.githubusercontent.com/dixtuel/darkos-updates/main"'
count = sum(data.count(line) for line in source_lines)
if count != 1:
    raise SystemExit("unrecognized updater entrypoint: expected exactly one known feed URL")
for line in source_lines:
    data = data.replace(line, target)
if data != expected.read_bytes():
    raise SystemExit("updater entrypoint has changes beyond the known feed URL; refusing replacement")
output.write_bytes(data)
PY
	[[ "$(sha256sum "$WORK/normalized-current" | awk '{print $1}')" == "$UPDATE_ENTRY_SHA256" ]] ||
		fail "The installed updater differs beyond the reviewed feed URL. No files were changed."

	BACKUP_DIR="/$ROM_ROOT/backup/darkosre-update/updater-migration-20261004"
	BACKUP_ARCHIVE="$BACKUP_DIR/Update.sh.before.tar"
	sudo mkdir -p "$BACKUP_DIR" || fail "Could not create the updater rollback directory on /$ROM_ROOT."
	[[ ! -e "$BACKUP_ARCHIVE" ]] || fail "A previous updater backup already exists at $BACKUP_ARCHIVE; refusing to overwrite it."
	sudo tar --numeric-owner --xattrs --acls -cpf "$BACKUP_ARCHIVE" -C / opt/system/Update.sh || fail "Could not preserve the original updater entrypoint."
	sudo tar -tf "$BACKUP_ARCHIVE" | grep -Fxq 'opt/system/Update.sh' || fail "The updater backup failed verification. No system file was changed."
	sudo sha256sum "$BACKUP_ARCHIVE" | sudo tee "$BACKUP_DIR/Update.sh.before.tar.sha256" >/dev/null
	sudo chmod 0644 "$BACKUP_ARCHIVE" "$BACKUP_DIR/Update.sh.before.tar.sha256"

	# The download is byte-for-byte the reviewed fork entrypoint. Install
	# atomically while retaining numeric owner, mode, and extended attributes.
	sudo python3 - "$DOWNLOADED_ENTRY" "$UPDATE_ENTRY" <<'PY'
import os, pathlib, stat, sys, tempfile
source, target = map(pathlib.Path, sys.argv[1:])
info = os.stat(target, follow_symlinks=False)
if not stat.S_ISREG(info.st_mode):
    raise SystemExit("update entrypoint is not a regular file")
attrs = {}
for name in os.listxattr(target, follow_symlinks=False):
    attrs[name] = os.getxattr(target, name, follow_symlinks=False)
fd, temporary = tempfile.mkstemp(prefix=".Update.sh.migration.", dir=target.parent)
try:
    with os.fdopen(fd, "wb") as stream:
        stream.write(source.read_bytes())
        stream.flush()
        os.fsync(stream.fileno())
    os.chown(temporary, info.st_uid, info.st_gid)
    os.chmod(temporary, stat.S_IMODE(info.st_mode))
    for name, value in attrs.items():
        os.setxattr(temporary, name, value, follow_symlinks=False)
    os.replace(temporary, target)
    directory = os.open(target.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)
PY
	[[ "$(sha256sum "$UPDATE_ENTRY" | awk '{print $1}')" == "$UPDATE_ENTRY_SHA256" ]] ||
		fail "The installed updater entrypoint failed its post-write hash check. Restore it from $BACKUP_ARCHIVE."
	show_message "Updater source is now set to dixtuel/darkos-updates. Original entrypoint backup: $BACKUP_ARCHIVE"
else
	show_message "Updater source already matches the maintained dArkOSRE-R36 fork."
fi

show_message "Starting the maintained sequential updater. The base OTA and R1 each restart the device; after each restart, run this tool or the normal Update menu again. After R1, compatibility, R2, R3, R4, and R5 run in order in one update session, and the device restarts after R5 succeeds."
exec "$UPDATE_ENTRY"
