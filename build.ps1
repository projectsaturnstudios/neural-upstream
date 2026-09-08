# Windows build. Mirrors build.sh, which is the upstream POSIX version.
#
# Output: neural-upstream.addon64 in this folder. It is NOT installed anywhere;
# see install.ps1, or copy it into the game yourself. The destination filename
# has to contain "nvngx.dll" -- the neural runtime checks the calling module's
# path for that substring and refuses anything else.
param(
    [string]$Out = "neural-upstream.addon64",
    [switch]$Debug
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# A MinGW-w64 g++ with C++20. Looked for beside the project first, so a portable
# toolchain can be dropped in without touching PATH, then on PATH.
$gxx = $null
foreach ($cand in @("$PSScriptRoot\tools\mingw64\bin\g++.exe",
                    "$PSScriptRoot\..\tools\mingw64\bin\g++.exe")) {
    if (Test-Path $cand) { $gxx = (Resolve-Path $cand).Path; break }
}
if (-not $gxx) {
    $onPath = Get-Command g++ -ErrorAction SilentlyContinue
    if ($onPath) { $gxx = $onPath.Source }
}
if (-not $gxx) {
    throw "No g++ found. Put a MinGW-w64 toolchain in .\tools\mingw64, or on PATH. See README."
}

foreach ($d in 'external\reshade\include', 'external\ngx\include',
                'external\minhook\include', 'external\imgui') {
    if (-not (Test-Path $d)) { throw "Missing $d. Run .\scripts\fetch-deps.ps1 first." }
}

$opt = if ($Debug) { @('-O0', '-g') } else { @('-O2', '-DNDEBUG') }
$args_ = @('-shared', '-std=c++20', '-w') + $opt + @(
    '-I', 'external/reshade/include',
    '-I', 'external/ngx/include',
    '-I', 'external/minhook/include',
    '-I', 'external/imgui',
    '-o', $Out,
    'src/addon.cpp',
    'external/minhook/src/hook.c',
    'external/minhook/src/buffer.c',
    'external/minhook/src/trampoline.c',
    'external/minhook/src/hde/hde64.c',
    '-ld3d12', '-ldxgi',
    '-static', '-static-libgcc', '-static-libstdc++'
)
& $gxx @args_
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
$f = Get-Item $Out
"built: $($f.Name)  $($f.Length) bytes"
