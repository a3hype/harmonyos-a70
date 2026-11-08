#!/system/bin/sh
# From recovery: installs the sensor stack. Expects in /tmp: hexagonrpcd, a70-hexrpcd,
# libhdi_sensor_impl.new, libhdi_sensor_impl.orig, a70rproc.cfg, a70rproc.cur.
# Undo: put libhdi_sensor_impl.orig back and the previous cfg (coproc/a70rproc.before-sensors.cfg).
S=/dev/block/by-name/system
H=/tmp/sysro/system/xgsi/harmony
R=$H/system
C=$R/etc/init/a70rproc.cfg
L=$H/vendor/lib64/libhdi_sensor_impl.z.so
echo "--- e2fsck before"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
for F in hexagonrpcd a70-hexrpcd libhdi_sensor_impl.new libhdi_sensor_impl.orig a70rproc.cfg a70rproc.cur; do
  [ -s "/tmp/$F" ] || { echo "ABORT: $F not pushed"; exit 2; }
done
mkdir -p /tmp/sysro
mount -t ext4 -o rw,noatime $S /tmp/sysro || { echo "mount failed"; exit 1; }
fail() { echo "ABORT: $1"; umount /tmp/sysro; exit 2; }
sum() { sha256sum "$1" | cut -d' ' -f1; }
[ "$(sum "$C")" = "$(sum /tmp/a70rproc.cur)" ] || fail "cfg on system is not the expected one"
[ "$(sum "$L")" = "$(sum /tmp/libhdi_sensor_impl.orig)" ] || fail "vendor library differs from backup"
LBL=$(ls -Z $R/bin/a70-rproc | sed 's/^ *//' | cut -d' ' -f1)
case "$LBL" in u:object_r:*) ;; *) fail "bad label [$LBL]";; esac
for B in hexagonrpcd a70-hexrpcd; do
  cat /tmp/$B > "$R/bin/$B" || fail "write $B failed"
  chown root:shell "$R/bin/$B"
  chmod 0755 "$R/bin/$B"
  chcon "$LBL" "$R/bin/$B"
done
cat /tmp/libhdi_sensor_impl.new > "$L" || fail "write library failed"
cat /tmp/a70rproc.cfg > "$C"
sync
for P in "$R/bin/hexagonrpcd /tmp/hexagonrpcd" "$R/bin/a70-hexrpcd /tmp/a70-hexrpcd" "$L /tmp/libhdi_sensor_impl.new" "$C /tmp/a70rproc.cfg"; do
  set -- $P
  [ "$(sum "$1")" = "$(sum "$2")" ] && echo "ok $(basename $1)" || echo "MISMATCH $(basename $1)"
done
ls -lZ "$R/bin/hexagonrpcd" "$R/bin/a70-hexrpcd" "$L" | cut -c1-90
umount /tmp/sysro; echo "umount=$?"
echo "--- e2fsck after"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
