"""Restores one partition from a phone_backup.py backup, from recovery.
Run: python -I tools/phone_restore.py BACKUPDIR NAME --write
Without --write it only checks the backup and the phone and says what it would do.
Every 64 MiB piece is pushed to /tmp, hashed on the phone, written with dd, then read back and hashed again.
At the end the whole partition must match the hash in manifest.json. Refuses boot firmware and mounted targets."""
import gzip
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time

ADB = os.environ.get("A70_ADB", "adb")
SERIAL = os.environ.get("A70_SERIAL", "")
DEV = ["-s", SERIAL] if SERIAL else []
PIECE = 64 << 20
REFUSE = {"xbl", "xbl_config", "tz", "hyp", "abl", "aop", "devcfg", "keymaster", "cmnlib", "cmnlib64", "qupfw",
          "uefisecapp", "storsec", "secdata", "ddr", "pmic", "apdp", "msadp", "dpo", "limits", "bksecapp", "vk"}


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


def sh(cmd, timeout=600):
    return subprocess.run([ADB, *DEV, "shell", cmd], capture_output=True, timeout=timeout).stdout.decode("utf-8", "replace").strip()


def online(wait=90):
    end = time.time() + wait
    while time.time() < end:
        if subprocess.run([ADB, *DEV, "get-state"], capture_output=True, text=True).stdout.strip() == "recovery":
            return True
        time.sleep(3)
    return False


def revive():
    subprocess.run([ADB, "kill-server"], capture_output=True)
    time.sleep(2)
    subprocess.run([ADB, "start-server"], capture_output=True)
    return online(90)


def main():
    out, name = sys.argv[1], sys.argv[2]
    write = "--write" in sys.argv
    if name in REFUSE:
        sys.exit(f"refusing to restore boot firmware: {name}")
    man = json.load(open(os.path.join(out, "manifest.json"), encoding="utf-8"))
    entry = man.get(name)
    if not entry or not entry.get("ok"):
        sys.exit(f"{name}: no verified backup")
    want, size = entry["sha256"], entry["bytes"]
    gz = os.path.join(out, name + ".gz")

    log(f"{name}: checking the backup file against the manifest")
    h = hashlib.sha256()
    with gzip.open(gz, "rb") as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                break
            h.update(b)
    if h.hexdigest() != want:
        sys.exit("backup file does not match its manifest hash")

    if not online(10) and not revive():
        sys.exit("phone not in recovery with adb")
    dev = f"/dev/block/by-name/{name}"
    real = sh(f"readlink -f {dev}")
    if (real + " ") in sh("cat /proc/mounts") or (dev + " ") in sh("cat /proc/mounts"):
        sys.exit(f"{name} is mounted, unmount it first")
    psize = int(sh(f"cat /sys/class/block/$(basename {real})/size")) * 512
    if psize != size:
        sys.exit(f"size differs: partition {psize}, backup {size}")
    now = sh(f"sha256sum {dev}").split()[0]
    log(f"{name}: {size >> 20} MB at {real}, current hash {now[:12]}, backup hash {want[:12]}, same={now == want}")
    if not write:
        log("dry run: nothing written. Add --write to restore.")
        return

    pieces = (size + PIECE - 1) // PIECE
    t0 = time.time()
    tmpdir = tempfile.mkdtemp()
    with gzip.open(gz, "rb") as f:
        for i in range(pieces):
            data = f.read(PIECE)
            ph = hashlib.sha256(data).hexdigest()
            local = os.path.join(tmpdir, "piece")
            with open(local, "wb") as lf:
                lf.write(data)
            count = (len(data) + (1 << 20) - 1) >> 20
            done = False
            for attempt in range(20):
                try:
                    if not online(10) and not revive():
                        continue
                    r = subprocess.run([ADB, *DEV, "push", local, "/tmp/restore_piece"], capture_output=True, timeout=300)
                    if r.returncode != 0:
                        raise RuntimeError("push failed")
                    if sh("sha256sum /tmp/restore_piece").split()[0] != ph:
                        raise RuntimeError("pushed piece hash differs")
                    sh(f"dd if=/tmp/restore_piece of={dev} bs=1048576 seek={i * 64} count={count} conv=notrunc,fsync 2>/dev/null; sync")
                    back = sh(f"dd if={dev} bs=1048576 skip={i * 64} count={count} 2>/dev/null | sha256sum").split()[0]
                    if back != ph:
                        raise RuntimeError(f"read-back hash differs {back[:12]}")
                    done = True
                    break
                except Exception as e:
                    log(f"  piece {i}: attempt {attempt + 1} failed: {e}")
                    revive()
            if not done:
                sh("rm -f /tmp/restore_piece")
                sys.exit(f"STOPPED at piece {i} of {pieces}. The partition is partly written; run again to finish.")
            if (i + 1) % 4 == 0 or i + 1 == pieces:
                log(f"  {i + 1}/{pieces} pieces written and read back, {int(time.time() - t0)} s")
    sh("rm -f /tmp/restore_piece")
    final = sh(f"sha256sum {dev}").split()[0]
    log(f"{name}: final hash {final[:12]} {'MATCHES the backup' if final == want else 'DOES NOT MATCH'}, {int(time.time() - t0)} s")
    sys.exit(0 if final == want else 1)


main()
