# Copies the built add-on into a game folder. Installing is deliberate and never
# a side effect of building: the target is usually a game someone is playing, and
# swapping the add-on underneath a working setup is how a good build becomes a
# broken one.
#
#   .\install.ps1 -GameFolder "D:\EpicGames\AlanWake2"
param(
    [Parameter(Mandatory = $true)][string]$GameFolder,
    [string]$Name = 'nvngx.dll.nrpre.addon64',
    [string]$Binary = 'neural-upstream.addon64'
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

# Load-bearing. The neural runtime gates feature creation on a substring search
# for "nvngx.dll" in the calling module's path; under any other name it returns
# 0xBAD00002 and nothing happens at all.
if ($Name -notlike '*nvngx.dll*') {
    throw "The add-on filename must contain 'nvngx.dll'. '$Name' would be refused by the runtime."
}
if ($Name -notlike '*.addon64') { throw "The filename must end in .addon64 for ReShade to load it." }
if (-not (Test-Path $Binary))   { throw "No $Binary here. Run .\build.ps1 first." }
if (-not (Test-Path $GameFolder)) { throw "No such folder: $GameFolder" }

# Two add-ons cannot both detour the NGX entry points; the game crashes when both
# drive the neural runtime. The add-on detects this at runtime and stands down,
# but saying so here saves a puzzling launch.
$rivals = Get-ChildItem $GameFolder -Filter '*.addon64' -ErrorAction SilentlyContinue |
          Where-Object { $_.Name -ne $Name -and
                         ($_.Name -like 'renodx-dlss*' -or $_.Name -like '*dlssnr*' -or
                          $_.Name -like '*nvngx*') }
foreach ($r in $rivals) {
    Write-Warning "$($r.Name) is also in that folder. Both hook the same NVIDIA entry points and cannot run together; rename one to end in .disabled."
}
if (-not (Test-Path (Join-Path $GameFolder 'nvngx_dlssnr.dll'))) {
    Write-Warning "nvngx_dlssnr.dll is not in that folder. The add-on will load and do nothing without it."
}

Copy-Item -Force $Binary (Join-Path $GameFolder $Name)
$i = Get-Item (Join-Path $GameFolder $Name)
"installed: $($i.FullName)  $($i.Length) bytes"
