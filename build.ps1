# Builds bin\RE6DLSS.addon32 with MSVC (Visual Studio 2022+ with C++ tools).
# Resident Evil 6 (BH6.exe) is a 32-bit D3D9 game, so this is a 32-bit (x86) add-on for the
# 32-bit ReShade (d3d9.dll).
# Usage: .\build.ps1 [-Install] [-GameDir <folder of BH6.exe>]
#   -Install  copies the add-on next to the game's exe (game closed).
param(
    [switch]$Install,
    [switch]$Package,
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Resident Evil 6"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = "BH6.exe"
$name = "RE6DLSS"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
# vcvars32.bat runs vswhere by name, and its batch blocks break on some PATH entries
# (parentheses), so it gets a minimal PATH (this process only).
$env:PATH = "$(Split-Path $vswhere);$env:SystemRoot\system32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x86/x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars32.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$addon = "$root\bin\$name.addon32"

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0.
$cmd = @(
    "call `"$vcvars`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$root\third_party\imgui`" /I `"$root\third_party\reshade\include`" " +
     "/Fo`"$obj\\`" `"$root\src\main.cpp`" user32.lib " +
     "/link /DLL /MACHINE:X86 /OUT:`"$addon`" /PDB:`"$obj\$name.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "Built $addon"

# The 64-bit helper process (DLSS is 64-bit only): bin\RE6DLSS-helper64.exe
$vcvars64 = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$obj64 = "$root\bin\obj64"
New-Item -ItemType Directory -Force $obj64 | Out-Null
$helper = "$root\bin\$name-helper64.exe"
$cmd = @(
    "call `"$vcvars64`" >nul",
    ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
     "/I `"$root\third_party\dlss\include`" " +
     "/Fo`"$obj64\\`" `"$root\src\helper.cpp`" `"$root\src\dlss.cpp`" user32.lib advapi32.lib d3d11.lib " +
     "`"$root\third_party\dlss\lib\nvsdk_ngx_s.lib`" " +
     "/link /SUBSYSTEM:WINDOWS /OUT:`"$helper`" /PDB:`"$obj64\$name-helper64.pdb`" /DEBUG")
) -join " && "
cmd /c $cmd
if ($LASTEXITCODE) { throw "helper build failed" }
Write-Host "Built $helper"

if ($Install) {
    if (-not (Test-Path "$GameDir\$exe")) { throw "$exe not found in $GameDir (pass -GameDir)" }
    if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Item $addon, $helper, "$root\third_party\bin\nvngx_dlss.dll" $GameDir -Force
    Write-Host "Installed to $GameDir"
}

if ($Package) {
    $version = [regex]::Match((Get-Content "$root\src\version.h" -Raw), 'RD_VERSION "([^"]+)"').Groups[1].Value
    $zip = "$root\bin\release\$name-$version.zip"
    New-Item -ItemType Directory -Force (Split-Path $zip) | Out-Null
    if (Test-Path $zip) { Remove-Item -Force $zip }
    # Flat zip, forward-slash entry names (Compress-Archive writes backslashes, which break on Linux).
    $files = [ordered]@{
        "RE6DLSS.addon32"          = $addon
        "RE6DLSS-helper64.exe"     = $helper
        "nvngx_dlss.dll"           = "$root\third_party\bin\nvngx_dlss.dll"
        "README.md"                = "$root\README.md"
        "LICENSE"                  = "$root\LICENSE"
        "THIRD_PARTY_NOTICES.txt"  = "$root\THIRD_PARTY_NOTICES.txt"
        "NVIDIA-DLSS-LICENSE.txt"  = "$root\third_party\dlss\LICENSE.txt"
    }
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::Open($zip, 'Create')
    try {
        foreach ($k in $files.Keys) {
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $files[$k], $k)
        }
    } finally { $z.Dispose() }
    Write-Host "Packaged $zip"
}
