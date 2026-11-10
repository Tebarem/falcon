using System;
using System.Diagnostics;
using System.Numerics;
using System.Runtime.CompilerServices;
using Falcon;

namespace Bench;

// The scripting API's speed test (CTest script_bench_jit / script_bench_aot, on the fox crowd):
//   - what each call costs, in ns: timed in tight loops twice (frames 300 and 600, warm), the
//     faster of the two against a ceiling well above what it costs (a regression back to a lookup
//     per call, or a transition per call, trips it; a slow machine does not);
//   - that a steady frame (queries writing components, characters' parameters and events, input,
//     the camera) allocates nothing on the managed heap, frames 700 to 1000.
// Prints "bench: PASS" or "bench: FAIL ..." (the test matches it) and the numbers.

[Component("Bench.Mover")]
public struct Mover
{
    public Vector3 Rest;
    public float Phase;
}

[Component("Bench.Pos")]
public struct Pos
{
    public Vector3 Value;
}

public sealed class BenchGame : Game
{
    [ModuleInitializer]
    internal static void Register() => Register<BenchGame>();

    private const int Movers = 256;
    private Query? _movers;
    private Entity _mesh, _characterEntity;
    private Character _character;
    private int _speed = -1;
    private long _allocStart;
    private int _sink;
    private readonly (string name, double limit)[] _items =
    {
        ("Input.IsDown", 20), ("Ecs.IsAlive", 80), ("Ecs.Has", 80), ("Ecs.Get", 80), ("Ecs.Set", 150),
        ("Camera.Position", 30), ("Animation.Parameter", 300), ("Animation.SetFloat(entity)", 120),
        ("Animation.SetFloat(handle)", 40), ("Animation.Events(handle)", 40), ("Ui.TryReceive", 40),
        ("query pass (256 movers, write)", 4000), ("Physics.Raycast", 50000),
        ("Ref.Get", 40), ("Entity.Parent", 40), ("Renderer.IsVisible", 40),
    };
    private readonly double[] _best = new double[16];

    public override void OnStart()
    {
        Array.Fill(_best, double.MaxValue);
        _movers = new Query(Query.InOut<Pos>(), Query.In<Mover>());
        if (_movers.Count() == 0)
            for (int i = 0; i < Movers; ++i)
            {
                Entity e = Ecs.Create();
                Ecs.Set(e, new Mover { Rest = new Vector3(i, 0, 0), Phase = i * 0.1f });
                Ecs.Set(e, new Pos());
            }
        using var meshes = new Query(Query.In<MeshInstance>());
        foreach (Chunk c in meshes)
            foreach (Entity e in c.Entities)
            {
                if (!_mesh.IsValid)
                    _mesh = e;
                Character ch = Character.Of(e);
                if (!_character.IsValid && ch.IsValid && Animation.Parameter(ch, "Speed") >= 0)
                {
                    _character = ch;
                    _characterEntity = e;
                    _speed = Animation.Parameter(ch, "Speed");
                }
            }
        Log.Info(_character.IsValid ? "bench: timing a character's calls too" : "bench: no character with a Speed parameter: its calls untimed");
    }

    public override void OnUpdate(float dt)
    {
        ulong frame = Time.Frame;
        Steady((float)Time.Total);
        if (frame == 300 || frame == 600)
            TimeCalls();
        if (frame == 700)
            _allocStart = Performance.ManagedAllocatedBytes;
        if (frame == 1000)
            Report(Performance.ManagedAllocatedBytes - _allocStart);
    }

    // A frame's typical work: everything here must allocate nothing.
    private void Steady(float t)
    {
        foreach (Chunk c in _movers!)
        {
            Span<Pos> pos = c.Write<Pos>(0);
            ReadOnlySpan<Mover> mover = c.Read<Mover>(1);
            for (int i = 0; i < c.Count; ++i)
                pos[i].Value = mover[i].Rest + new Vector3(0, MathF.Sin(t + mover[i].Phase), 0);
        }
        if (_character.IsValid)
        {
            Animation.SetFloat(_character, _speed, 2.5f + 2f * MathF.Sin(t));
            Span<AnimEvent> events = stackalloc AnimEvent[16];
            _sink += Animation.Events(_character, events);
        }
        _sink += Input.IsDown(Key.W) ? 1 : 0;
        _sink += (int)Camera.Position.X;
        _sink += Ui.TryReceive(out string _) ? 1 : 0;
        if (_mesh.IsValid)
            _sink += (int)Ecs.Get<WorldTransform>(_mesh).Position.X;
    }

    private static double Ns(long t0, long t1, int n) => (t1 - t0) * 1e9 / Stopwatch.Frequency / n;

    private void Keep(int index, double ns) => _best[index] = Math.Min(_best[index], ns);

    private void TimeCalls()
    {
        const int N = 20000;
        int sink = 0;
        long a, b;
        Entity mover = default;
        foreach (Chunk c in _movers!)
        {
            mover = c.Entities[0];
            break;
        }

        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < N; ++i) sink += Input.IsDown(Key.W) ? 1 : 0;
        b = Stopwatch.GetTimestamp(); Keep(0, Ns(a, b, N));
        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < N; ++i) sink += Ecs.IsAlive(mover) ? 1 : 0;
        b = Stopwatch.GetTimestamp(); Keep(1, Ns(a, b, N));
        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < N; ++i) sink += Ecs.Has<Mover>(mover) ? 1 : 0;
        b = Stopwatch.GetTimestamp(); Keep(2, Ns(a, b, N));
        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < N; ++i) sink += (int)Ecs.Get<Pos>(mover).Value.X;
        b = Stopwatch.GetTimestamp(); Keep(3, Ns(a, b, N));
        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < N; ++i) Ecs.Set(mover, new Pos { Value = new Vector3(i) });
        b = Stopwatch.GetTimestamp(); Keep(4, Ns(a, b, N));
        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < N; ++i) sink += (int)Camera.Position.X;
        b = Stopwatch.GetTimestamp(); Keep(5, Ns(a, b, N));
        if (_character.IsValid)
        {
            a = Stopwatch.GetTimestamp();
            for (int i = 0; i < N; ++i) sink += Animation.Parameter(_characterEntity, "Speed");
            b = Stopwatch.GetTimestamp(); Keep(6, Ns(a, b, N));
            a = Stopwatch.GetTimestamp();
            for (int i = 0; i < N; ++i) Animation.SetFloat(_characterEntity, _speed, 1f);
            b = Stopwatch.GetTimestamp(); Keep(7, Ns(a, b, N));
            a = Stopwatch.GetTimestamp();
            for (int i = 0; i < N; ++i) Animation.SetFloat(_character, _speed, 1f);
            b = Stopwatch.GetTimestamp(); Keep(8, Ns(a, b, N));
            Span<AnimEvent> events = stackalloc AnimEvent[16];
            a = Stopwatch.GetTimestamp();
            for (int i = 0; i < N; ++i) sink += Animation.Events(_character, events);
            b = Stopwatch.GetTimestamp(); Keep(9, Ns(a, b, N));
        }
        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < N; ++i) sink += Ui.TryReceive(out string _) ? 1 : 0;
        b = Stopwatch.GetTimestamp(); Keep(10, Ns(a, b, N));
        const int Q = 2000;
        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < Q; ++i)
            foreach (Chunk c in _movers)
            {
                Span<Pos> pos = c.Write<Pos>(0);
                ReadOnlySpan<Mover> m = c.Read<Mover>(1);
                for (int k = 0; k < c.Count; ++k)
                    pos[k].Value = m[k].Rest;
            }
        b = Stopwatch.GetTimestamp(); Keep(11, Ns(a, b, Q));
        Vector3 from = Camera.Position, down = new(0, -1, 0);
        const int R = 2000;
        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < R; ++i) sink += Physics.Raycast(from, down, out _, 50f) ? 1 : 0;
        b = Stopwatch.GetTimestamp(); Keep(12, Ns(a, b, R));
        var reference = new Ref<Pos>(mover);
        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < N; ++i) sink += (int)reference.Get().Value.X;
        b = Stopwatch.GetTimestamp(); Keep(13, Ns(a, b, N));
        a = Stopwatch.GetTimestamp();
        for (int i = 0; i < N; ++i) sink += (int)mover.Parent.Id;
        b = Stopwatch.GetTimestamp(); Keep(14, Ns(a, b, N));
        if (_mesh.IsValid)
        {
            a = Stopwatch.GetTimestamp();
            for (int i = 0; i < N; ++i) sink += Renderer.IsVisible(_mesh) ? 1 : 0;
            b = Stopwatch.GetTimestamp(); Keep(15, Ns(a, b, N));
        }
        _sink += sink;
    }

    private void Report(long allocated)
    {
        var line = new System.Text.StringBuilder("bench ns/call:");
        string failed = "";
        for (int i = 0; i < _items.Length; ++i)
        {
            if (_best[i] == double.MaxValue)
                continue;  // (untimed: no character)
            line.Append($" {_items[i].name}={_best[i]:F1}");
            if (_best[i] > _items[i].limit)
                failed += $" {_items[i].name} {_best[i]:F1} ns > {_items[i].limit} ns;";
        }
        Log.Info(line.ToString());
        Log.Info($"bench: a steady frame allocated {allocated} bytes over 300 frames");
        if (allocated != 0)
            failed += $" {allocated} bytes allocated in steady frames;";
        Log.Info(failed.Length == 0 ? "bench: PASS" : $"bench: FAIL{failed}");
    }
}
