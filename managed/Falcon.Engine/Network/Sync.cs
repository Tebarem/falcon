using System;
using System.Buffers.Binary;
using System.Numerics;
using System.Text;

namespace Falcon;

/// <summary>
/// A component's field or property that is part of the networked simulation's state: saved with
/// every tick (a rollback restores it), sent by the host to the other machines, and hashed by the
/// determinism checks. The source generator writes its code (no reflection). Supported: bool,
/// byte, int, uint, long, ulong, float, double, enums, string, Vector2/3/4, Quaternion and Entity.
/// The member must not be private (the generated code reads and writes it). State a component
/// keeps in other fields is its own: a rollback leaves it as it is.
/// </summary>
[AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
public sealed class SyncAttribute : Attribute
{
    /// <summary>Other machines smooth it between the host's updates (numbers and vectors).</summary>
    public bool Interpolate { get; init; }

    /// <summary>Its owner (the connection that owns the entity) writes it, not the host.</summary>
    public bool FromOwner { get; init; }
}

/// <summary>A [Sync] member's bytes written (the generated code; exact bits, little-endian).</summary>
public sealed class SyncWriter
{
    private byte[] _data = new byte[256];

    public int Length { get; private set; }
    public ReadOnlySpan<byte> Written => _data.AsSpan(0, Length);

    public void Clear() => Length = 0;

    private Span<byte> Take(int n)
    {
        if (Length + n > _data.Length)
            Array.Resize(ref _data, Math.Max(_data.Length * 2, Length + n));
        Span<byte> s = _data.AsSpan(Length, n);
        Length += n;
        return s;
    }

    public void Write(bool v) => Take(1)[0] = v ? (byte)1 : (byte)0;
    public void Write(byte v) => Take(1)[0] = v;
    public void Write(int v) => BinaryPrimitives.WriteInt32LittleEndian(Take(4), v);
    public void Write(uint v) => BinaryPrimitives.WriteUInt32LittleEndian(Take(4), v);
    public void Write(long v) => BinaryPrimitives.WriteInt64LittleEndian(Take(8), v);
    public void Write(ulong v) => BinaryPrimitives.WriteUInt64LittleEndian(Take(8), v);
    public void Write(float v) => BinaryPrimitives.WriteSingleLittleEndian(Take(4), v);
    public void Write(double v) => BinaryPrimitives.WriteDoubleLittleEndian(Take(8), v);
    public void Write(Vector2 v) { Write(v.X); Write(v.Y); }
    public void Write(Vector3 v) { Write(v.X); Write(v.Y); Write(v.Z); }
    public void Write(Vector4 v) { Write(v.X); Write(v.Y); Write(v.Z); Write(v.W); }
    public void Write(Quaternion v) { Write(v.X); Write(v.Y); Write(v.Z); Write(v.W); }
    /// <summary>An entity reference: a networked one by its NetId (the same on every machine and
    /// after a rollback re-makes it), another by its id here.</summary>
    public void Write(Entity v)
    {
        if (v.Id != 0 && Network.Count > 0 && Network.Of(v) is NetworkObject o)
        {
            Write((byte)1);
            Write(o.NetId);
        }
        else
        {
            Write((byte)0);
            Write(v.Id);
        }
    }
    public void Write(string? v)
    {
        v ??= "";
        int n = Encoding.UTF8.GetByteCount(v);
        Write(n);
        Encoding.UTF8.GetBytes(v, Take(n));
    }
    public void Write(ReadOnlySpan<byte> bytes) => bytes.CopyTo(Take(bytes.Length));

    /// <summary>An int written earlier (at `position`) replaced (a length known only after).</summary>
    public void Patch(int position, int value) => BinaryPrimitives.WriteInt32LittleEndian(_data.AsSpan(position, 4), value);
}

/// <summary>[Sync] members' bytes read back (the generated code).</summary>
public ref struct SyncReader
{
    private readonly ReadOnlySpan<byte> _data;
    private int _at;

    public SyncReader(ReadOnlySpan<byte> data)
    {
        _data = data;
        _at = 0;
    }

    public int Position => _at;
    public bool Ok { get; private set; } = true;

    private ReadOnlySpan<byte> Take(int n)
    {
        if (_at + n > _data.Length)
        {
            Ok = false;
            _at = _data.Length;
            return new byte[n];
        }
        ReadOnlySpan<byte> s = _data.Slice(_at, n);
        _at += n;
        return s;
    }

    public bool Bool() => Take(1)[0] != 0;
    public byte Byte() => Take(1)[0];
    public int Int() => BinaryPrimitives.ReadInt32LittleEndian(Take(4));
    public uint UInt() => BinaryPrimitives.ReadUInt32LittleEndian(Take(4));
    public long Long() => BinaryPrimitives.ReadInt64LittleEndian(Take(8));
    public ulong ULong() => BinaryPrimitives.ReadUInt64LittleEndian(Take(8));
    public float Float() => BinaryPrimitives.ReadSingleLittleEndian(Take(4));
    public double Double() => BinaryPrimitives.ReadDoubleLittleEndian(Take(8));
    public Vector2 Vector2() => new(Float(), Float());
    public Vector3 Vector3() => new(Float(), Float(), Float());
    public Vector4 Vector4() => new(Float(), Float(), Float(), Float());
    public Quaternion Quaternion() => new(Float(), Float(), Float(), Float());
    public Entity Entity()
    {
        byte kind = Byte();
        ulong id = ULong();
        return kind == 1 ? Network.Find(id)?.Entity ?? default : new Entity(id);
    }
    public string String()
    {
        int n = Int();
        return n <= 0 ? "" : Encoding.UTF8.GetString(Take(n));
    }
    public ReadOnlySpan<byte> Bytes(int n) => Take(n);
}
