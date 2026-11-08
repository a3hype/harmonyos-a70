#!/system/bin/sh
# From recovery: puts the original installs.cfg back. Expects /tmp/installs.cfg.orig.
# The shim library stays on disk, unused.
S=/dev/block/by-name/system
C=/tmp/sysro/system/xgsi/harmony/system/etc/init/installs.cfg
[ -s /tmp/installs.cfg.orig ] || { echo "ABORT: backup not pushed"; exit 2; }
mkdir -p /tmp/sysro
mount -t ext4 -o rw,noatime $S /tmp/sysro || { echo "mount failed"; exit 1; }
cat /tmp/installs.cfg.orig > "$C"
sync
[ "$(sha256sum "$C" | cut -d' ' -f1)" = "$(sha256sum /tmp/installs.cfg.orig | cut -d' ' -f1)" ] && echo "ok restored" || echo "MISMATCH"
umount /tmp/sysro; echo "umount=$?"
e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
