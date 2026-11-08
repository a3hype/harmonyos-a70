# Technical notes

What was found while bringing this up, and why things are the way they are. **Verified** means seen in logs or command output on the test phone. **Inferred** means a likely explanation that isn't proven.

## The base

- XGSI is a single ext4 `system` image. Its PID 1 chroots into `/system/xgsi/harmony` and hides block devices: `/dev/block` is empty and everything, including PID 1, runs under seccomp mode 2. (Verified.)
- The BrBL kernel (4.14) has binderfs, QRTR, ADSP RPC and the Spectra camera drivers. It lacks memcg, PID, IPC and user namespaces, `/dev/access_token_id`, fs-verity's code-sign ioctl, `/dev/xpm` and `qos_ctrl`. (Verified.)
- XGSI emulates access tokens in `libxgsi_tokens.so`, and hmdfs with tmpfs and bind mounts. (Verified.)
- A fix can be tested without touching `system`: bind-mount a file over a library, then restart the service.

## Services that never start

Many host services are defined as `disabled` with `start-mode: condition` in `chip_prod/etc/init/hdf_devhost.cfg`, and nothing starts them on this hardware. That alone caused several failures:

| Service | Symptom without it |
|---|---|
| `power_host` | Battery always shows 100%, and the real level is invisible to the system. (Verified: the phone was really at 1%.) |
| `blue_host` | Bluetooth never turns on |
| `sensor_host` | No sensors |

## Hangs of about 1 second

`memmgrservice` failed its init because the memcg files don't exist, so its system ability stayed unpublished. Every window visibility call then polled for it for about 1.2 s, blocking SceneBoard's main thread. Creating placeholder memcg files fixed it. (Verified: futex stalls went from 8 to 0.)

## Storage

`StartUser` failed because `/mnt/hmdfs` and `/mnt/data` didn't exist, so XGSI's emulation never ran. Creating them at `post-fs-data` fixed it. Creating folders then needed the real `Docs` bind-mounted over `/mnt/user/100/{sharefs,nosharefs}/docs/currentUser`. The bind's reach into the Files sandbox isn't fully understood: the folder was created by another uid. (Works, partly inferred.)

## Reboot and power off

Init stops all services, and then the phone sits alive with the screen off. The kernel never logs a restart or power-off. (Verified.)

A probe calling `reboot()` with an invalid command, from a service with full capabilities including `CAP_SYS_BOOT`, got `EPERM`. A stock kernel returns `EINVAL` there. So the call is refused early, most likely by XGSI's seccomp filter; a Samsung kernel hook isn't ruled out. (Inferred. The filter hasn't been dumped.)

`a70-shutdownd` works around it: when `foundation`, `render_service` and `hdcd` have all been gone for 3 checks, it syncs, remounts read-only and writes to `/proc/sysrq-trigger`. It reads the last `poweroff shutdown` or `poweroff reboot` line in the recent saved hilog to choose. With neither, it powers off at 3% battery or lower, and otherwise reboots. The low-battery case is untested.

## Charger-mode boots

A cable on a powered-off phone makes the bootloader show a charging logo, then boot the full OS with `androidboot.mode=charger`. The touch driver doesn't bind in that mode. (Verified.) A plain reboot returns in normal mode, so the daemon reboots once, up to twice, tracked by a counter.

## Sensors

The sensors live in the Snapdragon Sensor Core on the ADSP. Android and this port talk to it through QMI service 400 over QRTR, with protobuf messages.

- Service 400 was present at boot, but every physical sensor and `registry` returned "none" until `hexagonrpcd` served the config and registry to the DSP's sensors domain. (Verified.)
- `sns_reg_version` must be readable. Without it the DSP tries to rebuild the registry through a read-only server and gives up. (Verified.)
- The registry is served read-only from a copy, so runtime calibration isn't saved across boots.
- Early lookups answer "none" while the sensors register, so the library retries for 5 minutes. A library that gave up after one pass caused a dead compass at boot only. (Verified.)
- Proximity uses SSC event `769`, not the standard `1025`. Field 1 reads 1 near, 0 far, 2 data update, from one cover test. (Inferred from that sequence.)
- Auto-brightness appeared in the UI on its own once the sensors were up from boot. (Verified.)

## Auto-rotate (open)

Rotation isn't locked, and toggling it reaches SceneBoard. `src/motion` implements `HDF_MOTION_TYPE_ROTATION` from gravity, and the camera app does subscribe through `msdp`. But no app rotates, and no window manager subscription was seen. `foundation` does load `libmotion_client`. Where the window manager gets rotation from is unknown. A separate question is whether its own logic rejects this hardware.

## Bluetooth

The firmware (`apbtfw11.tlv`, `apnv11.bin`) matches the chip. `apnv11.bin` is a container with two sections, and pairing only worked after sending just the Bluetooth NVM section (type 2). (Verified.)

## Browser (open)

The ArkWeb engine has to be installed by hand, from `module.img`, because the module mount doesn't happen under XGSI. The renderer still logs `set access token id failed`, and the compositor crashes with SIGSEGV in `CompositorGpuTh`, through `libEGL_impl` into `libsurface`. An ABI mismatch there is a guess. `libxgsi_tokens.so` is the current lead for the token failure.

## Camera (open)

The kernel exposes the ISP, sensors, flash, actuator, EEPROM, JPEG and ICP. `vendor` has `camera.qcom.so` and Samsung sensor modules for `s5kgd1`, `s5kgd1_front`, `s5k4ha` and `gc5035`. Samsung's provider is named `_32`, and `lib/camera` is 35 MB against 1.2 MB in `lib64/camera`, so the working stack looks 32-bit. (Inferred.)

hmos2sweet's experimental route runs CamX in a private mini-Android through a bridge. Here it would probably need a 32-bit runtime. Nothing has been tried on the phone.

## Other things seen, not understood

- `contexthub_ble_host` crashes each boot, seemingly harmlessly.
- The status bar icons once sat correctly with Wi-Fi off, before the cutout fix.
- `kgsl` premature free faults and `eglSetDamageRegionKHR` errors.
- The boot job in `a70rproc.cfg` fires twice, at about 8 s and at boot completion, so `a70-rproc` runs two copies. The cause is unknown.
- `render_service` crashed once in a `GrResourceCache` purge.
- Battery cost of the CPU and GPU `performance` locks and of the sensor domain is unmeasured.
