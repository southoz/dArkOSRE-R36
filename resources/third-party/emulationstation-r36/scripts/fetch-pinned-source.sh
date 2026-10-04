#!/usr/bin/env bash
set -euo pipefail

readonly repo_url='https://github.com/christianhaitian/EmulationStation-fcamod.git'
readonly pin='74498be31cd016af6a42d00310f876d7256eff52'

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 DESTINATION" >&2
  exit 2
fi

dest=$1
if [[ -e "$dest" ]]; then
  echo "Destination already exists: $dest" >&2
  exit 1
fi

git clone --no-checkout --filter=blob:none "$repo_url" "$dest"
git -C "$dest" checkout --detach "$pin"
git -C "$dest" submodule update --init --recursive
actual=$(git -C "$dest" rev-parse HEAD)
if [[ "$actual" != "$pin" ]]; then
  echo "Unexpected source revision: $actual" >&2
  exit 1
fi
printf 'Prepared FCAMOD source at %s\n' "$actual"
