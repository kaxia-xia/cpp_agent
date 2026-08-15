# setup_curl_windows.ps1
# ------------------------------------------------------------------
# Fetch the official curl for Windows package (curl-for-win) and place
# the libcurl development files into third_party/curl-windows so that
# CMake can build coding-agent.exe without any manual configuration.
#
# Usage (PowerShell, run from the project root):
#   powershell -ExecutionPolicy Bypass -File scripts\setup_curl_windows.ps1
#
# The official package is a pure native Windows build (MinGW), no POSIX
# layer involved. It contains curl.exe, the libcurl DLL and the static/
# import libraries plus headers.
# ------------------------------------------------------------------

$ErrorActionPreference = "Stop"

$Root    = Split-Path -Parent $PSScriptRoot
$Dest    = Join-Path $Root "third_party\curl-windows"
$ZipPath = Join-Path $Root "third_party\curl-win64.zip"
$Extract = Join-Path $Root "third_party\_curl_extract"

# Fixed URL always resolves to the latest release.
$Url = "https://curl.se/windows/latest.cgi?p=win64-mingw.zip"

Write-Host "==> Downloading official curl for Windows package..."
Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing

Write-Host "==> Extracting..."
if (Test-Path $Extract) { Remove-Item -Recurse -Force $Extract }
Expand-Archive -Path $ZipPath -DestinationPath $Extract -Force

# The zip extracts into a single versioned folder (e.g. curl-8.21.0_7-win64-mingw).
$Top = Get-ChildItem -Directory $Extract | Select-Object -First 1
if (-not $Top) { throw "unexpected archive layout (no top-level directory)" }

Write-Host "==> Copying dev files to $Dest ..."
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
foreach ($d in @("include", "lib", "bin")) {
    $src = Join-Path $Top.FullName $d
    if (Test-Path $src) {
        Copy-Item -Recurse -Force $src $Dest
        Write-Host "    copied $d"
    } else {
        Write-Warning "    missing $d in archive"
    }
}

Write-Host "==> Cleaning up temporary files..."
Remove-Item -Recurse -Force $Extract
Remove-Item -Force $ZipPath

Write-Host ""
Write-Host "Done. libcurl dev files are now in:"
Write-Host "  $Dest\include\curl\*.h"
Write-Host "  $Dest\lib\libcurl*.a"
Write-Host ""
Write-Host "Now build coding-agent:"
Write-Host "  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release"
Write-Host "  cmake --build build"
Write-Host ""
Write-Host "If the executable needs the DLL at runtime (dynamic import library),"
Write-Host "copy $Dest\bin\libcurl-*.dll next to coding-agent.exe or add it to PATH."
