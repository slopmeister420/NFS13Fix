# Download the SDKs NFS13Fix builds against into external\ (SDL comes in as a git submodule).
#   .\fetch_deps.ps1
param(
    [string]$Dest = (Join-Path $PSScriptRoot "external")
)

$ProgressPreference = "SilentlyContinue" # Invoke-WebRequest is very slow with the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
New-Item -ItemType Directory -Force $Dest | Out-Null

function Get-File([string]$Url, [string]$Out) {
    New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Out
    Write-Host "  $(Split-Path $Out -Leaf)"
}

# libmem 5.1.5, 32-bit static library for /MD builds
$libmem = Join-Path $Dest "libmem-i686"
if (-not (Test-Path (Join-Path $libmem "lib\release\libmem.lib"))) {
    Write-Host "libmem"
    $name = "libmem-5.1.5-i686-windows-msvc-static-md"
    $tgz = Join-Path $env:TEMP "$name.tar.gz"
    Get-File "https://github.com/rdbo/libmem/releases/download/5.1.5/$name.tar.gz" $tgz
    $tmp = Join-Path $env:TEMP $name
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $tmp | Out-Null
    tar.exe -xzf $tgz -C $tmp
    if ($LASTEXITCODE -ne 0) { throw "could not extract $tgz" }
    $root = Get-ChildItem $tmp -Directory | Select-Object -First 1
    Remove-Item $libmem -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item $root.FullName $libmem
    Remove-Item $tmp, $tgz -Recurse -Force -ErrorAction SilentlyContinue
}

# NVIDIA DLSS SDK 310.9.1: headers, import library, runtime
$dlss = Join-Path $Dest "dlss"
if (-not (Test-Path (Join-Path $dlss "bin\nvngx_dlss.dll"))) {
    Write-Host "DLSS"
    $base = "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.1"
    $headers = (Invoke-WebRequest -UseBasicParsing "https://api.github.com/repos/NVIDIA/DLSS/contents/include?ref=v310.9.1" |
        ConvertFrom-Json) | Where-Object { $_.name -like "*.h" } | ForEach-Object { $_.name }
    foreach ($h in $headers) { Get-File "$base/include/$h" (Join-Path $dlss "include\$h") }
    Get-File "$base/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" (Join-Path $dlss "lib\x64\nvsdk_ngx_d.lib")
    Get-File "$base/lib/Windows_x86_64/rel/nvngx_dlss.dll" (Join-Path $dlss "bin\nvngx_dlss.dll")
    Get-File "$base/LICENSE.txt" (Join-Path $dlss "LICENSE.txt")
}

# AMD FidelityFX SDK 2.3.0 (FSR 3.1 / FSR 4): api headers and the signed runtime
$fsr = Join-Path $Dest "fsr"
if (-not (Test-Path (Join-Path $fsr "bin\amd_fidelityfx_upscaler_dx12.dll"))) {
    Write-Host "FidelityFX"
    $base = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.3.0/Kits/FidelityFX"
    foreach ($h in "ffx_api.h", "ffx_api.hpp", "ffx_api_loader.h", "ffx_api_types.h", "dx12/ffx_api_dx12.h", "dx12/ffx_api_dx12.hpp") {
        Get-File "$base/api/include/$h" (Join-Path $fsr "include\$h")
    }
    foreach ($h in "ffx_upscale.h", "ffx_upscale.hpp") {
        $out = Join-Path $fsr "include\$h"
        Get-File "$base/upscalers/include/$h" $out
        # one include folder here, not the sdk's tree
        (Get-Content $out -Raw).Replace('../../api/include/', '') | Set-Content $out -NoNewline -Encoding ascii
    }
    foreach ($b in "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll") {
        Get-File "$base/signedbin/$b" (Join-Path $fsr "bin\$b")
    }
    Get-File "$base/docs/license.md" (Join-Path $fsr "license.md")
}

Write-Host "done" -ForegroundColor Green
