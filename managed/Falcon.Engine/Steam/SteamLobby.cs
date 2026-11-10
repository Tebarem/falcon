using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Falcon.Steamworks;

namespace Falcon;

/// <summary>
/// A Steam lobby: a group of players (up to 250) before or around a game, with shared key/value
/// data, per-member data and a chat. Create one, find others (by their data), join them, and start
/// a session from one: its owner hosts over Steam (peer to peer), the others connect.
/// Friends join through the overlay (its invites, "Join game") or the lobby's id on the command line
/// (+connect_lobby): <see cref="JoinRequested"/>.
/// </summary>
public sealed class SteamLobby : IEquatable<SteamLobby>
{
    /// <summary>The lobby's SteamID.</summary>
    public ulong Id { get; }

    private SteamLobby(ulong id) => Id = id;

    /// <summary>A lobby by its id (to read its data, or join it).</summary>
    public static SteamLobby Of(ulong id) => new(id);

    // ---- making, finding, joining

    /// <summary>Creates a lobby (the player its owner, and in it). Null when Steam says no.</summary>
    public static async Task<SteamLobby?> CreateAsync(ELobbyType type = ELobbyType.k_ELobbyTypePublic, int maxMembers = 16)
    {
        if (!Steam.IsRunning)
            return null;
        LobbyCreated_t created = await SteamMatchmaking.CreateLobbyAsync(type, maxMembers);
        if (created.m_eResult != EResult.k_EResultOK)
        {
            Log.Warning($"Steam: the lobby wasn't created ({created.m_eResult})");
            return null;
        }
        var lobby = new SteamLobby(created.m_ulSteamIDLobby);
        Current = lobby;
        return lobby;
    }

    /// <summary>Joins a lobby (leaving the one the player is in). Null when it can't be joined (full,
    /// gone, not allowed).</summary>
    public async Task<SteamLobby?> JoinAsync()
    {
        if (!Steam.IsRunning)
            return null;
        Current?.Leave();
        LobbyEnter_t entered = await SteamMatchmaking.JoinLobbyAsync(Id);
        if (entered.m_EChatRoomEnterResponse != 1)  // (k_EChatRoomEnterResponseSuccess)
        {
            Log.Warning($"Steam: couldn't join lobby {Id} ({(EChatRoomEnterResponse)entered.m_EChatRoomEnterResponse})");
            return null;
        }
        Current = this;
        return this;
    }

    /// <summary>Lobbies of this game: those whose data match <paramref name="filters"/> (key, value),
    /// nearest first (Steam's region filter: <paramref name="distance"/>), with room for
    /// <paramref name="slots"/> more players.</summary>
    public static async Task<List<SteamLobby>> FindAsync(IEnumerable<(string key, string value)>? filters = null, int slots = 1,
                                                         ELobbyDistanceFilter distance = ELobbyDistanceFilter.k_ELobbyDistanceFilterDefault,
                                                         int max = 50)
    {
        var found = new List<SteamLobby>();
        if (!Steam.IsRunning)
            return found;
        if (filters != null)
            foreach ((string key, string value) in filters)
                SteamMatchmaking.AddRequestLobbyListStringFilter(key, value, ELobbyComparison.k_ELobbyComparisonEqual);
        if (slots > 0)
            SteamMatchmaking.AddRequestLobbyListFilterSlotsAvailable(slots);
        SteamMatchmaking.AddRequestLobbyListDistanceFilter(distance);
        SteamMatchmaking.AddRequestLobbyListResultCountFilter(max);
        LobbyMatchList_t list = await SteamMatchmaking.RequestLobbyListAsync();
        for (int i = 0; i < (int)list.m_nLobbiesMatching; ++i)
            found.Add(new SteamLobby(SteamMatchmaking.GetLobbyByIndex(i)));
        return found;
    }

    /// <summary>The lobby the player is in (null: none).</summary>
    public static SteamLobby? Current { get; private set; }

    /// <summary>Leaves the lobby (it goes when its last member leaves).</summary>
    public void Leave()
    {
        if (Steam.IsRunning)
            SteamMatchmaking.LeaveLobby(Id);
        if (Current?.Id == Id)
        {
            Current = null;
            if (Steam.IsRunning)
                SteamFriends.SetRichPresence("connect", "");
        }
    }

    // ---- who's in it

    /// <summary>Its owner (who can change its data and settings, and hosts its session).</summary>
    public SteamPlayer Owner => new(Steam.IsRunning ? SteamMatchmaking.GetLobbyOwner(Id) : 0);

    /// <summary>The player owns it.</summary>
    public bool IsOwner => Steam.IsRunning && SteamMatchmaking.GetLobbyOwner(Id) == SteamUser.GetSteamID();

    /// <summary>Its members (the player must be in it).</summary>
    public List<SteamPlayer> Members
    {
        get
        {
            var list = new List<SteamPlayer>();
            if (!Steam.IsRunning)
                return list;
            int n = SteamMatchmaking.GetNumLobbyMembers(Id);
            for (int i = 0; i < n; ++i)
                list.Add(new SteamPlayer(SteamMatchmaking.GetLobbyMemberByIndex(Id, i)));
            return list;
        }
    }

    /// <summary>How many players it takes.</summary>
    public int MemberLimit
    {
        get => Steam.IsRunning ? SteamMatchmaking.GetLobbyMemberLimit(Id) : 0;
        set
        {
            if (Steam.IsRunning)
                SteamMatchmaking.SetLobbyMemberLimit(Id, value);
        }
    }

    /// <summary>Others can join it (the owner sets it; false while a match runs, say).</summary>
    public bool Joinable
    {
        set
        {
            if (Steam.IsRunning)
                SteamMatchmaking.SetLobbyJoinable(Id, value);
        }
    }

    /// <summary>Who can find and join it (the owner sets it).</summary>
    public ELobbyType Type
    {
        set
        {
            if (Steam.IsRunning)
                SteamMatchmaking.SetLobbyType(Id, value);
        }
    }

    /// <summary>Invites a player (they get a Steam notification; accepting it fires their game's
    /// <see cref="JoinRequested"/>).</summary>
    public bool Invite(ulong player) => Steam.IsRunning && SteamMatchmaking.InviteUserToLobby(Id, player);

    /// <summary>The overlay's invite dialog for this lobby.</summary>
    public void OpenInviteDialog() => Steam.OpenInviteDialog(Id);

    // ---- data

    /// <summary>The lobby's data: anyone can read a key, its owner writes them ("" removes one).
    /// Lobbies are found by their data (<see cref="FindAsync"/>).</summary>
    public string this[string key]
    {
        get => Steam.IsRunning ? SteamMatchmaking.GetLobbyData(Id, key) : "";
        set
        {
            if (!Steam.IsRunning)
                return;
            if (string.IsNullOrEmpty(value))
                SteamMatchmaking.DeleteLobbyData(Id, key);
            else
                SteamMatchmaking.SetLobbyData(Id, key, value);
        }
    }

    /// <summary>All of its data.</summary>
    public unsafe Dictionary<string, string> Data
    {
        get
        {
            var data = new Dictionary<string, string>();
            if (!Steam.IsRunning)
                return data;
            byte* key = stackalloc byte[256];
            byte* value = stackalloc byte[8192];
            int n = SteamMatchmaking.GetLobbyDataCount(Id);
            for (int i = 0; i < n; ++i)
                if (SteamMatchmaking.GetLobbyDataByIndex(Id, i, key, 256, value, 8192))
                    data[SteamText.Read(key, 256)] = SteamText.Read(value, 8192);
            return data;
        }
    }

    /// <summary>A member's own data (a team, ready or not); each member writes their own.</summary>
    public string MemberData(ulong member, string key) => Steam.IsRunning ? SteamMatchmaking.GetLobbyMemberData(Id, member, key) : "";

    /// <summary>Sets the player's own data in the lobby.</summary>
    public void SetMyData(string key, string value)
    {
        if (Steam.IsRunning)
            SteamMatchmaking.SetLobbyMemberData(Id, key, value);
    }

    /// <summary>Says something in the lobby's chat (<see cref="ChatMessage"/> everywhere, the
    /// sender's too).</summary>
    public unsafe bool Say(string text)
    {
        if (!Steam.IsRunning)
            return false;
        byte[] utf8 = SteamText.Utf8(text)!;
        fixed (byte* p = utf8)
            return SteamMatchmaking.SendLobbyChatMsg(Id, p, utf8.Length);
    }

    // ---- a session from it

    private const string HostKey = "falcon:host";

    /// <summary>The owner: hosts the game's session over Steam (peer to peer, through Valve's
    /// relays) and tells the lobby where it is; its members connect (with
    /// <see cref="ConnectMembers"/>, at once).</summary>
    public bool HostSession(int port = 0)
    {
        if (!IsOwner || !Network.Host(port, "steam"))
            return false;
        this[HostKey] = $"steam://{SteamUser.GetSteamID()}:{port}";
        return true;
    }

    /// <summary>The owner: the lobby's game is on a dedicated server (its members connect to it,
    /// with <see cref="ConnectMembers"/>).</summary>
    public void SetGameServer(SteamServerInfo server)
    {
        if (!IsOwner)
            return;
        SteamMatchmaking.SetLobbyGameServer(Id, server.Ip, server.Port, server.SteamId);
        this[HostKey] = server.Address;
    }

    /// <summary>Where the lobby's session is ("" none yet).</summary>
    public string SessionAddress => this[HostKey];

    /// <summary>Members connect to the lobby's session as soon as its owner hosts it (default true).</summary>
    public static bool ConnectMembers { get; set; } = true;

    // ---- events (on the game's thread)

    /// <summary>A friend's invite or "Join game" was accepted (in the overlay, or the game was started
    /// with +connect_lobby): join this lobby.</summary>
    public static event Action<SteamLobby>? JoinRequested;

    /// <summary>Someone joined the player's lobby.</summary>
    public static event Action<SteamLobby, SteamPlayer>? MemberJoined;

    /// <summary>Someone left (or was disconnected from) the player's lobby.</summary>
    public static event Action<SteamLobby, SteamPlayer>? MemberLeft;

    /// <summary>The lobby's data (or a member's: the player given) changed.</summary>
    public static event Action<SteamLobby, SteamPlayer>? DataChanged;

    /// <summary>A chat message in the lobby: who said it, and what.</summary>
    public static event Action<SteamLobby, SteamPlayer, string>? ChatMessage;

    private static readonly List<IDisposable> s_listening = new();
    private static bool s_launchChecked;

    // At the game's start (Steam.Frame): Steam's lobby callbacks.
    internal static unsafe void Listen()
    {
        if (s_listening.Count > 0)
            return;
        s_listening.Add(Steam.On<GameLobbyJoinRequested_t>(e => JoinRequested?.Invoke(new SteamLobby(e.m_steamIDLobby))));
        // "Join game" on a friend in a lobby (their rich presence "connect": +connect_lobby <id>).
        s_listening.Add(Steam.On<GameRichPresenceJoinRequested_t>(e =>
        {
            string[] words = e.Connect.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            for (int i = 0; i + 1 < words.Length; ++i)
                if (words[i] == "+connect_lobby" && ulong.TryParse(words[i + 1], out ulong id))
                    JoinRequested?.Invoke(new SteamLobby(id));
        }));
        s_listening.Add(Steam.On<LobbyChatUpdate_t>(e =>
        {
            var lobby = new SteamLobby(e.m_ulSteamIDLobby);
            var who = new SteamPlayer(e.m_ulSteamIDUserChanged);
            if ((e.m_rgfChatMemberStateChange & 0x01) != 0)  // (k_EChatMemberStateChangeEntered)
                MemberJoined?.Invoke(lobby, who);
            else
                MemberLeft?.Invoke(lobby, who);
        }));
        s_listening.Add(Steam.On<LobbyDataUpdate_t>(e =>
        {
            var lobby = new SteamLobby(e.m_ulSteamIDLobby);
            DataChanged?.Invoke(lobby, new SteamPlayer(e.m_ulSteamIDMember));
            // A member: the owner hosts; connect.
            if (ConnectMembers && Current?.Id == lobby.Id && !lobby.IsOwner && Network.Role == NetworkRole.None &&
                lobby.SessionAddress is { Length: > 0 } address)
                Network.Connect(address);
        }));
        s_listening.Add(Steam.On<LobbyChatMsg_t>(e =>
        {
            byte* text = stackalloc byte[4096];
            ulong from;
            EChatEntryType type;
            int n = SteamMatchmaking.GetLobbyChatEntry(e.m_ulSteamIDLobby, (int)e.m_iChatID, &from, text, 4096, &type);
            if (n > 0)
                ChatMessage?.Invoke(new SteamLobby(e.m_ulSteamIDLobby), new SteamPlayer(from), SteamText.Read(text, n));
        }));
        // Entering a lobby: friends can join through the player's rich presence.
        s_listening.Add(Steam.On<LobbyEnter_t>(e =>
        {
            if (e.m_EChatRoomEnterResponse == 1)
                SteamFriends.SetRichPresence("connect", $"+connect_lobby {e.m_ulSteamIDLobby}");
        }));
        // Started by Steam to join a friend's lobby: +connect_lobby <id> on the command line.
        if (!s_launchChecked)
        {
            s_launchChecked = true;
            string[] args = Environment.GetCommandLineArgs();
            for (int i = 0; i + 1 < args.Length; ++i)
                if (args[i] == "+connect_lobby" && ulong.TryParse(args[i + 1], out ulong id))
                    s_pendingJoin = id;
        }
    }

    private static ulong s_pendingJoin;

    // Each frame: a join the command line asked for, once the game listens for it.
    internal static void Frame()
    {
        if (s_pendingJoin != 0 && JoinRequested != null)
        {
            ulong id = s_pendingJoin;
            s_pendingJoin = 0;
            JoinRequested(new SteamLobby(id));
        }
    }

    // The game stopped: the lobby is left, the handlers go.
    internal static void Stop()
    {
        Current?.Leave();
        Current = null;
        s_listening.Clear();  // (the callbacks themselves are dropped by SteamCallbacks.Reset)
        JoinRequested = null;
        MemberJoined = MemberLeft = null;
        DataChanged = null;
        ChatMessage = null;
    }

    public bool Equals(SteamLobby? other) => other != null && other.Id == Id;
    public override bool Equals(object? obj) => obj is SteamLobby l && Equals(l);
    public override int GetHashCode() => Id.GetHashCode();
    public override string ToString() => $"lobby {Id}";
}
