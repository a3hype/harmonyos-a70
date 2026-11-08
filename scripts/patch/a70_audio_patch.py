"""Makes the A70 audio library source from hmos2sweet's sweet_audio_vdi.cpp.

Usage: python -I a70_audio_patch.py <sweet_audio_vdi.cpp> <output.cpp>
Fails unless every expected piece of code is found exactly once.
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8", newline="").read()
edits = [
    # Mixer paths: the A70's own file, on /data because /vendor is read-only under XGSI.
    ('''const char *MIXER_XML[] = {
    "/vendor/etc/sweet/mixer_paths_idp.xml",
    "/vendor/etc/sweet/mixer_paths_overlay_static.xml",
};''',
     '''const char *MIXER_XML[] = {
    "/data/a70fw/audio/mixer_paths_idp.xml",
};'''),
    # Back end: the A70's speaker amp is a WSA881x on WSA_CDC_DMA_RX_0, 24 bit in
    # audio_platform_info.xml. sweet used Awinic amps on PRIM_MI2S.
    ('''        /* aw882xx amplifiers on the primary MI2S port, 24 bit like the stock HAL */
        g_route.SetCtl("PRIM_MI2S_RX SampleRate", r);
        g_route.SetCtl("PRIM_MI2S_RX Format", "S24_LE");
        g_route.SetCtl("PRIM_MI2S_RX Channels", "Two");''',
     '''        /* WSA881x amplifier on the WSA codec DMA port, 24 bit like the stock HAL */
        g_route.SetCtl("WSA_CDC_DMA_RX_0 SampleRate", r);
        g_route.SetCtl("WSA_CDC_DMA_RX_0 Format", "S24_LE");'''),
    # Usecase: on the A70 "deep-buffer-playback" alone routes to RX_CDC_DMA_RX_0, the headset
    # and earpiece port. The speaker needs "deep-buffer-playback speaker" for WSA.
    ('''r->usecasePath = (r->devicePath == "headphones" && g_route.HasPath(withDev)) ? withDev : base;''',
     '''r->usecasePath = g_route.HasPath(withDev) ? withDev : base;'''),
]
for old, new in edits:
    n = text.count(old)
    if n != 1:
        sys.exit(f"expected one match, found {n}: {old[:70]!r}")
    text = text.replace(old, new)
with open(dst, "w", encoding="utf-8", newline="") as o:
    o.write(text)
print("patched", len(edits), "places ->", dst)
