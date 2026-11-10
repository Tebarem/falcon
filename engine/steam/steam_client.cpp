#include "steam/steam_client.h"

#include "core/log.h"
#include "core/platform.h"
#include "steam/steam_transport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>


#if FALCON_STEAMWORKS
#include <steam/steam_gameserver.h>  // (the interface versions a game server asks for)
#endif

namespace fx::steam {

namespace {

Status g_status;
void* g_module = nullptr;
std::deque<Callback> g_queue;

// steam_api.h's manual dispatch.
struct CallbackMsg {  // CallbackMsg_t
    int32_t user;
    int32_t id;
    uint8_t* data;
    int32_t size;
};
struct CallCompleted {  // SteamAPICallCompleted_t
    uint64_t call;
    int32_t id;
    uint32_t size;
};
static_assert(sizeof(CallCompleted) == 16);
int (*g_pipe)() = nullptr;
void (*g_runFrame)(int) = nullptr;
bool (*g_next)(int, CallbackMsg*) = nullptr;
void (*g_free)(int) = nullptr;
bool (*g_callResult)(int, uint64_t, void*, int, int, bool*) = nullptr;

// steam_api.h: ESteamAPIInitResult SteamAPI_InitFlat(SteamErrMsg* pOutErrMsg), SteamErrMsg being
// char[1024]; k_ESteamAPIInitResult_OK is 0. steam_gameserver.h's server start likewise.
using InitFlatFn = int (*)(char (*)[1024]);
using InitServerFn = int (*)(uint32_t, uint16_t, uint16_t, int, const char*, const char*, char (*)[1024]);
using VoidFn = void (*)();

const char* initResultText(int result)
{
    switch (result) {
    case 1: return "failed (generic)";
    case 2: return "no Steam client running";
    case 3: return "the Steam client is older than this game's SDK";
    default: return "failed";
    }
}

// Steam's API library: steam_api64.dll, libsteam_api.so or libsteam_api.dylib.
std::string steamApiFile()
{
#if defined(_WIN32)
    return "steam_api64.dll";
#else
    return platform::libraryFileName("steam_api");
#endif
}

// Steam's API library from beside the executable, and the SteamAppId variables.
bool load(uint32_t appId)
{
    g_status.appId = appId;
    g_status.state = State::Failed;
    g_module = platform::loadLibrary(platform::executableDir() / steamApiFile());
    if (!g_module) {
        g_status.error = steamApiFile() + " isn't beside the executable (scripts/fetch-steamworks.ps1)";
        FX_WARN("steam: %s", g_status.error.c_str());
        return false;
    }
    // As if Steam had launched the game for this App ID (a shipped game launched through Steam
    // has these already).
    if (appId) {
        char id[16];
        std::snprintf(id, sizeof(id), "%u", appId);
        platform::setEnv("SteamAppId", id);
        platform::setEnv("SteamGameId", id);
    }
    return true;
}

void unload(const char* message, int result)
{
    g_status.error = message && message[0] ? message : initResultText(result);
    FX_WARN("steam: not running (%s)", g_status.error.c_str());
    platform::freeLibrary(g_module);
    g_module = nullptr;
}

// Manual dispatch, the callbacks' pipe, and the transports.
void started(State state, bool server)
{
    reinterpret_cast<VoidFn>(platform::librarySymbol(g_module, "SteamAPI_ManualDispatch_Init"))();
    g_pipe = reinterpret_cast<int (*)()>(platform::librarySymbol(g_module, server ? "SteamGameServer_GetHSteamPipe" : "SteamAPI_GetHSteamPipe"));
    g_runFrame = reinterpret_cast<void (*)(int)>(platform::librarySymbol(g_module, "SteamAPI_ManualDispatch_RunFrame"));
    g_next = reinterpret_cast<bool (*)(int, CallbackMsg*)>(platform::librarySymbol(g_module, "SteamAPI_ManualDispatch_GetNextCallback"));
    g_free = reinterpret_cast<void (*)(int)>(platform::librarySymbol(g_module, "SteamAPI_ManualDispatch_FreeLastCallback"));
    g_callResult = reinterpret_cast<bool (*)(int, uint64_t, void*, int, int, bool*)>(
        platform::librarySymbol(g_module, "SteamAPI_ManualDispatch_GetAPICallResult"));
    g_status.state = state;
    g_status.error.clear();
    net::registerTransport("steam", [] { return makeSteamTransport(false); });
    net::registerTransport("steamip", [] { return makeSteamTransport(true); });
}

}  // namespace

bool start(uint32_t appId)
{
    if (g_status.state != State::Off)
        return g_status.state == State::Running;
    if (!load(appId))
        return false;
    auto initFlat = reinterpret_cast<InitFlatFn>(platform::librarySymbol(g_module, "SteamAPI_InitFlat"));
    if (!initFlat || !platform::librarySymbol(g_module, "SteamAPI_ManualDispatch_Init")) {
        unload("steam_api64.dll is too old (no SteamAPI_InitFlat)", 0);
        return false;
    }
    char message[1024] = {};
    if (const int result = initFlat(&message); result != 0) {
        unload(message, result);
        return false;
    }
    started(State::Running, false);
    FX_INFO("steam: started for App ID %u", appId);
    return true;
}

bool restartThroughSteam(uint32_t appId)
{
    void* module = platform::loadLibrary(platform::executableDir() / steamApiFile());
    if (!module)
        return false;
    using RestartFn = bool (*)(uint32_t);
    auto restart = reinterpret_cast<RestartFn>(platform::librarySymbol(module, "SteamAPI_RestartAppIfNecessary"));
    const bool again = restart && restart(appId);
    if (again)
        FX_INFO("steam: started outside Steam: Steam starts the game again");
    platform::freeLibrary(module);
    return again;
}

bool startServer(uint32_t appId, uint16_t gamePort, uint16_t queryPort, const char* version)
{
    if (g_status.state != State::Off)
        return g_status.state == State::Server;
#if FALCON_STEAMWORKS
    if (!load(appId))
        return false;
    auto init = reinterpret_cast<InitServerFn>(platform::librarySymbol(g_module, "SteamInternal_GameServer_Init_V2"));
    if (!init || !platform::librarySymbol(g_module, "SteamAPI_ManualDispatch_Init")) {
        unload("steam_api64.dll is too old (no SteamInternal_GameServer_Init_V2)", 0);
        return false;
    }
    static const char versions[] = STEAMUTILS_INTERFACE_VERSION "\0" STEAMNETWORKINGUTILS_INTERFACE_VERSION "\0" STEAMGAMESERVER_INTERFACE_VERSION
        "\0" STEAMGAMESERVERSTATS_INTERFACE_VERSION "\0" STEAMHTTP_INTERFACE_VERSION "\0" STEAMINVENTORY_INTERFACE_VERSION "\0"
        STEAMNETWORKING_INTERFACE_VERSION "\0" STEAMNETWORKINGMESSAGES_INTERFACE_VERSION "\0" STEAMNETWORKINGSOCKETS_INTERFACE_VERSION
        "\0" STEAMUGC_INTERFACE_VERSION "\0";
    char message[1024] = {};
    const int result = init(0, gamePort, queryPort, eServerModeAuthenticationAndSecure, version, versions, &message);
    if (result != 0) {
        unload(message, result);
        return false;
    }
    started(State::Server, true);
    FX_INFO("steam: game server started for App ID %u (game port %u, queries on %u)", appId, unsigned(gamePort), unsigned(queryPort));
    return true;
#else
    (void)appId, (void)gamePort, (void)queryPort, (void)version;
    g_status.state = State::Failed;
    g_status.error = "this build has no Steamworks SDK";
    return false;
#endif
}

void stop()
{
    if (g_status.state != State::Running && g_status.state != State::Server)
        return;
    const char* name = g_status.state == State::Server ? "SteamGameServer_Shutdown" : "SteamAPI_Shutdown";
    if (auto shutdown = reinterpret_cast<VoidFn>(platform::librarySymbol(g_module, name)))
        shutdown();
    // (the DLL stays loaded: Steam's own threads may still be ending)
    g_status.state = State::Off;
}

const Status& status()
{
    return g_status;
}

void pump()
{
    if ((g_status.state != State::Running && g_status.state != State::Server) || !g_pipe || !g_runFrame || !g_next || !g_free ||
        !g_callResult)
        return;
    const int pipe = g_pipe();
    g_runFrame(pipe);
    CallbackMsg m;
    while (g_next(pipe, &m)) {
        Callback c;
        c.id = m.id;
        if (m.id == 703 && m.size >= int32_t(sizeof(CallCompleted))) {
            // A call result: fetched now, after the completion (16 bytes) and the failed flag (8).
            CallCompleted done;
            std::memcpy(&done, m.data, sizeof(done));
            c.data.assign(24 + done.size, 0);
            std::memcpy(c.data.data(), &done, sizeof(done));
            bool failed = false;
            const bool ok = g_callResult(pipe, done.call, c.data.data() + 24, int(done.size), done.id, &failed);
            c.data[16] = (!ok || failed) ? 1 : 0;
        } else if (m.size > 0) {
            c.data.assign(m.data, m.data + m.size);
        }
        g_free(pipe);
        g_queue.push_back(std::move(c));
    }
    while (g_queue.size() > 4096)  // (nobody reading: the oldest go)
        g_queue.pop_front();
}

bool nextCallback(Callback& out)
{
    if (g_queue.empty())
        return false;
    out = std::move(g_queue.front());
    g_queue.pop_front();
    return true;
}

void* proc(const char* name)
{
    if ((g_status.state != State::Running && g_status.state != State::Server) || !g_module || !name)
        return nullptr;
    return platform::librarySymbol(g_module, name);
}

}  // namespace fx::steam
