#!/usr/bin/env python3
"""Extract the largest icon from a Windows .exe as a single-image .ico file.

    tools/exe-icon.py Game.exe out.ico

Only reads the PE resource table (RT_GROUP_ICON / RT_ICON); nothing is executed.
Exit status 1 when the file has no icon.
"""
import struct
import sys

RT_ICON, RT_GROUP_ICON = 3, 14


def main(exe_path, out_path):
    d = open(exe_path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        return "not a PE file"
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt_size = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    dirs = opt + (96 if magic == 0x10B else 112)
    rsrc_rva = struct.unpack_from("<I", d, dirs + 2 * 8)[0]
    if not rsrc_rva:
        return "no resources"
    sections = []
    for i in range(nsec):
        s = opt + opt_size + i * 40
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", d, s + 8)
        sections.append((va, max(vsize, rawsize), raw))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError("rva outside sections")

    base = off(rsrc_rva)

    def entries(o):
        named, ids = struct.unpack_from("<HH", d, o + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from("<II", d, o + 16 + i * 8)
            yield name, target

    def first_leaf(target):
        # Walk name -> language levels down to the data entry.
        while target & 0x80000000:
            target = next(entries(base + (target & 0x7FFFFFFF)))[1]
        rva, size = struct.unpack_from("<II", d, base + target)
        return d[off(rva):off(rva) + size]

    icons, groups = {}, []
    for type_id, t in entries(base):
        if type_id not in (RT_ICON, RT_GROUP_ICON) or not t & 0x80000000:
            continue
        for name, n in entries(base + (t & 0x7FFFFFFF)):
            data = first_leaf(n)
            if type_id == RT_ICON:
                icons[name] = data
            else:
                groups.append(data)
    if not groups:
        return "no icon"

    # Pick the largest image of the first icon group (width/height 0 means 256).
    g = groups[0]
    count = struct.unpack_from("<H", g, 4)[0]
    best = None
    for i in range(count):
        w, h, colors, _, planes, bpp, size, icon_id = struct.unpack_from("<BBBBHHIH", g, 6 + i * 14)
        w, h = w or 256, h or 256
        key = (w * h, bpp)
        if icon_id in icons and (best is None or key > best[0]):
            best = (key, w, h, colors, planes, bpp, icons[icon_id])
    if best is None:
        return "no icon"
    _, w, h, colors, planes, bpp, data = best
    with open(out_path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, 1))
        f.write(struct.pack("<BBBBHHII", w % 256, h % 256, colors, 0, planes, bpp, len(data), 22))
        f.write(data)
    return None


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    err = main(sys.argv[1], sys.argv[2])
    if err:
        sys.stderr.write(err + "\n")
        sys.exit(1)
