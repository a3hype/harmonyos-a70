# Build

All of this was built with `clang` from the OpenHarmony 5.0 native SDK, targeting `aarch64-linux-ohos`, then tested on HarmonyOS NEXT 5.1 from XGSI.

```
CLANG="<sdk>/native/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=<sdk>/native/sysroot"
```

## Static tools

These need nothing from the phone:

```
$CLANG -static -O2 -Wall -o a70-shutdownd src/shutdown/a70_shutdownd.c
$CLANG -static -O2 -Wall -o a70-hexrpcd   src/sensors/a70_hexrpcd_start.c
$CLANG -static -O2 -Wall -o a70_ssc_probe src/tools/a70_ssc_probe.c
```

`a70-rproc` and `a70-bdf` also need `qrtr` from linux-msm: use `-include src/rproc/compat.h`, add `-I<msm>/qrtr/lib`, and link `<msm>/libqrtr/libqrtr.a`. Apply `src/rproc/upstream-patches` to `rmtfs`, `pd-mapper` and `tqftpserv`.

## hexagonrpcd (GPLv3)

1. Get [linux-msm/hexagonrpc](https://github.com/linux-msm/hexagonrpc), tested at commit `da7a3742`.
2. Use `src/hexagonrpcd/rpcd_builder.c` in place of `hexagonrpcd/rpcd_builder.c`. It adds `/sys/class/sensors/ssc_core` to the served tree.
3. Add `src/hexagonrpcd/sweet_fastrpc.c` and put `src/hexagonrpcd/compat` on the include path. These translate the upstream fastrpc interface to this kernel's `adsprpc` driver.
4. Compile `hexagonrpcd/*.c`, `hexagonrpcd/interface/*.c` and `libhexagonrpc/*.c`, except `rpcd_builder.c`, with `-static -D_GNU_SOURCE -Iinclude -Ihexagonrpcd`.

## Libraries that load into HarmonyOS services

The sensor, motion, audio and Bluetooth libraries load into firmware services, so they must match the firmware's C++ library exactly.

**The firmware's libc++ uses the `std::__h` namespace, while the SDK's uses `std::__n1`.** A plain build compiles but crashes on the phone. So:

1. Copy the SDK's `libcxx-ohos/include/c++/v1/__config_site` into a local folder and change `_LIBCPP_ABI_NAMESPACE` to `__h`. Put that folder first on the include path.
2. Pull `libc++.so`, `libutils.z.so` and `libhilog.so` **from your own phone** into a local folder. Don't commit them.
3. Link against those with `-nostdlib++ -Wl,--no-undefined -Wl,--allow-shlib-undefined`, so a mismatch fails at build time.

You also need these OpenHarmony 5.1.0 headers, which aren't in this repo: `hdi_base.h`, `hdf_load_vdi.h`, `hdf_base.h` from `drivers_hdf_core`, and `refbase.h` from `commonlibrary_c_utils`. `src/sensors/stub` holds small stand-ins for the rest.

```
clang++ --target=aarch64-linux-ohos --sysroot=<sdk>/native/sysroot -shared -fPIC -O2 -std=c++17 \
  -fvisibility=hidden -nostdlib++ -I<cfg> -Isrc/sensors/vdi -Isrc/sensors/stub -I<oh-headers> \
  -Wl,-soname,libhdi_sensor_impl.z.so -Wl,--no-undefined -Wl,--allow-shlib-undefined \
  -o libhdi_sensor_impl.z.so src/sensors/a70_sensors_vdi.cpp <devlibs>/libutils.z.so <devlibs>/libc++.so -lc
```

The motion library is the same, using `src/motion`, plus `libhilog.so` and the soname `libhdi_motion_impl.z.so`.

Audio and Bluetooth sources in `src` are generated output, and `scripts/patch/a70_audio_patch.py` and `a70_bt_patch.py` show how they were made from hmos2sweet's sources. `patch_vdi_retry.py` is already applied to `src/sensors/a70_sensors_vdi.cpp`; it's kept as a record.
