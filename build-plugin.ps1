# Dress Your Followers - C++ SKSE plugin build
# Sets up the MSVC dev environment + required env vars, then configures and
# builds the ImGui plugin with CMake/Ninja/vcpkg. Release by default.
param([string]$Config = "release")
# NOTE: do NOT use $ErrorActionPreference='Stop' here. CommonLibSSE-NG and CMake
# print status banners to stderr, which PowerShell 5.1 turns into terminating
# errors under Stop mode - aborting a build that is actually fine. We check
# $LASTEXITCODE explicitly instead.
$ErrorActionPreference = "Continue"

$Proj   = $PSScriptRoot
$Plugin = Join-Path $Proj "plugin"
$Vendor = Join-Path $Proj "vendor"
$Game   = "D:\SteamLibrary\steamapps\common\Skyrim Special Edition"
$VS     = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"

# --- required env vars for CMakeLists ---
$env:COMMONLIB_SSE_FOLDER = Join-Path $Vendor "CommonLibSSE-NG"
$env:SKYRIM_FOLDER        = $Game
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "C:\Program Files\vcpkg-master" }

Write-Host "COMMONLIB_SSE_FOLDER = $($env:COMMONLIB_SSE_FOLDER)"
Write-Host "SKYRIM_FOLDER        = $($env:SKYRIM_FOLDER)"
Write-Host "VCPKG_ROOT           = $($env:VCPKG_ROOT)"

# --- enter the VS x64 developer environment (puts cl.exe, ninja, rc.exe on PATH) ---
Import-Module (Join-Path $VS "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $VS -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

Set-Location $Plugin

# --- configure (first run downloads/builds vcpkg deps - can take a while) ---
cmake --preset $Config
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

# --- build ---
cmake --build (Join-Path $Plugin "build\$Config") --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

Write-Host "PLUGIN BUILD OK" -ForegroundColor Green
