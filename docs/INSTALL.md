# Install

> **Tested on one phone:** an SM-A705FN with Lineage recovery, going from Lineage 22.2 to HarmonyOS with the steps below. Other A70 variants, other recoveries such as TWRP, and other BrBL versions are untested. **This wipes the phone.**

## Quick install

### You need

| What | Where |
|---|---|
| A Galaxy A70 (`a70q`) with an **unlocked bootloader** and **Lineage 22.2** installed, with Lineage recovery | Unlocking trips Knox permanently and wipes the phone |
| The system partition **resized to 15 GB** with `A70-Repartitioner-NO-BACKUP` | [A70 Chat](https://t.me/a70chat) on Telegram. It rewrites the partition table, so back up first |
| The **BrBL** kernel zip (`BrBLKernel-a70q-…zip`) | [A70 Chat](https://t.me/a70chat) |
| **XGSI HarmonyOS NEXT 5.1.0.128(SP5)** `system.img` (6.3 GB) | XGSI's own release. This is the only build the patcher accepts |
| The **patcher zip** `a70-harmonyos-patcher.zip` | This repo's [Releases](../../releases) |
| A PC with `adb` and `fastboot` ([platform-tools](https://developer.android.com/tools/releases/platform-tools)) and Python 3 | |

Back up `efs`, `sec_efs`, `persist` and the modem partitions first, and keep those backups private: they hold your IMEI and calibration data.

### Steps

1. **Recovery → Apply update → Apply from ADB**, then on the PC:
   ```
   adb sideload BrBLKernel-a70q-<version>.zip
   ```
2. **Recovery → Advanced → Enter fastboot**, then flash XGSI in pieces:
   ```
   python flash_system.py system.img fastboot.exe flashwork
   ```
   `flash_system.py` is in `scripts/`. Don't use plain `fastboot flash system system.img`: it has written damaged data above 4 GiB while reporting OKAY.
3. **Enter recovery → Factory reset → Format data/factory reset.**
4. **Apply update → Apply from ADB**, then:
   ```
   adb sideload a70-harmonyos-patcher.zip
   ```
   Recovery says the signature can't be verified. Choose **Yes** to install anyway. The patcher checks your phone, the XGSI image (1–2 minutes), and every file before it writes anything, and stops with a message if something is wrong.
5. **Reboot system now.** The first boot takes a few minutes.

If the patcher stops, its log is in `/tmp/a70_patcher.log` in recovery (`adb pull /tmp/a70_patcher.log`). It's safe to run again.

### What the patcher does

- Writes 17 of this project's files to `system` and makes 6 small edits to XGSI's own config files. It contains no Huawei, Samsung or Qualcomm files. Every result is checked against the hash of the tested version.
- Copies the GPU firmware (`a612_zap`) from your phone's `apnhlos` into `vendor`, if it's missing.
- Copies your phone's own modem, DSP, sensor and audio files, and the ArkWeb engine, to `/data/a70fw`.

**Going back to Android:** sideload Lineage from recovery, then format data.

## Manual install (for development)

The rest of this page is how the phone was set up by hand, one change at a time. The patcher does all of it.

### What you need

- A Galaxy A70 (SM-A705FN tested; other variants unknown), with an unlocked bootloader. **Unlocking trips Knox permanently and wipes the phone.**
- A custom recovery with adb. Lineage recovery was used.
- A Lineage build for `a70q`, for its `vendor` partition.
- The BrBL kernel, and an XGSI image of HarmonyOS NEXT 5.1 from their own sources. Check their terms.
- `fastboot` and `adb`, and `hdc` and the compiler from the OpenHarmony SDK.

Back up your partition table, `efs`, `sec_efs`, `persist` and the modem partitions first, and keep those backups private.

### 1. Flash the base

1. Install Lineage once, so `vendor` is in place.
2. Sideload BrBL.
3. Flash XGSI to `system`, in chunks, and **verify it by hash**.

   Don't use plain `fastboot flash` for the image: fastboot 31.0.3 silently wrote wrong data above 4 GiB while reporting OKAY, and fastboot 37 ran out of memory on the raw image. A damaged image can look like a normal failed boot.
4. Copy `a612_zap.{mdt,b00,b01,b02}` from `apnhlos` into `vendor/firmware`. Without them the GPU faults (`RGMU Fault`) and nothing draws.
5. Remove every `k:SysRqFile` and `k:SysRq` line from `etc/hiview/event_logger_config` inside `system/xgsi/harmony/system`. On this kernel, those triggered resets when apps froze.
6. From recovery, format `userdata`. XGSI won't use encrypted `userdata`, and booting Lineage encrypts it again.

Reboot with the recovery menu's **Reboot system now**. `adb reboot` from recovery can return to recovery.

### 2. Copy the phone's own files to `/data`

XGSI hides block devices, so these all live under `/data/a70fw`. **Formatting `userdata` removes them.**

| Path | Comes from |
|---|---|
| `/data/a70fw/image` | `apnhlos` and `modem` partitions |
| `/data/a70fw/dsp` | `dsp` partition |
| `/data/a70fw/rmtfs` | Plain-file copies of `modemst1`, `modemst2`, `fsg`, `fsc`, so the modem never writes to the real partitions |
| `/data/a70fw/audio` | The A70's mixer XML, `mixer_paths_idp.xml` |
| `/data/a70fw/arkweb` | ArkWeb engine from `system/module_update/ArkWebCore/module.img`, copied from recovery |
| `/data/a70fw/sensors/root` | Sensor files, built by `scripts/install/sensors_setup.sh` from an archive of your `vendor/etc/sensors` and `persist/sensors` |

### 3. Install the components

Build everything first ([BUILD](BUILD.md)), then install from recovery with `system` mounted. All paths are under `system/xgsi/harmony`. The `system` image has few free inodes, so overwrite existing files in place where you can.

| Component | Goes to | Notes |
|---|---|---|
| Touch bridge | `system/bin/a70touch` plus a `.cfg` that starts it at `post-fs-data` | |
| Coprocessors | `system/bin/a70-rproc`, `a70-bdf`, `qrtr-ns`, `rmtfs`, `pd-mapper`, `tqftpserv` | Helpers from linux-msm with the patches in `src/rproc/upstream-patches` |
| Boot config | `system/etc/init/a70rproc.cfg` | `config/a70rproc.cfg`; starts almost everything else |
| Audio | `vendor/lib64/libaudio_primary_impl.z.so` | Plus a trimmed audio policy, not in this repo |
| Bluetooth | `vendor/lib64/libbt_vendor.z.so` | |
| Sensors | `vendor/lib64/libhdi_sensor_impl.z.so`, `system/bin/hexagonrpcd`, `a70-hexrpcd` | `scripts/install/install_sensors.sh` |
| Shutdown fix | `system/bin/a70-shutdownd` | `scripts/install/install_shutdownd.sh` |
| Installer shim | `system/lib64/liba70noverity.so`, `system/bin/a70-installs`, edited `installs.cfg` | Needed to install the ArkWeb engine; see the README's warnings |
| Status bar | Edit `sys_prod/etc/window/resources/display_manager_config.xml` | Set `defaultDisplayCutoutPath` to `M480 34 L600 34 v 74 h -120 Z`; the width is an estimate |
| Device name (optional) | Edit `sys_prod/etc/param/hw_defaults.para` | Change `const.product.name=HUAWEI Pura 70` to the name you want shown in About device. Cosmetic; keep the file's owner, mode and label |
| Renderer sandbox | `system/etc/sandbox/appdata-sandbox.json` | `scripts/patch/patch_sandbox.py` removes `pid` from the renderer entry |

`a70rproc.cfg` also creates the empty `/mnt/hmdfs`, `/mnt/data` and memcg files that storage and the memory manager need, and bind-mounts `Docs` after boot. Keep it in sync with what you actually install, because a `start` for a missing service only logs an error.

The scripts in `scripts/install` check file hashes and expect certain files in `/tmp`, so they won't run unmodified on a different setup.

### Tools

Set `A70_ADB` and `A70_HDC` if `adb` and `hdc` aren't on your PATH, and `A70_SERIAL` if more than one device is connected.

- **`scripts/phone_backup.py OUT CRITICAL`**, from recovery: backs up every partition on the main disk plus a few others, read-only. Each piece is hashed on the phone and on the PC, and it resumes after a dropped connection. Tested on one phone: 37 partitions, about 122 GB raw, saved as 6 GB.
- **`scripts/phone_restore.py OUT NAME`**, from recovery: dry run by default, `--write` to restore. Each 64 MiB piece is checked after upload and after writing, then the whole partition. It refuses boot firmware and mounted partitions. Tested once, on `vendor`.
- **`scripts/a70_install.py check`**, under HarmonyOS: compares every installed file with your builds by hash. On the test phone all 22 matched.
- **`scripts/a70_install.py install [--write]`**, from recovery: installs the 22 files. Dry run by default. It stops if a target is neither stock nor ours. Tested once on a freshly written stock `system`. It also expects your build outputs and stock backups at the paths in its `FILES` list, so adjust those for your setup.

Recovery's adb on the test phone dropped every ~128 MB on one cable and port; a different cable and port, plus a reboot of recovery, fixed it. The phone's own `gzip` stops at 1 MiB, so the scripts compress on the PC.

### 4. First boot

The first boot takes longer. With the installer shim already on `system`, the ArkWeb engine installs itself during the first boot, so no `bm install` is needed. The browser itself still crashes in the GPU driver on this phone. If the boot animation never finishes after a damaged-image boot, format `userdata`, because the app installer only scans on first boot.

Check with `hdc`:

- `dmesg | grep -E 'a70-rproc|a70sd'`
- `hidumper -s 3601 -a "-l"` should report `Total sensor:8`.
- `/proc/bus/input/devices` should list `sec_touchscreen` and `a70_touchscreen`.

## Things to know

- **Power off with a cable connected comes back on.** The bootloader starts the OS in charging mode, the touch driver doesn't load, and the daemon reboots once to fix it. At 5% battery or lower it stays in that mode so the phone can charge, so touch may not work.
- **A black screen with no response:** hold Volume Down + Power to force a reboot.
- **Going back:** sideload Lineage from recovery, which rewrites `system`, `vendor`, `boot`, `dtbo` and `vbmeta`.
