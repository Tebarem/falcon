using System;
using System.Numerics;
using Falcon.Native;

namespace Falcon;

/// <summary>
/// (API v35) Audio a game makes as it goes: a ring of float samples (1 or 2 channels, interleaved,
/// 48 kHz) the game writes on the main thread and a voice plays on the mixer's thread (no managed
/// code there). Keep some queued (Queued) and write what there is room for (Free) each frame; when
/// it runs dry it plays silence and counts an underrun. It plays 2D, at a point, or on an entity,
/// spatialized like any sound. Dispose (or the game's end) stops and frees it.
/// </summary>
public sealed unsafe class AudioStream : IDisposable
{
    public const int SampleRate = 48000;

    private uint _id;

    public int Channels { get; }

    /// <param name="channels">1 (mono: placed in the world when played there) or 2.</param>
    /// <param name="seconds">How much it holds.</param>
    public AudioStream(int channels = 1, float seconds = 0.5f)
    {
        Channels = channels == 2 ? 2 : 1;
        _id = Api.Native->streamCreate(Api.Ctx, (uint)Channels, seconds);
    }

    /// <summary>False with --no-audio (writes and plays do nothing).</summary>
    public bool IsValid => _id != 0;

    /// <summary>Frames written and not played yet.</summary>
    public int Queued => (int)Info().queued;

    /// <summary>Frames there is room for.</summary>
    public int Free => (int)Info().free;

    /// <summary>Blocks it could not fill since it first played something.</summary>
    public int Underruns => (int)Info().underruns;

    /// <summary>Interleaved samples (frames × Channels): as many whole frames as there is room for;
    /// how many frames were taken.</summary>
    public int Write(ReadOnlySpan<float> samples)
    {
        fixed (float* p = samples)
            return (int)Api.Native->streamWrite(Api.Ctx, _id, p, (uint)(samples.Length / Channels));
    }

    /// <summary>Plays it as written, not placed anywhere (2D).</summary>
    public SoundHandle Play2D(float volume = 1f, Bus bus = Bus.Effects) =>
        new(Api.Native->streamPlay(Api.Ctx, _id, null, 0, volume, BusIndex(bus)));

    /// <summary>Plays it at a point.</summary>
    public SoundHandle Play(Vector3 position, float volume = 1f, Bus bus = Bus.Effects) =>
        new(Api.Native->streamPlay(Api.Ctx, _id, (float*)&position, 0, volume, BusIndex(bus)));

    /// <summary>Plays it on an entity: it follows it.</summary>
    public SoundHandle PlayOn(Entity entity, float volume = 1f, Bus bus = Bus.Effects) =>
        new(Api.Native->streamPlay(Api.Ctx, _id, null, entity.Id, volume, BusIndex(bus)));

    public void Dispose()
    {
        if (_id != 0)
            Api.Native->streamDestroy(Api.Ctx, _id);
        _id = 0;
    }

    private static uint BusIndex(Bus bus) => bus <= Bus.Effects ? 0u : (uint)bus - 1u;  // (the mixer's: sfx, physics, ...)

    private (uint queued, uint free, uint underruns) Info()
    {
        uint q, f, u;
        Api.Native->streamInfo(Api.Ctx, _id, &q, &f, &u);
        return (q, f, u);
    }
}
