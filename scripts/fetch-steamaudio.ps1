[CmdletBinding()]
param(
    # An already-downloaded steamaudio_4.8.1.zip (checked like a fresh download) instead of fetching it.
    [string]$Zip
)

# Fetches what Falcon needs to call Valve's Steam Audio (Apache 2.0) on Windows x64: the C API headers,
# phonon.dll with its import library and the third-party notices, from the Steam Audio SDK v4.8.1
# release (steamaudio_4.8.1.zip, 181 MB, every platform). The zip and every file kept from it are
# verified by SHA-256; only these files are extracted into external/steamaudio.

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$saRoot = Join-Path $repoRoot 'external/steamaudio'
$zipUrl = 'https://github.com/ValveSoftware/steam-audio/releases/download/v4.8.1/steamaudio_4.8.1.zip'
$zipSha256 = '4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449'

# Path in the zip, destination (under external/steamaudio), SHA-256.
$files = @(
    @('steamaudio/include/phonon.h', 'include/phonon.h', 'cfab67684fe1ed75a16bb9bccb81c31276b6fb3bad904073407db900045229e7'),
    @('steamaudio/include/phonon_interfaces.h', 'include/phonon_interfaces.h', '90c9ae809080b463c57ef64187d732e5944f72abab851b7b130c702e1cea5ceb'),
    @('steamaudio/include/phonon_version.h', 'include/phonon_version.h', 'ed3a14daa8a32c5884400ff12fe9acaee80b324279450f16083519cfc25270b1'),
    @('steamaudio/lib/windows-x64/phonon.dll', 'lib/phonon.dll', 'ca3dbc01dbc24492717011e80f6a51404ca143ae344ca660971d2c983f1e058d'),
    @('steamaudio/lib/windows-x64/phonon.lib', 'lib/phonon.lib', 'b6b2dc8c5f91b4d7ae8d9eef03203df8d4675f1f8816fb7ac1b7a6ee3eb0f4d8'),
    @('steamaudio/THIRDPARTY.md', 'THIRDPARTY.md', 'a95e143e7d2466e82a28d6680c7287d9fc7d772136c323ab538a6d24472e70f0')
)

function Get-Sha256([string]$path) {
    (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$missing = @($files | Where-Object {
    $destination = Join-Path $saRoot $_[1]
    -not ((Test-Path -LiteralPath $destination) -and ((Get-Sha256 $destination) -eq $_[2]))
})
if ($missing.Count -eq 0) {
    Write-Host "Steam Audio root: $saRoot (up to date)"
    return
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("falcon-steamaudio-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    if (-not $Zip) {
        $Zip = Join-Path $temp 'steamaudio_4.8.1.zip'
        Write-Host "Downloading steamaudio_4.8.1.zip (181 MB)..."
        curl.exe --fail --silent --show-error --location --retry 2 --output $Zip $zipUrl
        if ($LASTEXITCODE -ne 0) {
            throw "Download of $zipUrl failed (curl exit code $LASTEXITCODE)."
        }
    }
    $actualZip = Get-Sha256 $Zip
    if ($actualZip -ne $zipSha256) {
        throw "steamaudio_4.8.1.zip SHA-256 mismatch. Expected $zipSha256, got $actualZip."
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach ($file in $missing) {
            $source, $relative, $expectedSha256 = $file
            $entry = $archive.GetEntry($source)
            if (-not $entry) {
                throw "$source is not in steamaudio_4.8.1.zip."
            }
            $destination = Join-Path $saRoot $relative
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

Write-Host "Steam Audio root: $saRoot"
