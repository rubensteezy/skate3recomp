# Package a Windows release build as Skate3Recomp-Windows.zip.
#
# Run from the repository root after building the release preset:
#   powershell -File tools/package_windows.ps1 [-BuildDir out/build/release]
#
# The zip holds a Skate3Recomp-Windows folder with the executable, the runtime
# DLL and the GPU plugin DLL, and is written to out/release.

param(
    [string]$BuildDir = "out/build/release"
)

$ErrorActionPreference = "Stop"

$files = @("skate3.exe", "rexruntime.dll", "rexgpu-xenos.dll")
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $BuildDir $f))) {
        throw "package_windows: $BuildDir/$f is missing; build the release preset first"
    }
}

$stage = "out/release/Skate3Recomp-Windows"
$zip = "out/release/Skate3Recomp-Windows.zip"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($f in $files) {
    Copy-Item (Join-Path $BuildDir $f) $stage
}

# Windows' bundled bsdtar writes zip entries with forward slashes, which
# Windows PowerShell 5.1's Compress-Archive does not.
& tar.exe -a -c -f $zip -C "out/release" "Skate3Recomp-Windows"
if ($LASTEXITCODE -ne 0) { throw "package_windows: tar failed" }
Write-Host "package_windows: wrote $zip"
