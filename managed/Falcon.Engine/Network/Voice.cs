using System;
using Falcon.Native;

namespace Falcon;

// Voice packets (SteamVoice): a datagram with its speaker's connection id; a client's go to the
// host, which plays them and passes them on to the other clients.

public static unsafe partial class Network
{
    private const byte DgVoice = 7;

    /// <summary>A connection by its id (the host: its clients; a client: the host is 0).</summary>
    public static Connection? ConnectionById(int id) => ConnectionOf(id);

    // This player's voice, compressed, to everyone in the session.
    internal static void SendVoice(ReadOnlySpan<byte> voice)
    {
        if (Transport == 0 || voice.Length > 1100)
            return;
        s_out.Clear();
        s_out.Write(DgVoice);
        s_out.Write(IsHost ? 0 : Local?.Id ?? -1);
        s_out.Write(voice.Length);
        s_out.Write(voice);
        foreach (Connection c in s_welcomed)
            fixed (byte* p = s_out.Written)
                Api.Native->netSend(Api.Ctx, Transport, c.Wire, -1, p, s_out.Length);
    }

    private static void ReceiveVoice(Connection from, ref SyncReader r, ReadOnlySpan<byte> datagram)
    {
        int speaker = r.Int();
        ReadOnlySpan<byte> voice = r.Bytes(r.Int());
        if (!r.Ok)
            return;
        if (IsHost)
        {
            speaker = from.Id;  // (a client speaks for itself only)
            foreach (Connection c in s_welcomed)
                if (c != from)
                    fixed (byte* p = datagram)
                        Api.Native->netSend(Api.Ctx, Transport, c.Wire, -1, p, datagram.Length);
        }
        SteamVoice.Heard(speaker, voice);
    }
}
