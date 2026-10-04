# Clean-image identity policy — 2026-10-04

These three ROOTFS files are **firmware-image inputs**, not OTA replacements for an existing user's system:

- `/etc/machine-id`: an empty regular file, deliberately installed as root:root mode0444. Git cannot encode0444; the image manifest/assembler must apply and verify it.
- `/var/lib/dbus/machine-id`: absolute symlink to `/etc/machine-id`, resolved within the image root.
- `/etc/systemd/system/sshd-keygen.service.d/10-generate-missing-host-keys.conf`: root:root0644, clears only the existing unit's `ConditionFirstBoot` predicate.

The pinned official R36 image already has `sshd-keygen.service`, enabled through SSH service/socket wants links. It calls `ssh-keygen -A` before SSH, creates only missing keys and preserves keys on later boots. Preserve the original unit, conditions, wants links and ordering; do not add a competing generator. During assembly remove the six cloned SSH host-key files and ensure no helper-generated keys are baked into the output.

An empty machine-id keeps systemd257's generic `ConditionFirstBoot` false while allowing a unique machine-id to be established. The D-Bus link prevents reuse of the cloned independent ID. Removing the file entirely would change firstboot behavior and could trigger an unsuitable password prompt; the image's partition/ROM expansion firstboot service is a separate flow and must be preserved.

Required output evidence: correct numeric metadata/links; original keygen service and activation links unchanged; target ARM OpenSSH key generation tested in isolated scratch with idempotence; no scratch keys or helper identity remain in the image; no credentials/private-key contents in build reports. Host-only keygen or source syntax checks do not prove physical clean-card R36S boot. Actual candidate and physical-boot status belongs in the build receipt.

Baseline: [official R36 release](https://github.com/southoz/dArkOSRE-R36/releases/tag/dArkOSRE-R36%2803082026%29), raw image SHA256 `616b0292c26fc4224cf90c2b6284bdcc3cbf30f7e3f75847b08d994785a18731`. The build procedure and precise image/OTA metadata decisions are kept in `resources/image-build/`; the source overlay alone is not a firmware image.
