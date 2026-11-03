# Builds a zip of the app that runs on a PC without Visual Studio installed.
# Usage: package.ps1 [version]. Expects a Release build in .\build.
param([string]$Version = "dev")

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$name = "mydaw-$Version-windows-x64"
$stage = Join-Path $root "dist\$name"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($file in "mydaw.exe", "mydaw-cli.exe") {
  Copy-Item (Join-Path $root "build\Release\$file") $stage
}
Copy-Item (Join-Path $root "README.md") $stage

# The app needs the Microsoft C++ runtime. Shipping these beside the exe avoids
# asking people to install it separately.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory |
  Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) { throw "Could not find the C++ runtime under $vs" }
foreach ($dll in "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
  Copy-Item (Join-Path $crt.FullName $dll) $stage
}

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Output $zip
