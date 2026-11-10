using System;
using System.Numerics;
using System.Runtime.InteropServices;
using Falcon.Native;

namespace Falcon;

// (API v19) Physics: bodies' state and pushes, teleports, gravity; queries with layers (rays, shape
// casts, overlaps); triggers; a character controller; joints.

public enum BodyType : uint
{
    Static = 0,
    Kinematic = 1,  // follows its entity (moved by scripts or animation); pushes others, nothing pushes it
    Dynamic = 2,
}

/// <summary>Motions a body is locked out of.</summary>
[Flags]
public enum BodyLocks : uint
{
    None = 0,
    MoveX = 1, MoveY = 2, MoveZ = 4,
    TurnX = 8, TurnY = 16, TurnZ = 32,
    Turning = TurnX | TurnY | TurnZ,
}

/// <summary>A body's state (Physics.Body): velocities, mass, gravity scale, damping, type, locks.</summary>
public struct BodyInfo
{
    public Vector3 Velocity, AngularVelocity;
    public float Mass, GravityScale, LinearDamping, AngularDamping;
    public BodyType Type;
    public BodyLocks Locks;
    public bool Awake, Bullet;
}

/// <summary>What a query hits: Collider layers (everything without a Collider of its own is on
/// layer 0), passing through one entity's shapes (yours).</summary>
public struct QueryFilter
{
    public uint Layers;
    public Entity Ignore;

    public static QueryFilter All => new() { Layers = ~0u };

    /// <summary>Only these layers.</summary>
    public static QueryFilter Only(params int[] layers)
    {
        uint bits = 0;
        foreach (int l in layers)
            bits |= 1u << (l & 31);
        return new QueryFilter { Layers = bits };
    }

    /// <summary>Every layer but these.</summary>
    public static QueryFilter AllBut(params int[] layers) => new() { Layers = ~Only(layers).Layers };

    public QueryFilter Ignoring(Entity entity) => this with { Ignore = entity };

    internal FxQueryFilter Native => new() { layers = Layers, ignore = Ignore.Id };
}

/// <summary>Something that came into a trigger (a Collider marked Is Trigger), or went out.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly struct TriggerEvent
{
    public readonly Entity Trigger;
    /// <summary>What came in or went out (invalid: the scene's ground or a cooked collider).</summary>
    public readonly Entity Other;
    private readonly uint _begin, _pad;

    public bool Entered => _begin != 0;
    public bool Exited => _begin == 0;
}

/// <summary>Where a character's capsule ended up (CharacterController.Move).</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly struct CharacterMove
{
    /// <summary>Its base (the feet).</summary>
    public readonly Vector3 Position;
    private readonly uint _grounded;
    /// <summary>The ground's normal (up when not grounded).</summary>
    public readonly Vector3 GroundNormal;
    private readonly uint _blocked;
    /// <summary>What it stands on (invalid: the scene's ground, or nothing).</summary>
    public readonly Entity Ground;

    /// <summary>Standing on ground no steeper than its slope limit.</summary>
    public bool Grounded => _grounded != 0;
    /// <summary>Something stopped it short of where it was going.</summary>
    public bool Blocked => _blocked != 0;
}

/// <summary>
/// A character's capsule moved through the world (Box3D's mover): it slides along walls, climbs
/// steps up to <see cref="StepHeight"/>, keeps to the ground walking down, and treats ground
/// steeper than <see cref="SlopeLimit"/> as a wall. Kinematic: nothing pushes it; gravity and
/// jumping are the game's (add them to the delta). Its entity's position is the capsule's base.
/// </summary>
public struct CharacterController
{
    public float Radius = 0.3f;
    public float Height = 1.8f;
    public float StepHeight = 0.35f;
    public float SlopeLimit = 50f;  // degrees
    public QueryFilter Filter = QueryFilter.All;

    public CharacterController() { }

    /// <summary>Moves <paramref name="entity"/> by <paramref name="delta"/> (its own colliders passed
    /// through) and returns where it ended up; the entity is moved there.</summary>
    public readonly unsafe CharacterMove Move(Entity entity, Vector3 delta)
    {
        CharacterMove m = Move(entity.Position, delta, entity);
        entity.Position = m.Position;
        return m;
    }

    /// <summary>Where a capsule with its base at <paramref name="position"/> ends up moved by
    /// <paramref name="delta"/> (nothing is moved).</summary>
    public readonly unsafe CharacterMove Move(Vector3 position, Vector3 delta, Entity ignore = default)
    {
        FxQueryFilter f = Filter.Native;
        CharacterMove m;
        Api.Native->characterMove(Api.Ctx, ignore.Id, (float*)&position, Radius, Height, StepHeight, SlopeLimit, (float*)&delta, &f, &m);
        return m;
    }
}

public enum JointType
{
    Fixed = 0,     // welded
    Hinge = 1,     // turns about its axis (a door)
    Ball = 2,      // turns every way about its anchor
    Slider = 3,    // slides along its axis
    Distance = 4,  // keeps the two anchors apart (a rope with Limit, a spring with Spring)
    Wheel = 5,     // spins about its axis, sprung (a car's)
}

/// <summary>How a joint is made (Physics.CreateJoint).</summary>
public struct JointSettings
{
    /// <summary>Where the bodies are joined (world).</summary>
    public Vector3 Anchor;
    /// <summary>A hinge's or wheel's turning axis, a slider's (world).</summary>
    public Vector3 Axis = Vector3.UnitY;
    /// <summary>Distance: the point on the second body (world).</summary>
    public Vector3 AnchorB;
    /// <summary>Limits: a hinge's angles (radians), a slider's travel, a distance's lengths.</summary>
    public bool Limit;
    public float Lower, Upper;
    /// <summary>A motor: speed (rad/s, m/s) and the most torque or force it uses.</summary>
    public bool Motor;
    public float MotorSpeed, MotorMax;
    /// <summary>Spring hertz (0: rigid) and damping ratio.</summary>
    public float Spring, Damping;
    /// <summary>The two bodies still collide.</summary>
    public bool CollideConnected;

    public JointSettings() { }
}

/// <summary>A joint (Physics.CreateJoint): gone with either body, Destroy, or the editor's Stop.</summary>
public readonly unsafe record struct Joint(ulong Handle)
{
    public bool IsValid => Handle != 0;
    public void Destroy() => Api.Native->jointDestroy(Api.Ctx, Handle);
    /// <summary>Runs its motor (a hinge's, slider's, wheel's or distance's).</summary>
    public void SetMotor(float speed, float max) => Api.Native->jointMotor(Api.Ctx, Handle, speed, max);
}

public static unsafe partial class Physics
{
    // ---- bodies: a spawned one, or a scene entity's (PhysicsBody, Collider) while the game runs

    /// <summary>A body's state; false when the entity has none.</summary>
    public static bool Body(Entity e, out BodyInfo info)
    {
        FxBodyState s;
        info = default;
        if (Api.Native->bodyGet(Api.Ctx, e.Id, &s) == 0)
            return false;
        info = new BodyInfo
        {
            Velocity = new Vector3(s.linear[0], s.linear[1], s.linear[2]),
            AngularVelocity = new Vector3(s.angular[0], s.angular[1], s.angular[2]),
            Mass = s.mass, GravityScale = s.gravityScale, LinearDamping = s.linearDamping, AngularDamping = s.angularDamping,
            Type = (BodyType)s.type, Locks = (BodyLocks)s.locks, Awake = s.awake != 0, Bullet = s.bullet != 0,
        };
        return true;
    }

    private static bool Set(Entity e, uint mask, in FxBodyState s)
    {
        fixed (FxBodyState* p = &s)
            return Api.Native->bodySet(Api.Ctx, e.Id, p, mask) != 0;
    }

    public static bool SetVelocity(Entity e, Vector3 velocity)
    {
        FxBodyState s = default;
        s.linear[0] = velocity.X; s.linear[1] = velocity.Y; s.linear[2] = velocity.Z;
        return Set(e, 1, s);
    }

    public static Vector3 AngularVelocity(Entity e) => Body(e, out BodyInfo b) ? b.AngularVelocity : default;

    public static bool SetAngularVelocity(Entity e, Vector3 velocity)
    {
        FxBodyState s = default;
        s.angular[0] = velocity.X; s.angular[1] = velocity.Y; s.angular[2] = velocity.Z;
        return Set(e, 2, s);
    }

    public static bool SetMass(Entity e, float kg) => Set(e, 4, new FxBodyState { mass = kg });
    public static bool SetGravityScale(Entity e, float scale) => Set(e, 8, new FxBodyState { gravityScale = scale });
    public static bool SetDamping(Entity e, float linear, float angular) =>
        Set(e, 16, new FxBodyState { linearDamping = linear, angularDamping = angular });
    public static bool SetType(Entity e, BodyType type) => Set(e, 32, new FxBodyState { type = (uint)type });
    public static bool SetLocks(Entity e, BodyLocks locks) => Set(e, 64, new FxBodyState { locks = (uint)locks });
    public static bool SetAwake(Entity e, bool awake) => Set(e, 128, new FxBodyState { awake = awake ? 1u : 0u });
    /// <summary>Continuous collision against moving bodies too (fast, thin things: bullets).</summary>
    public static bool SetBullet(Entity e, bool bullet) => Set(e, 256, new FxBodyState { bullet = bullet ? 1u : 0u });

    /// <summary>A force (N) for this step, at a world point (null: its centre of mass).</summary>
    public static bool AddForce(Entity e, Vector3 force, Vector3? at = null)
    {
        Vector3 p = at ?? default;
        return Api.Native->bodyPush(Api.Ctx, e.Id, (float*)&force, at.HasValue ? (float*)&p : null, null, 0) != 0;
    }

    /// <summary>An impulse (N s): a change of momentum at once, at a world point (null: its centre).</summary>
    public static bool AddImpulse(Entity e, Vector3 impulse, Vector3? at = null)
    {
        Vector3 p = at ?? default;
        return Api.Native->bodyPush(Api.Ctx, e.Id, (float*)&impulse, at.HasValue ? (float*)&p : null, null, 1) != 0;
    }

    public static bool AddTorque(Entity e, Vector3 torque) => Api.Native->bodyPush(Api.Ctx, e.Id, null, null, (float*)&torque, 0) != 0;
    public static bool AddAngularImpulse(Entity e, Vector3 impulse) => Api.Native->bodyPush(Api.Ctx, e.Id, null, null, (float*)&impulse, 1) != 0;

    /// <summary>Moves a body there at once: nothing is swept on the way and it doesn't glide there
    /// (setting a dynamic body's transform would be undone by the next step). Its entity moves too.</summary>
    public static bool Teleport(Entity e, Vector3 position, Quaternion? rotation = null)
    {
        Quaternion q = rotation ?? Quaternion.Identity;
        return Api.Native->bodyTeleport(Api.Ctx, e.Id, (float*)&position, rotation.HasValue ? (float*)&q : null) != 0;
    }

    /// <summary>Builds the entity's body again from its PhysicsBody and Collider as they are now.
    /// Setting or removing either (Ecs.Set, Ecs.Remove) does this by itself at the next step.</summary>
    public static bool Rebuild(Entity e) => Api.Native->bodyRebuild(Api.Ctx, e.Id) != 0;

    /// <summary>The world's gravity (m/s²). The editor's Stop puts it back.</summary>
    public static Vector3 Gravity
    {
        get
        {
            Vector3 g;
            Api.Native->physicsGravity(Api.Ctx, (float*)&g, null);
            return g;
        }
        set => Api.Native->physicsGravity(Api.Ctx, null, (float*)&value);
    }

    // ---- queries (triggers are never hit)

    /// <summary>The first surface along a ray, of the layers the filter hits, passing through the
    /// filter's entity.</summary>
    public static bool Raycast(Vector3 from, Vector3 direction, out SurfaceHit hit, float maxDistance, QueryFilter filter)
    {
        FxQueryFilter f = filter.Native;
        SurfaceHit h;
        bool found = Api.Native->raycastFiltered(Api.Ctx, (float*)&from, (float*)&direction, maxDistance, &f, &h) != 0;
        hit = found ? h : default;
        return found;
    }

    private static FxQueryShape Shape(uint type, Vector3 size, Vector3 position, Quaternion rotation)
    {
        FxQueryShape s = default;
        s.type = type;
        s.size[0] = size.X; s.size[1] = size.Y; s.size[2] = size.Z;
        s.position[0] = position.X; s.position[1] = position.Y; s.position[2] = position.Z;
        s.rotation[0] = rotation.X; s.rotation[1] = rotation.Y; s.rotation[2] = rotation.Z; s.rotation[3] = rotation.W;
        return s;
    }

    private static bool Cast(FxQueryShape shape, Vector3 direction, float maxDistance, QueryFilter filter, out SurfaceHit hit)
    {
        FxQueryFilter f = filter.Native;
        SurfaceHit h;
        bool found = Api.Native->shapeCast(Api.Ctx, &shape, (float*)&direction, maxDistance, &f, &h) != 0;
        hit = found ? h : default;
        return found;
    }

    /// <summary>The first surface a sphere sweeping along a direction meets (hit.Distance: how far it went).</summary>
    public static bool SphereCast(Vector3 from, float radius, Vector3 direction, out SurfaceHit hit, float maxDistance = 100f,
                                  QueryFilter? filter = null) =>
        Cast(Shape(1, new Vector3(radius, 0, 0), from, Quaternion.Identity), direction, maxDistance, filter ?? QueryFilter.All, out hit);

    /// <summary>The same with a box (half extents), turned.</summary>
    public static bool BoxCast(Vector3 from, Vector3 halfExtents, Quaternion rotation, Vector3 direction, out SurfaceHit hit,
                               float maxDistance = 100f, QueryFilter? filter = null) =>
        Cast(Shape(0, halfExtents, from, rotation), direction, maxDistance, filter ?? QueryFilter.All, out hit);

    /// <summary>The same with a capsule (its centre at <paramref name="from"/>, height along its Y).</summary>
    public static bool CapsuleCast(Vector3 from, float radius, float height, Quaternion rotation, Vector3 direction, out SurfaceHit hit,
                                   float maxDistance = 100f, QueryFilter? filter = null) =>
        Cast(Shape(2, new Vector3(radius, height, 0), from, rotation), direction, maxDistance, filter ?? QueryFilter.All, out hit);

    private static int Overlap(FxQueryShape shape, QueryFilter filter, Span<Entity> into)
    {
        FxQueryFilter f = filter.Native;
        fixed (Entity* p = into)
            return Api.Native->overlap(Api.Ctx, &shape, &f, (ulong*)p, into.Length);
    }

    /// <summary>The entities with a shape overlapping a sphere, copied into <paramref name="into"/>;
    /// returns how many there are (the scene's ground and cooked colliders are no entity's).</summary>
    public static int OverlapSphere(Vector3 center, float radius, Span<Entity> into, QueryFilter? filter = null) =>
        Overlap(Shape(1, new Vector3(radius, 0, 0), center, Quaternion.Identity), filter ?? QueryFilter.All, into);

    public static int OverlapBox(Vector3 center, Vector3 halfExtents, Quaternion rotation, Span<Entity> into, QueryFilter? filter = null) =>
        Overlap(Shape(0, halfExtents, center, rotation), filter ?? QueryFilter.All, into);

    public static int OverlapCapsule(Vector3 center, float radius, float height, Quaternion rotation, Span<Entity> into,
                                     QueryFilter? filter = null) =>
        Overlap(Shape(2, new Vector3(radius, height, 0), center, rotation), filter ?? QueryFilter.All, into);

    // ---- triggers

    [ThreadStatic] static TriggerEvent[]? triggers;

    /// <summary>This frame's trigger events (the last physics update's): what came into a trigger,
    /// and what went out. The span is valid until the next call.</summary>
    public static ReadOnlySpan<TriggerEvent> Triggers()
    {
        triggers ??= new TriggerEvent[32];
        int n;
        fixed (TriggerEvent* p = triggers)
            n = Api.Native->triggerPoll(Api.Ctx, p, triggers.Length);
        if (n > triggers.Length)
        {
            triggers = new TriggerEvent[Math.Max(n, triggers.Length * 2)];
            fixed (TriggerEvent* p = triggers)
                n = Api.Native->triggerPoll(Api.Ctx, p, triggers.Length);
        }
        return new ReadOnlySpan<TriggerEvent>(triggers, 0, Math.Min(n, triggers.Length));
    }

    // ---- joints

    /// <summary>Joins two entities' bodies (<paramref name="b"/> default: the world). Invalid when
    /// either has no body.</summary>
    public static Joint CreateJoint(JointType type, Entity a, Entity b, in JointSettings settings)
    {
        FxJointSettings s = default;
        s.anchor[0] = settings.Anchor.X; s.anchor[1] = settings.Anchor.Y; s.anchor[2] = settings.Anchor.Z;
        s.axis[0] = settings.Axis.X; s.axis[1] = settings.Axis.Y; s.axis[2] = settings.Axis.Z;
        s.anchorB[0] = settings.AnchorB.X; s.anchorB[1] = settings.AnchorB.Y; s.anchorB[2] = settings.AnchorB.Z;
        s.limit = settings.Limit ? 1u : 0u;
        s.lower = settings.Lower;
        s.upper = settings.Upper;
        s.motor = settings.Motor ? 1u : 0u;
        s.motorSpeed = settings.MotorSpeed;
        s.motorMax = settings.MotorMax;
        s.spring = settings.Spring;
        s.damping = settings.Damping;
        s.collide = settings.CollideConnected ? 1u : 0u;
        return new Joint(Api.Native->jointCreate(Api.Ctx, (int)type, a.Id, b.Id, &s));
    }

    /// <summary>A hinge about <paramref name="axis"/> through <paramref name="anchor"/> (a door).</summary>
    public static Joint Hinge(Entity a, Entity b, Vector3 anchor, Vector3 axis) =>
        CreateJoint(JointType.Hinge, a, b, new JointSettings { Anchor = anchor, Axis = axis });
}
