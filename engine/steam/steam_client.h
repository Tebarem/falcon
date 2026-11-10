#pragma once

// Steam (Valve's Steamworks): steam_api64.dll, loaded at run time from beside the executable, and
// started for an App ID before the GPU (so Steam's overlay can draw over the game). Callbacks are
// dispatched by hand (SteamAPI_ManualDispatch_*): pump() takes Steam's (call results fetched with
// them) into a queue, which the game's scripts read each frame; code waiting on Steam between
// frames pumps too (Steam's own work, its networking certificate say, only moves on while its
// callbacks are taken). C# calls the whole flat API through function pointers found by name here
// (managed/Falcon.Engine/Steam).
//
// Without the DLL, without the Steam client running or signed in, the engine runs as before:
// Steam is just unavailable (status() says why).

#include <cstdint>
#include <string>
#include <vector>

namespace fx::steam {

enum class State : int32_t {
    Off = 0,       // not asked for (no App ID)
    Running = 1,   // started for the signed-in player: the API can be called
    Server = 2,    // started as a dedicated game server (SteamGameServer_*: no player)
    Failed = -1,   // asked for, but it didn't start (the DLL missing, Steam not running, ...)
};

struct Status {
    State state = State::Off;
    uint32_t appId = 0;
    std::string error;  // why it failed
};

// Starts Steam for `appId` (dev builds: as if launched by Steam for it, through the SteamAppId
// environment variable; no steam_appid.txt). Once per process; false (and the reason in
// status()) when it couldn't.
bool start(uint32_t appId);
// A shipped game started outside Steam (from its folder): Steam starts it again, as the player's
// library would (DRM, the overlay, Steam's own launch options); true: this process should end now.
// Steam isn't asked when steam_api64.dll is missing, or when Steam itself launched the game.
bool restartThroughSteam(uint32_t appId);
// A dedicated server (--server): Steam's game server, listed in the server browser, its players'
// tickets checked, on `gamePort` (the session's) with server-browser queries on `queryPort`.
// Needs the Steam client installed (or SteamCMD's "Steamworks SDK Redist"); no one signs in.
bool startServer(uint32_t appId, uint16_t gamePort, uint16_t queryPort, const char* version);
void stop();
const Status& status();
// A function of steam_api64.dll by name (null: Steam isn't running, or no such function).
void* proc(const char* name);
// Steam's work, and its callbacks into the queue (once a frame, and while waiting on Steam).
void pump();

// A queued callback: its id (k_iCallback) and its struct's bytes. A call result comes as
// SteamAPICallCompleted_t (703): that struct (16 bytes), whether the call failed (8 bytes, 0 or 1),
// then the result's struct.
struct Callback {
    int32_t id = 0;
    std::vector<uint8_t> data;
};
// The next queued callback (false: none).
bool nextCallback(Callback& out);

}  // namespace fx::steam
