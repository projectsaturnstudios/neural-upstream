# Compiles every compute kernel in src/codec.hlsl.h with d3dcompiler_47 (the same
# compiler the addon uses at runtime), so a shader error is caught here and not as
# a silently disabled codec in-game.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$raw = Get-Content -Raw src/codec.hlsl.h
$m = [regex]::Match($raw, 'R"HLSL\((.*)\)HLSL"', 'Singleline')
if (-not $m.Success) { throw "could not find the HLSL block" }
$src = $m.Groups[1].Value
$entries = [regex]::Matches($src, 'void\s+(CS\w+)\s*\(') | ForEach-Object { $_.Groups[1].Value }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Fxc {
    [DllImport("d3dcompiler_47.dll", CharSet = CharSet.Ansi)]
    static extern int D3DCompile(byte[] src, IntPtr len, string name, IntPtr defs, IntPtr inc,
        string entry, string target, uint f1, uint f2, out IntPtr code, out IntPtr err);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate IntPtr GetPtr(IntPtr self);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate IntPtr GetSize(IntPtr self);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate uint Release(IntPtr self);
    static string BlobText(IntPtr blob) {
        if (blob == IntPtr.Zero) return "";
        IntPtr vt = Marshal.ReadIntPtr(blob);
        var gp = (GetPtr)Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(vt, 3 * IntPtr.Size), typeof(GetPtr));
        var gs = (GetSize)Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(vt, 4 * IntPtr.Size), typeof(GetSize));
        var rel = (Release)Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(vt, 2 * IntPtr.Size), typeof(Release));
        int n = (int)gs(blob);
        string s = Marshal.PtrToStringAnsi(gp(blob), n);
        rel(blob);
        return s;
    }
    public static string Compile(string source, string entry) {
        byte[] b = System.Text.Encoding.ASCII.GetBytes(source);
        IntPtr code, err;
        int hr = D3DCompile(b, (IntPtr)b.Length, "codec", IntPtr.Zero, IntPtr.Zero, entry, "cs_5_0", 0, 0, out code, out err);
        string msg = BlobText(err);
        if (code != IntPtr.Zero) BlobText(code);
        return (hr < 0 ? "FAIL 0x" + hr.ToString("X8") + " " : "ok   ") + entry + (msg.Length > 0 ? "\n" + msg : "");
    }
}
'@
$bad = 0
foreach ($e in $entries) {
    $r = [Fxc]::Compile($src, $e)
    $r
    if ($r.StartsWith("FAIL")) { $bad++ }
}
if ($bad -gt 0) { throw "$bad kernel(s) failed" }
"all $($entries.Count) kernels compile"
