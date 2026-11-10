using System;
using System.Collections.Generic;
using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using Falcon.Native;

namespace Falcon;

[Flags]
public enum ParticleFlags : uint
{
    None = 0,
    Delayed = 1,  // born, not shown yet (its start delay)
    Dead = 2,     // removed after this step (set it to kill a particle)
    Hit = 4,      // hit something this step
    New = 8,      // became live this step
}

/// <summary>One particle (128 bytes, as the engine keeps it: vfx/particle_system.h). Controllers
/// change it in place.</summary>
[StructLayout(LayoutKind.Sequential, Size = 128)]
public struct Particle
{
    public Vector3 Position;
    public float Age;          // seconds lived
    public Vector3 Velocity;
    public float Lifetime;     // seconds it lives
    public Vector4 Color;      // linear rgb, alpha in w
    public Vector3 Size;       // x, y (the sprite's), z: its start size
    public float Sequence;     // a sheet's position
    public Vector3 Angles;     // pitch, yaw, roll (degrees)
    public float TimeScale;
    public Vector3 StartAngles;
    public float Delay;        // seconds before it shows
    public uint Seed;
    public ParticleFlags Flags;
    public float SinceHit;     // seconds since it last hit something
    public uint Hits;
    public Vector4 User;       // the game's own

    /// <summary>How far through its life (0 born .. 1 dead).</summary>
    public readonly float Life => Lifetime > 0f ? Math.Clamp(Age / Lifetime, 0f, 1f) : 1f;
    public void Kill() => Flags |= ParticleFlags.Dead;
}

/// <summary>A particle's hit this frame: where, how fast into the surface, its normal, which
/// particle (its index in this frame's particles), and what it hit (default: the scene's geometry or
/// the ground; Surface: the ground's terrain layer + 1, else 0).</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly struct ParticleHit
{
    public readonly Vector3 Point;
    public readonly float Speed;
    public readonly Vector3 Normal;
    public readonly uint Particle;
    public readonly Entity Entity;
    public readonly uint Surface;
    private readonly uint _pad;
}

/// <summary>
/// A particle controller (s&amp;box's ParticleController): changes an effect's particles each frame
/// after the engine's step, while the game runs. Register its class from the game's
/// [ModuleInitializer] (<c>Particles.Register&lt;Swirl&gt;()</c>), then name it in a Particle Script
/// component (on the effect or below it), or attach one in code (<c>Particles.Attach</c>).
/// One instance per Particle Script.
/// </summary>
public abstract class ParticleController
{
    /// <summary>The entity with the Particle Script.</summary>
    public Entity Entity { get; internal set; }

    /// <summary>All of a frame's particles at once. By default: OnParticleCreated for new ones,
    /// OnParticleStep for each live one, OnParticleDestroyed for those dying, then OnCollision for
    /// each hit.</summary>
    public virtual void OnStep(Span<Particle> particles, ReadOnlySpan<ParticleHit> hits, float dt)
    {
        for (int i = 0; i < particles.Length; ++i)
        {
            ref Particle p = ref particles[i];
            if ((p.Flags & ParticleFlags.Delayed) != 0)
                continue;
            if ((p.Flags & ParticleFlags.Dead) != 0)
            {
                OnParticleDestroyed(in p);
                continue;
            }
            if ((p.Flags & ParticleFlags.New) != 0)
                OnParticleCreated(ref p);
            OnParticleStep(ref p, dt);
        }
        foreach (ref readonly ParticleHit hit in hits)
            if (hit.Particle < (uint)particles.Length)
                OnCollision(ref particles[(int)hit.Particle], in hit);
    }

    public virtual void OnParticleStep(ref Particle particle, float dt) { }
    public virtual void OnParticleCreated(ref Particle particle) { }
    public virtual void OnParticleDestroyed(in Particle particle) { }
    public virtual void OnCollision(ref Particle particle, in ParticleHit hit) { }
}

/// <summary>Particle effects (an entity with a Particle Effect, or any entity below it).</summary>
public static unsafe partial class Particles
{
    private static readonly Dictionary<string, Func<ParticleController>> s_types = new();
    private static readonly Dictionary<ulong, ParticleController> s_controllers = new();
    private static readonly Dictionary<ulong, ParticleController> s_attached = new();

    /// <summary>A controller class Particle Scripts can name (by its class name).</summary>
    public static void Register<T>() where T : ParticleController, new() => s_types[typeof(T).Name] = static () => new T();

    /// <summary>Runs <paramref name="controller"/> on the effect <paramref name="entity"/> works
    /// for (a Particle Script is put on the entity).</summary>
    public static void Attach(Entity entity, ParticleController controller)
    {
        controller.Entity = entity;
        s_attached[entity.Id] = controller;
        s_controllers.Remove(entity.Id);
        Span<byte> name = stackalloc byte[32];
        int n = Encoding.UTF8.GetBytes("(attached)", name);
        name[n] = 0;
        fixed (byte* p = name)
            Api.Native->particlesScript(Api.Ctx, entity.Id, p);
    }

    public static bool Play(Entity effect) => Api.Native->particlesControl(Api.Ctx, effect.Id, 0) != 0;
    /// <summary>Stops emitting; <paramref name="clear"/>: its particles go too.</summary>
    public static bool Stop(Entity effect, bool clear = false) => Api.Native->particlesControl(Api.Ctx, effect.Id, clear ? 2 : 1) != 0;
    /// <summary>From its start (a muzzle flash: once per shot).</summary>
    public static bool Restart(Entity effect) => Api.Native->particlesControl(Api.Ctx, effect.Id, 3) != 0;
    /// <summary>Starts <paramref name="count"/> particles now, from its first emitter's shape.</summary>
    public static int Emit(Entity effect, int count) => Api.Native->particlesEmit(Api.Ctx, effect.Id, count);
    /// <summary>How many particles it shows (-1: no effect there).</summary>
    public static int Count(Entity effect) => Api.Native->particlesCount(Api.Ctx, effect.Id);

    /// <summary>This frame's hits of its particles (Collision on): copies what fits, returns how many there are.</summary>
    public static int Hits(Entity effect, Span<ParticleHit> hits)
    {
        fixed (ParticleHit* p = hits)
            return Api.Native->particlesHits(Api.Ctx, effect.Id, p, hits.Length);
    }

    internal static void Step(ulong script, byte* className, Particle* particles, int count, ParticleHit* hits, int hitCount, float dt)
    {
        if (!s_attached.TryGetValue(script, out ParticleController? controller) && !s_controllers.TryGetValue(script, out controller))
        {
            string name = Marshal.PtrToStringUTF8((nint)className) ?? "";
            if (!s_types.TryGetValue(name, out Func<ParticleController>? make))
            {
                s_controllers[script] = new Missing(name);
                return;
            }
            controller = make();
            controller.Entity = new Entity(script);
            s_controllers[script] = controller;
        }
        if (controller is Missing)
            return;
        controller.OnStep(new Span<Particle>(particles, count), new ReadOnlySpan<ParticleHit>(hits, hitCount), dt);
    }

    // The game stopped: its controllers go (a new run makes new ones).
    internal static void ForgetControllers()
    {
        s_controllers.Clear();
        s_attached.Clear();
    }

    // The game's code is being replaced: its classes go too.
    internal static void ForgetTypes()
    {
        ForgetControllers();
        s_types.Clear();
    }

    private sealed class Missing : ParticleController
    {
        public Missing(string name) => Log.Warning($"Particle Script: no controller class '{name}' is registered (Particles.Register<T>())");
    }
}

/// <summary>Prefabs made while the game runs.</summary>
public static unsafe class Prefab
{
    /// <summary>The prefab (.fprefab, by project path) made at <paramref name="position"/>, turned by
    /// <paramref name="rotation"/>, under <paramref name="parent"/> (default: a root): its empties,
    /// lights, cameras, audio, particle effects and decals (not meshes yet). Its root, or an invalid
    /// entity. The editor's Stop deletes what the game made.</summary>
    public static Entity Spawn(string path, Vector3 position, Quaternion rotation, Entity parent = default)
    {
        Span<byte> utf8 = stackalloc byte[Encoding.UTF8.GetMaxByteCount(path.Length) + 1];
        int n = Encoding.UTF8.GetBytes(path, utf8);
        utf8[n] = 0;
        fixed (byte* p = utf8)
            return new Entity(Api.Native->prefabSpawn(Api.Ctx, p, (float*)&position, (float*)&rotation, parent.Id));
    }

    public static Entity Spawn(string path, Vector3 position) => Spawn(path, position, Quaternion.Identity);

    /// <summary>A rotation turning +Y to <paramref name="up"/> (a surface's normal), then
    /// <paramref name="yaw"/> degrees about it: an impact's prefab placed on what it hit.</summary>
    public static Quaternion Facing(Vector3 up, float yaw = 0f)
    {
        up = Vector3.Normalize(up);
        Vector3 from = Vector3.UnitY;
        float d = Vector3.Dot(from, up);
        Quaternion q = d > 0.9999f ? Quaternion.Identity
            : d < -0.9999f ? Quaternion.CreateFromAxisAngle(Vector3.UnitX, MathF.PI)
            : Quaternion.Normalize(new Quaternion(Vector3.Cross(from, up), 1f + d));
        return yaw == 0f ? q : Quaternion.CreateFromAxisAngle(up, yaw * (MathF.PI / 180f)) * q;
    }
}

/// <summary>Decals placed while the game runs (bullet holes): a material projected onto what is
/// under it.</summary>
public static unsafe class Decals
{
    /// <summary>A decal of <paramref name="material"/> (.fmat) at <paramref name="point"/> on a surface
    /// facing <paramref name="normal"/>, <paramref name="size"/> metres across, turned
    /// <paramref name="angle"/> degrees (NaN: at random), gone after <paramref name="lifetime"/>
    /// seconds (0: until cleared), moving with <paramref name="attach"/>. The oldest go when there
    /// are too many.</summary>
    public static bool Place(string material, Vector3 point, Vector3 normal, float size, float angle = float.NaN, float lifetime = 30f,
                             Entity attach = default, float depth = 0f)
    {
        Span<byte> utf8 = stackalloc byte[Encoding.UTF8.GetMaxByteCount(material.Length) + 1];
        int n = Encoding.UTF8.GetBytes(material, utf8);
        utf8[n] = 0;
        if (float.IsNaN(angle))
            angle = Random.Shared.NextSingle() * 360f;
        if (depth <= 0f)
            depth = size * 0.5f;
        fixed (byte* p = utf8)
            return Api.Native->decalPlace(Api.Ctx, p, (float*)&point, (float*)&normal, size, depth, angle, lifetime, attach.Id) != 0;
    }

    /// <summary>Every decal placed so far goes.</summary>
    public static void Clear() => Api.Native->decalsClear(Api.Ctx);
}
