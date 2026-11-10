[CmdletBinding()]
param()

# Fetches what Falcon needs to call AMD FSR through the FidelityFX API on D3D12: the ffx_api and
# upscaler headers, the signed loader / upscaler DLLs (plus the loader's import library) and the
# license, from the AMD FSR SDK (FidelityFX-SDK) v2.3.0. Files are pinned to the tag and verified
# by SHA-256. Headers keep the SDK's api/include and upscalers/include layout because
# ffx_upscale.h includes "../../api/include/ffx_api.h".

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fxRoot = Join-Path $repoRoot 'external/fidelityfx'
$baseUrl = 'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.3.0/Kits/FidelityFX'

# SDK path (under Kits/FidelityFX), destination (under external/fidelityfx), SHA-256.
$files = @(
    @('api/include/ffx_api.h', 'include/api/include/ffx_api.h', 'e1a9d1b559eaf75f4cb401f1c43a66e2f3dc9621abfeb58b9c6fac56443177fd'),
    @('api/include/ffx_api.hpp', 'include/api/include/ffx_api.hpp', 'e1fab07fa9ab489db74813e6b754fc91dad6ded68870ab558002e92b6a32566a'),
    @('api/include/ffx_api_types.h', 'include/api/include/ffx_api_types.h', 'cc0adc0ffc804dd7cd03c39c15581b575e416383177ffe95060201d52cc0fc49'),
    @('api/include/ffx_api_loader.h', 'include/api/include/ffx_api_loader.h', '18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e'),
    @('api/include/dx12/ffx_api_dx12.h', 'include/api/include/dx12/ffx_api_dx12.h', 'c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530'),
    @('api/include/dx12/ffx_api_dx12.hpp', 'include/api/include/dx12/ffx_api_dx12.hpp', '31972deed9c5b94243eb72c97e24c1b4e1a0171ae0d0109f7fe842b6efdeddc7'),
    @('upscalers/include/ffx_upscale.h', 'include/upscalers/include/ffx_upscale.h', '65be36d5653eb71c10379e0204f7bb7cd6a8aeec96e6f8cffc5f59ffeb1f89bf'),
    @('upscalers/include/ffx_upscale.hpp', 'include/upscalers/include/ffx_upscale.hpp', '52066379fd629080ed5c9a2821627cd7a3b972e6592565c7f3fa54a95404bac0'),
    @('signedbin/amd_fidelityfx_loader_dx12.dll', 'bin/amd_fidelityfx_loader_dx12.dll', 'e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa'),
    @('signedbin/amd_fidelityfx_loader_dx12.lib', 'bin/amd_fidelityfx_loader_dx12.lib', '01d76ff0ae9db6dbb335a765991cc14cda207d2597803d9df983dc14ab504902'),
    @('signedbin/amd_fidelityfx_upscaler_dx12.dll', 'bin/amd_fidelityfx_upscaler_dx12.dll', 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46'),
    @('docs/license.md', 'license.md', '6757f2fec461238da6f84f83536ec195e8c036b1773a06f6f34301b9ed8c0963')
)

function Get-Sha256([string]$path) {
    (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

foreach ($file in $files) {
    $source, $relative, $expectedSha256 = $file
    $destination = Join-Path $fxRoot $relative

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

Write-Host "FidelityFX root: $fxRoot"
