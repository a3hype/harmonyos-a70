#!/system/bin/sh
# From recovery: replaces the sys_prod display config. Expects /tmp/dmc.new and /tmp/dmc.orig.
# Undo: run again with dmc.new and dmc.orig swapped.
S=/dev/block/by-name/system
C=/tmp/sysro/system/xgsi/harmony/sys_prod/etc/window/resources/display_manager_config.xml
echo "--- e2fsck before"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
for F in /tmp/dmc.new /tmp/dmc.orig; do [ -s "$F" ] || { echo "ABORT: $F not pushed"; exit 2; }; done
mkdir -p /tmp/sysro
mount -t ext4 -o rw,noatime $S /tmp/sysro || { echo "mount failed"; exit 1; }
fail() { echo "ABORT: $1"; umount /tmp/sysro; exit 2; }
[ -f "$C" ] || fail "no config at expected path"
[ "$(sha256sum "$C" | cut -d' ' -f1)" = "$(sha256sum /tmp/dmc.orig | cut -d' ' -f1)" ] || fail "config on system differs from my backup"
cat /tmp/dmc.new > "$C"
sync
[ "$(sha256sum "$C" | cut -d' ' -f1)" = "$(sha256sum /tmp/dmc.new | cut -d' ' -f1)" ] && echo "ok config" || echo "MISMATCH config"
ls -lZ "$C" | cut -c1-100
umount /tmp/sysro; echo "umount=$?"
echo "--- e2fsck after"; e2fsck -fn $S 2>&1 | tail -1 | cut -c1-110
