[CmdletBinding()]
param()

# Fetches what the DLSS plugin (bin/dlss/falcon_dlss.dll, engine/render/dlss_plugin/) needs from
# NVIDIA's DLSS SDK v310.9.16 on GitHub: the NGX headers, the static NGX library (release and
# debug, static CRT), the DLSS runtime DLL and NVIDIA's license. Files are pinned to the tag and
# verified by SHA-256. They are NVIDIA's, under NVIDIA's license, and are not committed: only
# falcon_dlss.dll links them, never falcon.exe, which loads the plugin at runtime when present.

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dlssRoot = Join-Path $repoRoot 'external/dlss'
$baseUrl = 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16'

# SDK path, destination (under external/dlss), SHA-256.
$files = @(
    @('include/nvsdk_ngx.h', 'include/nvsdk_ngx.h', 'dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848'),
    @('include/nvsdk_ngx_defs.h', 'include/nvsdk_ngx_defs.h', 'ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e'),
    @('include/nvsdk_ngx_defs_dlssd.h', 'include/nvsdk_ngx_defs_dlssd.h', '2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716'),
    @('include/nvsdk_ngx_defs_dlssg.h', 'include/nvsdk_ngx_defs_dlssg.h', 'c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1'),
    @('include/nvsdk_ngx_defs_vk.h', 'include/nvsdk_ngx_defs_vk.h', '0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031'),
    @('include/nvsdk_ngx_helpers.h', 'include/nvsdk_ngx_helpers.h', '5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471'),
    @('include/nvsdk_ngx_helpers_d3d.h', 'include/nvsdk_ngx_helpers_d3d.h', 'ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba'),
    @('include/nvsdk_ngx_helpers_dlssd.h', 'include/nvsdk_ngx_helpers_dlssd.h', 'a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532'),
    @('include/nvsdk_ngx_helpers_dlssd_d3d.h', 'include/nvsdk_ngx_helpers_dlssd_d3d.h', '6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293'),
    @('include/nvsdk_ngx_params.h', 'include/nvsdk_ngx_params.h', '943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc'),
    @('include/nvsdk_ngx_params_dlssd.h', 'include/nvsdk_ngx_params_dlssd.h', 'd41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e'),
    @('include/nvsdk_ngx_params_dlssg.h', 'include/nvsdk_ngx_params_dlssg.h', 'c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad'),
    @('include/nvsdk_ngx_loader.h', 'include/nvsdk_ngx_loader.h', 'abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75'),
    @('lib/Windows_x86_64/x64/nvsdk_ngx_s.lib', 'lib/nvsdk_ngx_s.lib', '7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765'),
    @('lib/Windows_x86_64/x64/nvsdk_ngx_s_dbg.lib', 'lib/nvsdk_ngx_s_dbg.lib', 'd4beb60cfd83468ea1f58ac885c3e826011a1199e358ba159e7fa0604ace290e'),
    @('lib/Windows_x86_64/rel/nvngx_dlss.dll', 'bin/nvngx_dlss.dll', 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'),
    @('LICENSE.txt', 'LICENSE.txt', 'd4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0')
)

function Get-Sha256([string]$path) {
    (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

foreach ($file in $files) {
    $source, $relative, $expectedSha256 = $file
    $destination = Join-Path $dlssRoot $relative

    if ((Test-Path -LiteralPath $destination) -and ((Get-Sha256 $destination) -eq $expectedSha256)) {
        continue
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    $partial = "$destination.partial"
    Write-Host "Downloading $source..."
    curl.exe --fail --silent --show-error --location --retry 2 --output $partial "$baseUrl/$source"
    if ($LASTEXITCODE -ne 0) {
        throw "Download of $source failed (curl exit code $LASTEXITCODE)."
    }

    $actualSha256 = Get-Sha256 $partial
    if ($actualSha256 -ne $expectedSha256) {
        Remove-Item -LiteralPath $partial
        throw "$source SHA-256 mismatch. Expected $expectedSha256, got $actualSha256."
    }
    Move-Item -LiteralPath $partial -Destination $destination -Force
}

Write-Host "DLSS SDK root: $dlssRoot"
