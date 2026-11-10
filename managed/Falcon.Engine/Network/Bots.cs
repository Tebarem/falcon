using System;
using System.Collections.Generic;
using System.Text;
using Falcon.Native;

namespace Falcon;

/// <summary>
/// Load testing (--bots N --connect address): N connections from one process, each a player the
/// host simulates. A bot does the session's handshake (as a bot: the host builds its input with
/// the game's own logic, <see cref="Network.BuildingForBot"/>) and acknowledges the snapshots it
/// gets, so the host carries a real player's simulation, interest and replication; the bot itself
/// simulates and shows nothing. The game doesn't start in a bot process.
/// </summary>
public static unsafe class BotSwarm
{
    private sealed class Bot
    {
        public uint Wire;
        public bool Welcomed;
        public long Snapshots;
    }

    private static uint s_transport;
    private static readonly Dictionary<uint, Bot> s_bots = new();
    private static byte[] s_buffer = new byte[64 * 1024];
    private static readonly SyncWriter s_out = new();
    private static double s_lastReport;

    /// <summary>A bot process: the game itself doesn't run.</summary>
    public static bool Active => s_transport != 0;

    internal static void Start(int count, string address)
    {
        byte[] scheme = Encoding.UTF8.GetBytes(address + "\0");
        fixed (byte* p = scheme)
            s_transport = Api.Native->netOpen(Api.Ctx, p);
        if (s_transport == 0)
        {
            Log.Error($"bots: no transport for {address}");
            return;
        }
        int at = address.IndexOf("://", StringComparison.Ordinal);
        byte[] rest = Encoding.UTF8.GetBytes((at < 0 ? address : address[(at + 3)..]) + "\0");
        for (int i = 0; i < count; ++i)
        {
            uint wire;
            fixed (byte* p = rest)
                wire = Api.Native->netConnect(Api.Ctx, s_transport, p);
            s_bots[wire] = new Bot { Wire = wire };
        }
        Log.Info($"bots: {count} connecting to {address}");
    }

    internal static void Stop()
    {
        if (s_transport != 0)
            Api.Native->netClose(Api.Ctx, s_transport);
        s_transport = 0;
        s_bots.Clear();
    }

    // Each frame: the bots' traffic.
    internal static void Update()
    {
        if (s_transport == 0)
            return;
        FxNetEvent e;
        while (true)
        {
            int got;
            fixed (byte* p = s_buffer)
                got = Api.Native->netPoll(Api.Ctx, s_transport, &e, p, s_buffer.Length);
            if (got == 0)
                break;
            if (got < 0)
            {
                s_buffer = new byte[Math.Max(e.size, s_buffer.Length * 2)];
                continue;
            }
            if (!s_bots.TryGetValue(e.connection, out Bot? bot))
                continue;
            var r = new SyncReader(s_buffer.AsSpan(0, e.size));
            switch (e.type)
            {
            case 1:  // connected: the handshake, as a bot
                s_out.Clear();
                s_out.Write((byte)1);  // (MsgHello)
                s_out.Write(Network.ProtocolVersion);
                s_out.Write(Network.SchemaHashOf());
                s_out.Write("bot");
                s_out.Write(Network.Password);
                s_out.Write(true);  // a bot
                Send(bot, 0, s_out.Written);
                break;
            case 2:
                s_bots.Remove(e.connection);
                Log.Warning($"bots: one went: {Encoding.UTF8.GetString(s_buffer, 0, e.size)}");
                break;
            case 3:
                if (r.Byte() == 2)  // (MsgWelcome)
                    bot.Welcomed = true;
                break;
            case 4:
                if (r.Byte() == 1)  // (a snapshot datagram: acknowledged as a client does)
                {
                    ulong tick = r.ULong();
                    int part = r.Int();
                    ++bot.Snapshots;
                    s_out.Clear();
                    s_out.Write((byte)2);  // (DgAck)
                    s_out.Write(tick);
                    s_out.Write(part);
                    Send(bot, -1, s_out.Written);
                }
                break;
            }
        }
        double now = System.Diagnostics.Stopwatch.GetTimestamp() / (double)System.Diagnostics.Stopwatch.Frequency;
        if (now - s_lastReport > 5.0)
        {
            s_lastReport = now;
            int welcomed = 0;
            long snapshots = 0;
            foreach (Bot b in s_bots.Values)
            {
                welcomed += b.Welcomed ? 1 : 0;
                snapshots += b.Snapshots;
            }
            Log.Info($"bots: {welcomed} of {s_bots.Count} playing, {snapshots} snapshot datagrams so far");
        }
    }

    private static void Send(Bot bot, int channel, ReadOnlySpan<byte> data)
    {
        fixed (byte* p = data)
            Api.Native->netSend(Api.Ctx, s_transport, bot.Wire, channel, p, data.Length);
    }
}
