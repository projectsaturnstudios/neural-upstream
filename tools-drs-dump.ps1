# Dumps the NVIDIA driver-profile (DRS) settings that apply to an executable,
# straight from nvapi64.dll. This is what the driver and the DLSS DLL actually
# read; the NVIDIA App UI is only a view onto it.
param([string]$Exe = "SHProto-Win64-Shipping.exe")
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class NvDrs {
    [DllImport("nvapi64.dll", EntryPoint = "nvapi_QueryInterface", CallingConvention = CallingConvention.Cdecl)]
    static extern IntPtr QueryInterface(uint id);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int D_Init();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int D_CreateSession(out IntPtr h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int D_LoadSettings(IntPtr h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int D_DestroySession(IntPtr h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int D_GetBaseProfile(IntPtr h, out IntPtr p);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int D_FindAppByName(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] string name, out IntPtr p, IntPtr app);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int D_GetProfileInfo(IntPtr h, IntPtr p, IntPtr info);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int D_EnumSettings(IntPtr h, IntPtr p, uint start, ref uint count, IntPtr settings);

    static T Fn<T>(uint id) {
        IntPtr p = QueryInterface(id);
        if (p == IntPtr.Zero) throw new Exception("nvapi id 0x" + id.ToString("X8") + " not found");
        return (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
    }

    const int SETTING_SIZE = 4 + 4096 + 4 + 4 + 4 + 4 + 4 + 4100 + 4100;   // NVDRS_SETTING_V1 = 12320
    const uint SETTING_VER = (uint)SETTING_SIZE | (1u << 16);
    const int APP_SIZE = 4 + 4 + 4096 * 3;                                     // NVDRS_APPLICATION_V1
    const uint APP_VER = (uint)APP_SIZE | (1u << 16);
    const int PROFILE_SIZE = 4 + 4096 + 4 + 4 + 4 + 4;                        // NVDRS_PROFILE_V1
    const uint PROFILE_VER = (uint)PROFILE_SIZE | (1u << 16);

    static string ProfileName(IntPtr h, IntPtr prof) {
        var gi = Fn<D_GetProfileInfo>(0x61CD6FD6);
        IntPtr buf = Marshal.AllocHGlobal(PROFILE_SIZE);
        try {
            for (int i = 0; i < PROFILE_SIZE; i++) Marshal.WriteByte(buf, i, 0);
            Marshal.WriteInt32(buf, 0, (int)PROFILE_VER);
            int r = gi(h, prof, buf);
            if (r != 0) return "(GetProfileInfo " + r + ")";
            string s = Marshal.PtrToStringUni(buf + 4, 2048);
            int z = s.IndexOf('\0'); if (z >= 0) s = s.Substring(0, z);
            int n = Marshal.ReadInt32(buf, 4 + 4096 + 8 + 4);
            return s + "  (" + n + " settings)";
        } finally { Marshal.FreeHGlobal(buf); }
    }

    static void DumpProfile(IntPtr h, IntPtr prof, StringBuilder sb) {
        var en = Fn<D_EnumSettings>(0xAE3039DA);
        const int MAX = 512;
        IntPtr arr = Marshal.AllocHGlobal(SETTING_SIZE * MAX);
        try {
            for (int i = 0; i < SETTING_SIZE * MAX; i++) Marshal.WriteByte(arr, i, 0);
            for (int i = 0; i < MAX; i++) Marshal.WriteInt32(arr, i * SETTING_SIZE, (int)SETTING_VER);
            uint count = MAX;
            int r = en(h, prof, 0, ref count, arr);
            if (r != 0) { sb.AppendLine("  EnumSettings -> " + r); return; }
            for (int i = 0; i < count; i++) {
                IntPtr s = arr + i * SETTING_SIZE;
                string name = Marshal.PtrToStringUni(s + 4, 2048);
                int z = name.IndexOf('\0'); if (z >= 0) name = name.Substring(0, z);
                uint id = (uint)Marshal.ReadInt32(s, 4 + 4096);
                int type = Marshal.ReadInt32(s, 4 + 4096 + 4);
                int loc = Marshal.ReadInt32(s, 4 + 4096 + 8);
                int predefValid = Marshal.ReadInt32(s, 4 + 4096 + 16);
                uint pre = (uint)Marshal.ReadInt32(s, 4 + 4096 + 20);
                uint cur = (uint)Marshal.ReadInt32(s, 4 + 4096 + 20 + 4100);
                string locs = loc == 0 ? "profile" : loc == 1 ? "global" : loc == 2 ? "base" : "loc" + loc;
                sb.AppendLine(string.Format("  0x{0:X8}  cur=0x{1:X8}  pre=0x{2:X8}  type={3} {4,-7} {5}", id, cur, pre, type, locs, name));
            }
        } finally { Marshal.FreeHGlobal(arr); }
    }

    public static string Dump(string exe) {
        var sb = new StringBuilder();
        int r = Fn<D_Init>(0x0150E828)();
        sb.AppendLine("NvAPI_Initialize -> " + r);
        IntPtr h;
        r = Fn<D_CreateSession>(0x0694D52E)(out h); sb.AppendLine("CreateSession -> " + r);
        r = Fn<D_LoadSettings>(0x375DBD6B)(h);     sb.AppendLine("LoadSettings -> " + r);
        IntPtr app = Marshal.AllocHGlobal(APP_SIZE);
        for (int i = 0; i < APP_SIZE; i++) Marshal.WriteByte(app, i, 0);
        Marshal.WriteInt32(app, 0, (int)APP_VER);
        IntPtr prof;
        r = Fn<D_FindAppByName>(0xEEE566B2)(h, exe, out prof, app);
        sb.AppendLine("FindApplicationByName(" + exe + ") -> " + r);
        if (r == 0) {
            sb.AppendLine("== application profile: " + ProfileName(h, prof));
            DumpProfile(h, prof, sb);
        }
        IntPtr bp;
        r = Fn<D_GetBaseProfile>(0xDA8466A0)(h, out bp);
        sb.AppendLine("GetBaseProfile -> " + r);
        if (r == 0) {
            sb.AppendLine("== base (global) profile: " + ProfileName(h, bp));
            DumpProfile(h, bp, sb);
        }
        Marshal.FreeHGlobal(app);
        Fn<D_DestroySession>(0xDAD9CFF8)(h);
        return sb.ToString();
    }
}
'@
[NvDrs]::Dump($Exe)
