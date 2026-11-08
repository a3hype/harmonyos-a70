#!/system/bin/sh
# From recovery: installs a70-shutdownd and the cfg that starts it, which also drops the probe service.
# Expects /tmp/a70-shutdownd, /tmp/a70rproc.cfg and /tmp/a70rproc.cur (the cfg expected on system now).
S=/dev/block/by-name/system
R=/tmp/sysro/system/xgsi/harmony/system
C=$R/etc/init/a70rproc.cfg
B=$R/bin/a70-shutdownd
echo "--- e2fsck before"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
for F in /tmp/a70-shutdownd /tmp/a70rproc.cfg /tmp/a70rproc.cur; do
  [ -s "$F" ] || { echo "ABORT: $F not pushed"; exit 2; }
done
mkdir -p /tmp/sysro
mount -t ext4 -o rw,noatime $S /tmp/sysro || { echo "mount failed"; exit 1; }
fail() { echo "ABORT: $1"; umount /tmp/sysro; exit 2; }
[ -f "$C" ] || fail "no cfg"
[ "$(sha256sum "$C" | cut -d' ' -f1)" = "$(sha256sum /tmp/a70rproc.cur | cut -d' ' -f1)" ] || fail "cfg on system is not the expected one"
LBL=$(ls -Z $R/bin/a70-rproc | sed 's/^ *//' | cut -d' ' -f1)
case "$LBL" in u:object_r:*) ;; *) fail "bad label [$LBL]";; esac
[ -e "$B" ] && echo "note: replacing existing binary"
cat /tmp/a70-shutdownd > "$B" || fail "write failed"
chown root:shell "$B"
chmod 0755 "$B"
chcon "$LBL" "$B"
cat /tmp/a70rproc.cfg > "$C"
sync
[ "$(sha256sum "$B" | cut -d' ' -f1)" = "$(sha256sum /tmp/a70-shutdownd | cut -d' ' -f1)" ] && echo "ok binary" || echo "MISMATCH binary"
[ "$(sha256sum "$C" | cut -d' ' -f1)" = "$(sha256sum /tmp/a70rproc.cfg | cut -d' ' -f1)" ] && echo "ok cfg" || echo "MISMATCH cfg"
ls -lZ "$B" "$C" | cut -c1-90
umount /tmp/sysro; echo "umount=$?"
echo "--- e2fsck after"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
