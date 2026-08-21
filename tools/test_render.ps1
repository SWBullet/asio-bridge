# Test renderer: play 440Hz sine into a render endpoint for N seconds.
# With process loopback the bridge taps by PID, so render to the default device
# (empty -Device) or any -Device substring; the bridge picks up this process.
# Usage: pwsh -File test_render.ps1 [-Seconds 10] [-Device ""] [-Delay 2]
# NOTE: keep this file ASCII-only (Windows PowerShell 5.1 reads BOM-less scripts as ANSI).
param(
    [int]$Seconds = 10,
    [string]$Device = "",
    [int]$Delay = 2
)

Start-Sleep -Seconds $Delay

$code = @'
using System;
using System.Threading;
using System.Runtime.InteropServices;

[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
public class MMDeviceEnumeratorComObject { }

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceEnumerator {
    [PreserveSig] int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
    [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
}

[ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDeviceCollection {
    [PreserveSig] int GetCount(out uint count);
    [PreserveSig] int Item(uint index, out IMMDevice device);
}

[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMMDevice {
    [PreserveSig] int Activate(ref Guid iid, int clsCtx, IntPtr pActParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    [PreserveSig] int OpenPropertyStore(int access, out IPropertyStore store);
    [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
}

[ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IPropertyStore {
    [PreserveSig] int GetCount(out uint count);
    [PreserveSig] int GetAt(uint index, out PROPERTYKEY key);
    [PreserveSig] int GetValue(ref PROPERTYKEY key, out PROPVARIANT pv);
}

[StructLayout(LayoutKind.Sequential)]
public struct PROPERTYKEY { public Guid fmtid; public uint pid; }

[StructLayout(LayoutKind.Explicit, Size = 24)]
public struct PROPVARIANT {
    [FieldOffset(0)] public ushort vt;
    [FieldOffset(8)] public IntPtr pwszVal;
}

[ComImport, Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioClient {
    [PreserveSig] int Initialize(int shareMode, int streamFlags, long hnsBufDur, long hnsPeriod, ref WaveFormatEx fmt, ref Guid sessionGuid);
    [PreserveSig] int GetBufferSize(out uint frames);
    void _VtblGap2_2();
    [PreserveSig] int GetCurrentPadding(out uint padding);
    void _VtblGap4_4();
    [PreserveSig] int GetMixFormat(out IntPtr fmtPtr);
    void _VtblGap6_6();
    [PreserveSig] int Start();
    [PreserveSig] int Stop();
    void _VtblGap9_9();
    [PreserveSig] int SetEventHandle(IntPtr h);
    [PreserveSig] int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object svc);
}

[ComImport, Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IAudioRenderClient {
    [PreserveSig] int GetBuffer(uint frames, out IntPtr data);
    [PreserveSig] int ReleaseBuffer(uint frames, uint flags);
}

[StructLayout(LayoutKind.Sequential, Pack = 2)]
public struct WaveFormatEx {
    public ushort wFormatTag;
    public ushort nChannels;
    public uint nSamplesPerSec;
    public uint nAvgBytesPerSec;
    public ushort nBlockAlign;
    public ushort wBitsPerSample;
    public ushort cbSize;
}

public static class RenderTest {
    [DllImport("ole32.dll")] static extern int CoInitializeEx(IntPtr pv, uint coInit);
    [DllImport("ole32.dll")] static extern void CoUninitialize();
    [DllImport("ole32.dll")] static extern int PropVariantClear(ref PROPVARIANT pv);
    [DllImport("kernel32.dll")] static extern ulong GetTickCount64();

    public static string Run(string deviceSubstr, int seconds) {
        var sb = new System.Text.StringBuilder();
        var t = new Thread(() => {
            CoInitializeEx(IntPtr.Zero, 0); // MTA
            var t2 = Type.GetTypeFromCLSID(new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E"));
            var en = (IMMDeviceEnumerator)Activator.CreateInstance(t2);
            IMMDevice dev = null;
            PROPERTYKEY pk = new PROPERTYKEY {
                fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 14
            };
            if (deviceSubstr == null || deviceSubstr.Length == 0) {
                en.GetDefaultAudioEndpoint(0, 1, out dev);   // eRender, eConsole
            } else {
                IMMDeviceCollection coll;
                en.EnumAudioEndpoints(0, 1, out coll);
                uint n; coll.GetCount(out n);
                for (uint i = 0; i < n; i++) {
                    IMMDevice d; coll.Item(i, out d);
                    IPropertyStore st; d.OpenPropertyStore(0, out st);
                    PROPVARIANT pv = new PROPVARIANT();
                    string name = "";
                    if (st.GetValue(ref pk, out pv) == 0 && pv.vt == 31)
                        name = Marshal.PtrToStringUni(pv.pwszVal);
                    PropVariantClear(ref pv);
                    Marshal.FinalReleaseComObject(st);
                    if (name.Contains(deviceSubstr)) { dev = d; break; }
                    Marshal.FinalReleaseComObject(d);
                }
                Marshal.FinalReleaseComObject(coll);
            }
            if (dev == null) {
                sb.AppendLine("未找到端点: " + (deviceSubstr.Length == 0 ? "(默认渲染设备)" : deviceSubstr));
                CoUninitialize(); return;
            }

            Guid iidAc = new Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2");
            object o1; dev.Activate(ref iidAc, 23, IntPtr.Zero, out o1);
            var ac = (IAudioClient)o1;
            IntPtr mfp;
            ac.GetMixFormat(out mfp);
            var mix = (WaveFormatEx)Marshal.PtrToStructure(mfp, typeof(WaveFormatEx));
            bool isFloat = false;
            bool is24In32 = false;
            if (mix.wFormatTag == 3) isFloat = true;
            else if (mix.wFormatTag == 0xFFFE && mix.cbSize >= 22) {
                Guid sub = (Guid)Marshal.PtrToStructure(mfp + 24, typeof(Guid));
                Guid kFloat = new Guid("00000003-0000-0010-8000-00aa00389b71");
                isFloat = (sub == kFloat);
                if (!isFloat && mix.wBitsPerSample == 32) {
                    short vb = Marshal.ReadInt16(mfp, 18);
                    if (vb == 24) is24In32 = true;
                }
            }
            Marshal.FreeCoTaskMem(mfp);
            sb.AppendLine(string.Format("端点混音格式: {0}bit/{1}Hz/{2}ch tag={3} float={4}",
                mix.wBitsPerSample, mix.nSamplesPerSec, mix.nChannels, mix.wFormatTag, isFloat));

            Guid g = Guid.Empty;
            var fmt = mix; fmt.cbSize = 0;
            if (fmt.wFormatTag == 0xFFFE) fmt.wFormatTag = 3;
            int hr = ac.Initialize(0, 0x40000, 0, 0, ref fmt, ref g);
            sb.AppendLine("Initialize = 0x" + hr.ToString("X8"));
            if (hr != 0) { CoUninitialize(); return; }

            IntPtr evt = CreateEvent(IntPtr.Zero, false, false, null);
            ac.SetEventHandle(evt);
            Guid iidRc = new Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2");
            object o2; ac.GetService(ref iidRc, out o2);
            var rc = (IAudioRenderClient)o2;
            ac.Start();

            uint bufSize; ac.GetBufferSize(out bufSize);
            double phase = 0;
            double rate = mix.nSamplesPerSec;
            double inc = 2.0 * Math.PI * 440.0 / rate;
            ulong deadline = GetTickCount64() + (ulong)(seconds * 1000L);
            long framesWritten = 0;
            while (GetTickCount64() < deadline) {
                WaitForSingleObject(evt, 100);
                uint pad; ac.GetCurrentPadding(out pad);
                uint avail = bufSize - pad;
                if (avail > 0) {
                    IntPtr data;
                    rc.GetBuffer(avail, out data);
                    int nSamp = (int)(avail * mix.nChannels);
                    float[] buf = new float[nSamp];
                    for (int i = 0; i < nSamp; i++) {
                        buf[i] = 0.30f * (float)Math.Sin(phase);
                        phase += inc;
                        if (phase > 2.0 * Math.PI) phase -= 2.0 * Math.PI;
                    }
                    if (isFloat) {
                        Marshal.Copy(buf, 0, data, nSamp);
                    } else if (mix.wBitsPerSample == 32) {
                        int[] ib = new int[nSamp];
                        for (int i = 0; i < nSamp; i++) {
                            int v = (int)(buf[i] * 2147483647.0);
                            if (is24In32) v = v & unchecked((int)0xFFFFFF00);
                            ib[i] = v;
                        }
                        Marshal.Copy(ib, 0, data, nSamp);
                    } else if (mix.wBitsPerSample == 16) {
                        short[] sb2 = new short[nSamp];
                        for (int i = 0; i < nSamp; i++) sb2[i] = (short)(buf[i] * 32767.0);
                        Marshal.Copy(sb2, 0, data, nSamp);
                    } else {
                        Marshal.Copy(buf, 0, data, nSamp);
                    }
                    rc.ReleaseBuffer(avail, 0);
                    framesWritten += avail;
                }
            }
            ac.Stop();
            sb.AppendLine("渲染完成: 共 " + framesWritten + " 帧 (约 " + (framesWritten / rate).ToString("0.0") + " 秒)");
            Marshal.FinalReleaseComObject(rc);
            Marshal.FinalReleaseComObject(ac);
            Marshal.FinalReleaseComObject(dev);
            Marshal.FinalReleaseComObject(en);
            CoUninitialize();
        });
        t.Start();
        t.Join();
        return sb.ToString();
    }

    [DllImport("kernel32.dll")] static extern IntPtr CreateEvent(IntPtr a, bool b, bool c, string d);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
}
'@

Add-Type -TypeDefinition $code -Language CSharp
[RenderTest]::Run($Device, $Seconds)
