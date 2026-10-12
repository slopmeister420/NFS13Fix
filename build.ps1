# Build NFS13Fix and (optionally) deploy it next to the game exe.
#   .\build.ps1              build only
#   .\build.ps1 -Deploy      build, then copy the .asi into the game folder
#   .\build.ps1 -Uninstall   remove it from the game folder
param(
    [switch]$Deploy,
    [switch]$Uninstall,
    [string]$Config = "Release",
    [string]$GameDir = "F:\SteamLibrary\steamapps\common\Need for Speed(TM) Most Wanted"
)

$target = Join-Path $GameDir "NFS13Fix.asi"
if ($Uninstall) {
    Remove-Item $target -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $GameDir "NFS13Fix.log") -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $GameDir "NFS13Upscaler") -Recurse -ErrorAction SilentlyContinue
    Write-Host "Removed $target" -ForegroundColor Green
    return
}

$root = $PSScriptRoot
$build = Join-Path $root "build"

cmake -S $root -B $build -A Win32
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

cmake --build $build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

# the upscaler helper is 64-bit: the dlss and fsr sdks ship no 32-bit binaries
$helperBuild = Join-Path $root "build-helper"
cmake -S (Join-Path $root "helper") -B $helperBuild -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure (helper) failed." }
cmake --build $helperBuild --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build (helper) failed." }

$asi = Join-Path $build "$Config\NFS13Fix.asi"
if (-not (Test-Path $asi)) { throw "Expected output not found: $asi" }
Write-Host "Built $asi" -ForegroundColor Green

if ($Deploy) {
    if (-not (Test-Path $GameDir)) { throw "Game directory not found: $GameDir" }
    Copy-Item $asi $target -Force
    # dlss / fsr helper and the sdk runtimes, in their own folder
    $helperDir = Join-Path $GameDir "NFS13Upscaler"
    New-Item -ItemType Directory -Force $helperDir | Out-Null
    Get-ChildItem (Join-Path $helperBuild $Config) -Include *.exe, *.dll -File -Recurse |
        Copy-Item -Destination $helperDir -Force
    # NFS13Fix supersedes the old borderless-only plugin
    foreach ($old in "NFS13Borderless.asi", "NFS13Borderless.log") {
        Remove-Item (Join-Path $GameDir $old) -ErrorAction SilentlyContinue
    }
    Write-Host "Deployed to $GameDir" -ForegroundColor Green
}
