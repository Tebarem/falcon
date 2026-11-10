using System;
using System.Numerics;
using System.Text.Json;
using Falcon.Native;

namespace Falcon.Level;

/// <summary>
/// Walks its entity over the scene's navigation mesh (Navigation/Bake) to a destination, along the
/// path and round the other agents (Detour's crowd), its feet on the mesh, turned the way it goes.
/// A Destination set in the Inspector is where it heads when the game starts; scripts call
/// SetDestination / Stop. While the game runs only.
/// </summary>
public sealed unsafe class NavAgent : Component
{
    public float Speed = 3.5f;          // metres a second, at most
    public float Acceleration = 10f;    // m/s²
    public float Radius = 0.4f;         // how much room it takes (and keeps from the others)
    public float Height = 1.8f;
    public Vector3 Destination;         // (the Inspector's: where it goes at the start; zero: nowhere)
    public bool TurnToMove = true;      // faces the way it goes

    private int _id = -1;
    private bool _going;
    private Vector3 _target;
    private static ulong s_stepped = ulong.MaxValue;  // (the frame the crowd last moved on)

    /// <summary>Its velocity now (m/s).</summary>
    public Vector3 Velocity { get; private set; }
    /// <summary>How far it still has to go along its path's end (0: there, or going nowhere).</summary>
    public float Remaining { get; private set; }
    /// <summary>It has a destination and is there.</summary>
    public bool Arrived => _going && Remaining < MathF.Max(Radius, 0.2f);

    /// <summary>Walks to `to` (the nearest point on the mesh); false when there is none near.</summary>
    public bool SetDestination(Vector3 to)
    {
        _target = to;
        _going = true;
        FxLevelApi* l = LevelNative.Api;
        if (l == null || l->crowdTarget == null || !Join())
            return false;
        return l->crowdTarget(l->context, _id, (float*)&to) == 1;
    }

    /// <summary>Stops where it is.</summary>
    public void Stop()
    {
        _going = false;
        FxLevelApi* l = LevelNative.Api;
        if (l != null && l->crowdTarget != null && _id >= 0)
            l->crowdTarget(l->context, _id, null);
    }

    public override void OnStart()
    {
        if (Destination != Vector3.Zero)
            SetDestination(Destination);
    }

    public override void OnUpdate(float dt)
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null || l->crowdUpdate == null || !Join())
            return;
        if (s_stepped != Time.Frame)  // (the whole crowd once a frame, by whichever agent comes first)
        {
            s_stepped = Time.Frame;
            l->crowdUpdate(l->context, dt);
        }
        Vector3 at, velocity;
        float left = l->crowdState(l->context, _id, (float*)&at, (float*)&velocity);
        if (left < 0f)  // (a new navigation mesh: its crowd again)
        {
            _id = -1;
            return;
        }
        Remaining = left;
        Velocity = velocity;
        Entity.Position = at;
        var flat = new Vector3(velocity.X, 0f, velocity.Z);
        if (TurnToMove && flat.LengthSquared() > 0.01f)
            Entity.Rotation = Quaternion.CreateFromAxisAngle(Vector3.UnitY, MathF.Atan2(flat.X, flat.Z));
    }

    public override void OnDestroy()
    {
        FxLevelApi* l = LevelNative.Api;
        if (l != null && l->crowdRemove != null && _id >= 0)
            l->crowdRemove(l->context, _id);
        _id = -1;
    }

    // In the crowd (again, with its destination, after a new navigation mesh).
    private bool Join()
    {
        FxLevelApi* l = LevelNative.Api;
        if (l == null || l->crowdAdd == null)
            return false;
        if (_id >= 0 && l->crowdState(l->context, _id, null, null) >= 0f)
            return true;
        Vector3 at = Entity.Position;
        _id = l->crowdAdd(l->context, (float*)&at, Radius, Height, Speed, Acceleration);
        if (_id < 0)
            return false;
        if (_going)
        {
            Vector3 to = _target;
            l->crowdTarget(l->context, _id, (float*)&to);
        }
        return true;
    }

    internal static ComponentType Type_ => new("Falcon.Level.NavAgent", "Nav Agent", static () => new NavAgent(),
        new PropertyInfo[]
        {
            new("Speed", "float", 0f, float.NaN, null, null, "Metres a second, at most", "Motion"),
            new("Acceleration", "float", 0f, float.NaN, null, null, "m/s²", "Motion"),
            new("Radius", "float", 0.05f, 2f, null, null, "How much room it takes", "Size"),
            new("Height", "float", 0.1f, float.NaN, null, null, null, "Size"),
            new("Destination", "vec3", float.NaN, float.NaN, null, null, "Where it heads when the game starts (zero: nowhere)", "Goal"),
            new("TurnToMove", "bool", float.NaN, float.NaN, null, null, "Faces the way it goes", "Motion"),
        },
        static (c, w) =>
        {
            var x = (NavAgent)c;
            w.WriteNumber("Speed", x.Speed);
            w.WriteNumber("Acceleration", x.Acceleration);
            w.WriteNumber("Radius", x.Radius);
            w.WriteNumber("Height", x.Height);
            ComponentJson.Write(w, "Destination", x.Destination);
            w.WriteBoolean("TurnToMove", x.TurnToMove);
        },
        static (c, o) =>
        {
            var x = (NavAgent)c;
            JsonElement v;
            if (o.TryGetProperty("Speed", out v)) x.Speed = (float)ComponentJson.Number(v, x.Speed);
            if (o.TryGetProperty("Acceleration", out v)) x.Acceleration = (float)ComponentJson.Number(v, x.Acceleration);
            if (o.TryGetProperty("Radius", out v)) x.Radius = (float)ComponentJson.Number(v, x.Radius);
            if (o.TryGetProperty("Height", out v)) x.Height = (float)ComponentJson.Number(v, x.Height);
            if (o.TryGetProperty("Destination", out v)) x.Destination = ComponentJson.Vector3(v, x.Destination);
            if (o.TryGetProperty("TurnToMove", out v)) x.TurnToMove = v.ValueKind == JsonValueKind.True || (v.ValueKind != JsonValueKind.False && x.TurnToMove);
        });
}
