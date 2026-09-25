#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
校验 PE 文件是否内嵌应用图标资源（RT_ICON / RT_GROUP_ICON）。

背景：图标此前只在 build/build_bridge.sh 里用 rc.exe 链入，而 iss 打包取的是
CMake 产物 → 每次发版装出来的都是无图标 exe（「更新后图标丢失」的根因）。
本脚本用于发布前把关：只要 exe 缺图标就直接非零退出。

用法:
  python tools/verify_icon.py                       # 默认查 build/Release/asio_bridge.exe
  python tools/verify_icon.py a.exe b.exe           # 指定文件
  python tools/verify_icon.py --installer           # 额外查安装器与卸载器
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = [
    os.path.join(ROOT, "build", "Release", "asio_bridge.exe"),
]
INSTALLER = [
    os.path.join(ROOT, "packaging", "output", "asio_bridge_setup.exe"),
    os.path.join(ROOT, "packaging", "output", "uninstall_helper.exe"),
]

RT_ICON, RT_GROUP_ICON, RT_MANIFEST = 3, 14, 24


def _sections(d):
    e = struct.unpack("<I", d[0x3C:0x40])[0]
    if d[e:e + 4] != b"PE\x00\x00":
        raise ValueError("not a PE file")
    nsec = struct.unpack("<H", d[e + 6:e + 8])[0]
    optsz = struct.unpack("<H", d[e + 20:e + 22])[0]
    magic = struct.unpack("<H", d[e + 24:e + 26])[0]
    secs = []
    so = e + 24 + optsz
    for i in range(nsec):
        o = so + i * 40
        name = d[o:o + 8].rstrip(b"\x00").decode("latin1")
        vsize, va = struct.unpack("<II", d[o + 8:o + 16])
        rawsz, rawptr = struct.unpack("<II", d[o + 16:o + 24])
        secs.append((name, va, vsize, rawptr, rawsz))
    ddoff = e + 24 + (112 if magic == 0x20B else 96)
    return secs, ddoff


def _rva2off(secs, rva):
    for _n, va, vsize, rawptr, rawsz in secs:
        if va <= rva < va + max(vsize, rawsz):
            return rawptr + (rva - va)
    return None


def _dir_entries(d, base, off):
    nn, ni = struct.unpack("<HH", d[base + off + 12:base + off + 16])
    out = []
    for i in range(nn + ni):
        e = base + off + 16 + i * 8
        idv, offv = struct.unpack("<II", d[e:e + 8])
        out.append((idv, offv, bool(idv & 0x80000000)))
    return out


def inspect(path):
    """返回 (types, group_sizes)；types 为资源类型 id 列表，group_sizes 为图标尺寸列表"""
    d = open(path, "rb").read()
    secs, ddoff = _sections(d)
    resrva = struct.unpack("<I", d[ddoff + 16:ddoff + 20])[0]
    if resrva == 0:
        return [], []
    base = _rva2off(secs, resrva)
    if base is None:
        return [], []

    types, sizes = [], []
    for tid, toff, isnamed in _dir_entries(d, base, 0):
        if isnamed:
            continue
        types.append(tid)
        if tid != RT_GROUP_ICON:
            continue
        for _nid, noff, _nm in _dir_entries(d, base, toff & 0x7FFFFFFF):
            for _lid, loff, _lm in _dir_entries(d, base, noff & 0x7FFFFFFF):
                de = base + (loff & 0x7FFFFFFF)
                data_rva, data_sz = struct.unpack("<II", d[de:de + 8])
                doff = _rva2off(secs, data_rva)
                if doff is None or data_sz < 6:
                    continue
                # GRPICONDIR: reserved(2) type(2) count(2) + entries(14 each)
                cnt = struct.unpack("<H", d[doff + 4:doff + 6])[0]
                for k in range(cnt):
                    b = doff + 6 + k * 14
                    if b + 2 > len(d):
                        break
                    w, h = d[b], d[b + 1]
                    sizes.append((w or 256, h or 256))
    return types, sizes


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    files = args if args else (DEFAULT + INSTALLER if "--installer" in sys.argv else DEFAULT)
    bad = 0
    for f in files:
        rel = os.path.relpath(f, ROOT)
        if not os.path.exists(f):
            print("MISSING  %s" % rel)
            bad += 1
            continue
        types, sizes = inspect(f)
        ok = RT_ICON in types and RT_GROUP_ICON in types
        szs = ",".join(str(s[0]) for s in sizes) if sizes else "-"
        print("%-8s %-46s types=%s" % ("OK" if ok else "NO-ICON", rel,
                                       ",".join(hex(t) for t in types) or "none"))
        if sizes:
            print("         sizes: %s" % szs)
        if not ok:
            bad += 1
    if bad:
        print("\n[FAIL] %d 个文件缺少图标资源（打包前必须修好，否则用户端图标丢失）" % bad)
        return 1
    print("\n[PASS] 全部文件都内嵌了图标资源")
    return 0


if __name__ == "__main__":
    sys.exit(main())
