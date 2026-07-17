# Dress Your Followers - build script
# Usage:
#   .\build.ps1            -> compile scripts to mod\Scripts
#   .\build.ps1 -Deploy    -> compile, then copy mod files into the game Data folder (dev iteration)
#   .\build.ps1 -Package   -> compile, then zip mod\ into dist\ for Vortex/Nexus
param(
    [switch]$Deploy,
    [switch]$Package
)
$ErrorActionPreference = "Stop"

$Game = "D:\SteamLibrary\steamapps\common\Skyrim Special Edition"
$Proj = $PSScriptRoot
$Compiler = Join-Path $Game "Papyrus Compiler\PapyrusCompiler.exe"

if (-not (Test-Path $Compiler)) {
    throw "PapyrusCompiler.exe not found. Install the Creation Kit first (Steam app 1946180) and launch it once."
}

# --- Ensure vanilla script sources are extracted (CK ships them as Data\Scripts.zip).
# They are extracted into the PROJECT (vendor\VanillaScripts), never into the game's
# Data folder, so the existing modded install stays untouched.
$VanillaSrc = Join-Path $Proj "vendor\VanillaScripts"
if (-not (Test-Path (Join-Path $VanillaSrc "Debug.psc"))) {
    $zip = Join-Path $Game "Data\Scripts.zip"
    if (-not (Test-Path $zip)) { throw "Vanilla sources not vendored and Data\Scripts.zip not found - install/launch the Creation Kit once, or verify the CK install." }
    Write-Host "Extracting vanilla script sources from Scripts.zip into vendor\VanillaScripts..."
    $tmp = Join-Path $env:TEMP "dyf_scripts_zip"
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force -Confirm:$false }
    Expand-Archive $zip -DestinationPath $tmp
    # Zip layout varies between CK versions; find the folder that holds the .psc files
    $pscDir = Get-ChildItem $tmp -Recurse -Filter "Debug.psc" | Select-Object -First 1 | ForEach-Object { $_.DirectoryName }
    if (-not $pscDir) { throw "Could not locate .psc files inside Scripts.zip" }
    New-Item -ItemType Directory -Force $VanillaSrc | Out-Null
    Copy-Item "$pscDir\*.psc" $VanillaSrc -Force
    Remove-Item $tmp -Recurse -Force -Confirm:$false
    Write-Host "Vanilla sources vendored to $VanillaSrc"
}

# --- Compile ---
# Import order = precedence: our sources, vendored headers (MCM Helper),
# SKSE-extended sources (Data\Scripts\Source), vanilla CK sources (Data\Source\Scripts).
$src = Join-Path $Proj "mod\Source\Scripts"
$out = Join-Path $Proj "mod\Scripts"
New-Item -ItemType Directory -Force $out | Out-Null
$imports = @(
    $src,
    (Join-Path $Proj "vendor\Scripts\Source"),
    (Join-Path $Game "Data\Scripts\Source"),
    (Join-Path $Game "Data\Source\Scripts"),
    $VanillaSrc
) -join ";"

& $Compiler $src -all -quiet "-output=$out" "-import=$imports" "-flags=TESV_Papyrus_Flags.flg"
if ($LASTEXITCODE -ne 0) { throw "Papyrus compile FAILED (exit $LASTEXITCODE)" }
Write-Host "Compile OK -> $out" -ForegroundColor Green

# --- Deploy (dev): copy loose files into game Data ---
if ($Deploy) {
    $data = Join-Path $Game "Data"
    Copy-Item (Join-Path $Proj "mod\Scripts\*.pex") (Join-Path $data "Scripts") -Force
    New-Item -ItemType Directory -Force (Join-Path $data "MCM\Config\DressYourFollowers") | Out-Null
    Copy-Item (Join-Path $Proj "mod\MCM\Config\DressYourFollowers\*") (Join-Path $data "MCM\Config\DressYourFollowers") -Force
    $esp = Join-Path $Proj "mod\DressYourFollowers.esp"
    if (Test-Path $esp) { Copy-Item $esp $data -Force }
    Write-Host "Deployed to $data (note: Vortex may flag these as external changes - that's fine for dev)" -ForegroundColor Yellow
}

# --- Package: zip for Vortex/Nexus ---
# Stages a Data-relative layout so the SKSE plugin DLL and PrismaUI view end up
# alongside the Papyrus/MCM/esp files, not just the mod\ subset.
if ($Package) {
    $dist = Join-Path $Proj "dist"
    $staging = Join-Path $dist "staging"
    if (Test-Path $staging) { Remove-Item $staging -Recurse -Force -Confirm:$false }
    New-Item -ItemType Directory -Force $staging | Out-Null
    Copy-Item (Join-Path $Proj "mod\*") $staging -Recurse -Force

    $dll = Join-Path $Proj "plugin\build\release\DressYourFollowers.dll"
    if (-not (Test-Path $dll)) { throw "Plugin DLL not found at $dll - run build-plugin.ps1 first" }
    New-Item -ItemType Directory -Force (Join-Path $staging "SKSE\Plugins") | Out-Null
    Copy-Item $dll (Join-Path $staging "SKSE\Plugins") -Force

    New-Item -ItemType Directory -Force (Join-Path $staging "PrismaUI\views\DressYourFollowers") | Out-Null
    Copy-Item (Join-Path $Proj "plugin\view\index.html") (Join-Path $staging "PrismaUI\views\DressYourFollowers") -Force

    $zipOut = Join-Path $dist "DressYourFollowers.zip"
    if (Test-Path $zipOut) { Remove-Item $zipOut -Force -Confirm:$false }
    Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $zipOut
    Remove-Item $staging -Recurse -Force -Confirm:$false
    Write-Host "Packaged -> $zipOut" -ForegroundColor Green
}
