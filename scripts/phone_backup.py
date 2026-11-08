"""Read-only backup of the phone from recovery. Run: python -I tools/phone_backup.py OUTDIR CRITICALDIR
Backs up the main disk's partitions and a few others by name. Skips boot firmware, which should never be
restored, and whole-disk reads beyond sda, since reading sdc coincided with adb dropping. Resumes: anything
already verified in manifest.json is kept. For each item the sha256 is computed on the phone, the data is
streamed gzip-compressed, then the PC decompresses and hashes it. Nothing is written to the phone's partitions."""
import gzip
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

ADB = os.environ.get("A70_ADB", "adb")
SERIAL = os.environ.get("A70_SERIAL", "")
DEV = ["-s", SERIAL] if SERIAL else []
OUT, CRIT = sys.argv[1], sys.argv[2]
LIMIT_GB = 45  # stop a partition if its compressed size passes this
FIRST = ["efs", "sec_efs", "persist", "modemst1", "modemst2", "fsc", "ssd", "fsg", "param", "bluetooth", "misc",
         "keystore", "persistent", "vbmeta", "devinfo", "boot", "dtbo", "recovery", "dsp", "apnhlos", "modem"]
OTHER_SDA = ["bota", "fota", "steady", "dqmdbg", "debug", "cache", "hidden", "omr"]
LAST = ["vendor", "product", "system", "userdata"]
CRITICAL = {"efs", "sec_efs", "persist", "modemst1", "modemst2", "fsc", "ssd", "fsg", "param", "bluetooth", "keystore", "persistent", "devinfo"}
MANIFEST = os.path.join(OUT, "manifest.json")


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(os.path.join(OUT, "backup.log"), "a", encoding="utf-8") as f:
        f.write(line + "\n")


def adb(*args, **kw):
    return subprocess.run([ADB, *DEV, *args], capture_output=True, **kw)


def online(wait=120):
    end = time.time() + wait
    while time.time() < end:
        r = subprocess.run([ADB, *DEV, "get-state"], capture_output=True, text=True)
        if r.stdout.strip() == "recovery":
            return True
        subprocess.run([ADB, "reconnect", "offline"], capture_output=True)
        time.sleep(5)
    return False


def sh(cmd, timeout=3600):
    return adb("shell", cmd, timeout=timeout).stdout.decode("utf-8", "replace").strip()


def stream(cmd, path):
    p = subprocess.Popen([ADB, *DEV, "exec-out", cmd], stdout=subprocess.PIPE)
    total, last = 0, time.time()
    # The phone's gzip stops at 1 MiB, so compress here. Level 1 keeps up with USB.
    with open(path, "wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", compresslevel=1) as f:
        while True:
            b = p.stdout.read(1 << 20)
            if not b:
                break
            f.write(b)
            total += len(b)
            if raw.tell() > LIMIT_GB * (1 << 30):
                p.kill()
                log(f"  LIMIT reached at {raw.tell() >> 20} MB saved")
                return False
            if time.time() - last > 60:
                log(f"  ... {total >> 20} MB read, {raw.tell() >> 20} MB saved")
                last = time.time()
    return p.wait() == 0


def gz_info(path):
    h, n = hashlib.sha256(), 0
    with gzip.open(path, "rb") as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                break
            h.update(b)
            n += len(b)
    return h.hexdigest(), n


def save(manifest):
    with open(MANIFEST, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)


def attempt(name, dev, sha_cmd, read_cmd, manifest):
    path = os.path.join(OUT, name + ".gz")
    t0 = time.time()
    want = sh(sha_cmd).split()
    if not want or len(want[0]) != 64:
        return f"phone hash failed: {want[:3]}"
    if not stream(read_cmd, path):
        return "transfer failed"
    try:
        got, size = gz_info(path)
    except Exception as e:
        return f"bad compressed file: {e}"
    if got != want[0]:
        return f"HASH MISMATCH phone {want[0][:12]} pc {got[:12]}"
    manifest[name] = {"ok": True, "device": dev, "sha256": want[0], "bytes": size, "gz_bytes": os.path.getsize(path), "read": read_cmd}
    save(manifest)
    log(f"{name}: ok {size >> 20} MB raw, {os.path.getsize(path) >> 20} MB saved, {int(time.time() - t0)} s")
    if name in CRITICAL or name.startswith("gpt_"):
        shutil.copy2(path, os.path.join(CRIT, name + ".gz"))
    return None


def backup_one(name, dev, sha_cmd, read_cmd, manifest):
    if manifest.get(name, {}).get("ok"):
        return
    for i in range(2):
        if not online():
            why = "adb not back within 2 minutes"
        else:
            why = attempt(name, dev, sha_cmd, read_cmd, manifest)
        if why is None:
            return
        log(f"{name}: try {i + 1} failed: {why}")
        time.sleep(10)
    manifest[name] = {"ok": False, "why": why}
    save(manifest)


CHUNK_MB = 128
ZERO_CHUNK = bytes(CHUNK_MB << 20)
ZERO_SHA = hashlib.sha256(ZERO_CHUNK).hexdigest()


def revive():
    """adb goes offline after long transfers. Restarting the PC-side server brings it back."""
    subprocess.run([ADB, "kill-server"], capture_output=True)
    time.sleep(2)
    subprocess.run([ADB, "start-server"], capture_output=True)
    return online(90)


def read_chunk(dev, index):
    """One verified chunk, or None. Hashes on the phone and on the PC."""
    cmd = f"dd if={dev} bs=1048576 skip={index * CHUNK_MB} count={CHUNK_MB} 2>/dev/null"
    want = sh(cmd + " | sha256sum", timeout=300).split()
    if not want or len(want[0]) != 64:
        return None
    if want[0] == ZERO_SHA:
        return ZERO_CHUNK  # all zeros, proven by the phone's hash, so nothing to transfer
    r = subprocess.run([ADB, *DEV, "exec-out", cmd], capture_output=True, timeout=300)
    if hashlib.sha256(r.stdout).hexdigest() != want[0]:
        return None
    return r.stdout


def backup_big(name, manifest):
    """Chunked backup. Each chunk becomes one gzip member, and progress survives a restart."""
    if manifest.get(name, {}).get("ok"):
        return
    dev = f"/dev/block/by-name/{name}"
    path = os.path.join(OUT, name + ".gz")
    prog_path = os.path.join(OUT, name + ".progress.json")
    prog = {"chunks": 0, "gz": 0, "raw": 0}
    if os.path.exists(prog_path):
        prog = json.load(open(prog_path, encoding="utf-8"))
    if prog["chunks"] == 0 and os.path.exists(path):
        os.remove(path)
    t0 = time.time()
    if not online(30) and not revive():
        manifest[name] = {"ok": False, "why": "adb not available"}
        save(manifest)
        return
    full = prog.get("sha256") or sh(f"sha256sum {dev}", timeout=3600).split()[0]
    prog["sha256"] = full
    size_bytes = int(sh(f"cat /sys/class/block/$(basename $(readlink -f {dev}))/size")) * 512
    total = (size_bytes + CHUNK_MB * (1 << 20) - 1) // (CHUNK_MB * (1 << 20))
    log(f"{name}: {size_bytes >> 20} MB, {total} chunks, resuming at {prog['chunks']}, phone sha {full[:12]}")
    with open(path, "ab") as f:
        f.truncate(prog["gz"])
        f.seek(prog["gz"])
        while prog["chunks"] < total:
            data = None
            for attempt_no in range(30):
                try:
                    data = read_chunk(dev, prog["chunks"])
                except subprocess.TimeoutExpired:
                    data = None
                if data is not None:
                    break
                log(f"  {name} chunk {prog['chunks']}: retry {attempt_no + 1}")
                revive()
            if data is None:
                f.close()
                manifest[name] = {"ok": False, "why": f"chunk {prog['chunks']} kept failing"}
                save(manifest)
                log(f"{name}: FAILED at chunk {prog['chunks']}")
                return
            f.write(gzip.compress(data, 1))
            f.flush()
            prog["chunks"] += 1
            prog["gz"] = f.tell()
            prog["raw"] += len(data)
            with open(prog_path, "w", encoding="utf-8") as pf:
                json.dump(prog, pf)
            if prog["chunks"] % 20 == 0:
                log(f"  {name}: {prog['chunks']}/{total}, {prog['gz'] >> 20} MB saved, {int(time.time() - t0)} s")
            if prog["gz"] > LIMIT_GB * (1 << 30):
                manifest[name] = {"ok": False, "why": "over size limit"}
                save(manifest)
                log(f"{name}: over the {LIMIT_GB} GB limit")
                return
    got, size = gz_info(path)
    good = got == full and size == size_bytes
    manifest[name] = {"ok": good, "device": dev, "sha256": full, "bytes": size, "gz_bytes": os.path.getsize(path), "read": "chunked", "why": None if good else f"final hash {got[:12]} size {size}"}
    save(manifest)
    log(f"{name}: {'ok' if good else 'FINAL HASH MISMATCH'} {size >> 20} MB raw, {os.path.getsize(path) >> 20} MB saved, {int(time.time() - t0)} s")
    if good:
        os.remove(prog_path)


def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(CRIT, exist_ok=True)
    manifest = {}
    if os.path.exists(MANIFEST):
        manifest = json.load(open(MANIFEST, encoding="utf-8"))
    manifest = {k: v for k, v in manifest.items() if v.get("ok")}
    if not online(10) and not revive():
        sys.exit("phone not in recovery with adb")
    log(f"resume with {len(manifest)} verified; device {sh('getprop ro.product.device')} battery {sh('cat /sys/class/power_supply/battery/capacity')}%")
    names = sh("ls /dev/block/by-name").split()

    sectors = int(sh("cat /sys/class/block/sda/size") or 0)
    if sectors:
        for tag, skip in (("head", 0), ("tail", sectors // 8 - 256)):
            read = f"dd if=/dev/block/sda bs=4096 skip={skip} count=256 2>/dev/null"
            backup_one(f"gpt_sda_{tag}", "/dev/block/sda", read + " | sha256sum", read, manifest)

    for n in FIRST + OTHER_SDA + LAST:
        if n not in names:
            log(f"{n}: no such partition")
            continue
        if n in LAST:
            backup_big(n, manifest)
            continue
        dev = f"/dev/block/by-name/{n}"
        backup_one(n, dev, f"sha256sum {dev}", f"dd if={dev} bs=1048576 2>/dev/null", manifest)

    save(manifest)
    shutil.copy2(MANIFEST, os.path.join(CRIT, "manifest.json"))
    if os.path.exists(os.path.join(OUT, "partition_map.txt")):
        shutil.copy2(os.path.join(OUT, "partition_map.txt"), os.path.join(CRIT, "partition_map.txt"))
    bad = [k for k, v in manifest.items() if not v.get("ok")]
    total = sum(v.get("gz_bytes", 0) for v in manifest.values())
    log(f"DONE: {len(manifest)} items, {total >> 20} MB saved, failed: {bad}")
    sys.exit(1 if bad else 0)


main()
