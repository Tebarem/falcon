using System;
using System.Collections.Generic;
using System.Numerics;
using Falcon.Native;

namespace Falcon;

/// <summary>The mixer's buses: each sound plays on one (its .fsound says which).</summary>
public enum Bus : uint
{
    Master = 0,
    Effects = 1,
    Physics = 2,
    Ambience = 3,
    Music = 4,
    Ui = 5,
}

/// <summary>What a surface sounds like hit, scraped or rolled on: the engine's surface presets
/// (a material's "Surface Sounds", an entity's Sounds Like, a terrain layer's). Unknown: a surface
/// with its own values.</summary>
public enum Surface : uint
{
    Unknown = 0,
    Generic, Wood, Concrete, Brick, Rock, Metal, Glass, Ceramic, Plastic,
    Gravel, Sand, Dirt, Grass, Snow, Carpet, Cloth, Water, Foliage,
}

/// <summary>A sound playing (Audio.Play): stop it, change its level, pitch or place, or ask whether
/// it still plays. Default: none (every call does nothing).</summary>
public readonly unsafe record struct SoundHandle(uint Id)
{
    public bool IsValid => Id != 0;
    public bool IsPlaying => Id != 0 && Api.Native->audioPlaying(Api.Ctx, Id) != 0;

    public void Stop(float fadeSeconds = 0.05f) => Api.Native->audioStop(Api.Ctx, Id, fadeSeconds);

    /// <summary>Its volume (multiplying the sound's own).</summary>
    public void SetVolume(float volume) => Api.Native->audioSet(Api.Ctx, Id, MathF.Max(volume, 0f), 0f, null);

    /// <summary>Its pitch (2: an octave up).</summary>
    public void SetPitch(float pitch) => Api.Native->audioSet(Api.Ctx, Id, -1f, MathF.Max(pitch, 0.01f), null);

    /// <summary>Moves it (one following an entity stops following it).</summary>
    public void SetPosition(Vector3 position) => Api.Native->audioSet(Api.Ctx, Id, -1f, 0f, (float*)&position);
}

/// <summary>
/// Sounds by project path: a .fsound (its clips, variation, distances, bus) or a .wav / .ogg clip
/// (placed in the world when given a place, with the defaults; else 2D). Volume and pitch multiply
/// the sound's. Everything a game plays stops when it stops; nothing plays with --no-audio.
/// </summary>
public static unsafe class Audio
{
    [ThreadStatic] static Dictionary<string, uint>? ids;

    /// <summary>Plays a sound at a point.</summary>
    public static SoundHandle Play(string sound, Vector3 position, float volume = 1f, float pitch = 1f, bool loop = false) =>
        new(Api.Native->audioPlay(Api.Ctx, Id(sound), (float*)&position, 0, volume, pitch, loop ? 1u : 0u));

    /// <summary>Plays a sound as recorded, not placed anywhere (UI, music).</summary>
    public static SoundHandle Play2D(string sound, float volume = 1f, float pitch = 1f, bool loop = false) =>
        new(Api.Native->audioPlay(Api.Ctx, Id(sound), null, 0, volume, pitch, loop ? 1u : 0u));

    /// <summary>Plays a sound on an entity: it follows it while it plays.</summary>
    public static SoundHandle PlayOn(Entity entity, string sound, float volume = 1f, float pitch = 1f, bool loop = false) =>
        new(Api.Native->audioPlay(Api.Ctx, Id(sound), null, entity.Id, volume, pitch, loop ? 1u : 0u));

    /// <summary>A surface's impact as physics plays it (synthesized from its character): volume
    /// like a hit's loudness (0 .. 1.3), pitch lower for heavier things. Footsteps without sounds of
    /// their own, say.</summary>
    public static SoundHandle PlayImpact(Surface surface, Vector3 position, float volume = 1f, float pitch = 1f) =>
        new(Api.Native->audioImpact(Api.Ctx, (uint)surface, (float*)&position, volume, pitch));

    /// <summary>A bus's volume from the game, multiplying the project's (back to 1 when it stops).</summary>
    public static void SetBusVolume(Bus bus, float volume) => Api.Native->audioBusVolume(Api.Ctx, (uint)bus, volume);

    private static uint Id(string path)
    {
        ids ??= new Dictionary<string, uint>();
        if (ids.TryGetValue(path, out uint id))
            return id;
        Span<byte> utf8 = stackalloc byte[1024];
        int n = System.Text.Encoding.UTF8.GetBytes(path, utf8[..1023]);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            id = Api.Native->audioSound(Api.Ctx, p);
        ids[path] = id;
        return id;
    }
}
