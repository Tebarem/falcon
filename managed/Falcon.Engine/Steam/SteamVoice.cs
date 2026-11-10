using System;
using System.Collections.Generic;
using Falcon.Native;
using Falcon.Steamworks;

namespace Falcon;

/// <summary>
/// Voice chat through Steam: the player's microphone, compressed by Steam, sent to the session (the
/// host passes it on), and each speaker heard through an <see cref="AudioStream"/>, on their
/// character when <see cref="SpeakerEntity"/> says which (spatialized like any sound), else flat.
/// Push to talk: set <see cref="Talking"/> while the key is held; or <see cref="Open"/> mic.
/// </summary>
public static unsafe class SteamVoice
{
    /// <summary>The player is talking (push to talk: true while its key is held).</summary>
    public static bool Talking { get; set; }

    /// <summary>The microphone is always open (Steam leaves out the silence).</summary>
    public static bool Open { get; set; }

    /// <summary>Others' voices, 0..2 (1: as recorded).</summary>
    public static float Volume { get; set; } = 1f;

    /// <summary>Where a speaker's voice comes from: their character (null: heard flat).</summary>
    public static Func<Connection, Entity?>? SpeakerEntity { get; set; }

    /// <summary>Players whose voices aren't played (by connection id).</summary>
    public static readonly HashSet<int> Muted = new();

    /// <summary>The players speaking now (heard in the last half second), by connection id.</summary>
    public static IEnumerable<int> Speaking
    {
        get
        {
            foreach ((int id, Speaker s) in s_speakers)
                if (Time.Total - s.HeardAt < 0.5)
                    yield return id;
        }
    }

    private sealed class Speaker
    {
        public AudioStream? Stream;
        public SoundHandle Sound;
        public double HeardAt;
    }

    private static readonly Dictionary<int, Speaker> s_speakers = new();
    private static bool s_recording;
    private static readonly byte[] s_compressed = new byte[8192];
    private static readonly byte[] s_pcm = new byte[96000];  // (half a second at 48 kHz, 16 bits)
    private static readonly float[] s_samples = new float[48000];

    // Each frame (Steam.Frame): the microphone's new voice to the session.
    internal static void Frame()
    {
        if (!Steam.IsRunning || !Network.IsActive)
            return;
        bool want = Talking || Open;
        if (want && !s_recording)
        {
            SteamUser.StartVoiceRecording();
            s_recording = true;
        }
        else if (!want && s_recording)
        {
            SteamUser.StopVoiceRecording();  // (Steam still hands out the end of it for a moment)
            s_recording = false;
        }
        uint available;
        if (SteamUser.GetAvailableVoice(&available, null, 0) != EVoiceResult.k_EVoiceResultOK || available == 0)
            return;
        uint written;
        fixed (byte* p = s_compressed)
            if (SteamUser.GetVoice(true, p, (uint)s_compressed.Length, &written, false, null, 0, null, 0) != EVoiceResult.k_EVoiceResultOK ||
                written == 0)
                return;
        Network.SendVoice(s_compressed.AsSpan(0, (int)written));
    }

    // A voice packet from a player (Network): decompressed, into their stream.
    internal static void Heard(int speaker, ReadOnlySpan<byte> compressed)
    {
        if (!Steam.IsRunning || Muted.Contains(speaker) || compressed.Length == 0)
            return;
        uint written;
        fixed (byte* c = compressed)
        fixed (byte* pcm = s_pcm)
            if (SteamUser.DecompressVoice(c, (uint)compressed.Length, pcm, (uint)s_pcm.Length, &written, (uint)AudioStream.SampleRate) !=
                EVoiceResult.k_EVoiceResultOK)
                return;
        int count = (int)written / 2;
        fixed (byte* pcm = s_pcm)
        {
            short* samples = (short*)pcm;
            for (int i = 0; i < count; ++i)
                s_samples[i] = samples[i] / 32768f * Volume;
        }
        if (!s_speakers.TryGetValue(speaker, out Speaker? s))
            s_speakers[speaker] = s = new Speaker();
        s.HeardAt = Time.Total;
        if (s.Stream == null)
        {
            s.Stream = new AudioStream(1, 0.5f);
            Entity? on = Network.ConnectionById(speaker) is Connection who && SpeakerEntity != null ? SpeakerEntity(who) : null;
            s.Sound = on is Entity e && e.IsValid ? s.Stream.PlayOn(e, 1f, Bus.Effects) : s.Stream.Play2D(1f, Bus.Effects);
        }
        s.Stream.Write(s_samples.AsSpan(0, Math.Min(count, s.Stream.Free)));
    }

    // The game stopped: the microphone off, the streams freed.
    internal static void Stop()
    {
        if (s_recording && Steam.IsRunning)
            SteamUser.StopVoiceRecording();
        s_recording = false;
        foreach (Speaker s in s_speakers.Values)
            s.Stream?.Dispose();
        s_speakers.Clear();
        Talking = Open = false;
    }
}
