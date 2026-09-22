"""
音频端点探针（纯 ctypes + CoreAudio，零依赖）

用途：
  1. 读取所有渲染端点的静音态与主音量 —— 验证桥的端点静音是否残留、
     以及「桥输出端点被静音吞掉」这一架构冲突是否存在。
  2. 轮询端点 ID 集合 —— 检出设备枚举抖动（USB 音频设备反复出现/消失，
     是「断续咔哒」的典型来源）。

用法：
  python audio_probe.py                        单次快照
  python audio_probe.py --watch 60             轮询 60 秒，报告端点增删与静音态变化
  python audio_probe.py --unmute <id 子串>     解除该端点静音（"ALL" = 全部）
  python audio_probe.py --mute   <id 子串>     置该端点静音（"ALL" = 全部）
    写操作用于急救：桥被强杀后端点静音残留、或需要把被静默掉的系统音量救回来。

输出可直接与桥 /api/devices 的 key 对照得到设备名。
"""

import ctypes
import sys
import time
from ctypes import (
    POINTER, WINFUNCTYPE, byref, c_float, c_int, c_ubyte, c_uint, c_ulong,
    c_ushort, c_void_p, c_wchar_p,
)

ole32 = ctypes.windll.ole32

HRESULT = c_int
S_OK = 0
CLSCTX_ALL = 23           # INPROC_SERVER | INPROC_HANDLER | LOCAL_SERVER | REMOTE_SERVER
eRender = 0
DEVICE_STATE_ACTIVE = 1


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", c_ulong), ("Data2", c_ushort), ("Data3", c_ushort),
        ("Data4", c_ubyte * 8),
    ]


def guid(d1, d2, d3, s):
    return GUID(d1, d2, d3, (c_ubyte * 8)(*s))


CLSID_MMDeviceEnumerator = guid(
    0xBCDE0395, 0xE52F, 0x467C, (0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E))
IID_IMMDeviceEnumerator = guid(
    0xA95664D2, 0x9614, 0x4F35, (0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6))
IID_IAudioEndpointVolume = guid(
    0x5CDF2C82, 0x841E, 0x4546, (0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A))

# --- COM vtable 直调 ---------------------------------------------------------
# 对象首字段即 vtable 指针；index 按各接口声明顺序（IUnknown 占 0/1/2）。


def method(obj, index, restype, *argtypes):
    vt = ctypes.cast(obj, POINTER(c_void_p))[0]
    addr = ctypes.cast(vt, POINTER(c_void_p))[index]
    return WINFUNCTYPE(restype, c_void_p, *argtypes)(addr)


def release(obj):
    if obj:
        method(obj, 2, c_ulong)(obj)


def enumerate_endpoints(action=None):
    """返回 [{id, mute, volume}]；失败抛异常。

    action(vol_ptr, dev_id, entry) 可选：对每个成功拿到 IAudioEndpointVolume 的
    端点调用一次，用于就地做写操作（如解除静音）。entry 此时已填好 mute/volume，
    便于调用方按需决策。
    """
    en = c_void_p()
    hr = ole32.CoCreateInstance(
        byref(CLSID_MMDeviceEnumerator), None, CLSCTX_ALL,
        byref(IID_IMMDeviceEnumerator), byref(en))
    if hr != S_OK:
        raise RuntimeError("CoCreateInstance(MMDeviceEnumerator) hr=0x%08X" % (hr & 0xFFFFFFFF))

    out = []
    try:
        coll = c_void_p()
        hr = method(en, 3, HRESULT, c_int, c_ulong, POINTER(c_void_p))(
            en, eRender, DEVICE_STATE_ACTIVE, byref(coll))
        if hr != S_OK:
            raise RuntimeError("EnumAudioEndpoints hr=0x%08X" % (hr & 0xFFFFFFFF))
        try:
            n = c_uint()
            method(coll, 3, HRESULT, POINTER(c_uint))(coll, byref(n))
            for i in range(n.value):
                dev = c_void_p()
                if method(coll, 4, HRESULT, c_uint, POINTER(c_void_p))(
                        coll, i, byref(dev)) != S_OK:
                    continue
                try:
                    wid = c_wchar_p()
                    if method(dev, 5, HRESULT, POINTER(c_wchar_p))(dev, byref(wid)) != S_OK:
                        continue
                    dev_id = wid.value or ""
                    ole32.CoTaskMemFree(wid)

                    entry = {"id": dev_id, "mute": None, "volume": None}
                    vol = c_void_p()
                    hr = method(dev, 3, HRESULT, POINTER(GUID), c_ulong, c_void_p,
                                POINTER(c_void_p))(
                        dev, byref(IID_IAudioEndpointVolume), CLSCTX_ALL, None, byref(vol))
                    if hr == S_OK and vol:
                        try:
                            mv = c_int()
                            if method(vol, 15, HRESULT, POINTER(c_int))(vol, byref(mv)) == S_OK:
                                entry["mute"] = bool(mv.value)
                            fv = c_float()
                            if method(vol, 9, HRESULT, POINTER(c_float))(vol, byref(fv)) == S_OK:
                                entry["volume"] = round(fv.value, 4)
                            if action is not None:
                                action(vol, dev_id, entry)
                        finally:
                            release(vol)
                    out.append(entry)
                finally:
                    release(dev)
        finally:
            release(coll)
    finally:
        release(en)
    return out


def snapshot_line(e):
    m = {True: "MUTE", False: "    ", None: "??  "}[e["mute"]]
    v = "%.3f" % e["volume"] if e["volume"] is not None else "  ?  "
    return "[%s] vol=%s  %s" % (m, v, e["id"])


def force_mute(vol, dev_id, entry, value):
    """IAudioEndpointVolume::SetMute（vtable 14）。SetMute(BOOL, LPCGUID)。"""
    hr = method(vol, 14, HRESULT, c_int, c_void_p)(vol, 1 if value else 0, None)
    print("    -> SetMute(%s) hr=0x%08X" % ("TRUE" if value else "FALSE", hr & 0xFFFFFFFF))
    return hr == S_OK


def main():
    watch = 0
    if "--watch" in sys.argv:
        i = sys.argv.index("--watch")
        watch = int(sys.argv[i + 1]) if i + 1 < len(sys.argv) else 60

    # 写操作：--unmute <id 子串> / --mute <id 子串>，子串 "ALL" 表示全部端点。
    # 用途：桥异常退出后端点静音残留、或需要把被静默掉的系统音量救回来时，
    # 直接由外部解除，不必依赖桥自身恢复。
    write_action = None
    target = None
    for flag, val in (("--unmute", False), ("--mute", True)):
        if flag in sys.argv:
            write_action = val
            j = sys.argv.index(flag) + 1
            target = sys.argv[j] if j < len(sys.argv) else "ALL"

    ole32.CoInitializeEx(None, 0)   # STA
    try:
        if write_action is not None:
            hits = []

            def act(vol, dev_id, entry):
                if target != "ALL" and target not in dev_id:
                    return
                hits.append(dev_id)
                print("[写] %s  当前 mute=%s vol=%s" % (dev_id, entry["mute"], entry["volume"]))
                force_mute(vol, dev_id, entry, write_action)

            enumerate_endpoints(action=act)
            print("=== 命中并已设置 %d 个端点（%s -> mute=%s）===" % (
                len(hits), "全部" if target == "ALL" else target,
                "TRUE" if write_action else "FALSE"))
            if not hits:
                print("!! 没有任何端点匹配 %r —— 检查子串是否写对" % target)
            return

        first = enumerate_endpoints()
        print("=== 快照: 共 %d 个 ACTIVE 渲染端点 ===" % len(first))
        for e in first:
            print(snapshot_line(e))

        if watch <= 0:
            return

        print("\n=== 轮询 %d 秒（每 1 秒），只打印变化 ===" % watch)
        base = {e["id"]: e for e in first}
        t0 = time.time()
        end = t0 + watch
        while time.time() < end:
            time.sleep(1.0)
            try:
                cur = enumerate_endpoints()
            except Exception as ex:
                print("[%.1fs] 枚举失败: %s" % (time.time() - t0, ex))
                continue
            cmap = {e["id"]: e for e in cur}
            for k in set(cmap) - set(base):
                print("[%6.1fs] + 端点出现 %s" % (time.time() - t0, k))
            for k in set(base) - set(cmap):
                print("[%6.1fs] - 端点消失 %s" % (time.time() - t0, k))
            for k in set(cmap) & set(base):
                if cmap[k]["mute"] != base[k]["mute"]:
                    print("[%6.1fs] ~ 静音态变化 %s: %s -> %s" % (
                        time.time() - t0, k, base[k]["mute"], cmap[k]["mute"]))
                if cmap[k]["volume"] != base[k]["volume"]:
                    print("[%6.1fs] ~ 音量变化 %s: %s -> %s" % (
                        time.time() - t0, k, base[k]["volume"], cmap[k]["volume"]))
            base = cmap
        print("=== 轮询结束：端点集合 %d 个 ===" % len(base))
    finally:
        ole32.CoUninitialize()


if __name__ == "__main__":
    main()
