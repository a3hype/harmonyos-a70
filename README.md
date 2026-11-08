# HarmonyOS NEXT on the Galaxy A70

An experimental bring-up of HarmonyOS NEXT 5.1 on the Samsung Galaxy A70 (SM-A705FN, `a70q`, Snapdragon 675 / sm6150). It runs the XGSI image on the BrBL kernel, with the missing hardware support added on top.

**This is a personal test project, not a daily-driver ROM.** It has been tested on one phone. It relies on modified system files and weakened security checks, so don't sign in to real accounts on it.

## Install

From Lineage 22.2 with a 15 GB system partition: sideload BrBL, flash XGSI 5.1.0.128(SP5), format data, then sideload the patcher zip from [Releases](../../releases). Full steps: **[docs/INSTALL.md](docs/INSTALL.md)**.

## Status

| Feature | State | Notes |
|---|---|---|
| Boot, launcher, GPU, display | Works | 60 Hz |
| Touch | Works | Through a small input bridge |
| Wi-Fi | Works | Randomized MAC; no real MAC source yet |
| Bluetooth | Works | Paired and played audio with one speaker; file transfer and calls untested |
| Speaker and mic | Works | Samsung's speaker tuning (ACDB) isn't loaded, so keep the volume reasonable |
| Storage, Files, Photos | Works | |
| Battery level and charging | Works | Low-battery warning and shutdown untested |
| Restart and Power off | Works | Through a sysrq workaround; a plugged-in Power off comes back on |
| Compass, auto-brightness, camera level | Works | Through the Snapdragon Sensor Core |
| Proximity | Partial | Readings work; not tested in a call |
| Status bar layout | Works | Cutout width is an estimate; landscape unchecked |
| Auto-rotate | Not working | Sensor data is fine; the window manager doesn't seem to use it. See [TECHNICAL](docs/TECHNICAL.md) |
| Browser | Not working | The ArkWeb engine installs, but the browser crashes in the GPU driver while drawing |
| About device | Works | Custom name through one parameter line; the version lines are built at runtime and can't be changed this way |
| Camera | Not working | Kernel side is present; no camera service for this hardware |
| Calls, mobile data, IMEI | Not working | The modem runs, but there is no RIL |
| Earpiece, headphones | Untested | |
| Fingerprint | Not working | |

## What this repo is

Source code, instructions and the patcher zip. **It contains no firmware.** You supply XGSI, BrBL and a Lineage vendor yourself, and your phone's own Samsung files are copied from its own partitions.

| Path | Contents |
|---|---|
| `src/rproc` | Starts the ADSP, modem and Wi-Fi from userspace, sends Wi-Fi board data |
| `src/touch` | Input bridge for the touchscreen |
| `src/audio` | Audio library for the A70's sound card |
| `src/bluetooth` | Bluetooth vendor library and bare HCI test tools |
| `src/sensors` | Sensor library over QRTR/QMI, plus the `hexagonrpcd` launcher |
| `src/hexagonrpcd` | The A70 changes to linux-msm `hexagonrpc` (GPLv3) |
| `src/motion` | Rotation from gravity; works for the camera app, **not installed by default** |
| `src/shutdown` | Finishes reboots and power-offs, fixes charger-mode boots |
| `src/shim` | Fakes fs-verity for the app installer, see the warnings below |
| `src/tools` | Read-only test tools |
| `patcher` | The recovery zip's install script, edits and manifest; `scripts/make_patcher_zip.py` builds it |
| `config`, `scripts` | Boot config, install and patch scripts, `flash_system.py` |
| `docs` | [Install](docs/INSTALL.md), [Build](docs/BUILD.md), [Technical notes](docs/TECHNICAL.md) |

## Security warnings

Know what this changes before you install it:

- **Bootloader:** unlocking trips Knox permanently and wipes the phone.
- **Code signing:** `src/shim` makes the app installer believe fs-verity succeeded, because this kernel doesn't have it.
- **App sandbox:** the PID namespace is removed for the browser renderer.
- **Root daemons:** several helpers run as root, including `hexagonrpcd`, which serves files to the DSP.
- **SELinux:** permissive in this setup.
- **Sysrq workaround:** XGSI refuses the reboot call, even from a process with `CAP_SYS_BOOT`. `a70-shutdownd` uses `/proc/sysrq-trigger` instead, which steps around that block. Why the call is blocked isn't known.

## Privacy

Your sensor registry, EFS and modem partitions hold calibration data and identifiers such as the IMEI. **Never commit or publish your own backups.** The `.gitignore` covers the usual names, but check before you push.

## Credits

Ported by **[@a3hype](https://github.com/a3hype)**. This builds directly on other people's work. See [NOTICE](NOTICE.md).

- **XGSI**: the base system.
- **BrBL kernel**, by Brian & BlazeSpire ([A70 Chat](https://t.me/a70chat) on Telegram). Its source isn't public yet, so this repo only links to it and includes none of its files.
- **[hmos2sweet](https://github.com/tsryzenstech/hmos2sweet)**: the approach and much of the audio, Bluetooth and sensor code.
- **[linux-msm/hexagonrpc](https://github.com/linux-msm/hexagonrpc)**: the DSP file server.
- **OpenHarmony**: the driver interface headers.

Not affiliated with Huawei, Samsung, Qualcomm or any project above. HarmonyOS and Galaxy are trademarks of their owners.
