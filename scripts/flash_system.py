"""Flashes the XGSI system.img to the system partition in pieces, from fastbootd.

  python -I flash_system.py system.img path\\to\\fastboot.exe workdir

Why: plain `fastboot flash system system.img` silently wrote wrong data above 4 GiB with fastboot 31,
and fastboot 37 ran out of memory on the raw image. The patcher zip checks the whole image's hash afterwards.

Flash a raw image as many small Android sparse files, each addressing its own block range.

Every block of the image is covered by exactly one piece: data blocks as RAW, all-zero
blocks as FILL 0. Everything else in a piece is DONT_CARE. Stops at the first failure.
"""
import os, struct, subprocess, sys

img, fb, workdir = sys.argv[1], sys.argv[2], sys.argv[3]
BS = 4096
MAX_RAW = 200 * 1024 * 1024 // BS  # raw blocks per piece
MAX_CHUNKS = 2000
size = os.path.getsize(img)
assert size % BS == 0, "image is not block aligned"
total = size // BS
ZERO = bytes(BS)
RAW, FILL, DC = 0xCAC1, 0xCAC2, 0xCAC3


def write_piece(path, chunks, start, end):
    """chunks: list of (kind, nblocks, payload bytes or None) covering [start, end)."""
    out = []
    if start > 0:
        out.append(struct.pack("<HHII", DC, 0, start, 12))
    for kind, n, data in chunks:
        if kind == RAW:
            out.append(struct.pack("<HHII", RAW, 0, n, 12 + n * BS))
            out.append(data)
        else:
            out.append(struct.pack("<HHII", FILL, 0, n, 16))
            out.append(struct.pack("<I", 0))
    if end < total:
        out.append(struct.pack("<HHII", DC, 0, total - end, 12))
    n_chunks = len(chunks) + (start > 0) + (end < total)
    hdr = struct.pack("<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, BS, total, n_chunks, 0)
    with open(path, "wb") as o:
        o.write(hdr)
        for x in out:
            o.write(x)


os.makedirs(workdir, exist_ok=True)
piece_path = os.path.join(workdir, "piece.simg")
covered = 0
n_piece = 0
with open(img, "rb") as f:
    blk = 0
    while blk < total:
        start = blk
        chunks = []
        raw_used = 0
        cur_kind, cur_n, cur_buf = None, 0, []
        while blk < total and raw_used < MAX_RAW and len(chunks) < MAX_CHUNKS:
            b = f.read(BS)
            assert len(b) == BS
            kind = FILL if b == ZERO else RAW
            if kind != cur_kind:
                if cur_kind is not None:
                    chunks.append((cur_kind, cur_n, b"".join(cur_buf) if cur_kind == RAW else None))
                cur_kind, cur_n, cur_buf = kind, 0, []
            cur_n += 1
            if kind == RAW:
                cur_buf.append(b)
                raw_used += 1
            blk += 1
        if cur_kind is not None:
            chunks.append((cur_kind, cur_n, b"".join(cur_buf) if cur_kind == RAW else None))
        write_piece(piece_path, chunks, start, blk)
        covered += sum(c[1] for c in chunks)
        n_piece += 1
        print(f"piece {n_piece}: blocks {start}-{blk - 1} raw={raw_used} chunks={len(chunks)} file={os.path.getsize(piece_path) // 1048576}MiB", flush=True)
        r = subprocess.run([fb, "flash", "system", piece_path], capture_output=True, text=True, errors="replace")
        tail = (r.stderr or r.stdout).strip().splitlines()[-2:]
        print("   ", r.returncode, " | ".join(tail)[:150], flush=True)
        if r.returncode != 0:
            print("FAILED; stopping", flush=True)
            sys.exit(1)
os.remove(piece_path)
print(f"DONE pieces={n_piece} covered={covered} total={total} complete={covered == total}", flush=True)
