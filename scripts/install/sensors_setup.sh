#!/system/bin/sh
# Prepares /data/a70fw/sensors from the pushed archive and lays out the tree hexagonrpcd serves.
# Touches only /data/a70fw/sensors. The real persist and vendor partitions stay as they are.
S=/data/a70fw/sensors
A=/data/local/tmp/sc.tgz
[ -s $A ] || { echo "no archive"; exit 2; }
mkdir -p $S/x $S/root/sensors
cd $S/x && tar xzf $A || { echo "tar failed"; exit 2; }
R=$S/root
rm -rf $R/sensors/config $R/sensors/registry $R/sensors/sns_reg.conf $R/dsp $R/socinfo $R/acdb
cp -r $S/x/sc/vendor_etc_sensors/config $R/sensors/config
cp -r $S/x/sc/persist_sensors/registry/registry $R/sensors/registry
cp $S/x/sc/vendor_etc_sensors/sns_reg_config $R/sensors/sns_reg.conf
cp $S/x/sc/persist_sensors/registry/sns_reg_version $R/sensors/sns_reg_version
mkdir -p $R/acdb
ln -s /data/a70fw/dsp $R/dsp
ln -s /sys/devices/soc0 $R/socinfo
chmod -R a+rX $S
rm -rf $S/x
chmod 755 /data/local/tmp/hexagonrpcd
ls -l $R | cut -c1-70
echo "config $(ls $R/sensors/config | wc -l) registry $(ls $R/sensors/registry | wc -l) conf $(wc -c < $R/sensors/sns_reg.conf)"
ls $R/dsp/adsp | wc -l
cat $R/socinfo/soc_id 2>&1 | cut -c1-20
