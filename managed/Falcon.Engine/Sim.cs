using System;
using System.Numerics;

namespace Falcon;

/// <summary>
/// The deterministic simulation. Everything a replay or the network must reproduce exactly runs
/// in ticks (OnFixedUpdate, collision callbacks: <see cref="Time.InTick"/>), reads
/// <see cref="Time.Tick"/>, uses <see cref="DMath"/> for transcendental math and
/// <see cref="Random"/> for randomness.
/// </summary>
public static class Sim
{
    /// <summary>The seed every tick's random numbers derive from (a match's, a level's).</summary>
    public static ulong Seed { get; set; }

    /// <summary>Random numbers for the simulation. Inside a tick they depend only on
    /// <see cref="Seed"/>, the tick and how many were drawn before in it: every replay and every
    /// peer draws the same ones, and a tick run again (a rollback) draws them again. Outside ticks
    /// (OnUpdate: effects) they follow the frame.</summary>
    public static SimRandom Random { get; } = new();

    // The tick's numbers start over: OnFixedUpdate's (stream 0), then its collision callbacks' (1).
    internal static void StartTick(ulong tick, int stream) => Random.Start((tick << 2) | (uint)stream);
}

/// <summary>Counter-based random numbers (SplitMix64 of the seed, the tick and a counter): no
/// state to save but where in the tick it is.</summary>
public sealed class SimRandom
{
    private ulong _stream = ulong.MaxValue;
    private ulong _counter;
    private ulong _frame = ulong.MaxValue;

    internal void Start(ulong stream)
    {
        _stream = stream;
        _counter = 0;
    }

    /// <summary>The next 64 random bits.</summary>
    public ulong NextBits()
    {
        if (!Time.InTick && _frame != Time.Frame)
        {
            _frame = Time.Frame;
            Start((1ul << 63) | Time.Frame);
        }
        ulong z = Sim.Seed + _stream * 0x9E3779B97F4A7C15ul + ++_counter * 0xD1B54A32D192ED03ul;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ul;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBul;
        return z ^ (z >> 31);
    }

    /// <summary>0 (inclusive) to 1 (exclusive).</summary>
    public float Float() => (NextBits() >> 40) * (1f / (1 << 24));

    /// <summary>min (inclusive) to max (exclusive).</summary>
    public float Range(float min, float max) => min + (max - min) * Float();

    /// <summary>min (inclusive) to max (exclusive).</summary>
    public int Range(int min, int max) => max <= min ? min : min + (int)((NextBits() >> 32) % (ulong)(max - min));

    /// <summary>True with probability p.</summary>
    public bool Chance(float p) => Float() < p;

    /// <summary>A point inside the unit sphere (uniform).</summary>
    public Vector3 InsideUnitSphere()
    {
        while (true)
        {
            var v = new Vector3(Range(-1f, 1f), Range(-1f, 1f), Range(-1f, 1f));
            if (v.LengthSquared() <= 1f)
                return v;
        }
    }

    /// <summary>A point inside the unit circle (uniform).</summary>
    public Vector2 InsideUnitCircle()
    {
        while (true)
        {
            var v = new Vector2(Range(-1f, 1f), Range(-1f, 1f));
            if (v.LengthSquared() <= 1f)
                return v;
        }
    }
}
