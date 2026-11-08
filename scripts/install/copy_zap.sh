#!/system/bin/sh
# Copies a612_zap.* from apnhlos (read-only) into the vendor partition's firmware dir.
V=/dev/block/by-name/vendor
echo "--- e2fsck before"; e2fsck -fn $V 2>&1 | tail -2 | cut -c1-110
mkdir -p /tmp/fw /tmp/v
mount -t vfat -o ro /dev/block/by-name/apnhlos /tmp/fw || { echo "apnhlos mount failed"; exit 1; }
mount -t ext4 -o rw,noatime $V /tmp/v || { echo "vendor rw mount failed"; umount /tmp/fw; exit 1; }
df /tmp/v | tail -1
echo "--- reference file"; ls -lZ /tmp/v/firmware/a612_rgmu.bin
for n in a612_zap.mdt a612_zap.b00 a612_zap.b01 a612_zap.b02; do
  if [ -e /tmp/v/firmware/$n ]; then echo "already exists: $n; stopping"; umount /tmp/v; umount /tmp/fw; exit 2; fi
done
for n in a612_zap.mdt a612_zap.b00 a612_zap.b01 a612_zap.b02; do
  cat /tmp/fw/image/$n > /tmp/v/firmware/$n || { echo "copy failed $n"; umount /tmp/v; umount /tmp/fw; exit 3; }
  chown root:root /tmp/v/firmware/$n
  chmod 0644 /tmp/v/firmware/$n
  chcon --reference=/tmp/v/firmware/a612_rgmu.bin /tmp/v/firmware/$n 2>/dev/null || chcon u:object_r:vendor_firmware_file:s0 /tmp/v/firmware/$n
done
sync
echo "--- verify"
for n in a612_zap.mdt a612_zap.b00 a612_zap.b01 a612_zap.b02; do
  a=$(sha256sum /tmp/fw/image/$n | cut -d' ' -f1); b=$(sha256sum /tmp/v/firmware/$n | cut -d' ' -f1)
  [ "$a" = "$b" ] && echo "match $n" || echo "MISMATCH $n"
done
ls -lZ /tmp/v/firmware | grep -E 'a612'
umount /tmp/v; echo "umount vendor=$?"
umount /tmp/fw; echo "umount apnhlos=$?"
echo "--- e2fsck after"; e2fsck -fn $V 2>&1 | tail -2 | cut -c1-110
echo "--- system check"; e2fsck -fn /dev/block/by-name/system 2>&1 | tail -1 | cut -c1-110
