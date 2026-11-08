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
# Existing copies are kept: the modem writes to them, and a re-run must not undo that.
for P in modemst1:modem_fs1 modemst2:modem_fs2 fsg:modem_fsg fsc:modem_fsc; do
  F=$O/rmtfs/${P#*:}
  if [ -s $F ]; then echo "kept ${P#*:}"; else dd if=/dev/block/by-name/${P%%:*} of=$F bs=1048576 2>/dev/null; fi
done
chmod 700 $O/rmtfs
echo "--- audio"
for X in $M/ven/etc/mixer_paths_idp.xml $M/ven/etc/audio/mixer_paths_idp.xml; do [ -f $X ] && cp $X $O/audio/ && break; done
ls $O/audio | xargs echo
echo "--- sensors"
R=$O/sensors/root
# Remove old copies first, or cp -r nests them (config/config) on a re-run.
rm -rf $R/sensors/config $R/sensors/registry
cp -r $M/ven/etc/sensors/config $R/sensors/config
cp -r $M/per/sensors/registry/registry $R/sensors/registry
cp $M/ven/etc/sensors/sns_reg_config $R/sensors/sns_reg.conf
cp $M/per/sensors/registry/sns_reg_version $R/sensors/sns_reg_version
mkdir -p $R/acdb
rm -f $R/dsp $R/socinfo
ln -s /data/a70fw/dsp $R/dsp
ln -s /sys/devices/soc0 $R/socinfo
echo "--- arkweb (ArkWebCore.hap read out of module.img with dd)"
# Loop devices fail inside a recovery sideload (I/O error at sector 0), so the .hap is copied block by block.
# The block runs were read from XGSI 5.1.0.128(SP5)'s module.img by tools/ext4_extents.py; both hashes are checked.
IMG=$M/sys/system/xgsi/harmony/system/module_update/ArkWebCore/module.img
IMG_SHA=00c74eb3f1f918738548a2bc9cfbd220109e79c60e1a28609c4b9a3896c3f6ba
HAP_SHA=8b0ec458ae45abe6ee177e8acbdadba12afac9a1bc13cff12328d0b23a4ddd90
HAP_SIZE=108246414
[ -f $IMG ] || fail "arkweb: module.img not found"
[ "$(sha256sum $IMG | cut -d' ' -f1)" = $IMG_SHA ] || fail "arkweb: module.img is not the expected version"
D=$O/arkweb/app/com.huawei.hmos.arkwebcore
rm -rf $O/arkweb/*
mkdir -p $D
HAP=$D/ArkWebCore.hap
: > $HAP
for X in 23:0:8 32:8:15 1840:23:4976 6816:5000:21428; do
  S=${X%%:*}; R2=${X#*:}; T=${R2%%:*}; N=${R2#*:}
  dd if=$IMG of=$HAP bs=4096 skip=$S seek=$T count=$N conv=notrunc 2>/dev/null || fail "arkweb: dd failed"
done
truncate -s $HAP_SIZE $HAP
[ "$(sha256sum $HAP | cut -d' ' -f1)" = $HAP_SHA ] || fail "arkweb: ArkWebCore.hap hash differs"
chmod 755 $O/arkweb/app $D; chmod 644 $HAP
echo "arkweb: ArkWebCore.hap verified"
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
