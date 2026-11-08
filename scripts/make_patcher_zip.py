"""Builds the recovery patcher zip from patcher/ and a folder of built binaries.

  python -I scripts/make_patcher_zip.py BIN_DIR OUT.zip

BIN_DIR holds your builds, named as their install targets (a70touch, a70touch.cfg, a70-rproc, ...,
libhdi_sensor_impl.z.so, liba70noverity.so). Each must match the hash in patcher/manifest.txt, which lists the
files tested on the phone. If your build differs, update the manifest on purpose and test it before sharing.
The release zip is made this way.
"""
import hashlib
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    bindir, out = sys.argv[1], sys.argv[2]
    man = os.path.join(ROOT, "patcher", "manifest.txt")
    entries, bad = [], []
    for line in open(man, encoding="utf-8").read().splitlines():
        t, target, src, *_, ours = line.split("|")
        if t == "F":
            local = os.path.join(bindir, os.path.basename(target))
            if not os.path.isfile(local):
                bad.append(f"missing {local}")
            elif sha(local) != ours:
                bad.append(f"hash differs: {local}")
            entries.append((local, src))
        else:
            entries.append((os.path.join(ROOT, "patcher", src), src))
    if bad:
        sys.exit("\n".join(bad))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(ROOT, "patcher", "update-binary"), "META-INF/com/google/android/update-binary")
        z.writestr("META-INF/com/google/android/updater-script", "# dummy, the work is done by update-binary\n")
        z.write(man, "manifest.txt")
        z.write(os.path.join(ROOT, "scripts", "install", "a70_data_stage.sh"), "data_stage.sh")
        z.write(os.path.join(ROOT, "patcher", "README.txt"), "README.txt")
        for local, name in entries:
            z.write(local, name)
    print(f"{out}: {len(entries)} entries, sha256 {sha(out)}")


main()
