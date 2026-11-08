# Components and licenses

Unless listed below, the code here is licensed under Apache-2.0.

| Path | Origin | License |
|---|---|---|
| `src/hexagonrpcd/rpcd_builder.c` | [linux-msm/hexagonrpc](https://github.com/linux-msm/hexagonrpc) at `da7a3742`, changed for the A70 | GPL-3.0-or-later, see `src/hexagonrpcd/COPYING` |
| `src/hexagonrpcd/sweet_fastrpc.c`, `src/hexagonrpcd/compat` | [hmos2sweet](https://github.com/tsryzenstech/hmos2sweet) at `1be080c`, shim for the downstream `adsprpc` driver | GPL-3.0-or-later |
| `src/sensors/a70_sensors_vdi.cpp` | Based on hmos2sweet `src/sensors/sweet_sensors_vdi.cpp`, changed for the A70 | Apache-2.0 |
| `src/sensors/vdi`, `src/motion/imotion_*.h` | OpenHarmony `drivers_peripheral` VDI headers | Apache-2.0 |
| `src/audio/a70_audio_vdi.cpp` | Generated from hmos2sweet `src/audio/sweet_audio_vdi.cpp`, which uses tinyalsa (BSD-3-Clause) | Apache-2.0 |
| `src/bluetooth/a70_bt_vendor.c` | Adapted from hmos2sweet `src/bluetooth` (WCN3990 sequence as in Linux `btqca` / `hci_qca`) | Apache-2.0 |
| `src/rproc/a70-rproc.c` | New for the A70; the approach follows hmos2sweet `sweet-rproc.c` | Apache-2.0 |
| `src/rproc/a70-bdf.c` | New for the A70; the sequence and file naming follow hmos2sweet's kernel patches 0011 and 0014, which are GPL-2.0 | Apache-2.0, see the note below |
| `src/rproc/upstream-patches` | Patches to linux-msm `qrtr`, `rmtfs`, `tqftpserv`, `pd-mapper` | BSD-3-Clause |

**Note on `a70-bdf.c`:** it's a separate userspace program following the protocol those patches use. It doesn't copy kernel code as far as is known, but no one has compared the two for that purpose. If you know it should carry a different licence, please open an issue.

## Binaries

A `hexagonrpcd` binary is GPLv3. If you share one, you must offer its source, including the changes here.

## Not included

This repo has no firmware or vendor libraries from Huawei, Samsung, Qualcomm or Xiaomi, and no XGSI, BrBL or Lineage images. You supply those, and you're responsible for following their terms.

Not affiliated with Huawei, Samsung, Qualcomm, or any project named here. HarmonyOS and Galaxy are trademarks of their owners.
