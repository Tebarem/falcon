using System;
using System.IO;
using System.Text;
using System.Text.Json;

namespace Falcon;

// The project's network settings (Project Settings > Network, given at the start), and the
// session's state as JSON for the editor's Network tab.

public static partial class Network
{
    // The launch's "project": the project's settings (a command line's flags win over them).
    private static void ApplyProjectSettings(JsonElement p)
    {
        if (p.TryGetProperty("mode", out JsonElement mode))
            Mode = mode.GetString() == "rollback" ? NetworkMode.Rollback : NetworkMode.Authoritative;
        if (p.TryGetProperty("sendRate", out JsonElement rate))
            SendRate = Math.Clamp(rate.GetInt32(), 1, 60);
        if (p.TryGetProperty("bandwidthKbps", out JsonElement kbps))
            ClientBandwidth = Math.Max(kbps.GetInt32(), 16) * 1000;
        if (p.TryGetProperty("maxPlayers", out JsonElement max))
            MaxConnections = Math.Max(max.GetInt32(), 0);
        if (p.TryGetProperty("interpolationMs", out JsonElement interpolation))
            InterpolationDelay = (float)(interpolation.GetDouble() / 1000.0);
        if (p.TryGetProperty("inputDelay", out JsonElement delay))
            InputDelay = Math.Clamp(delay.GetInt32(), 0, 8);
        if (p.TryGetProperty("rollbackWindow", out JsonElement window))
            RollbackWindow = Math.Clamp(window.GetInt32(), 4, 120);
    }

    /// <summary>The session as JSON (the editor's Network tab): role, mode, players with their
    /// ping, loss and traffic, objects, and the host's, prediction's or rollback's numbers.</summary>
    public static string Status()
    {
        using var stream = new MemoryStream();
        using (var w = new Utf8JsonWriter(stream))
        {
            w.WriteStartObject();
            w.WriteString("role", Role.ToString().ToLowerInvariant());
            w.WriteString("mode", Mode == NetworkMode.Rollback ? "rollback" : "authoritative");
            w.WriteNumber("tick", Time.Tick);
            w.WriteNumber("port", Port);
            w.WriteNumber("objects", Count);
            w.WriteNumber("sendRate", SendRate);
            w.WriteNumber("bandwidthKbps", ClientBandwidth / 1000);
            w.WriteStartArray("players");
            foreach (Connection c in Connections)
            {
                w.WriteStartObject();
                w.WriteNumber("id", c.Id);
                w.WriteString("name", c.Name);
                w.WriteNumber("ping", Math.Round(c.Ping, 1));
                w.WriteNumber("loss", Math.Round(c.Loss, 2));
                w.WriteNumber("sentKB", c.BytesSent / 1024);
                w.WriteNumber("receivedKB", c.BytesReceived / 1024);
                w.WriteBoolean("bot", c.IsBot);
                if (c.SteamId != 0)
                    w.WriteString("steamId", c.SteamId.ToString());
                w.WriteEndObject();
            }
            w.WriteEndArray();
            if (IsHost && Mode == NetworkMode.Authoritative)
            {
                var s = ServerStats;
                w.WriteStartObject("host");
                w.WriteNumber("encodeMs", Math.Round(s.encodeMs, 2));
                w.WriteNumber("clientsMs", Math.Round(s.clientsMs, 2));
                w.WriteNumber("sendMs", Math.Round(s.sendMs, 2));
                w.WriteNumber("outKBs", Math.Round(s.bytesPerSecond / 1024, 1));
                w.WriteNumber("objectsPerPlayer", Math.Round(s.relevant, 0));
                w.WriteEndObject();
            }
            if (IsClient && Mode == NetworkMode.Authoritative)
            {
                w.WriteStartObject("prediction");
                w.WriteNumber("predicted", PredictedTicks);
                w.WriteNumber("checked", PredictionChecks);
                w.WriteNumber("corrected", Corrections);
                w.WriteEndObject();
            }
            if (Mode == NetworkMode.Rollback)
            {
                var r = RollbackStats;
                w.WriteStartObject("rollback");
                w.WriteBoolean("started", RollbackStarted);
                w.WriteBoolean("waiting", RollbackStalled);
                w.WriteNumber("rollbacks", r.rollbacks);
                w.WriteNumber("resimulated", r.resimulated);
                w.WriteNumber("tooLate", r.tooLate);
                w.WriteNumber("hashChecks", r.hashChecks);
                w.WriteNumber("desyncs", r.desyncs);
                w.WriteEndObject();
            }
            w.WriteEndObject();
        }
        return Encoding.UTF8.GetString(stream.ToArray());
    }
}
