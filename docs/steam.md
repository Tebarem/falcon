# Steam

Games get the whole Steamworks API (SDK 1.65) from C#, with a friendly layer for the common things.

**Setup:**

- **The SDK** needs a Steamworks partner login, so it isn't downloaded. Unpack the zip you downloaded:

  ```bash
  powershell -File scripts/fetch-steamworks.ps1 -Zip <path>\steamworks_sdk_165.zip
  ```

  It keeps the public headers, `steam_api.json`, `steam_api64.dll` and the SDK's readme in `external/steamworks/` (git-ignored). The zip and each file are checked by SHA-256.
- **The build** copies `steam_api64.dll` beside `falcon.exe` (`cmake/FalconSteamworks.cmake`). The engine loads it at run time. A build or a machine without it runs as before, without Steam.
- **The App ID:** `"steam": {"appId": N}` in `project.falcon` (480 is Valve's Spacewar, for development). Or pass `--steam [appid]`; with no number it uses the project's ID, else 480.

**Starting:**

- Steam starts before the GPU, so its overlay can draw over the game.
- A development run is started as if Steam had launched it for that App ID (the `SteamAppId` environment variable). No `steam_appid.txt` is written.
- Without the Steam client running and signed in, `Steam.IsRunning` is false and `Steam.Error` says why. The game runs on.

**From C# (`Falcon.Steam`):**

| Area | What |
|---|---|
| People | `Steam.Me`, `Steam.Friends`: `SteamPlayer` with its `Id`, `Name`, `State`, `Playing`, `RichPresence(key)`, `AvatarAsync()` |
| Rich presence | `Steam.SetRichPresence(key, value)`, `ClearRichPresence()` |
| Overlay | `OpenOverlay(dialog)`, `OpenProfile(id)`, `OpenWebPage(url)`, `OpenStore(appId)`, `OpenInviteDialog(lobby or connect string)`, `SetNotificationPosition`, `OverlayEnabled`, `OverlayOpen` and `OverlayChanged` |
| The app | `AppId`, `Language`, `UiLanguage`, `IsSubscribed`, `Beta`, `Dlc`, `HasDlc(appId)` |
| The machine | `Hardware` (Steam Deck, Machine, Frame), `HardwareConfig`, `IsSteamDeck`, `IsProton`, `ServerTime`, `Country` |
| Input | `TextInputAsync(...)`: Steam's on-screen keyboard (Big Picture, Steam Deck) |
| Images | `SteamImage`: RGBA pixels, `ToPng()`, and `ToDataUrl()` for the HTML UI |

Everything is safe to call without Steam: it gives empty answers.

**The whole API (`Falcon.Steamworks`):**

- One static class per interface: `SteamFriends`, `SteamUserStats`, `SteamUGC`, `SteamNetworkingSockets`, `SteamGameServer` and the rest, with all their methods (1,018 functions).
- Every struct and callback, every enum and constant. Strings come and go as C# strings, and `bool` as `bool`.
- `Steam.On<T>(handler)` hears any callback, such as `PersonaStateChange_t` or `GameLobbyJoinRequested_t`.
- Every call that returns a call result also has an `...Async` version that gives a `Task` of its result, for example `await SteamUserStats.FindLeaderboardAsync("Feet Traveled")`.
- **Dispatch:** the engine dispatches callbacks and call results by hand once a frame, on the game's thread, before its update. A game's handlers and the results it waits for go when it stops, so hot reload is safe.
- `SteamFlat` has the raw functions, as they are.

**The bindings are generated:** `python tools/steamgen/steamgen.py` turns `steam_api.json` into `managed/Falcon.Engine/Steam/Generated/*.g.cs`, which are committed.

- **Layouts aren't guessed.** A probe built with MSVC from the SDK's own headers measures every struct's size and every field's offset, and the C# structs use them. This covers Steam's packing (8 bytes for callbacks, 1 for the networking addresses). A field whose C# type would be a different size becomes bytes of the measured size; only one does, in a nested union.
- **Constants** come from the probe too.
- **Calls** go through function pointers looked up by name in `steam_api64.dll`. That works with NativeAOT and needs no import library.

| Test | What it checks |
|---|---|
| `steam_jit`, `steam_aot` | With `--steam 480`: the signed-in player, a call result (Spacewar's leaderboard) and the player's avatar. Skipped when the Steam client isn't running or signed in. |

**Networking over Steam.** Two more transports, made by `engine/steam/steam_transport.cpp` on `ISteamNetworkingSockets`:

| Address | What |
|---|---|
| `steam://<steamid64>[:port]` | Peer to peer by SteamID, through Valve's relays (SDR). It handles NAT traversal and keeps players' IP addresses hidden. `port` is a virtual port. |
| `steamip://host:port` | Steam's sockets over plain UDP, for dedicated servers and LAN. |

- **Channels** are lanes 0–3 (reliable, each ordered); datagrams go unreliable on lane 4. Steam encrypts every connection.
- **Hosting:** a host picks its transport with `--net-transport steam|steamip` (or `Network.Host(port, "steamip")`). The client processes it launches get the matching address.
- **Readiness:** before the first connection, the transport waits for this machine's Steam certificate and Steam's network config (about half a second, once a process).

**Callbacks:** the engine owns Steam's callback queue (API v42).

- Its pump runs Steam's frame and takes the callbacks into a queue, fetching each call result as its completion arrives. The scripts read the queue each frame.
- Code that waits on Steam between frames pumps too. Steam's own work, its networking certificate for one, only moves on while its callbacks are taken.

**Auth tickets.** A client running Steam sends its SteamID and an auth ticket in its hello, on any transport.

- A host running Steam has Steam check the ticket (`BeginAuthSession`). It drops the player when Steam says no, at join or later: a VAC ban, no license, signed in elsewhere.
- `Connection.SteamId` and `SteamVerified` say who a player is.
- `Network.RequireSteam` refuses players who send no ticket.
- `Network.Kick(connection, why)` drops a player.
- Players' names are their Steam names when they run Steam.
- Protocol 8 (7 until RPC messages got a kind byte of their own: 30. At 20 they shared rollback's
  start message, so clients took every reliable RPC from the host for it and dropped it).

| Test | What it checks |
|---|---|
| `net_steam` | A session over `steamip://`: a host and one client process, verified, with the player's ticket checked by Steam. Skipped without Steam. |

**Not checked automatically:** `steam://` peer to peer needs two Steam accounts (Steam won't connect an account to itself). It is a manual check on two machines.

**Lobbies (`SteamLobby`).** A lobby is a group of up to 250 players before or around a game, with shared data, per-member data and a chat.

| What | How |
|---|---|
| Making | `CreateAsync(type, max)` |
| Finding | `FindAsync(filters on its data, slots, distance)` |
| Joining and leaving | `JoinAsync()`, `Leave()` |
| Who's in it | `Owner`, `Members`, `MemberLimit`, `Joinable`, `Type` |
| Data | `lobby[key]` (the owner writes), `Data`, `MemberData` and `SetMyData` |
| Chat | `Say(text)` |
| Invites | `Invite(player)`, `OpenInviteDialog()` |
| Events | `MemberJoined`, `MemberLeft`, `DataChanged`, `ChatMessage` |

- **Joining from Steam:** `SteamLobby.JoinRequested` fires when a friend's invite or "Join game" is accepted in the overlay, or when the game was started with `+connect_lobby <id>`.
- **Rich presence:** entering a lobby sets the player's rich presence `connect`, so friends can join from their friends list.
- **A session from a lobby:** `HostSession(port)` has the owner host over Steam peer to peer and writes the address into the lobby. Its members connect at once (`ConnectMembers`). For a dedicated server, `SetGameServer(server)` points the lobby at it.

**Dedicated servers on Steam (`SteamServer`).** `--server --steam` starts Steam's game server instead of a player's Steam (`SteamGameServer_Init`).

- **Ports:** the game port is the session's; queries go on the next port. On the default port 27015, Steam's LAN search finds the server.
- **Logon:** the server logs on anonymously, or with `SteamServer.LoginToken` (a game server login token: a persistent SteamID).
- **Listing:** it is listed while it runs, with `Name`, `Map`, `Tags`, `Description` and rules (`SetRule`). Max players and the password follow the session's settings.
- **Players:** each player's ticket is checked by the server itself (`SteamGameServer.BeginAuthSession`, VAC secure).
- **Sockets:** `steamip://` hosting uses the game server's own sockets, and the transport waits for its certificate once it has logged on.
- **`RestartRequested`** says Steam has an update for the game.

**The server browser:** `SteamServer.FindLanAsync()`, `FindInternetAsync(filters)`, `FindFriendsAsync()` and `FindFavoritesAsync(history)`.

- Each returns `SteamServerInfo`: name, map, players, ping, tags, password and secure flags. Its `Address` goes straight to `Network.Connect`.
- The browser's response object is a C++ interface. It is built in C# as a vtable of `[UnmanagedCallersOnly]` functions.

| Test | What it checks |
|---|---|
| `steam_jit`, `steam_aot` | Also a lobby: created, found by its data, left. |
| `net_steam_server` | A dedicated server on Steam (anonymous logon, VAC secure): a client verified, its ticket checked by the server, and the server in the client's LAN server list. |

**Stats and achievements (`SteamStats`):**

- **Stats:** `GetInt`/`GetFloat`, `Set`, `Add`, `AddToAverage` (rate stats).
- **Achievements:** `Achievements` (their API names), `IsUnlocked`, `UnlockedAt`, `Display(name, "name"|"desc"|"hidden")`, `Unlock`, `Clear`, `ShowProgress`.
- **Global numbers:** `GlobalPercentagesAsync()` (the share of players with each achievement) and `PlayersNowAsync()`.
- **Storing:** changes stay local until `Store()` sends them; an achievement's popup shows then, and `Unlock` stores at once by default. The engine stores what changed when the game stops.

**Leaderboards (`SteamLeaderboard`):**

- `FindAsync(name, create, sort, display)`, then `Name` and `EntryCount`.
- `UploadAsync(score, keepBest, details)` gives back whether it changed and the rank before and after.
- `EntriesAsync(global | around the player | friends, start, end)` gives `SteamLeaderboardEntry` values: player, rank, score, details.

**Steam Cloud (`SteamCloud`):**

- `IsEnabled` (the player's and the app's switches), `Quota`, `Write`, `Read`, `Exists`, `Delete`, `Files`.
- `Storage` (the game's saves) uses it by itself when it is on. A save also goes to the cloud, and a load reads the cloud's copy first: the newest, from whichever PC. `Storage.UseSteamCloud = false` turns that off.

**More on `Steam`:**

- **Screenshots:** `Screenshot()` takes one into the player's library (as F12 does); `AddScreenshot(path, w, h, location)` adds an image file.
- **Game recording timeline:** `TimelineEvent(title, description, icon, priority, secondsAgo, clip)`, `SetTimelineMode`, `SetTimelineTooltip`, and `GamePhase(start, id)`, which groups the recording by match or level.
- **Remote Play:** `RemotePlaySessions`, `InviteToRemotePlay(friend)`, `ShowRemotePlayTogether()`.

**Tests:** `steam_jit` and `steam_aot` also read the achievements, a stat, the top five entries of Spacewar's leaderboard and the cloud quota. They only read: unlocking, uploading and cloud writes would change the player's account. The leaderboard download is tried twice, since Steam's servers fail one now and then.

**The Workshop (`SteamWorkshop`):**

- **Finding:** `FindAsync(rank, page, search, tags)` (ranked by vote, newest, trend...) and `DetailsAsync(ids)`. They give back `SteamWorkshopItem` values: title, description, tags, owner, votes, score, preview URL, size.
- **Subscribing:** `SubscribeAsync`/`UnsubscribeAsync`. Steam downloads the items and keeps them updated. `Subscribed` lists them, `Installed` gives each one's folder, and there are `Download`/`DownloadProgress`.
- **Publishing:** `PublishAsync(folder, title, description, preview, tags, visibility, existing, changeNote, progress)` uploads a folder of files as a new item, or updates `existing`. It gives back the item's id, and whether the player must first accept the Workshop's agreement.

**Inventory (`SteamItems`):** each call goes through Steam's servers, and its result comes back as the player's items (`SteamItem`: instance, definition, quantity).

- `AllAsync`, `ConsumeAsync`, `TriggerDropAsync` (timed drops), `GrantPromosAsync`, `ExchangeAsync` (the schema's recipes).
- `BuyAsync` opens Steam's checkout for the item store.
- `Definitions` and `Property(definition, "name"|"icon_url"|...)` read the item schema.

**Steam Input (`SteamControllers`):**

- `Start(manifest)` turns it on with the game's action manifest. Every controller Steam supports, with the player's own bindings, then reads through it.
- **Reading:** `Digital(action)`, `Analog(action)`, `ActivateSet(set)`.
- **Prompts:** `Glyph(action, set)` gives the PNG of what to press.
- **Also:** `ShowBindings()` (Steam's binding screen) and `Vibrate`.
- **With `InputAction`:** `Input.Action("jump").BindSteam("jump")`, or `BindSteam("move", 0)` / `(…, 1)` for an analog action's x and y, so games read Steam Input through the same names as keys and pads.
- **Determinism:** Steam Input values aren't in the tick input ring, so a replay doesn't reproduce them. Over the network they travel as the inputs `BuildInput` builds.

**Voice chat (`SteamVoice`):**

- **Talking:** `Talking` (push to talk: true while its key is held) or `Open` (an always-open mic; Steam leaves out the silence).
- **The path:** the voice is compressed by Steam and sent as datagrams to the session. The host plays it and passes it on (it ignores a client's claim to speak for another).
- **Hearing:** each speaker plays through an `AudioStream`, on their character when `SpeakerEntity` names it (spatialized like any sound), otherwise flat.
- **Controls:** `Volume`, `Muted` (by connection), and `Speaking` (who was heard in the last half second).

**Tests:** `steam_jit` and `steam_aot` also query Spacewar's Workshop (its 50 top-voted items) and start Steam Input. Voice needs a microphone and a second player, so it is a manual check. Publishing isn't tested automatically: it would put an item on the Workshop.

**Not checked here:** Spacewar's inventory refuses `GetAllItems` on the development account, so the inventory calls are untested against Steam.

**Not done:** the editor's Publish to Workshop (scenes, prefabs, libraries) and Steam Input glyphs in the HTML UI.

**Shipping on Steam:**

- **Project Settings > Steam:** the App ID, the Windows depot (0: the App ID + 1, Steamworks' default), the branch an upload is set live on, and **Start through Steam**.
- **Build Game for a Steam game:**
  - The build has `steam_api64.dll` and no `steam_appid.txt`. The shipped game starts Steam for its project's App ID.
  - Started outside Steam (from its folder), it starts again through Steam (`SteamAPI_RestartAppIfNecessary`), which brings the overlay, DRM and launch options. Development runs and servers aren't asked.
  - SteamPipe's build scripts go beside the build, in `Build/<name> SteamPipe/`, so they aren't uploaded: `app_build_<app>.vdf` (description, content root, build output, the branch set live; Steam refuses to set `default` live from a script) and `depot_build_<depot>.vdf` (everything under the build, `.pdb` files left out).
  - steamcmd isn't shipped with the game.
- **File > Upload to Steam…**
  - It uploads the last build through SteamPipe. steamcmd comes from the SDK: `scripts/fetch-steamworks.ps1` unpacks it to `bin/steamcmd`.
  - steamcmd runs in a console window of its own: `+login <account> +run_app_build <vdf> +quit`. You type your password and Steam Guard code there, and they never pass through the editor.
  - The dialog asks for the account name, a description and the branch each time. Then set the build live on Steamworks, or name a branch.
- **A dedicated server on a machine without the Steam client** needs Steam's server runtime beside the executable: `steamclient64.dll`, `tier0_s64.dll` and `vstdlib_s64.dll`, from SteamCMD's "Steamworks SDK Redist" (app 1007, available anonymously). Install them with `steamcmd +login anonymous +app_update 1007 +quit`. The engine doesn't download them.

**Tests:** `editor_game_build` also builds a Steam game (App ID 480). It checks the SteamPipe scripts beside the build and that steamcmd stays out of it. `game_build_run` then starts the built game with Start through Steam off.
