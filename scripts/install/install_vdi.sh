#!/system/bin/sh
# From recovery: replaces the sensor library only. Expects /tmp/vdi.new and /tmp/vdi.cur.
S=/dev/block/by-name/system
L=/tmp/sysro/system/xgsi/harmony/vendor/lib64/libhdi_sensor_impl.z.so
echo "--- e2fsck before"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
for F in /tmp/vdi.new /tmp/vdi.cur; do [ -s "$F" ] || { echo "ABORT: $F not pushed"; exit 2; }; done
mkdir -p /tmp/sysro
mount -t ext4 -o rw,noatime $S /tmp/sysro || { echo "mount failed"; exit 1; }
[ "$(sha256sum "$L" | cut -d' ' -f1)" = "$(sha256sum /tmp/vdi.cur | cut -d' ' -f1)" ] || { echo "ABORT: library on system is not the expected one"; umount /tmp/sysro; exit 5; }
cat /tmp/vdi.new > "$L"
sync
[ "$(sha256sum "$L" | cut -d' ' -f1)" = "$(sha256sum /tmp/vdi.new | cut -d' ' -f1)" ] && echo "ok library" || echo "MISMATCH library"
umount /tmp/sysro; echo "umount=$?"
echo "--- e2fsck after"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
