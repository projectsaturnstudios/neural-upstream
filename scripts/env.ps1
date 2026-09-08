# Puts the portable toolchain and git on PATH for the current PowerShell session.
# Dot-source it:   . .\scripts\env.ps1
# Neither tool is installed system-wide; they live beside the project in
# ..\tools (or .\tools) and nothing else on the machine knows about them.
$root = Split-Path -Parent $PSScriptRoot
foreach ($base in @("$root\tools", "$root\..\tools")) {
    foreach ($sub in @('mingw64\bin', 'git\cmd')) {
        $p = Join-Path $base $sub
        if ((Test-Path $p) -and ($env:Path -notlike "*$p*")) {
            $env:Path = (Resolve-Path $p).Path + ';' + $env:Path
        }
    }
}
$g = Get-Command git -ErrorAction SilentlyContinue
$c = Get-Command g++ -ErrorAction SilentlyContinue
"git: " + $(if ($g) { $g.Source } else { 'not found' })
"g++: " + $(if ($c) { $c.Source } else { 'not found' })
