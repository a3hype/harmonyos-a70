#!/system/bin/sh
# From recovery: replaces a70rproc.cfg only. Expects /tmp/a70rproc.cfg.
S=/dev/block/by-name/system
C=/tmp/sysro/system/xgsi/harmony/system/etc/init/a70rproc.cfg
echo "--- e2fsck before"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
[ -s /tmp/a70rproc.cfg ] || { echo "ABORT: cfg not pushed"; exit 2; }
mkdir -p /tmp/sysro
mount -t ext4 -o rw,noatime $S /tmp/sysro || { echo "mount failed"; exit 1; }
[ -f "$C" ] || { echo "ABORT: no existing cfg"; umount /tmp/sysro; exit 2; }
cat /tmp/a70rproc.cfg > "$C"
sync
[ "$(sha256sum /tmp/a70rproc.cfg | cut -d' ' -f1)" = "$(sha256sum "$C" | cut -d' ' -f1)" ] && echo "ok cfg" || echo "MISMATCH cfg"
ls -lZ "$C" | cut -c1-90
umount /tmp/sysro; echo "umount=$?"
echo "--- e2fsck after"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
