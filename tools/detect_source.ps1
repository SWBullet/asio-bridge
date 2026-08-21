# detect_source.ps1 - Probe the current audio source feeding the bridge.
#   v2 (process loopback): lists ALL active render endpoints and their audio
#   sessions (pid / process / state / volume / peak), plus the bridge's own
#   measured target + rates from the web console API.
# Usage:  detect_source.ps1
# ASCII only (PS 5.1 ANSI-safe). Requires FullLanguage (Add-Type).
$src = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
public class MMDeviceEnumeratorCom { }

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceEnumerator {
    int EnumAudioEndpoints(int flow, int mask, out IMMDeviceCollection c);
    int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice d);
    int GetDevice(string id, out IMMDevice d);
    int RegisterEndpointNotificationCallback(IntPtr cb);
    int UnregisterEndpointNotificationCallback(IntPtr cb);
}
[ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceCollection {
    int GetCount(out int n);
    int Item(int i, out IMMDevice d);
}
[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDevice {
    int Activate(ref Guid id, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o);
    int OpenPropertyStore(int access, out IPropertyStore ps);
    int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    int GetState(out int s);
}
[ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IPropertyStore {
    int GetCount(out int n);
    int GetAt(int i, out PropertyKey k);
    int GetValue(ref PropertyKey k, out PropVariant v);
    int SetValue(ref PropertyKey k, ref PropVariant v);
    int Commit();
}
[StructLayout(LayoutKind.Sequential)]
public struct PropertyKey { public Guid fmtid; public int pid; }
[StructLayout(LayoutKind.Sequential)]
public struct PropVariant {
    public ushort vt; public ushort r1, r2, r3;
    public IntPtr p0; public IntPtr p1;
}
[ComImport, Guid("BFA971F1-4D5E-40BB-935E-967039BFBEE4"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioSessionManager2 {
    int GetAudioSessionControl(IntPtr g, int f, out IAudioSessionControl c);
    int GetSimpleAudioVolume(IntPtr g, int f, out ISimpleAudioVolume v);
    int GetSessionEnumerator(out IAudioSessionEnumerator e);
    int RegisterSessionNotification(IntPtr n);
    int UnregisterSessionNotification(IntPtr n);
    int RegisterDuckNotification(string id, IntPtr n);
    int UnregisterDuckNotification(string id, IntPtr n);
}
[ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioSessionEnumerator {
    int GetCount(out int n);
    int GetSession(int i, [MarshalAs(UnmanagedType.IUnknown)] out object c);
}
// IAudioSessionControl2 declared standalone (includes the 9 IAudioSessionControl
// methods on the same vtable, avoiding derived-interface QI pitfalls)
[ComImport, Guid("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioSessionControl2 {
    int GetState(out int s);
    int GetDisplayName([MarshalAs(UnmanagedType.LPWStr)] out string n);
    int SetDisplayName(string n, ref Guid c);
    int GetIconPath([MarshalAs(UnmanagedType.LPWStr)] out string p);
    int SetIconPath(string p, ref Guid c);
    int GetGroupingParam(out Guid g);
    int SetGroupingParam(ref Guid g, ref Guid c);
    int RegisterAudioSessionNotification(IntPtr n);
    int UnregisterAudioSessionNotification(IntPtr n);
    int GetSessionIdentifier([MarshalAs(UnmanagedType.LPWStr)] out string id);
    int GetSessionInstanceIdentifier([MarshalAs(UnmanagedType.LPWStr)] out string id);
    int GetProcessId(out uint pid);
    int IsSystemSoundsSession();
    int SetDuckingPreference(bool b);
}
[ComImport, Guid("F4B1A599-7266-431E-A8CA-E7ACB8E5D9E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioSessionControl {
    int GetState(out int s);
    int GetDisplayName([MarshalAs(UnmanagedType.LPWStr)] out string n);
    int SetDisplayName(string n, ref Guid c);
    int GetIconPath([MarshalAs(UnmanagedType.LPWStr)] out string p);
    int SetIconPath(string p, ref Guid c);
    int GetGroupingParam(out Guid g);
    int SetGroupingParam(ref Guid g, ref Guid c);
    int RegisterAudioSessionNotification(IntPtr n);
    int UnregisterAudioSessionNotification(IntPtr n);
}
[ComImport, Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ISimpleAudioVolume {
    int SetMasterVolume(float f, ref Guid c);
    int GetMasterVolume(out float f);
    int SetMute(bool m, ref Guid c);
    int GetMute(out bool m);
}
// ISimpleAudioMeter (undocumented; IID/vtable from public docs) for per-session peak
[ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface ISimpleAudioMeter {
    int GetPeakValue(out float peak);
}
[ComImport, Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioClient {
    int Initialize(int mode, int flags, long dur, long period, IntPtr fmt, IntPtr guid);
    int GetBufferSize(out uint frames);
    int GetStreamLatency(out long lat);
    int GetCurrentPadding(out uint pad);
    int IsFormatSupported(int mode, IntPtr fmt, out IntPtr closest);
    int GetMixFormat(out IntPtr fmt);
    int GetDevicePeriod(out long defp, out long minp);
    int Start();
    int Stop();
    int Reset();
    int SetEventHandle(IntPtr h);
    int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object o);
}

public static class AudioProbe {
    static string FriendlyName(IMMDevice dev) {
        try {
            IPropertyStore ps; dev.OpenPropertyStore(0, out ps);
            PropertyKey k = new PropertyKey();
            k.fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"); k.pid = 14;
            PropVariant v;
            if (ps.GetValue(ref k, out v) == 0 && v.vt == 31 && v.p0 != IntPtr.Zero)
                return Marshal.PtrToStringUni(v.p0);
        } catch { }
        return "?";
    }

    static string MixFormat(IMMDevice dev) {
        try {
            object o;
            Guid iid = typeof(IAudioClient).GUID;
            dev.Activate(ref iid, 1, IntPtr.Zero, out o);
            var ac = (IAudioClient)o;
            IntPtr pwf; int hr = ac.GetMixFormat(out pwf);
            if (hr == 0 && pwf != IntPtr.Zero) {
                int rate = Marshal.ReadInt32(pwf, 4);
                short ch = Marshal.ReadInt16(pwf, 2);
                short bits = Marshal.ReadInt16(pwf, 14);
                short tag = Marshal.ReadInt16(pwf, 0);
                string ft = tag == 3 ? "float32" : (tag == -2 ? "ext" : "pcm");
                Marshal.FreeCoTaskMem(pwf);
                return rate + "Hz/" + ch + "ch/" + bits + "bit/" + ft;
            }
            if (pwf != IntPtr.Zero) Marshal.FreeCoTaskMem(pwf);
        } catch { }
        return "?";
    }

    // Returns one string enumerating every active render endpoint, its engine
    // mix format, and every audio session on it (pid|state|volume|peak|display).
    public static string ProbeAll() {
        var sb = new StringBuilder();
        var en = (IMMDeviceEnumerator)(new MMDeviceEnumeratorCom());
        IMMDeviceCollection coll;
        if (en.EnumAudioEndpoints(0, 1, out coll) != 0) return "enum-failed";
        int n; coll.GetCount(out n);
        for (int i = 0; i < n; i++) {
            IMMDevice dev; coll.Item(i, out dev);
            string name = FriendlyName(dev);
            sb.Append("ENDPOINT\t" + name + "\t" + MixFormat(dev) + "\n");
            try {
                object o;
                Guid iid = typeof(IAudioSessionManager2).GUID;
                dev.Activate(ref iid, 1, IntPtr.Zero, out o);
                var mgr = (IAudioSessionManager2)o;
                IAudioSessionEnumerator se; mgr.GetSessionEnumerator(out se);
                int m; se.GetCount(out m);
                for (int j = 0; j < m; j++) {
                    try {
                        object raw; se.GetSession(j, out raw);
                        var c2 = (IAudioSessionControl2)raw;
                        if (c2.IsSystemSoundsSession() != 0) continue;  // S_OK(0)=not system sounds
                        uint pid; c2.GetProcessId(out pid);
                        int st; c2.GetState(out st);
                        var vol = raw as ISimpleAudioVolume;
                        float lv = -1f; if (vol != null) vol.GetMasterVolume(out lv);
                        var meter = raw as ISimpleAudioMeter;
                        float pk = -1f; if (meter != null) meter.GetPeakValue(out pk);
                        string disp = "?"; c2.GetDisplayName(out disp);
                        sb.Append("SESSION\t" + pid + "\t" + st + "\t" +
                            lv.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture) + "\t" +
                            pk.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture) + "\t" + disp + "\n");
                    } catch { }
                }
            } catch { }
            Marshal.FinalReleaseComObject(dev);
        }
        Marshal.FinalReleaseComObject(en);
        return sb.ToString();
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

"== Render endpoints + audio sessions (all active) =="
$lines = [AudioProbe]::ProbeAll() -split "`n"
foreach ($ln in $lines) {
    if ($ln -eq '') { continue }
    $p = $ln -split "`t"
    if ($p[0] -eq 'ENDPOINT') {
        "`n[endpoint] $($p[1])  (engine mix format: $($p[2]))"
    } elseif ($p[0] -eq 'SESSION') {
        $proc = (Get-Process -Id ([int]$p[1]) -ErrorAction SilentlyContinue).ProcessName
        if (-not $proc) { $proc = "(exited)" }
        $st = switch ($p[2]) { '1' { 'active' } '0' { 'inactive' } default { "s$($p[2])" } }
        "  PID $($p[1]) [$proc] state=$st volume=$($p[3]) peak=$($p[4]) display=$($p[5])"
    }
}

"`n== Bridge measured status (from http://127.0.0.1:3999/api/status) =="
try {
    $api = (Invoke-WebRequest -Uri 'http://127.0.0.1:3999/api/status' -UseBasicParsing -TimeoutSec 5).Content | ConvertFrom-Json
    "targetPid=$($api.targetPid) targetActive=$($api.targetActive)"
    "capRate=$($api.capRate) Hz   asioRate=$($api.asioRate) Hz"
    "inRate=$($api.inRate) Hz   outRate=$($api.outRate) Hz   ratioBase=$($api.ratioBase)"
    "watermark=$($api.watermark) / target=$($api.target)   underruns=$($api.underruns)   dropped=$($api.dropped)   drift=$($api.drift) ppm"
    "passthrough=$($api.passthrough)   srcTaps=$($api.srcTaps)   dither=$($api.dither)   latencyMs=$($api.latencyMs)"
} catch {
    "Bridge API unreachable (bridge not running?): $_"
}
