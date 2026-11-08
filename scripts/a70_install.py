"""A70 HarmonyOS installer.

  python -I tools/a70_install.py check
      Read-only, while HarmonyOS runs (hdc). For every component file, says whether the phone has our build,
      the stock file, or something else. Use it to confirm the list is complete on a working phone.

  python -I tools/a70_install.py install            (dry run, from recovery with adb)
  python -I tools/a70_install.py install --write    (writes)
      Mounts system read-write, checks each target is either stock or ours, then writes ours, keeping labels,
      owner and mode. Stock or already-installed files are fine; anything else stops the install before writing.

Not covered yet, do by hand (see docs/INSTALL.md): the a612_zap copy into the real vendor partition,
the /data/a70fw files (firmware, rmtfs copies, mixer XML, sensors, ArkWeb), and installing ArkWeb with bm.
"""
import hashlib
import os
import subprocess
import sys
import tempfile
import time

HDC = os.environ.get("A70_HDC", "hdc")      # path to hdc, or hdc on PATH
ADB = os.environ.get("A70_ADB", "adb")      # path to adb, or adb on PATH
SERIAL = os.environ.get("A70_SERIAL", "")   # optional; empty means the only connected device
HARMONY = "/tmp/sysro/system/xgsi/harmony"  # in recovery, with system mounted at /tmp/sysro

# (component, target under harmony/, our file, stock file or None if new, mode, owner, label reference)
BIN = ("0755", "root:shell", "system/bin/hilog")
CFG = ("0500", "root:root", "system/etc/init/multimodalinput.cfg")
LIB = ("0644", "root:root", None)  # existing files keep their own label
FILES = [
    ("touch", "system/bin/a70touch", "touch_bridge/a70touch", None, *BIN),
    ("touch", "system/etc/init/a70touch.cfg", "touch_bridge/a70touch.cfg", None, *CFG),
    ("touch", "system/etc/hiview/event_logger_config", "backup/event_logger_config.nosysrq", "backup/event_logger_config.orig", *LIB),
    ("coproc", "system/bin/a70-rproc", "coproc/bin/a70-rproc", None, *BIN),
    ("coproc", "system/bin/a70-bdf", "coproc/bin/a70-bdf", None, *BIN),
    ("coproc", "system/bin/qrtr-ns", "coproc/bin/qrtr-ns", None, *BIN),
    ("coproc", "system/bin/rmtfs", "coproc/bin/rmtfs", None, *BIN),
    ("coproc", "system/bin/pd-mapper", "coproc/bin/pd-mapper", None, *BIN),
    ("coproc", "system/bin/tqftpserv", "coproc/bin/tqftpserv", None, *BIN),
    ("coproc", "system/etc/init/a70rproc.cfg", "coproc/a70rproc.cfg", None, *CFG),
    ("audio", "vendor/lib64/libaudio_primary_impl.z.so", "coproc/audio/libaudio_primary_impl.z.so", "backup/audio-orig/orig/libaudio_primary_impl.z.so.orig", *LIB),
    ("audio", "vendor/etc/audio/audio_policy_config.xml", "coproc/audio/audio_policy_config.xml", "backup/audio-orig/orig/audio_policy_config.xml.orig", *LIB),
    ("bluetooth", "vendor/lib64/libbt_vendor.z.so", "coproc/bt/libbt_vendor.z.so", "backup/bt-orig/libbt_vendor.z.so", *LIB),
    ("sensors", "vendor/lib64/libhdi_sensor_impl.z.so", "coproc/bin/libhdi_sensor_impl.z.so", "backup/sensors-orig/libhdi_sensor_impl.z.so", *LIB),
    ("sensors", "system/bin/hexagonrpcd", "coproc/bin/hexagonrpcd", None, *BIN),
    ("sensors", "system/bin/a70-hexrpcd", "coproc/bin/a70-hexrpcd", None, *BIN),
    ("shutdown", "system/bin/a70-shutdownd", "coproc/bin/a70-shutdownd", None, *BIN),
    ("shim", "system/lib64/liba70noverity.so", "coproc/bin/liba70noverity.so", None, "0644", "root:root", "system/lib64/libcode_sign_utils.z.so"),
    ("shim", "system/bin/a70-installs", "coproc/bin/a70-installs", None, *BIN),
    ("shim", "system/etc/init/installs.cfg", "coproc/installs.cfg", "backup/installs-orig/installs.cfg", *LIB),
    ("browser", "system/etc/sandbox/appdata-sandbox.json", "coproc/appdata-sandbox.json", "backup/sandbox-orig/appdata-sandbox.json", *LIB),
    ("statusbar", "sys_prod/etc/window/resources/display_manager_config.xml", "coproc/display_manager_config.xml", "backup/window-orig/sys_prod_display_manager_config.xml", *LIB),
]


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def local_hashes():
    out, missing = [], []
    for comp, target, ours, stock, mode, owner, ref in FILES:
        if not os.path.exists(ours):
            missing.append(ours)
            continue
        out.append((comp, target, ours, sha(ours), sha(stock) if stock and os.path.exists(stock) else None, stock, mode, owner, ref))
    if missing:
        sys.exit("missing local files: " + ", ".join(missing))
    return out


def run(tool, *args, timeout=120):
    args = list(args)
    if SERIAL and args and args[0] in ("-t", "-s"):
        args[1] = SERIAL
    elif args and args[0] in ("-t", "-s"):
        args = args[2:]
    r = subprocess.run([tool, *args], capture_output=True, timeout=timeout)
    return r.stdout.decode("utf-8", "replace")


def classify(phone_sha, ours_sha, stock_sha):
    if phone_sha is None:
        return "absent"
    if phone_sha == ours_sha:
        return "ours"
    if stock_sha and phone_sha == stock_sha:
        return "stock"
    return "OTHER"


def check():
    items = local_hashes()
    paths = " ".join("/" + t for _, t, *_ in items)
    raw = run(HDC, "-t", SERIAL, "shell", f"for f in {paths}; do if [ -f $f ]; then sha256sum $f; else echo MISSING $f; fi; done")
    got = {}
    for line in raw.splitlines():
        parts = line.split()
        if len(parts) == 2 and len(parts[0]) == 64:
            got[parts[1]] = parts[0]
    counts = {}
    for comp, target, ours, oh, sh_, stock, *_ in items:
        state = classify(got.get("/" + target), oh, sh_)
        counts[state] = counts.get(state, 0) + 1
        print(f"{state:7} {comp:10} /{target}")
    print("summary:", counts)
    print("not checked by this tool: the a612_zap copy in the real vendor partition, /data/a70fw, ArkWeb install")


def install(write):
    items = local_hashes()
    state = run(ADB, "-s", SERIAL, "get-state").strip()
    if state != "recovery":
        sys.exit(f"phone must be in recovery with adb (state: {state or 'none'})")
    tmp = tempfile.mkdtemp()
    script = ["#!/system/bin/sh", "# generated by a70_install.py", "set -u",
              "S=/dev/block/by-name/system", f"H={HARMONY}",
              "fail() { echo \"ABORT: $1\"; umount /tmp/sysro 2>/dev/null; exit 2; }",
              "e2fsck -fn $S 2>&1 | tail -1 | cut -c1-100",
              "mkdir -p /tmp/sysro",
              f"mount -t ext4 -o {'rw' if write else 'ro'},noatime $S /tmp/sysro || fail 'mount failed'",
              "[ -d $H/system/bin ] || fail 'not an XGSI system image'",
              "OK=0; NEW=0; SAME=0"]
    for i, (comp, target, ours, oh, sh_, stock, mode, owner, ref) in enumerate(items):
        p = f"/tmp/a70i/{i}"
        allowed = oh + (f" {sh_}" if sh_ else "")
        script += [
            f"T=$H/{target}",
            f"if [ -f $T ]; then C=$(sha256sum $T | cut -d' ' -f1); else C=absent; fi",
            f"case \" {allowed} absent \" in *\" $C \"*) ;; *) fail 'unexpected file at /{target}' ;; esac",
            f"[ \"$(sha256sum {p} | cut -d' ' -f1)\" = {oh} ] || fail 'pushed file damaged: {ours}'",
            f"if [ \"$C\" = {oh} ]; then SAME=$((SAME+1)); echo 'same    /{target}'; else",
        ]
        if write:
            label = f"$(ls -Z $H/{ref} | sed 's/^ *//' | cut -d' ' -f1)" if ref else ""
            script += [
                f"  [ \"$C\" = absent ] && NEW=$((NEW+1))",
                f"  cat {p} > $T || fail 'write failed /{target}'",
                f"  chown {owner} $T; chmod {mode} $T",
            ]
            if ref:
                script += [f"  L={label}; case \"$L\" in u:object_r:*) chcon \"$L\" $T ;; *) fail 'bad label for /{target}' ;; esac"]
            script += [f"  [ \"$(sha256sum $T | cut -d' ' -f1)\" = {oh} ] || fail 'verify failed /{target}'",
                       f"  OK=$((OK+1)); echo 'wrote   /{target}'"]
        else:
            script += [f"  echo \"would write /{target} (now: $C)\""]
        script += ["fi"]
    script += ["sync", "umount /tmp/sysro; echo umount=$?", "e2fsck -fn $S 2>&1 | tail -1 | cut -c1-100",
               "echo \"done: written $OK (new $NEW), already ours $SAME\""]
    sp = os.path.join(tmp, "a70_install.sh")
    with open(sp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(script) + "\n")
    run(ADB, "-s", SERIAL, "shell", "rm -rf /tmp/a70i; mkdir -p /tmp/a70i")
    for i, item in enumerate(items):
        dev = ["-s", SERIAL] if SERIAL else []
        r = subprocess.run([ADB, *dev, "push", item[2], f"/tmp/a70i/{i}"], capture_output=True)
        if r.returncode:
            sys.exit(f"push failed: {item[2]}")
    subprocess.run([ADB, *(["-s", SERIAL] if SERIAL else []), "push", sp, "/tmp/a70i/run.sh"], capture_output=True)
    print(("WRITING" if write else "DRY RUN, system mounted read-only") + f" at {time.strftime('%H:%M:%S')}")
    print(run(ADB, "-s", SERIAL, "shell", "sh /tmp/a70i/run.sh", timeout=600))


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("check", "install"):
        sys.exit(__doc__)
    if sys.argv[1] == "check":
        check()
    else:
        install("--write" in sys.argv)
