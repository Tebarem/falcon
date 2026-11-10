using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;

namespace Falcon;

/// <summary>
/// An entity of the networked simulation. Its transform (unless <see cref="SyncTransform"/> is
/// off) and its components' [Sync] members are its state: saved every tick (a rollback restores
/// them), sent by the host to the other machines, hashed by the determinism checks. Each has a
/// <see cref="NetId"/> that names it on every machine. Add it in the editor (Add Component >
/// Network Object) or in code; destroy one with <see cref="Network.Destroy"/>.
/// </summary>
public sealed class NetworkObject : Component
{
    /// <summary>Its transform is part of its state (off: something else places it, e.g. it hangs
    /// under a bone).</summary>
    [Property(Tooltip = "Its transform is part of the networked state")]
    public bool SyncTransform = true;

    /// <summary>Its name on every machine (0 until it wakes). Given in the order objects wake: the
    /// scene's in scene order, then in the order the simulation spawns them.</summary>
    public ulong NetId { get; internal set; }

    /// <summary>Destroyed by <see cref="Network.Destroy"/> while a rollback could still need it: it
    /// sleeps (disabled) until the tick it went is older than the rollback window.</summary>
    public bool Dormant { get; internal set; }
    internal ulong DespawnTick;

    /// <summary>How far from a client's own characters it is sent to that client (0: to every
    /// client). Beyond it (and a fifth more, so it doesn't flicker at the edge) it is despawned there.</summary>
    [Property(Tooltip = "Sent to clients whose characters are within this distance (0: to all)")]
    public float RelevanceRadius;

    /// <summary>The connection that owns it (-1: the host's alone). Set by the host.</summary>
    public int Owner { get; set; } = -1;

    /// <summary>On a client, an object the host replicates (its components don't run
    /// OnFixedUpdate there; its state comes from the host).</summary>
    public bool IsProxy => Network.IsProxy(this);

    /// <summary>The prefab it was spawned from (Network.Spawn; clients spawn the same one).</summary>
    public string? Prefab { get; internal set; }

    public override void OnAwake() => Network.Register(this);
    public override void OnDestroy() => Network.Unregister(this);

    internal static ComponentType Type_ => new("Falcon.NetworkObject", "Network Object", static () => new NetworkObject(),
        new PropertyInfo[]
        {
            new("SyncTransform", "bool", float.NaN, float.NaN, null, null, "Its transform is part of the networked state", null),
            new("RelevanceRadius", "float", 0f, float.NaN, null, null, "Sent to clients whose characters are within this distance (0: to all)", null),
        },
        static (c, w) =>
        {
            w.WriteBoolean("SyncTransform", ((NetworkObject)c).SyncTransform);
            w.WriteNumber("RelevanceRadius", ((NetworkObject)c).RelevanceRadius);
        },
        static (c, o) =>
        {
            if (o.TryGetProperty("SyncTransform", out JsonElement v))
                ((NetworkObject)c).SyncTransform = v.ValueKind != JsonValueKind.False;
            if (o.TryGetProperty("RelevanceRadius", out v))
                ((NetworkObject)c).RelevanceRadius = (float)ComponentJson.Number(v, 0);
        });
}

/// <summary>
/// A character moved in the simulation's ticks: it accelerates toward the velocity it is asked
/// for, falls, jumps, slides along walls, climbs steps and keeps to slopes (Box3D's mover, a
/// deterministic query against the world). Its velocity and whether it stands are [Sync] state, so
/// a rollback restores them and the same inputs move it the same way on every machine. Call
/// <see cref="Move"/> once per tick (OnFixedUpdate / OnSimulate). Its entity's position is the
/// capsule's base.
/// </summary>
public sealed class CharacterMover : Component
{
    [Property(Group = "Capsule")] public float Radius = 0.35f;
    [Property(Group = "Capsule")] public float Height = 1.8f;
    [Property(Group = "Capsule", Tooltip = "Steps up to this high are climbed")] public float StepHeight = 0.35f;
    [Property(Group = "Capsule", Tooltip = "Steeper ground is a wall (degrees)")] public float SlopeLimit = 50f;
    [Property(Group = "Motion", Tooltip = "m/s² on the ground")] public float Acceleration = 50f;
    [Property(Group = "Motion", Tooltip = "Share of the acceleration in the air")] public float AirControl = 0.3f;
    [Property(Group = "Motion", Tooltip = "m/s²")] public float Gravity = 20f;
    [Property(Group = "Motion", Tooltip = "m/s upward")] public float JumpSpeed = 6.5f;

    /// <summary>Its velocity (m/s). [Sync].</summary>
    [Sync] public Vector3 Velocity;

    /// <summary>Standing on walkable ground after the last move. [Sync].</summary>
    [Sync] public bool Grounded;

    /// <summary>One tick's move: toward <paramref name="wishVelocity"/> (its vertical part is
    /// ignored), a jump if <paramref name="jump"/> and it stands. Returns the query's outcome.</summary>
    public CharacterMove Move(Vector3 wishVelocity, bool jump)
    {
        float dt = Time.TickDelta;
        var horizontal = new Vector3(Velocity.X, 0f, Velocity.Z);
        var wish = new Vector3(wishVelocity.X, 0f, wishVelocity.Z);
        Vector3 change = wish - horizontal;
        float most = (Grounded ? Acceleration : Acceleration * AirControl) * dt;
        float length = change.Length();
        if (length > most)
            change *= most / length;
        horizontal += change;
        float vertical = Velocity.Y;
        if (Grounded)
            vertical = jump ? JumpSpeed : 0f;
        vertical -= Gravity * dt;
        var controller = new CharacterController { Radius = Radius, Height = Height, StepHeight = StepHeight, SlopeLimit = SlopeLimit };
        Vector3 from = Entity.Position;
        CharacterMove m = controller.Move(Entity, (horizontal + new Vector3(0f, vertical, 0f)) * dt);
        // What it really did: a wall leaves only the slide along it; standing, it doesn't fall; a
        // ceiling stops the rise.
        Vector3 moved = (m.Position - from) / dt;
        Grounded = m.Grounded && vertical <= 0f;
        if (Grounded)
            vertical = 0f;
        else if (vertical > 0f && moved.Y < vertical * 0.5f)
            vertical = 0f;
        Velocity = new Vector3(moved.X, vertical, moved.Z);
        return m;
    }

    internal static ComponentType Type_ => new("Falcon.CharacterMover", "Character Mover", static () => new CharacterMover(),
        new PropertyInfo[]
        {
            new("Radius", "float", float.NaN, float.NaN, null, null, null, "Capsule"),
            new("Height", "float", float.NaN, float.NaN, null, null, null, "Capsule"),
            new("StepHeight", "float", float.NaN, float.NaN, null, null, "Steps up to this high are climbed", "Capsule"),
            new("SlopeLimit", "float", float.NaN, float.NaN, null, null, "Steeper ground is a wall (degrees)", "Capsule"),
            new("Acceleration", "float", float.NaN, float.NaN, null, null, "m/s² on the ground", "Motion"),
            new("AirControl", "float", float.NaN, float.NaN, null, null, "Share of the acceleration in the air", "Motion"),
            new("Gravity", "float", float.NaN, float.NaN, null, null, "m/s²", "Motion"),
            new("JumpSpeed", "float", float.NaN, float.NaN, null, null, "m/s upward", "Motion"),
        },
        static (c, w) =>
        {
            var x = (CharacterMover)c;
            w.WriteNumber("Radius", x.Radius);
            w.WriteNumber("Height", x.Height);
            w.WriteNumber("StepHeight", x.StepHeight);
            w.WriteNumber("SlopeLimit", x.SlopeLimit);
            w.WriteNumber("Acceleration", x.Acceleration);
            w.WriteNumber("AirControl", x.AirControl);
            w.WriteNumber("Gravity", x.Gravity);
            w.WriteNumber("JumpSpeed", x.JumpSpeed);
        },
        static (c, o) =>
        {
            var x = (CharacterMover)c;
            JsonElement v;
            if (o.TryGetProperty("Radius", out v)) x.Radius = (float)ComponentJson.Number(v, x.Radius);
            if (o.TryGetProperty("Height", out v)) x.Height = (float)ComponentJson.Number(v, x.Height);
            if (o.TryGetProperty("StepHeight", out v)) x.StepHeight = (float)ComponentJson.Number(v, x.StepHeight);
            if (o.TryGetProperty("SlopeLimit", out v)) x.SlopeLimit = (float)ComponentJson.Number(v, x.SlopeLimit);
            if (o.TryGetProperty("Acceleration", out v)) x.Acceleration = (float)ComponentJson.Number(v, x.Acceleration);
            if (o.TryGetProperty("AirControl", out v)) x.AirControl = (float)ComponentJson.Number(v, x.AirControl);
            if (o.TryGetProperty("Gravity", out v)) x.Gravity = (float)ComponentJson.Number(v, x.Gravity);
            if (o.TryGetProperty("JumpSpeed", out v)) x.JumpSpeed = (float)ComponentJson.Number(v, x.JumpSpeed);
        })
    {
        SyncWrite = static (c, w) =>
        {
            var x = (CharacterMover)c;
            w.Write(x.Velocity);
            w.Write(x.Grounded);
        },
        SyncRead = static (Component c, ref SyncReader r) =>
        {
            var x = (CharacterMover)c;
            x.Velocity = r.Vector3();
            x.Grounded = r.Bool();
        },
    };
}

// The engine's own script components, registered before (and again after) the game's.
internal static class BuiltinComponents
{
    internal static void Register()
    {
        ComponentTypes.Register(NetworkObject.Type_);
        ComponentTypes.Register(CharacterMover.Type_);
        ComponentTypes.Register(Hitbox.Type_);
        ComponentTypes.Register(Falcon.Level.NavAgent.Type_);
        ComponentTypes.Register(Falcon.Level.NetworkedCuts.Type_);
    }
}
