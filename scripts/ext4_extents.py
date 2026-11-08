"""Minimal read-only ext4 reader: finds a file by path and prints or extracts it.

  python -I tools/ext4_extents.py IMAGE PATH extents        -> size, then "logical_block physical_block count" lines
  python -I tools/ext4_extents.py IMAGE PATH extract OUT    -> writes the file's bytes to OUT

Supports extent-mapped files and linear or htree directories (read as linear). Enough for XGSI's images.
"""
import struct
import sys


class Ext4:
    def __init__(self, path):
        self.f = open(path, "rb")
        sb = self.read(1024, 1024)
        if struct.unpack_from("<H", sb, 56)[0] != 0xEF53:
            raise SystemExit("not ext4")
        self.bs = 1024 << struct.unpack_from("<I", sb, 24)[0]
        self.ipg = struct.unpack_from("<I", sb, 40)[0]
        self.isz = struct.unpack_from("<H", sb, 88)[0]
        incompat = struct.unpack_from("<I", sb, 96)[0]
        self.is64 = bool(incompat & 0x80)
        self.dsz = struct.unpack_from("<H", sb, 254)[0] if self.is64 else 32
        self.gdt = (1 if self.bs > 1024 else 2) * self.bs

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def inode(self, ino):
        g, i = divmod(ino - 1, self.ipg)
        d = self.read(self.gdt + g * self.dsz, self.dsz)
        tbl = struct.unpack_from("<I", d, 8)[0]
        if self.is64 and self.dsz >= 64:
            tbl |= struct.unpack_from("<I", d, 0x28)[0] << 32
        return self.read(tbl * self.bs + i * self.isz, self.isz)

    def extents(self, raw):
        flags = struct.unpack_from("<I", raw, 32)[0]
        if not flags & 0x80000:
            raise SystemExit("file is not extent-mapped")
        out = []
        self._walk(raw[40:100], out)
        return sorted(out)

    def _walk(self, node, out):
        magic, entries, _, depth = struct.unpack_from("<HHHH", node, 0)
        if magic != 0xF30A:
            raise SystemExit("bad extent header")
        for k in range(entries):
            e = node[12 + 12 * k: 24 + 12 * k]
            if depth == 0:
                lblk, ln, hi, lo = struct.unpack("<IHHI", e)
                unwritten = ln > 32768
                if unwritten:
                    ln -= 32768
                out.append((lblk, (hi << 32) | lo, ln, unwritten))
            else:
                lblk, lo, hi = struct.unpack_from("<IIH", e)
                self._walk(self.read(((hi << 32) | lo) * self.bs, self.bs), out)

    def size(self, raw):
        return struct.unpack_from("<I", raw, 4)[0] | (struct.unpack_from("<I", raw, 108)[0] << 32)

    def data(self, raw):
        size, buf = self.size(raw), bytearray()
        for lblk, pblk, ln, unwritten in self.extents(raw):
            if len(buf) < lblk * self.bs:
                buf.extend(bytes(lblk * self.bs - len(buf)))
            buf.extend(bytes(ln * self.bs) if unwritten else self.read(pblk * self.bs, ln * self.bs))
        return bytes(buf[:size]) + bytes(max(0, size - len(buf)))

    def lookup(self, path):
        ino = 2
        for part in [p for p in path.split("/") if p]:
            d = self.data(self.inode(ino))
            found = None
            for blk in range(0, len(d), self.bs):
                off = blk
                while off < blk + self.bs:
                    e_ino, rec, nlen = struct.unpack_from("<IHB", d, off)
                    if rec < 8:
                        break
                    if e_ino and d[off + 8: off + 8 + nlen].decode("utf-8", "replace") == part:
                        found = e_ino
                        break
                    off += rec
                if found:
                    break
            if not found:
                raise SystemExit(f"not found: {part} in {path}")
            ino = found
        return self.inode(ino)


def main():
    img, path, mode = sys.argv[1], sys.argv[2], sys.argv[3]
    fs = Ext4(img)
    raw = fs.lookup(path)
    if mode == "extents":
        print(f"size {fs.size(raw)} bs {fs.bs}")
        for lblk, pblk, ln, unwritten in fs.extents(raw):
            print(lblk, pblk, ln, "unwritten" if unwritten else "")
    else:
        with open(sys.argv[4], "wb") as o:
            o.write(fs.data(raw))


main()
