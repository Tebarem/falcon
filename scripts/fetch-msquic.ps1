[CmdletBinding()]
param(
    # An already-downloaded microsoft.native.quic.msquic.openssl.2.6.3.nupkg (checked like a fresh
    # download) instead of fetching it.
    [string]$Package
)

# Fetches what Falcon needs to speak QUIC with Microsoft's MsQuic (MIT) on Windows x64: the C API
# headers, msquic.dll (its OpenSSL build: TLS 1.3 statically linked, certificates from memory, 0-RTT)
# and the licence and third-party notices, from the NuGet package
# Microsoft.Native.Quic.MsQuic.OpenSSL 2.6.3 (19 MB, every Windows architecture). The package and
# every file kept from it are verified by SHA-256; only these files are extracted into
# external/msquic. The engine loads msquic.dll at run time (engine/net/quic_transport.cpp).

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$quicRoot = Join-Path $repoRoot 'external/msquic'
$packageName = 'microsoft.native.quic.msquic.openssl.2.6.3.nupkg'
$packageUrl = "https://api.nuget.org/v3-flatcontainer/microsoft.native.quic.msquic.openssl/2.6.3/$packageName"
$packageSha256 = '41505c3861ef386c8788dd6d9e0ffea75b275b7d0651c2b940628ab729cf27bd'

# Path in the package, destination (under external/msquic), SHA-256.
$files = @(
    @('build/native/include/msquic.h', 'include/msquic.h', 'ebd3499686c2b3008ed0ee5b06dae1a62a50192816abfe9a67bbbba97cabb861'),
    @('build/native/include/msquic_winuser.h', 'include/msquic_winuser.h', 'd14e13992268e7534a7595f2b222f915c9899a38a110020a857f85f55b733bd2'),
    @('build/native/include/msquicp.h', 'include/msquicp.h', '25306330cd6944ab2d29dd8b7e2abd1537d6173bc69af29de1e92f518e7d17d7'),
    @('build/native/bin/x64/msquic.dll', 'bin/msquic.dll', '290b6c8716ac09eea043913f86164e0b2f5096c666c9f1ca81c0c5c1864e1cb0'),
    @('LICENSE', 'LICENSE', '903df5512f7d02609fed0c780a9b704f5a3eeb6e4d84ebe42a29845c81899a3c'),
    @('THIRD-PARTY-NOTICES', 'THIRD-PARTY-NOTICES', 'c823706397513b6eab16af7bd5ca30f8e98272712c71688ac246d7aec00fa5bb')
)

function Get-Sha256([string]$path) {
    (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$missing = @($files | Where-Object {
    $destination = Join-Path $quicRoot $_[1]
    -not ((Test-Path -LiteralPath $destination) -and ((Get-Sha256 $destination) -eq $_[2]))
})
if ($missing.Count -eq 0) {
    Write-Host "MsQuic root: $quicRoot (up to date)"
    return
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("falcon-msquic-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    if (-not $Package) {
        $Package = Join-Path $temp $packageName
        Write-Host "Downloading $packageName (19 MB)..."
        curl.exe --fail --silent --show-error --location --retry 2 --output $Package $packageUrl
        if ($LASTEXITCODE -ne 0) {
            throw "Download of $packageUrl failed (curl exit code $LASTEXITCODE)."
        }
    }
    $actualPackage = Get-Sha256 $Package
    if ($actualPackage -ne $packageSha256) {
        throw "$packageName SHA-256 mismatch. Expected $packageSha256, got $actualPackage."
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Package)
    try {
        foreach ($file in $missing) {
            $source, $relative, $expectedSha256 = $file
            $entry = $archive.GetEntry($source)
            if (-not $entry) {
                throw "$source is not in $packageName."
            }
            $destination = Join-Path $quicRoot $relative
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
            $partial = "$destination.partial"
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $partial, $true)
            $actualSha256 = Get-Sha256 $partial
            if ($actualSha256 -ne $expectedSha256) {
                Remove-Item -LiteralPath $partial
                throw "$source SHA-256 mismatch. Expected $expectedSha256, got $actualSha256."
            }
            Move-Item -LiteralPath $partial -Destination $destination -Force
        }
    } finally {
        $archive.Dispose()
    }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "MsQuic root: $quicRoot"
