HarmonyOS NEXT on the Samsung Galaxy A70 (a70q) - patcher
Ported by @a3hype. Source: https://github.com/a3hype/harmonyos-a70

For XGSI HarmonyOS NEXT 5.1.0.128(SP5) with the BrBL kernel only.
Flash from recovery with: adb sideload <this zip>

It writes system (our files plus small edits to XGSI's config files), copies the GPU firmware into vendor
if it is missing, and copies this phone's own firmware into /data/a70fw. It contains no Huawei, Samsung or
Qualcomm files. Every file is checked by hash, and it stops before writing if anything is unexpected.

hexagonrpcd is GPL-3.0-or-later; its source is in the repository. The rest is Apache-2.0 (see NOTICE.md there).
