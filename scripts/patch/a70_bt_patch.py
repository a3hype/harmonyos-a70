"""Adapts hmos2sweet's Bluetooth vendor library to the A70. Every replacement must match.
Firmware: /vendor/firmware/apbtfw11.tlv + apnv11.bin. Its header says product 0x000a, rom 0x0201,
which is what the chip reports, so the rom-to-file mapping is explicit and anything else fails.
apnv11.bin is a type 4 container: section type 2 (Bluetooth tags), then section type 3. Sending
the whole container makes the controller refuse paging with 0x09; only section 2 is sent.
Also asks the chip for its version after the upload, to show whether the patch took effect.
Usage: a70_bt_patch.py IN OUT"""
import sys

src = open(sys.argv[1], encoding="utf-8", newline="").read()
REPL = [
    ('libbt_vendor.z.so for HarmonyOS on the Redmi Note 10 Pro (sweet).',
     'libbt_vendor.z.so for HarmonyOS on the Galaxy A70.\n * Adapted from hmos2sweet src/bluetooth/sweet_bt_vendor.c (tsryzenstech/hmos2sweet).\n * Uses apbtfw11/apnv11 from /vendor/firmware (product 0x000a, rom 0x0201).'),
    ('fn(3, 4, 0xD000105, "SWEET_BT", "%{public}s", buf);', 'fn(3, 4, 0xD000105, "A70_BT", "%{public}s", buf);'),
    ('"<6>sweet-bt: %s\\n"', '"<6>a70-bt: %s\\n"'),
    ('static const char *FW_DIRS[] = {"/vendor/bt_firmware/image", "/vendor/bt_firmware", "/vendor/firmware_mnt/image",\n    "/vendor/firmware", NULL};',
     'static const char *FW_DIRS[] = {"/vendor/firmware", NULL};'),
    ('    int mode = 0;\n    if (is_patch && size > 16)',
     '    /* NVM container: send only the Bluetooth section */\n    if (!is_patch && size > 8 && fw[0] == 4 && fw[4] == 2) {\n        size_t inner = (size_t)(fw[5] | fw[6] << 8 | fw[7] << 16);\n        if (8 + inner <= size) {\n            memmove(fw, fw + 4, 4 + inner);\n            size = 4 + inner;\n            LOG("%s: sending the Bluetooth section, %zu bytes", name, size);\n        }\n    }\n    int mode = 0;\n    if (is_patch && size > 16)'),
    ('    snprintf(name, sizeof(name), "crbtfw%02x.tlv", v);\n    if (qca_download(fd, name, 1) < 0 && qca_download(fd, "crbtfw21.tlv", 1) < 0)\n        return -1;',
     '    LOG("rom 0x%04x, firmware suffix %02x", rom, v);\n    if (rom != 0x0201) {\n        LOG("no firmware known for rom 0x%04x", rom);\n        return -1;\n    }\n    if (qca_download(fd, "apbtfw11.tlv", 1) < 0)\n        return -1;'),
    ('    snprintf(name, sizeof(name), "crnv%02x.bin", v);\n    if (qca_download(fd, name, 0) < 0 && qca_download(fd, "crnv21.bin", 0) < 0)\n        return -1;\n    usleep(50000);',
     '    if (qca_download(fd, "apnv11.bin", 0) < 0)\n        return -1;\n    usleep(50000);\n    LOG("version after upload:");\n    if (qca_read_version(fd, &soc_id, &rom) < 0)\n        LOG("no version answer after upload");'),
]
for a, b in REPL:
    a2, b2 = a, b
    if "\r\n" in src:
        a2, b2 = a.replace("\n", "\r\n"), b.replace("\n", "\r\n")
    if src.count(a2) != 1:
        sys.exit(f"no unique match: {a[:60]}")
    src = src.replace(a2, b2)
open(sys.argv[2], "w", encoding="utf-8", newline="").write(src)
print("patched", len(REPL), "places")
