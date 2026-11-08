#!/system/bin/sh
# Builds /data/a70fw from the phone's own partitions, from recovery.
#   sh a70_data_stage.sh verify   builds it in /tmp/a70fw_new and compares with the existing /data/a70fw
#                                 (userdata mounted read-only). Writes nothing to any partition.
#   sh a70_data_stage.sh write    builds it directly in userdata (userdata mounted read-write).
# Covers: image (apnhlos + modem), dsp, rmtfs (plain copies of the modem partitions), sensors (vendor + persist),
# audio mixer XML (vendor), arkweb (module.img from system). Not covered: a612_zap into vendor, see copy_zap.sh.
MODE=${1:-verify}
set -u
M=/tmp/a70m
mkdir -p $M/apn $M/mod $M/dsp $M/ven $M/per $M/sys $M/ud $M/ark
fail() { echo "ABORT: $1"; for d in ark sys per ven dsp mod apn ud; do umount $M/$d 2>/dev/null; done; exit 2; }
mount -o ro /dev/block/by-name/apnhlos $M/apn || fail "apnhlos"
mount -o ro /dev/block/by-name/modem $M/mod || fail "modem"
mount -o ro /dev/block/by-name/dsp $M/dsp || fail "dsp"
mount -o ro,noload /dev/block/by-name/vendor $M/ven || fail "vendor"
mount -o ro,noload /dev/block/by-name/persist $M/per || fail "persist"
mount -o ro,noload /dev/block/by-name/system $M/sys || fail "system"
if [ "$MODE" = write ]; then
  mount -t f2fs -o rw /dev/block/by-name/userdata $M/ud || fail "userdata rw"
  O=$M/ud/a70fw
else
  mount -t f2fs -o ro /dev/block/by-name/userdata $M/ud || fail "userdata ro"
  O=/tmp/a70fw_new
  rm -rf $O
fi
mkdir -p $O/image $O/dsp $O/rmtfs $O/audio $O/sensors/root/sensors $O/arkweb

echo "--- image"
cp $M/apn/image/* $O/image/ 2>/dev/null
cp $M/mod/image/* $O/image/ 2>/dev/null
echo "files $(ls $O/image | wc -l)"
echo "--- dsp"
cp -r $M/dsp/* $O/dsp/
chmod -R 777 $O/dsp
echo "--- rmtfs (copies, so the modem never writes the real partitions)"
dd if=/dev/block/by-name/modemst1 of=$O/rmtfs/modem_fs1 bs=1048576 2>/dev/null
dd if=/dev/block/by-name/modemst2 of=$O/rmtfs/modem_fs2 bs=1048576 2>/dev/null
dd if=/dev/block/by-name/fsg of=$O/rmtfs/modem_fsg bs=1048576 2>/dev/null
dd if=/dev/block/by-name/fsc of=$O/rmtfs/modem_fsc bs=1048576 2>/dev/null
chmod 700 $O/rmtfs
echo "--- audio"
for X in $M/ven/etc/mixer_paths_idp.xml $M/ven/etc/audio/mixer_paths_idp.xml; do [ -f $X ] && cp $X $O/audio/ && break; done
ls $O/audio | xargs echo
echo "--- sensors"
R=$O/sensors/root
cp -r $M/ven/etc/sensors/config $R/sensors/config
cp -r $M/per/sensors/registry/registry $R/sensors/registry
cp $M/ven/etc/sensors/sns_reg_config $R/sensors/sns_reg.conf
cp $M/per/sensors/registry/sns_reg_version $R/sensors/sns_reg_version
mkdir -p $R/acdb
rm -f $R/dsp $R/socinfo
ln -s /data/a70fw/dsp $R/dsp
ln -s /sys/devices/soc0 $R/socinfo
echo "--- arkweb (module.img from system, through a loop device)"
IMG=$M/sys/system/xgsi/harmony/system/module_update/ArkWebCore/module.img
if [ -f $IMG ]; then
  cp $IMG /tmp/a70_module.img
  if mount -o ro,loop /tmp/a70_module.img $M/ark; then
    cp -r $M/ark/* $O/arkweb/
    umount $M/ark
  else
    echo "arkweb: loop mount failed"
  fi
  rm -f /tmp/a70_module.img
else
  echo "arkweb: module.img not found"
fi
chmod -R a+rX $O/image $O/audio $O/sensors $O/arkweb
sync

if [ "$MODE" = verify ]; then
  echo "--- compare with the existing /data/a70fw"
  E=$M/ud/a70fw
  (cd $O && find . -type f | sort | while read F; do echo "$(sha256sum "$F" | cut -d' ' -f1) $F"; done) > /tmp/a70_new.txt
  (cd $E && find ./image ./dsp ./rmtfs ./audio ./sensors ./arkweb -type f 2>/dev/null | sort | while read F; do echo "$(sha256sum "$F" | cut -d' ' -f1) $F"; done) > /tmp/a70_old.txt
  echo "built $(wc -l < /tmp/a70_new.txt) files, existing $(wc -l < /tmp/a70_old.txt)"
  echo "same: $(grep -cxFf /tmp/a70_old.txt /tmp/a70_new.txt)"
  echo "only in built (first 15):"; grep -vxFf /tmp/a70_old.txt /tmp/a70_new.txt | head -15 | cut -c66-150
  echo "only on phone (first 15):"; grep -vxFf /tmp/a70_new.txt /tmp/a70_old.txt | head -15 | cut -c66-150
  rm -rf $O
fi
for d in sys per ven dsp mod apn ud; do umount $M/$d 2>/dev/null; done
echo "mounts left: $(grep -c ' /tmp/a70m/' /proc/mounts)"
