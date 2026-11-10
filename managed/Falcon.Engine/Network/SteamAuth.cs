using System;
using System.Collections.Generic;
using Falcon.Steamworks;

namespace Falcon;

// Steam auth tickets in a session's handshake, on any transport: a client running Steam sends its
// SteamID and a ticket; a host running Steam checks it with Steam (BeginAuthSession) and drops the
// player when Steam says no (a VAC ban, no license, signed in elsewhere). Network.RequireSteam
// refuses players without one.

public sealed partial class Connection
{
    /// <summary>The player's SteamID (0: they didn't give one, or Steam didn't check it yet).</summary>
    public ulong SteamId { get; internal set; }

    /// <summary>Steam checked the player's ticket (false: not yet, or no ticket).</summary>
    public bool SteamVerified { get; internal set; }

    internal ulong SteamClaimed;  // the SteamID its ticket names, while Steam checks it
}

public static unsafe partial class Network
{
    /// <summary>The host: refuse players who don't send a Steam ticket (a Steam-only game). Players'
    /// tickets are checked whenever the host runs Steam, either way.</summary>
    public static bool RequireSteam { get; set; }

    private static uint s_ticket;  // a client's ticket, cancelled when it leaves
    private static IDisposable? s_validating;

    // A client's hello: its SteamID and a ticket (none without Steam).
    private static void WriteSteamHello(SyncWriter w)
    {
        if (s_ticket != 0)
        {
            SteamUser.CancelAuthTicket(s_ticket);
            s_ticket = 0;
        }
        if (!Steam.IsRunning)
        {
            w.Write(0ul);
            w.Write(0);
            return;
        }
        byte* ticket = stackalloc byte[1024];
        uint length = 0;
        s_ticket = SteamUser.GetAuthSessionTicket(ticket, 1024, &length, null);
        w.Write(SteamUser.GetSteamID());
        w.Write((int)length);
        w.Write(new ReadOnlySpan<byte>(ticket, (int)length));
    }

    // The host: a hello's SteamID and ticket checked (null), or why the player is refused.
    private static string? CheckSteamHello(Connection from, ref SyncReader r)
    {
        ulong steamId = r.Ok ? r.ULong() : 0;
        ReadOnlySpan<byte> ticket = r.Ok ? r.Bytes(r.Int()) : default;
        if (!r.Ok || steamId == 0 || ticket.Length == 0)
            return RequireSteam ? "this game needs Steam (no Steam ticket)" : null;
        if (!Steam.IsRunning && !Steam.IsServer)
            return RequireSteam ? "the host can't check Steam tickets (Steam isn't running)" : null;
        EBeginAuthSessionResult result;
        fixed (byte* p = ticket)
            result = Steam.IsServer ? SteamGameServer.BeginAuthSession(p, ticket.Length, steamId)  // (a dedicated server)
                                    : SteamUser.BeginAuthSession(p, ticket.Length, steamId);
        if (result != EBeginAuthSessionResult.k_EBeginAuthSessionResultOK)
            return $"Steam refused the player's ticket ({result.ToString().Replace("k_EBeginAuthSessionResult", "")})";
        from.SteamClaimed = steamId;
        s_validating ??= Steam.On<ValidateAuthTicketResponse_t>(OnSteamValidated);
        return null;
    }

    // Steam's verdict on a player's ticket (now, and again whenever it changes: a VAC ban mid-game).
    private static void OnSteamValidated(ValidateAuthTicketResponse_t e)
    {
        foreach (Connection c in s_welcomed)
        {
            if (c.SteamClaimed != e.m_SteamID)
                continue;
            if (e.m_eAuthSessionResponse == EAuthSessionResponse.k_EAuthSessionResponseOK)
            {
                c.SteamId = e.m_SteamID;
                c.SteamVerified = true;
                Log.Info($"network: {c} is Steam player {c.SteamId}");
            }
            else
            {
                string why = e.m_eAuthSessionResponse.ToString().Replace("k_EAuthSessionResponse", "");
                Log.Warning($"network: Steam dropped {c}: {why}");
                Kick(c, $"Steam: {why}");
            }
            return;
        }
    }

    // A player left (the host), or the session ended.
    private static void EndSteamAuth(Connection c)
    {
        if (c.SteamClaimed != 0 && Steam.IsServer)
            SteamGameServer.EndAuthSession(c.SteamClaimed);
        else if (c.SteamClaimed != 0 && Steam.IsRunning)
            SteamUser.EndAuthSession(c.SteamClaimed);
        c.SteamClaimed = 0;
    }

    private static void ShutdownSteamAuth()
    {
        if (s_ticket != 0 && Steam.IsRunning)
            SteamUser.CancelAuthTicket(s_ticket);
        s_ticket = 0;
        s_validating?.Dispose();
        s_validating = null;
    }
}
