#!/system/bin/sh
# From recovery: adds a70-installs and updates installs.cfg to use it.
# Expects /tmp/a70-installs and /tmp/installs.cfg.new. Undo: restore_installs.sh
S=/dev/block/by-name/system
R=/tmp/sysro/system/xgsi/harmony/system
C=$R/etc/init/installs.cfg
B=$R/bin/a70-installs
echo "--- e2fsck before"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
for F in /tmp/a70-installs /tmp/installs.cfg.new; do
  [ -s "$F" ] || { echo "ABORT: $F not pushed"; exit 2; }
done
mkdir -p /tmp/sysro
mount -t ext4 -o rw,noatime $S /tmp/sysro || { echo "mount failed"; exit 1; }
fail() { echo "ABORT: $1"; umount /tmp/sysro; exit 2; }
[ -f "$C" ] || fail "no installs.cfg"
[ -f "$R/lib64/liba70noverity.so" ] || fail "shim missing"
[ -f "$R/bin/hilog" ] || fail "reference hilog missing"
LBL=$(ls -Z $R/bin/hilog | sed 's/^ *//' | cut -d' ' -f1)
case "$LBL" in u:object_r:*) ;; *) fail "bad bin label [$LBL]";; esac
cat /tmp/a70-installs > "$B"
chown root:shell "$B"
chmod 0755 "$B"
chcon "$LBL" "$B"
cat /tmp/installs.cfg.new > "$C"
sync
[ "$(sha256sum "$B" | cut -d' ' -f1)" = "$(sha256sum /tmp/a70-installs | cut -d' ' -f1)" ] && echo "ok wrapper" || echo "MISMATCH wrapper"
[ "$(sha256sum "$C" | cut -d' ' -f1)" = "$(sha256sum /tmp/installs.cfg.new | cut -d' ' -f1)" ] && echo "ok cfg" || echo "MISMATCH cfg"
ls -lZ "$B" | cut -c1-90
umount /tmp/sysro; echo "umount=$?"
echo "--- e2fsck after"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
