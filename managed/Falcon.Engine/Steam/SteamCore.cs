using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using Falcon.Native;

namespace Falcon.Steamworks;

// What the generated bindings (Generated/*.g.cs, tools/steamgen) stand on: steam_api64.dll's
// functions by name, C++'s one-byte bool, text, and callbacks dispatched by hand once a frame.

/// <summary>C++'s bool (one byte), as Steam's structs and functions have it.</summary>
[StructLayout(LayoutKind.Sequential, Size = 1)]
public readonly struct Bool8
{
    private readonly byte _value;
    private Bool8(bool value) => _value = value ? (byte)1 : (byte)0;
    public static implicit operator bool(Bool8 b) => b._value != 0;
    public static implicit operator Bool8(bool b) => new(b);
    public override string ToString() => (_value != 0).ToString();
}

/// <summary>Steam's text: UTF-8, ended with a NUL.</summary>
public static unsafe class SteamText
{
    /// <summary>A NUL-ended UTF-8 string (null stays null).</summary>
    public static string Read(byte* text) => text == null ? "" : Marshal.PtrToStringUTF8((nint)text) ?? "";

    /// <summary>Text in a fixed buffer of <paramref name="capacity"/> bytes (to its NUL).</summary>
    public static string Read(byte* text, int capacity)
    {
        int n = 0;
        while (n < capacity && text[n] != 0)
            ++n;
        return Encoding.UTF8.GetString(text, n);
    }

    /// <summary>A string as NUL-ended UTF-8 (null: null).</summary>
    public static byte[]? Utf8(string? text)
    {
        if (text == null)
            return null;
        byte[] bytes = new byte[Encoding.UTF8.GetByteCount(text) + 1];
        Encoding.UTF8.GetBytes(text, bytes);
        return bytes;
    }
}

/// <summary>A Steam callback struct (its id: k_iCallback).</summary>
public interface ISteamCallback
{
    static abstract int CallbackId { get; }
}

/// <summary>A Steam call that failed (its call result didn't come, or came marked failed).</summary>
public sealed class SteamException(string message) : Exception(message);

/// <summary>steam_api64.dll, as the engine started it (engine/steam/steam_client.h).</summary>
public static unsafe class SteamLibrary
{
    private static int s_state = int.MinValue;
    private static uint s_appId;
    private static string s_error = "";

    /// <summary>Steam started (for <see cref="AppId"/>), for the signed-in player or as a dedicated
    /// game server: the API can be called (the interfaces of the one it is).</summary>
    public static bool IsRunning
    {
        get
        {
            Refresh();
            return s_state > 0;
        }
    }

    /// <summary>Steam runs as a dedicated game server (SteamGameServer*: no player signed in).</summary>
    public static bool IsServer
    {
        get
        {
            Refresh();
            return s_state == 2;
        }
    }

    /// <summary>The App ID Steam was started for (0: it wasn't asked for).</summary>
    public static uint AppId
    {
        get
        {
            Refresh();
            return s_appId;
        }
    }

    /// <summary>Why Steam didn't start ("" when it did, or wasn't asked for).</summary>
    public static string Error
    {
        get
        {
            Refresh();
            return s_error;
        }
    }

    /// <summary>Changes when Steam starts or stops: interfaces' objects are looked up again.</summary>
    public static int Epoch { get; private set; }

    private static void Refresh()
    {
        if (s_state != int.MinValue)
            return;
        if (Api.Native == null || Api.Native->steamStatus == null)
        {
            s_state = 0;
            return;
        }
        byte* error = stackalloc byte[512];
        uint appId;
        s_state = Api.Native->steamStatus(Api.Ctx, &appId, error, 512);
        s_appId = appId;
        s_error = SteamText.Read(error);
        ++Epoch;
    }

    // The game stopped (or the engine's state may have changed): read again when next asked.
    internal static void Forget() => s_state = int.MinValue;

    internal static bool Ready(string what) => IsRunning;

    /// <summary>A steam_api64.dll function by name (throws when Steam isn't running).</summary>
    public static void* Proc(string name)
    {
        void* p = null;
        if (IsRunning)
        {
            byte[] utf8 = SteamText.Utf8(name)!;
            fixed (byte* n = utf8)
                p = Api.Native->steamProc(Api.Ctx, n);
        }
        return p != null ? p : throw new InvalidOperationException($"Steam: {name} isn't available ({(IsRunning ? "not in this steam_api64.dll" : "Steam isn't running")})");
    }
}

/// <summary>
/// Steam's callbacks (events Steam sends: a friend's name changing, the overlay opening, a lobby
/// invite) and call results (the answers to calls that take a while): the engine dispatches them
/// once a frame, on the game's thread, before the game's update.
/// </summary>
public static unsafe class SteamCallbacks
{
    private static readonly Dictionary<int, Action<nint, int>> s_dispatch = new();
    private static readonly List<Action> s_clear = new();
    private static readonly Dictionary<ulong, Action<nint, bool>> s_waiting = new();

    private static class Handlers<T> where T : unmanaged, ISteamCallback
    {
        public static readonly List<Action<T>> List = new();
    }

    /// <summary>Calls <paramref name="handler"/> with each <typeparamref name="T"/> Steam sends,
    /// until disposed (or the game stops).</summary>
    public static IDisposable On<T>(Action<T> handler) where T : unmanaged, ISteamCallback
    {
        if (!s_dispatch.ContainsKey(T.CallbackId))
        {
            s_dispatch[T.CallbackId] = static (data, size) =>
            {
                T value = default;
                Buffer.MemoryCopy((void*)data, &value, sizeof(T), Math.Min(size, sizeof(T)));
                foreach (Action<T> h in Handlers<T>.List.ToArray())
                {
                    try
                    {
                        h(value);
                    }
                    catch (Exception e)
                    {
                        Log.Error($"Steam: a {typeof(T).Name} handler failed: {e}");
                    }
                }
            };
            s_clear.Add(static () => Handlers<T>.List.Clear());
        }
        Handlers<T>.List.Add(handler);
        return new Subscription(() => Handlers<T>.List.Remove(handler));
    }

    /// <summary>The result of a Steam call that returned a SteamAPICall_t, when it comes.</summary>
    public static Task<T> Result<T>(ulong call) where T : unmanaged, ISteamCallback
    {
        if (call == 0)
            return Task.FromException<T>(new SteamException($"Steam: the call for {typeof(T).Name} didn't start"));
        var done = new TaskCompletionSource<T>();
        s_waiting[call] = (data, failed) =>
        {
            if (failed)
                done.TrySetException(new SteamException($"Steam: the call for {typeof(T).Name} failed"));
            else
                done.TrySetResult(*(T*)data);
        };
        return done.Task;
    }

    private sealed class Subscription(Action remove) : IDisposable
    {
        private Action? _remove = remove;

        public void Dispose()
        {
            _remove?.Invoke();
            _remove = null;
        }
    }

    private static byte[] s_buffer = new byte[4096];

    // Once a frame: everything Steam has sent since (the engine queues it: engine/steam/steam_client.h).
    internal static void Pump()
    {
        if (!SteamLibrary.IsRunning || Api.Native->steamPump == null)
            return;
        Api.Native->steamPump(Api.Ctx);
        for (;;)
        {
            int id, size;
            fixed (byte* b = s_buffer)
                size = Api.Native->steamNextCallback(Api.Ctx, &id, b, s_buffer.Length);
            if (size == 0)
                break;
            if (size < 0)
            {
                s_buffer = new byte[-size];
                continue;
            }
            fixed (byte* b = s_buffer)
            {
                if (id == SteamAPICallCompleted_t.Id)
                {
                    var call = *(SteamAPICallCompleted_t*)b;
                    if (s_waiting.Remove(call.m_hAsyncCall, out Action<nint, bool>? done))
                        done((nint)(b + 24), b[16] != 0);
                }
                else if (s_dispatch.TryGetValue(id, out Action<nint, int>? dispatch))
                {
                    dispatch((nint)b, size);
                }
            }
        }
    }

    // The game stopped: its handlers and the results it waited for go (hot reload unloads its code).
    internal static void Reset()
    {
        foreach (Action clear in s_clear)
            clear();
        foreach (Action<nint, bool> done in s_waiting.Values)
            done(0, true);
        s_waiting.Clear();
        SteamLibrary.Forget();
    }
}
