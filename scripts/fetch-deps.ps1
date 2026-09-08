# Fetches the third-party headers this add-on builds against into .\external.
# None of them are vendored: they carry their own licences and some are large.
#
# ImGui is pinned. ReShade's overlay header hard-asserts a matching ImGui version
# and refuses to compile against anything else, so "latest" is not an option here.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$IMGUI_TAG = 'v1.92.5-docking'      # must match ReShade's imgui_function_table
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("nu-deps-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null

function Get-Zip($url, $name) {
    $zip = Join-Path $tmp "$name.zip"
    Write-Host "  fetching $name ..."
    Invoke-WebRequest -UseBasicParsing $url -OutFile $zip
    $dir = Join-Path $tmp $name
    Expand-Archive -Force $zip $dir
    return (Get-ChildItem $dir -Directory | Select-Object -First 1).FullName
}

try {
    foreach ($d in 'external\reshade\include', 'external\ngx\include',
                    'external\minhook', 'external\imgui') {
        New-Item -ItemType Directory -Force (Join-Path $root $d) | Out-Null
    }

    Write-Host "ReShade add-on headers (BSD 3-clause)"
    $r = Get-Zip 'https://github.com/crosire/reshade/archive/refs/heads/main.zip' 'reshade'
    Copy-Item -Force "$r\include\*" "$root\external\reshade\include\"

    Write-Host "MinHook (BSD 2-clause)"
    $m = Get-Zip 'https://github.com/TsudaKageyu/minhook/archive/refs/heads/master.zip' 'minhook'
    Copy-Item -Recurse -Force "$m\include" "$root\external\minhook\"
    Copy-Item -Recurse -Force "$m\src"     "$root\external\minhook\"

    Write-Host "Dear ImGui $IMGUI_TAG (MIT)"
    $i = Get-Zip "https://github.com/ocornut/imgui/archive/refs/tags/$IMGUI_TAG.zip" 'imgui'
    Copy-Item -Force "$i\*.h"   "$root\external\imgui\"
    Copy-Item -Force "$i\*.cpp" "$root\external\imgui\"

    Write-Host "NVIDIA NGX / DLSS SDK headers (NVIDIA licence, see the DLSS repo)"
    $ngxBase = 'https://raw.githubusercontent.com/NVIDIA/DLSS/main/include'
    foreach ($h in 'nvsdk_ngx.h', 'nvsdk_ngx_defs.h', 'nvsdk_ngx_params.h',
                   'nvsdk_ngx_helpers.h', 'nvsdk_ngx_defs_dlssd.h',
                   'nvsdk_ngx_params_dlssd.h', 'nvsdk_ngx_helpers_dlssd.h',
                   'nvsdk_ngx_defs_dlssg.h', 'nvsdk_ngx_helpers_dlssg.h') {
        Invoke-WebRequest -UseBasicParsing "$ngxBase/$h" -OutFile "$root\external\ngx\include\$h"
    }

    $v = (Select-String -Path "$root\external\imgui\imgui.h" -Pattern '#define IMGUI_VERSION\s').Line
    Write-Host ""
    Write-Host "done. $($v.Trim())"
    Write-Host "next: .\build.ps1"
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
