#!/bin/bash
set -euo pipefail

# Vanilla dArkOS commit 4837de95426d2390f0ac2b8c87e8d29973ce04e removes the
# generic OpenCL linker alias that points at the Mali GLES library. Preserve
# the versioned OpenCL loader installed by Debian and remove only this alias.
for libdir in /usr/lib/aarch64-linux-gnu /usr/lib/arm-linux-gnueabihf; do
	alias="$libdir/libOpenCL.so"
	if [[ -L "$alias" && "$(readlink "$alias")" == "libMali.so" ]]; then
		rm -- "$alias"
	fi
done
