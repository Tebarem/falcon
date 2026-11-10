using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Falcon.Steamworks;

namespace Falcon;

/// <summary>
/// A dedicated server on Steam (<c>--server --steam</c>): listed in Steam's server browser (name,
/// map, players, rules), its players' Steam tickets checked by the server itself, logged on
/// anonymously or with a game server login token. And the browser: <see cref="FindLanAsync"/>,
/// <see cref="FindInternetAsync"/>.
/// </summary>
public static unsafe class SteamServer
{
    private static bool s_configured;
    private static string s_name = "", s_map = "", s_tags = "", s_description = "Falcon game";
    private static int s_maxPlayers = -1;
    private static bool s_password;
    private static readonly List<IDisposable> s_listening = new();

    /// <summary>The server's name in the browser (default: the machine's name).</summary>
    public static string Name
    {
        get => s_name;
        set
        {
            s_name = value;
            if (s_configured)
                SteamGameServer.SetServerName(value);
        }
    }

    /// <summary>The map (scene) it runs.</summary>
    public static string Map
    {
        get => s_map;
        set
        {
            s_map = value;
            if (s_configured)
                SteamGameServer.SetMapName(value);
        }
    }

    /// <summary>Tags, comma separated: the browser filters by them ("gametagsand").</summary>
    public static string Tags
    {
        get => s_tags;
        set
        {
            s_tags = value;
            if (s_configured)
                SteamGameServer.SetGameTags(value);
        }
    }

    /// <summary>The game's description in the browser (set before the server starts).</summary>
    public static string Description { get => s_description; set => s_description = value; }

    /// <summary>A game server login token (steamcommunity.com/dev/managegameservers): a persistent
    /// SteamID and reputation. Without one the server logs on anonymously. Set before it starts.</summary>
    public static string? LoginToken { get; set; }

    /// <summary>A rule shown in the browser's server details ("" removes it).</summary>
    public static void SetRule(string key, string value)
    {
        if (Steam.IsServer)
            SteamGameServer.SetKeyValue(key, value);
    }

    /// <summary>Steam's servers know it (its SteamID set, listed).</summary>
    public static bool LoggedOn => Steam.IsServer && SteamGameServer.BLoggedOn();

    /// <summary>Its SteamID (once logged on).</summary>
    public static ulong SteamId => Steam.IsServer ? SteamGameServer.GetSteamID() : 0;

    /// <summary>Steam asked it to restart (an update of the game is out).</summary>
    public static bool RestartRequested => Steam.IsServer && SteamGameServer.WasRestartRequested();

    // At the game's start, before its session (the transport waits for the server's certificate,
    // which comes once it logs on).
    internal static void Start()
    {
        if (!Steam.IsServer || s_configured)
            return;
        s_configured = true;
        SteamGameServer.SetModDir("falcon");
        SteamGameServer.SetProduct(Steam.AppId.ToString());
        SteamGameServer.SetGameDescription(s_description);
        SteamGameServer.SetDedicatedServer(true);
        SteamGameServer.SetServerName(s_name.Length > 0 ? s_name : (s_name = Environment.MachineName));
        SteamGameServer.SetMapName(s_map);
        SteamGameServer.SetGameTags(s_tags);
        s_listening.Add(Steam.On<SteamServersConnected_t>(_ =>
            Log.Info($"steam: game server logged on as {SteamGameServer.GetSteamID()} ({(SteamGameServer.BSecure() ? "VAC secure" : "not secure")})")));
        s_listening.Add(Steam.On<SteamServerConnectFailure_t>(e =>
            Log.Warning($"steam: game server couldn't log on ({e.m_eResult}{(e.m_bStillRetrying ? ", retrying" : "")})")));
        if (string.IsNullOrEmpty(LoginToken))
            SteamGameServer.LogOnAnonymous();
        else
            SteamGameServer.LogOn(LoginToken);
        SteamGameServer.SetAdvertiseServerActive(true);
    }

    // Each frame: what the browser shows follows the session's settings.
    internal static void Frame()
    {
        if (!s_configured)
            return;
        int max = Network.MaxConnections > 0 ? Network.MaxConnections : 64;
        if (max != s_maxPlayers)
            SteamGameServer.SetMaxPlayerCount(s_maxPlayers = max);
        if ((Network.Password.Length > 0) != s_password)
            SteamGameServer.SetPasswordProtected(s_password = Network.Password.Length > 0);
    }

    internal static void Stop()
    {
        if (s_configured && Steam.IsServer)
            SteamGameServer.SetAdvertiseServerActive(false);
        s_configured = false;
        s_maxPlayers = -1;
        s_password = false;
        s_listening.Clear();
        foreach (Request r in s_requests.Values)
            r.Done.TrySetResult(r.Servers);
        s_requests.Clear();
    }

    // ---- the server browser

    /// <summary>This game's servers on the local network.</summary>
    public static Task<List<SteamServerInfo>> FindLanAsync() =>
        Begin(response => SteamMatchmakingServers.RequestLANServerList(Steam.AppId, response));

    /// <summary>This game's servers on the internet, filtered by Steam's server filters (key, value:
    /// "map", "gametagsand", "notfull", "hasplayers", "dedicated", "secure", "name_match"...).</summary>
    public static Task<List<SteamServerInfo>> FindInternetAsync(IEnumerable<(string key, string value)>? filters = null) =>
        Begin(response => WithFilters(filters, (list, n) => SteamMatchmakingServers.RequestInternetServerList(Steam.AppId, (MatchMakingKeyValuePair_t**)list, n, response)));

    /// <summary>Servers the player's friends play on.</summary>
    public static Task<List<SteamServerInfo>> FindFriendsAsync() =>
        Begin(response => WithFilters(null, (list, n) => SteamMatchmakingServers.RequestFriendsServerList(Steam.AppId, (MatchMakingKeyValuePair_t**)list, n, response)));

    /// <summary>The player's favourite servers, and the ones they played on.</summary>
    public static Task<List<SteamServerInfo>> FindFavoritesAsync(bool history = false) =>
        Begin(response => WithFilters(null, (list, n) => history ? SteamMatchmakingServers.RequestHistoryServerList(Steam.AppId, (MatchMakingKeyValuePair_t**)list, n, response)
                                                                   : SteamMatchmakingServers.RequestFavoritesServerList(Steam.AppId, (MatchMakingKeyValuePair_t**)list, n, response)));

    private sealed class Request
    {
        public readonly TaskCompletionSource<List<SteamServerInfo>> Done = new();
        public readonly List<SteamServerInfo> Servers = new();
    }

    private static readonly Dictionary<nint, Request> s_requests = new();
    private static nint s_response;  // an ISteamMatchmakingServerListResponse: its vtable, three functions

    private static Task<List<SteamServerInfo>> Begin(Func<nint, nint> request)
    {
        if (!Steam.IsRunning)
            return Task.FromResult(new List<SteamServerInfo>());
        if (s_response == 0)
        {
            nint* vtable = (nint*)NativeMemory.Alloc(3, (nuint)sizeof(nint));
            vtable[0] = (nint)(delegate* unmanaged<nint, nint, int, void>)&ServerResponded;
            vtable[1] = (nint)(delegate* unmanaged<nint, nint, int, void>)&ServerFailedToRespond;
            vtable[2] = (nint)(delegate* unmanaged<nint, nint, int, void>)&RefreshComplete;
            nint* self = (nint*)NativeMemory.Alloc(1, (nuint)sizeof(nint));
            self[0] = (nint)vtable;
            s_response = (nint)self;
        }
        var r = new Request();
        nint handle = request(s_response);
        if (handle == 0)
            return Task.FromResult(r.Servers);
        s_requests[handle] = r;
        return r.Done.Task;
    }

    private static nint WithFilters(IEnumerable<(string key, string value)>? filters, Func<nint, uint, nint> request)
    {
        var list = new List<(string, string)>(filters ?? []);
        if (list.Count == 0)
            return request(0, 0);
        // MatchMakingKeyValuePair_t: char key[256], char value[256]; passed as an array of pointers.
        var pairs = new MatchMakingKeyValuePair_t[list.Count];
        var pointers = new nint[list.Count];
        fixed (MatchMakingKeyValuePair_t* p = pairs)
        fixed (nint* pp = pointers)
        {
            for (int i = 0; i < list.Count; ++i)
            {
                (string key, string value) = list[i];
                byte* k = (byte*)&p[i];
                byte* v = k + 256;
                System.Text.Encoding.UTF8.GetBytes(key, new Span<byte>(k, 255));
                System.Text.Encoding.UTF8.GetBytes(value, new Span<byte>(v, 255));
                pp[i] = (nint)(p + i);
            }
            return request((nint)pp, (uint)list.Count);
        }
    }

    [UnmanagedCallersOnly]
    private static void ServerResponded(nint self, nint request, int server)
    {
    }

    [UnmanagedCallersOnly]
    private static void ServerFailedToRespond(nint self, nint request, int server)
    {
    }

    [UnmanagedCallersOnly]
    private static void RefreshComplete(nint self, nint request, int response)
    {
        try
        {
            if (!s_requests.Remove(request, out Request? r))
                return;
            int n = SteamMatchmakingServers.GetServerCount(request);
            for (int i = 0; i < n; ++i)
            {
                gameserveritem_t* item = SteamMatchmakingServers.GetServerDetails(request, i);
                if (item != null && item->m_bHadSuccessfulResponse)
                    r.Servers.Add(new SteamServerInfo(*item));
            }
            SteamMatchmakingServers.ReleaseRequest(request);
            r.Done.TrySetResult(r.Servers);
        }
        catch (Exception e)
        {
            Log.Error($"Steam: the server list failed: {e}");
        }
    }
}

/// <summary>A server as Steam's browser has it.</summary>
public sealed class SteamServerInfo
{
    public string Name { get; }
    public string Map { get; }
    public string Description { get; }
    public string Tags { get; }
    public int Players { get; }
    public int MaxPlayers { get; }
    public int Bots { get; }
    public int Ping { get; }
    public bool Password { get; }
    public bool Secure { get; }
    public ulong SteamId { get; }
    public uint Ip { get; }
    public ushort Port { get; }
    public ushort QueryPort { get; }

    /// <summary>Its address for <see cref="Network.Connect"/> (Steam's sockets over UDP).</summary>
    public string Address => $"steamip://{Ip >> 24}.{(Ip >> 16) & 255}.{(Ip >> 8) & 255}.{Ip & 255}:{Port}";

    internal unsafe SteamServerInfo(gameserveritem_t item)
    {
        Name = item.ServerName;
        Map = item.Map;
        Description = item.GameDescription;
        Tags = item.GameTags;
        Players = item.m_nPlayers;
        MaxPlayers = item.m_nMaxPlayers;
        Bots = item.m_nBotPlayers;
        Ping = item.m_nPing;
        Password = item.m_bPassword;
        Secure = item.m_bSecure;
        SteamId = item.m_steamID;
        Ip = item.m_NetAdr.m_unIP;
        Port = item.m_NetAdr.m_usConnectionPort;
        QueryPort = item.m_NetAdr.m_usQueryPort;
    }

    public override string ToString() => $"{Name} ({Map}, {Players}/{MaxPlayers}, {Ping} ms, {Address})";
}
