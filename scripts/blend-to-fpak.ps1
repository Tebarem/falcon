# Blender scene -> engine scene: exports a .blend to glTF with Blender (headless, the file's own
# Python never runs) and cooks it.
#   ./scripts/blend-to-fpak.ps1 path/to/scene.blend [-Name scene] [-Preset profile] [-Blender path/to/blender.exe]
# Writes samples/_src/blender/<name>/<name>.gltf (+ textures) and samples/cooked/<name>.fpak
# (+ .fvt); open it with falcon --editor --scene samples/cooked/<name>.fpak.
# See scripts/blender_export.py for what is exported and what is fixed up.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $Blend,
    [string] $Name,
    [ValidateSet('debug', 'profile', 'release')] [string] $Preset = 'profile',
    [string] $Blender
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Blend = (Resolve-Path $Blend).Path
if (-not $Name) { $Name = [System.IO.Path]::GetFileNameWithoutExtension($Blend).ToLowerInvariant() -replace '[^a-z0-9_-]', '_' }

if (-not $Blender) {
    # The newest Blender installed under Program Files (or blender on PATH).
    $installs = Get-ChildItem "$env:ProgramFiles\Blender Foundation" -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'blender.exe') } |
        Sort-Object { [version](($_.Name -replace '[^0-9.]', '') + '.0') } -Descending
    $Blender = if ($installs) { Join-Path $installs[0].FullName 'blender.exe' } else { (Get-Command blender -ErrorAction SilentlyContinue).Source }
}
if (-not $Blender -or -not (Test-Path $Blender)) { throw 'Blender not found: pass -Blender path\to\blender.exe' }

$srcDir = Join-Path $root "samples/_src/blender/$Name"
New-Item -ItemType Directory -Force $srcDir | Out-Null
$gltf = Join-Path $srcDir "$Name.gltf"
& $Blender -b -Y $Blend --python (Join-Path $PSScriptRoot 'blender_export.py') -- $gltf
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $gltf)) { throw 'Blender export failed' }

$falcon = Join-Path $root "build/$Preset/bin/falcon.exe"
if (-not (Test-Path $falcon)) { throw "$falcon not found: build first (./build.ps1 -Preset $Preset)" }
$fpak = Join-Path $root "samples/cooked/$Name.fpak"
New-Item -ItemType Directory -Force (Split-Path $fpak) | Out-Null
& $falcon cook $gltf $fpak --force --verify
if ($LASTEXITCODE -ne 0) { throw 'falcon cook failed' }
Write-Host "Open it: $falcon --editor --scene $fpak"
