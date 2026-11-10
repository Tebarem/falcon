# Configures and builds a preset inside a VS x64 developer environment.
#   ./build.ps1 [-Preset debug|profile|release] [-Run] [-- args for falcon]
[CmdletBinding()]
param(
    [ValidateSet('debug', 'profile', 'release')]
    [string] $Preset = 'profile',
    [switch] $Run,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $GameArgs
)

# Native tools write progress to stderr; failures are detected via $LASTEXITCODE.
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot

if (-not $env:VCToolsInstallDir) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath }
          else { "$env:ProgramFiles\Microsoft Visual Studio\2022\Community" }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs" }
    cmd /s /c "`"$vcvars`" >nul 2>nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}

Push-Location $root
try {
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
    cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
    if ($Run) {
        & "$root/build/$Preset/bin/falcon.exe" @GameArgs
        exit $LASTEXITCODE
    }
}
finally {
    Pop-Location
}
