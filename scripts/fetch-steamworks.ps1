[CmdletBinding()]
param(
    # The Steamworks SDK 1.65 zip (steamworks_sdk_165.zip, 25 MB) from partner.steamgames.com: it
    # needs a Steamworks partner login, so it isn't downloaded here.
    [Parameter(Mandatory = $true)]
    [string]$Zip
)

# Unpacks what Falcon needs to use Steam on Windows x64 from the Steamworks SDK 1.65: the public
# headers, steam_api.json (tools/steamgen makes the C# bindings from it), steam_api64.dll, steamcmd
# (SteamPipe uploads: the editor's Upload to Steam) and the SDK's readme. The zip and every file kept from it are verified by SHA-256; only these files are
# extracted into external/steamworks (git-ignored).

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$swRoot = Join-Path $repoRoot 'external/steamworks'
$zipSha256 = '8c42792e09100988e31e3dc069de2eb1bc60702a0445bb37298ba0c54067c202'

# Path in the zip, destination (under external/steamworks), SHA-256.
$files = @(
    @('sdk/Readme.txt', 'Readme.txt', '3f42c649706fbb1bce15c0812ca19efc52b019bb6bce8cb10c457cac7bd488c8'),
    @('sdk/public/steam/isteamapps.h', 'include/steam/isteamapps.h', 'ca67f7ee268ed6b75fd22062f3f7936468ca2f7deb639670acd2ec007d260fd7'),
    @('sdk/public/steam/isteamappticket.h', 'include/steam/isteamappticket.h', 'ce3f2e3a09114c93a5740aa08956ff5414a8777f3417e1ca1bfba2afe5afe9df'),
    @('sdk/public/steam/isteamclient.h', 'include/steam/isteamclient.h', 'f82d6ef6dc352b6371eb50012e196013f9d2ac7e58b2f968f9184e81cd9c495d'),
    @('sdk/public/steam/isteamcontroller.h', 'include/steam/isteamcontroller.h', '641a3c84cfb437b3bc32a3a45f21cb18240ae85863262fcdb3bc419ffa183802'),
    @('sdk/public/steam/isteamdualsense.h', 'include/steam/isteamdualsense.h', 'f7e0ae87fab3126a4e1e9a69adaa7ddc83fccdca080d65ce80f9622bab9ea9f3'),
    @('sdk/public/steam/isteamfriends.h', 'include/steam/isteamfriends.h', '79e4ee324a7a3c42e225553d2b79c7f705bf6ee6b6f88a017ff5c457814bc50e'),
    @('sdk/public/steam/isteamgamecoordinator.h', 'include/steam/isteamgamecoordinator.h', '54cdfd91755af4cec9ae4c20e1a895d68619aebbac35a4bc38456fba7277ae0e'),
    @('sdk/public/steam/isteamgameserver.h', 'include/steam/isteamgameserver.h', '27424b04906b8ae7e8f74cecab3431183dd5285a50e2f0fdc08488de9960031a'),
    @('sdk/public/steam/isteamgameserverstats.h', 'include/steam/isteamgameserverstats.h', 'feed52d38d4107604bc3b20bda50f1e22f5dc5d52eeeab0c11fea2202c6fee15'),
    @('sdk/public/steam/isteamhtmlsurface.h', 'include/steam/isteamhtmlsurface.h', '7960531e07a7c9610e7818cd1d0cf8c091ca20d8120879944e597e65dd4eb5cc'),
    @('sdk/public/steam/isteamhttp.h', 'include/steam/isteamhttp.h', '20e6e41d3fdf59a5c61d5a28df76df5d8473487ad4df19999ba4a084c6b2e312'),
    @('sdk/public/steam/isteaminput.h', 'include/steam/isteaminput.h', 'f10f758216474c763d4280194edb82a47d9f598375242906c032be46fee219a2'),
    @('sdk/public/steam/isteaminventory.h', 'include/steam/isteaminventory.h', '17e3b5a7ea299a9dacb976719400d1d52292fe1c49c6f6fd42b4be6b3cfef2e9'),
    @('sdk/public/steam/isteammatchmaking.h', 'include/steam/isteammatchmaking.h', 'b44747b968df2bdedbf742686524c4694dbed86a937ff0cc53e7d3ba52b23bd3'),
    @('sdk/public/steam/isteammusic.h', 'include/steam/isteammusic.h', 'fe1ec934fee3c83658ed7ce65f52994454582b8b373ad35bc085657861c8bac9'),
    @('sdk/public/steam/isteamnetworking.h', 'include/steam/isteamnetworking.h', 'a770bd908cbf435d7642e721d3943fc70a9792a3afd3ee2c7b1dc42385a06590'),
    @('sdk/public/steam/isteamnetworkingmessages.h', 'include/steam/isteamnetworkingmessages.h', '2cd1813e640cff24ddf43df01e241055e2cf8871da9d4e0db35c66dd2dd88e5e'),
    @('sdk/public/steam/isteamnetworkingsockets.h', 'include/steam/isteamnetworkingsockets.h', '13a694e95332993a6c86f44119dd5f4c83f32436c7a2ded545c6c894094df44b'),
    @('sdk/public/steam/isteamnetworkingutils.h', 'include/steam/isteamnetworkingutils.h', '510fd579dacfb64b5d5342817e1b4f83159b9c56b5fdffe9a814fe77a11461b2'),
    @('sdk/public/steam/isteamparentalsettings.h', 'include/steam/isteamparentalsettings.h', '67f7e3d7f2c825bac53e588d6f10fe3771dff2d0965e4527e6474117ea25cec1'),
    @('sdk/public/steam/isteamps3overlayrenderer.h', 'include/steam/isteamps3overlayrenderer.h', '1dc18b70af957ce0ce1525e01e9bcecf16714c648ec7e95c1c33e2242499a935'),
    @('sdk/public/steam/isteamremoteplay.h', 'include/steam/isteamremoteplay.h', '04324de4bd3ccfe138ff6fa2bc2c05435069e2ed73964829b47eabfb10c26a2e'),
    @('sdk/public/steam/isteamremotestorage.h', 'include/steam/isteamremotestorage.h', 'c4e65650d9eab986ae99956d14a231b00b85584108c555e829b9b9818f1d3c65'),
    @('sdk/public/steam/isteamscreenshots.h', 'include/steam/isteamscreenshots.h', '010c01baefccdcd44fd9b14d537728d55a3f409187dfbee25c6df9ab804a936a'),
    @('sdk/public/steam/isteamtimeline.h', 'include/steam/isteamtimeline.h', '9e7e2458f99f6010e0385091517facd2b1b4e65bbba416e20ef545031629a102'),
    @('sdk/public/steam/isteamugc.h', 'include/steam/isteamugc.h', '456fff706ca57263473be44693a03339bbacd9d9c467c39d3c432e4b3aac69c7'),
    @('sdk/public/steam/isteamuser.h', 'include/steam/isteamuser.h', 'a6848980bfbc92a83f460ab8141422f0aaaf52539b7a8ac9b5ed3d77edf1f992'),
    @('sdk/public/steam/isteamuserstats.h', 'include/steam/isteamuserstats.h', 'a0e8d5b9a6ff8d6a9bcb84b036d22a66d619c38b3c8ef69d608a79ff27261caa'),
    @('sdk/public/steam/isteamutils.h', 'include/steam/isteamutils.h', '11dcbc040b2b10cec6a689d3b2db944da17909575cec5d340dc3cac838e86720'),
    @('sdk/public/steam/isteamvideo.h', 'include/steam/isteamvideo.h', 'cbe1ebe0eb0c69508162a07516a76a5c0179822f9196024f5ec0aceed8045ee5'),
    @('sdk/public/steam/matchmakingtypes.h', 'include/steam/matchmakingtypes.h', '56d15796c68d4818fa3a25056946d7747245f564f6dd4e0976659a34422329cc'),
    @('sdk/public/steam/steam_api.h', 'include/steam/steam_api.h', '2e936125192e54277657bc022688694c9e5ec40801d4e4fcdd939724b80d3670'),
    @('sdk/public/steam/steam_api.json', 'include/steam/steam_api.json', '8c1038d41b236571c69dfdeb3f26171a7ff3659372d897ecec7f9d66d8e17d20'),
    @('sdk/public/steam/steam_api_common.h', 'include/steam/steam_api_common.h', '682b12d7d355d5ad7142b3a68ed66626d52d02673f3865d9b4577c6d4bc08f4f'),
    @('sdk/public/steam/steam_api_flat.h', 'include/steam/steam_api_flat.h', 'de13c310b5df6a02c8e5f97211a2b28c82980960f4c8011342fea37730308e39'),
    @('sdk/public/steam/steam_api_internal.h', 'include/steam/steam_api_internal.h', '6382f3c8829496ca91cdd50a694b3f97972c6a24793431377cea5b93de9c81cd'),
    @('sdk/public/steam/steam_gameserver.h', 'include/steam/steam_gameserver.h', '76e532d60b5471f9d91b330360bad2d9371662bd5e8b3a58875fb729cf75bcd7'),
    @('sdk/public/steam/steamclientpublic.h', 'include/steam/steamclientpublic.h', '9b3122a153ad3999103135cc3f9b0cdd4027b26bc5fd65088af51397d892db78'),
    @('sdk/public/steam/steamencryptedappticket.h', 'include/steam/steamencryptedappticket.h', '61f60ce892cc607ac394cb0ba6f70792acb9520a8caa33decb0aef92395100a9'),
    @('sdk/public/steam/steamhttpenums.h', 'include/steam/steamhttpenums.h', 'c7381b825717b6425b349d4633cc319f58a17f2db9e42ef24d435ca0a69ef742'),
    @('sdk/public/steam/steamnetworkingfakeip.h', 'include/steam/steamnetworkingfakeip.h', '36d0826ade3e17484b59e9415e939d301113ba61377d98aa9a58bd2409949f27'),
    @('sdk/public/steam/steamnetworkingtypes.h', 'include/steam/steamnetworkingtypes.h', '0ff72cf7bbc88517e525f5aba747bbb06558bd2c2b76e770cba455a441cb3eaa'),
    @('sdk/public/steam/steamps3params.h', 'include/steam/steamps3params.h', '26734abf0f621b7ec5ac7ad8690878ffbb537bc39cc6991d413677d970bb90fc'),
    @('sdk/public/steam/steamtypes.h', 'include/steam/steamtypes.h', '817e4e768e54deeaceb80349e08b1dbbcb892da581f8ab8e637ed159ff90c80d'),
    @('sdk/public/steam/steamuniverse.h', 'include/steam/steamuniverse.h', '91b43f660be112f6496eb26480732b28a9981fc7a48c78a41c90966e802b05a3'),
    @('sdk/tools/ContentBuilder/builder/steamcmd.exe', 'steamcmd/steamcmd.exe', '492a7c7bdcb4277113575b8837b320c7d2870c940768fd761de2a75dd63129b5'),
    @('sdk/redistributable_bin/win64/steam_api64.dll', 'lib/steam_api64.dll', 'e6d9bafb9a41e42fba7b21553db49f8719027af3f0deb23ff86cfc44d60e776d')
)

function Get-Sha256([string]$path) {
    (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$missing = @($files | Where-Object {
    $destination = Join-Path $swRoot $_[1]
    -not ((Test-Path -LiteralPath $destination) -and ((Get-Sha256 $destination) -eq $_[2]))
})
if ($missing.Count -eq 0) {
    Write-Host "Steamworks root: $swRoot (up to date)"
    return
}

$actualZip = Get-Sha256 $Zip
if ($actualZip -ne $zipSha256) {
    throw "steamworks_sdk_165.zip SHA-256 mismatch. Expected $zipSha256, got $actualZip."
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($Zip)
try {
    foreach ($file in $missing) {
        $source, $relative, $expectedSha256 = $file
        $entry = $archive.GetEntry($source)
        if (-not $entry) {
            throw "$source is not in steamworks_sdk_165.zip."
        }
        $destination = Join-Path $swRoot $relative
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

Write-Host "Steamworks root: $swRoot"
