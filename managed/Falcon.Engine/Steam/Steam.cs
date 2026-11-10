using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Threading.Tasks;
using Falcon.Steamworks;

namespace Falcon;

/// <summary>
/// Steam, the friendly way: who's playing, their friends, avatars, rich presence, the overlay,
/// DLC and the like. The engine starts Steam for the project's App ID (Project Settings, or
/// <c>--steam [appid]</c>); everything here is safe to call without it (<see cref="IsRunning"/>
/// false: empty answers). The whole Steamworks API is in <see cref="Falcon.Steamworks"/> (one
/// static class per interface: <see cref="SteamFriends"/>, <see cref="SteamUGC"/>, ...), and
/// <see cref="On{T}"/> hears any of its callbacks.
/// </summary>
public static unsafe class Steam
{
    /// <summary>Steam is running for this game, for its player (signed in, the API available).</summary>
    public static bool IsRunning => SteamLibrary.IsRunning && !SteamLibrary.IsServer;

    /// <summary>Steam runs this process as a dedicated game server (<see cref="SteamServer"/>).</summary>
    public static bool IsServer => SteamLibrary.IsServer;

    /// <summary>The App ID Steam runs this game as (0: none).</summary>
    public static uint AppId => IsRunning ? SteamUtils.GetAppID() : IsServer ? SteamGameServerUtils.GetAppID() : SteamLibrary.AppId;

    /// <summary>Why Steam isn't running, when it was asked for.</summary>
    public static string Error => SteamLibrary.Error;

    /// <summary>Calls <paramref name="handler"/> with each <typeparamref name="T"/> Steam sends
    /// (on the game's thread, before its update), until disposed or the game stops.</summary>
    public static IDisposable On<T>(Action<T> handler) where T : unmanaged, ISteamCallback => SteamCallbacks.On(handler);

    // ---- people

    /// <summary>The player signed in to Steam.</summary>
    public static SteamPlayer Me => IsRunning ? new SteamPlayer(SteamUser.GetSteamID()) : default;

    /// <summary>The player's friends (Steam's "immediate" friends: not pending requests).</summary>
    public static List<SteamPlayer> Friends
    {
        get
        {
            var list = new List<SteamPlayer>();
            if (!IsRunning)
                return list;
            const int immediate = 0x04;  // (k_EFriendFlagImmediate)
            int count = SteamFriends.GetFriendCount(immediate);
            for (int i = 0; i < count; ++i)
                list.Add(new SteamPlayer(SteamFriends.GetFriendByIndex(i, immediate)));
            return list;
        }
    }

    /// <summary>Sets a rich presence key ("status", "connect", "steam_display", ...): what the
    /// player's friends see. Null or "" removes it.</summary>
    public static bool SetRichPresence(string key, string? value) => IsRunning && SteamFriends.SetRichPresence(key, value ?? "");

    /// <summary>Removes all of the player's rich presence.</summary>
    public static void ClearRichPresence()
    {
        if (IsRunning)
            SteamFriends.ClearRichPresence();
    }

    // ---- the overlay

    /// <summary>Steam's overlay can show over the game (Steam's settings, and the game started
    /// with Steam before the GPU).</summary>
    public static bool OverlayEnabled => IsRunning && SteamUtils.IsOverlayEnabled();

    /// <summary>The overlay is open (from its GameOverlayActivated_t: pause the game, show the
    /// cursor).</summary>
    public static bool OverlayOpen { get; private set; }

    /// <summary>The overlay opened or closed.</summary>
    public static event Action<bool>? OverlayChanged;

    /// <summary>Opens the overlay: "friends", "community", "players", "settings",
    /// "officialgamegroup", "stats", "achievements".</summary>
    public static void OpenOverlay(string dialog = "friends")
    {
        if (IsRunning)
            SteamFriends.ActivateGameOverlay(dialog);
    }

    /// <summary>Opens a player's profile (or "chat", "steamid", "jointrade", "stats",
    /// "achievements", "friendadd", "friendremove", "friendrequestaccept", "friendrequestignore").</summary>
    public static void OpenProfile(ulong player, string dialog = "steamid")
    {
        if (IsRunning)
            SteamFriends.ActivateGameOverlayToUser(dialog, player);
    }

    /// <summary>Opens a web page in the overlay's browser.</summary>
    public static void OpenWebPage(string url, bool modal = false)
    {
        if (IsRunning)
            SteamFriends.ActivateGameOverlayToWebPage(url, modal ? EActivateGameOverlayToWebPageMode.k_EActivateGameOverlayToWebPageMode_Modal
                                                                 : EActivateGameOverlayToWebPageMode.k_EActivateGameOverlayToWebPageMode_Default);
    }

    /// <summary>Opens an app's store page (this game's, by default).</summary>
    public static void OpenStore(uint appId = 0, bool addToCart = false)
    {
        if (IsRunning)
            SteamFriends.ActivateGameOverlayToStore(appId != 0 ? appId : AppId,
                addToCart ? EOverlayToStoreFlag.k_EOverlayToStoreFlag_AddToCart : EOverlayToStoreFlag.k_EOverlayToStoreFlag_None);
    }

    /// <summary>Opens the invite dialog: for a lobby, or (without one) to join the player's game
    /// through its rich presence "connect" string.</summary>
    public static void OpenInviteDialog(ulong lobby = 0, string? connect = null)
    {
        if (!IsRunning)
            return;
        if (lobby != 0)
            SteamFriends.ActivateGameOverlayInviteDialog(lobby);
        else
            SteamFriends.ActivateGameOverlayInviteDialogConnectString(connect ?? "");
    }

    /// <summary>Where Steam's notifications show over the game.</summary>
    public static void SetNotificationPosition(ENotificationPosition position)
    {
        if (IsRunning)
            SteamUtils.SetOverlayNotificationPosition(position);
    }

    // ---- this app

    /// <summary>The game's language as Steam has it ("english", "german", ...).</summary>
    public static string Language => IsRunning ? SteamApps.GetCurrentGameLanguage() : "";

    /// <summary>Steam's own interface language.</summary>
    public static string UiLanguage => IsRunning ? SteamUtils.GetSteamUILanguage() : "";

    /// <summary>The player owns the game (or has it through Family Sharing, a free weekend...).</summary>
    public static bool IsSubscribed => IsRunning && SteamApps.BIsSubscribed();

    /// <summary>The beta branch the game runs from ("" the default branch).</summary>
    public static string Beta
    {
        get
        {
            if (!IsRunning)
                return "";
            byte* name = stackalloc byte[256];
            return SteamApps.GetCurrentBetaName(name, 256) ? SteamText.Read(name, 256) : "";
        }
    }

    /// <summary>The game's DLC: each one's App ID, whether the player has it, and its name.</summary>
    public static List<(uint appId, bool owned, string name)> Dlc
    {
        get
        {
            var list = new List<(uint, bool, string)>();
            if (!IsRunning)
                return list;
            byte* name = stackalloc byte[256];
            int count = SteamApps.GetDLCCount();
            for (int i = 0; i < count; ++i)
            {
                uint id;
                Bool8 available;
                if (SteamApps.BGetDLCDataByIndex(i, &id, &available, name, 256))
                    list.Add((id, SteamApps.BIsDlcInstalled(id), SteamText.Read(name, 256)));
            }
            return list;
        }
    }

    /// <summary>The player has this DLC installed.</summary>
    public static bool HasDlc(uint appId) => IsRunning && SteamApps.BIsDlcInstalled(appId);

    // ---- the machine

    /// <summary>The Steam hardware the game runs on (a Steam Deck, Machine, Frame; for statistics
    /// and support: decide features with <see cref="HardwareConfig"/> or the input devices).</summary>
    public static ESteamHardwareType Hardware => IsRunning ? SteamUtils.IsRunningOnSteamHardware() : default;

    /// <summary>The settings Steam suggests for this machine (a Steam Deck's, or similar hardware's).</summary>
    public static ESteamHardwareDefaultConfig HardwareConfig => IsRunning ? SteamUtils.GetSteamHardwareDefaultConfig() : default;

    /// <summary>The game runs on a Steam Deck.</summary>
    public static bool IsSteamDeck => Hardware == ESteamHardwareType.k_ESteamHardwareTypeSteamDeck;

    /// <summary>The game runs under Proton (Linux).</summary>
    public static bool IsProton => IsRunning && SteamUtils.IsRunningUnderProton();

    /// <summary>Steam's server time (Unix seconds; 0 without Steam).</summary>
    public static uint ServerTime => IsRunning ? SteamUtils.GetServerRealTime() : 0;

    /// <summary>The country the player's IP address is in (two letters).</summary>
    public static string Country => IsRunning ? SteamUtils.GetIPCountry() : "";

    /// <summary>
    /// Steam's on-screen keyboard (Big Picture, Steam Deck): what the player typed, or null when
    /// they cancelled (or it couldn't show).
    /// </summary>
    public static Task<string?> TextInputAsync(string description, string existing = "", int maxLength = 256, bool password = false,
                                               bool multipleLines = false)
    {
        if (!IsRunning || !SteamUtils.ShowGamepadTextInput(
                password ? EGamepadTextInputMode.k_EGamepadTextInputModePassword : EGamepadTextInputMode.k_EGamepadTextInputModeNormal,
                multipleLines ? EGamepadTextInputLineMode.k_EGamepadTextInputLineModeMultipleLines
                              : EGamepadTextInputLineMode.k_EGamepadTextInputLineModeSingleLine,
                description, (uint)maxLength, existing))
            return Task.FromResult<string?>(null);
        var done = new TaskCompletionSource<string?>();
        IDisposable? listening = null;
        listening = On<GamepadTextInputDismissed_t>(e =>
        {
            listening?.Dispose();
            if (!e.m_bSubmitted)
            {
                done.TrySetResult(null);
                return;
            }
            byte[] text = new byte[e.m_unSubmittedText + 1];
            fixed (byte* p = text)
                done.TrySetResult(SteamUtils.GetEnteredGamepadTextInput(p, (uint)text.Length) ? SteamText.Read(p, text.Length) : null);
        });
        return done.Task;
    }

    // ---- screenshots, game recording, Remote Play

    /// <summary>Takes a screenshot into the player's Steam library (as F12 does).</summary>
    public static void Screenshot()
    {
        if (IsRunning)
            SteamScreenshots.TriggerScreenshot();
    }

    /// <summary>Adds an image file (JPEG/PNG; a thumbnail made if none) to the player's Steam screenshots.</summary>
    public static bool AddScreenshot(string path, int width, int height, string? location = null)
    {
        if (!IsRunning)
            return false;
        uint shot = SteamScreenshots.AddScreenshotToLibrary(path, null, width, height);
        if (shot != 0 && location != null)
            SteamScreenshots.SetLocation(shot, location);
        return shot != 0;
    }

    /// <summary>Game recording (Steam's background recording): a moment on the timeline, with an
    /// icon ("steam_marker", "steam_death", ...), a priority (0-1000), and how likely it makes a clip.</summary>
    public static ulong TimelineEvent(string title, string description = "", string icon = "steam_marker", uint priority = 100,
                                      float secondsAgo = 0f, ETimelineEventClipPriority clip = ETimelineEventClipPriority.k_ETimelineEventClipPriority_Standard) =>
        IsRunning ? SteamTimeline.AddInstantaneousTimelineEvent(title, description, icon, priority, -secondsAgo, clip) : 0;

    /// <summary>What the game is doing, for the timeline's colours (playing, menus, staging, loading).</summary>
    public static void SetTimelineMode(ETimelineGameMode mode)
    {
        if (IsRunning)
            SteamTimeline.SetTimelineGameMode(mode);
    }

    /// <summary>The timeline's tooltip now ("Level 3", "In a match"); "" clears it.</summary>
    public static void SetTimelineTooltip(string text)
    {
        if (!IsRunning)
            return;
        if (text.Length == 0)
            SteamTimeline.ClearTimelineTooltip(0f);
        else
            SteamTimeline.SetTimelineTooltip(text, 0f);
    }

    /// <summary>A game phase (a match, a level) starts or ends: the recording groups by it.</summary>
    public static void GamePhase(bool start, string? id = null)
    {
        if (!IsRunning)
            return;
        if (start)
        {
            SteamTimeline.StartGamePhase();
            if (id != null)
                SteamTimeline.SetGamePhaseID(id);
        }
        else
        {
            SteamTimeline.EndGamePhase();
        }
    }

    /// <summary>Remote Play sessions playing the game now (Remote Play Together's guests too).</summary>
    public static List<(uint session, SteamPlayer player, bool together, string client)> RemotePlaySessions
    {
        get
        {
            var list = new List<(uint, SteamPlayer, bool, string)>();
            if (!IsRunning)
                return list;
            uint n = SteamRemotePlay.GetSessionCount();
            for (int i = 0; i < n; ++i)
            {
                uint id = SteamRemotePlay.GetSessionID(i);
                list.Add((id, new SteamPlayer(SteamRemotePlay.GetSessionSteamID(id)), SteamRemotePlay.BSessionRemotePlayTogether(id),
                          SteamRemotePlay.GetSessionClientName(id)));
            }
            return list;
        }
    }

    /// <summary>Invites a friend to play this game with the player through Remote Play Together.</summary>
    public static bool InviteToRemotePlay(ulong friend) => IsRunning && SteamRemotePlay.BSendRemotePlayTogetherInvite(friend);

    /// <summary>Remote Play Together's overlay (invite friends to play here).</summary>
    public static bool ShowRemotePlayTogether() => IsRunning && SteamRemotePlay.ShowRemotePlayTogetherUI();

    // ---- avatars

    /// <summary>A player's avatar (184 pixels square), when Steam has it; null without one.</summary>
    public static Task<SteamImage?> AvatarAsync(ulong player)
    {
        if (!IsRunning)
            return Task.FromResult<SteamImage?>(null);
        int image = SteamFriends.GetLargeFriendAvatar(player);
        if (image == 0)
            return Task.FromResult<SteamImage?>(null);  // (no avatar)
        if (image > 0)
            return Task.FromResult(SteamImage.Of(image));
        // -1: being downloaded; AvatarImageLoaded_t comes.
        var done = new TaskCompletionSource<SteamImage?>();
        IDisposable? listening = null;
        listening = On<AvatarImageLoaded_t>(e =>
        {
            if (e.m_steamID != player)
                return;
            listening?.Dispose();
            done.TrySetResult(SteamImage.Of(e.m_iImage));
        });
        return done.Task;
    }

    // ---- the engine's side

    private static IDisposable? s_overlay;

    // Each frame, before the game's update (Host): callbacks, and the engine's own listeners.
    internal static void Frame()
    {
        if (s_overlay == null && IsRunning)
            s_overlay = On<GameOverlayActivated_t>(e =>
            {
                OverlayOpen = e.m_bActive != 0;
                OverlayChanged?.Invoke(OverlayOpen);
            });
        if (IsRunning)
            SteamLobby.Listen();
        SteamServer.Frame();
        SteamCallbacks.Pump();
        SteamWorkshop.Frame();
        SteamControllers.Frame();
        SteamVoice.Frame();
        SteamLobby.Frame();
    }

    // The game stopped: its listeners go (the engine's are made again at the next start).
    internal static void Stop()
    {
        SteamStats.Stop();
        SteamWorkshop.Stop();
        SteamItems.Stop();
        SteamControllers.Stop();
        SteamVoice.Stop();
        SteamLobby.Stop();
        SteamServer.Stop();
        SteamCallbacks.Reset();
        s_overlay = null;
        OverlayOpen = false;
        OverlayChanged = null;
    }
}

/// <summary>A Steam player: their id, and what Steam says about them.</summary>
public readonly struct SteamPlayer(ulong id) : IEquatable<SteamPlayer>
{
    /// <summary>Their SteamID (64 bits).</summary>
    public ulong Id { get; } = id;

    /// <summary>Their persona name (Steam knows it for friends and players you've played with).</summary>
    public string Name => !Steam.IsRunning || Id == 0 ? ""
        : Id == SteamUser.GetSteamID() ? SteamFriends.GetPersonaName() : SteamFriends.GetFriendPersonaName(Id);

    /// <summary>Online, away, busy...</summary>
    public EPersonaState State => Steam.IsRunning && Id != 0 ? SteamFriends.GetFriendPersonaState(Id) : EPersonaState.k_EPersonaStateOffline;

    /// <summary>The App ID of the game they're playing (0: none).</summary>
    public unsafe uint Playing
    {
        get
        {
            if (!Steam.IsRunning || Id == 0)
                return 0;
            FriendGameInfo_t info;
            return SteamFriends.GetFriendGamePlayed(Id, &info) ? (uint)(info.m_gameID & 0xFFFFFF) : 0;
        }
    }

    /// <summary>One of their rich presence values ("status", "connect", ...).</summary>
    public string RichPresence(string key) => Steam.IsRunning && Id != 0 ? SteamFriends.GetFriendRichPresence(Id, key) : "";

    /// <summary>Their avatar, when Steam has it.</summary>
    public Task<SteamImage?> AvatarAsync() => Steam.AvatarAsync(Id);

    public bool Equals(SteamPlayer other) => Id == other.Id;
    public override bool Equals(object? obj) => obj is SteamPlayer p && Equals(p);
    public override int GetHashCode() => Id.GetHashCode();
    public override string ToString() => $"{Name} ({Id})";
}

/// <summary>An image from Steam (avatars): RGBA, 8 bits a channel, rows top to bottom.</summary>
public sealed class SteamImage(int width, int height, byte[] rgba)
{
    public int Width { get; } = width;
    public int Height { get; } = height;
    public byte[] Rgba { get; } = rgba;

    internal static unsafe SteamImage? Of(int image)
    {
        uint w, h;
        if (image <= 0 || !SteamUtils.GetImageSize(image, &w, &h) || w == 0 || h == 0)
            return null;
        byte[] rgba = new byte[w * h * 4];
        fixed (byte* p = rgba)
            if (!SteamUtils.GetImageRGBA(image, p, rgba.Length))
                return null;
        return new SteamImage((int)w, (int)h, rgba);
    }

    /// <summary>The image as a PNG file's bytes.</summary>
    public byte[] ToPng()
    {
        using var file = new MemoryStream();
        file.Write([137, 80, 78, 71, 13, 10, 26, 10]);
        Span<byte> header = stackalloc byte[13];
        System.Buffers.Binary.BinaryPrimitives.WriteInt32BigEndian(header, Width);
        System.Buffers.Binary.BinaryPrimitives.WriteInt32BigEndian(header[4..], Height);
        header[8] = 8;  // bits a channel
        header[9] = 6;  // RGBA
        Chunk(file, "IHDR", header);
        using (var pixels = new MemoryStream())
        {
            using (var z = new ZLibStream(pixels, CompressionLevel.Fastest, leaveOpen: true))
                for (int y = 0; y < Height; ++y)
                {
                    z.WriteByte(0);  // (no filter)
                    z.Write(Rgba, y * Width * 4, Width * 4);
                }
            Chunk(file, "IDAT", pixels.ToArray());
        }
        Chunk(file, "IEND", []);
        return file.ToArray();
    }

    /// <summary>The image as a data: URL, for the HTML UI's &lt;img src&gt;.</summary>
    public string ToDataUrl() => "data:image/png;base64," + Convert.ToBase64String(ToPng());

    private static void Chunk(Stream file, string type, ReadOnlySpan<byte> data)
    {
        Span<byte> word = stackalloc byte[4];
        System.Buffers.Binary.BinaryPrimitives.WriteInt32BigEndian(word, data.Length);
        file.Write(word);
        byte[] typed = new byte[4 + data.Length];
        for (int i = 0; i < 4; ++i)
            typed[i] = (byte)type[i];
        data.CopyTo(typed.AsSpan(4));
        file.Write(typed);
        System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(word, Crc32(typed));
        file.Write(word);
    }

    private static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint crc = 0xFFFFFFFF;
        foreach (byte b in data)
        {
            crc ^= b;
            for (int k = 0; k < 8; ++k)
                crc = (crc >> 1) ^ (0xEDB88320u & (uint)-(int)(crc & 1));
        }
        return ~crc;
    }
}
