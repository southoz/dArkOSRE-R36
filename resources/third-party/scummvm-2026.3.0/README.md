# ScummVM2026.3.0 exact binary storage

The reviewed RK3326/AArch64 executable is108,414,800bytes (103MiB), exceeding GitHub's100MiB normal Git-file limit. GitHub denied uploading its new LFS object to this public fork. Store the exact bytes in a deterministic gzip artifact (38,501,224bytes) within normal Git instead; no stripping, patching, rebuilding or gameplay change is involved. Both compressed and restored hashes/sizes are in `manifest.json`.

From the firmware fork root, run before source-overlay/image preparation:

```sh
python3 resources/third-party/scummvm-2026.3.0/materialize.py
```

This verifies the vendored archive and restores `files/ROOTFS/opt/scummvm/scummvm` offline with executable mode0755. Repeated invocation accepts the exact existing binary; unexpected data and symlinks are refused. The restored file is ignored by Git to keep the public fork LFS-independent. Every byte needed to restore it is present in this repository. Image assemblers must verify/materialize it before consuming the overlay and use the image's separately reviewed numeric metadata rules.

The updated OTA already carries this same executable; compressed source storage does not alter the installed path or update runtime. [Original emulator provenance](../../emulator-artifact-provenance.md) retains upstream input/build references. This storage change adds no license grant and does not change ScummVM's original licensing/source obligations.
