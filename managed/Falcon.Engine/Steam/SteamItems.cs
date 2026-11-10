using System;
using System.Collections.Generic;
using System.Numerics;
using System.Threading.Tasks;
using Falcon.Steamworks;

namespace Falcon;

/// <summary>
/// Steam Inventory: the items a player owns in the game (defined in the app's item schema on
/// Steamworks): list them, consume or exchange them, drops (timed rewards), promo items, and the
/// item store. Every change goes through Steam's servers; the results arrive later (the Async calls).
/// </summary>
public static class SteamItems
{
    private static readonly Dictionary<int, TaskCompletionSource<List<SteamItem>?>> s_waiting = new();
    private static IDisposable? s_listening;

    /// <summary>The player's items (null: Steam said no).</summary>
    public static unsafe Task<List<SteamItem>?> AllAsync()
    {
        int result;
        return Steam.IsRunning && SteamInventory.GetAllItems(&result) ? Wait(result) : Task.FromResult<List<SteamItem>?>(null);
    }

    /// <summary>Uses up <paramref name="quantity"/> of an item (its instance).</summary>
    public static unsafe Task<List<SteamItem>?> ConsumeAsync(ulong item, uint quantity = 1)
    {
        int result;
        return Steam.IsRunning && SteamInventory.ConsumeItem(&result, item, quantity) ? Wait(result) : Task.FromResult<List<SteamItem>?>(null);
    }

    /// <summary>A drop from a drop list, when one is due (call it now and then while playing).</summary>
    public static unsafe Task<List<SteamItem>?> TriggerDropAsync(int dropList)
    {
        int result;
        return Steam.IsRunning && SteamInventory.TriggerItemDrop(&result, dropList) ? Wait(result) : Task.FromResult<List<SteamItem>?>(null);
    }

    /// <summary>The promo items the player is owed (owning another game, say).</summary>
    public static unsafe Task<List<SteamItem>?> GrantPromosAsync()
    {
        int result;
        return Steam.IsRunning && SteamInventory.GrantPromoItems(&result) ? Wait(result) : Task.FromResult<List<SteamItem>?>(null);
    }

    /// <summary>Makes <paramref name="make"/> from <paramref name="use"/> (a recipe in the schema):
    /// item definitions to generate, item instances (and how many) to destroy.</summary>
    public static unsafe Task<List<SteamItem>?> ExchangeAsync(int make, IReadOnlyList<(ulong item, uint quantity)> use)
    {
        if (!Steam.IsRunning)
            return Task.FromResult<List<SteamItem>?>(null);
        int result;
        uint one = 1;
        ulong* items = stackalloc ulong[use.Count];
        uint* counts = stackalloc uint[use.Count];
        for (int i = 0; i < use.Count; ++i)
        {
            items[i] = use[i].item;
            counts[i] = use[i].quantity;
        }
        return SteamInventory.ExchangeItems(&result, &make, &one, 1, items, counts, (uint)use.Count) ? Wait(result) : Task.FromResult<List<SteamItem>?>(null);
    }

    /// <summary>Opens Steam's checkout for these item definitions (the item store); true: it opened.</summary>
    public static async Task<bool> BuyAsync(IReadOnlyList<(int definition, uint quantity)> items)
    {
        if (!Steam.IsRunning)
            return false;
        SteamInventoryStartPurchaseResult_t r = await StartPurchase(items);
        return r.m_result == EResult.k_EResultOK;
    }

    private static unsafe Task<SteamInventoryStartPurchaseResult_t> StartPurchase(IReadOnlyList<(int definition, uint quantity)> items)
    {
        int* defs = stackalloc int[items.Count];
        uint* counts = stackalloc uint[items.Count];
        for (int i = 0; i < items.Count; ++i)
        {
            defs[i] = items[i].definition;
            counts[i] = items[i].quantity;
        }
        return SteamInventory.StartPurchaseAsync(defs, counts, (uint)items.Count);
    }

    /// <summary>The item schema's definitions (loaded when Steam has them: <see cref="LoadDefinitions"/>).</summary>
    public static unsafe List<int> Definitions
    {
        get
        {
            var list = new List<int>();
            if (!Steam.IsRunning)
                return list;
            uint n = 0;
            if (!SteamInventory.GetItemDefinitionIDs(null, &n) || n == 0)
                return list;
            int* ids = stackalloc int[(int)n];
            if (SteamInventory.GetItemDefinitionIDs(ids, &n))
                for (int i = 0; i < n; ++i)
                    list.Add(ids[i]);
            return list;
        }
    }

    /// <summary>Asks Steam for the item schema (SteamInventoryDefinitionUpdate_t when it has it).</summary>
    public static bool LoadDefinitions() => Steam.IsRunning && SteamInventory.LoadItemDefinitions();

    /// <summary>A definition's property ("name", "description", "icon_url", "price_category"...).</summary>
    public static unsafe string Property(int definition, string name)
    {
        if (!Steam.IsRunning)
            return "";
        uint size = 0;
        if (!SteamInventory.GetItemDefinitionProperty(definition, name, null, &size) || size == 0)
            return "";
        byte[] text = new byte[size];
        fixed (byte* p = text)
            return SteamInventory.GetItemDefinitionProperty(definition, name, p, &size) ? SteamText.Read(p, (int)size) : "";
    }

    // A result handle: its items when Steam's answer comes (SteamInventoryResultReady_t).
    private static Task<List<SteamItem>?> Wait(int result)
    {
        s_listening ??= Steam.On<SteamInventoryResultReady_t>(Ready);
        var done = new TaskCompletionSource<List<SteamItem>?>();
        s_waiting[result] = done;
        return done.Task;
    }

    /// <summary>Steam's answer to the last call (why one gave null).</summary>
    public static EResult LastResult { get; private set; } = EResult.k_EResultOK;

    private static unsafe void Ready(SteamInventoryResultReady_t e)
    {
        LastResult = e.m_result;
        if (!s_waiting.Remove(e.m_handle, out TaskCompletionSource<List<SteamItem>?>? done))
            return;
        List<SteamItem>? items = null;
        if (e.m_result == EResult.k_EResultOK)
        {
            items = new List<SteamItem>();
            uint n = 0;
            if (SteamInventory.GetResultItems(e.m_handle, null, &n) && n > 0)
            {
                var details = new SteamItemDetails_t[n];
                fixed (SteamItemDetails_t* d = details)
                    if (SteamInventory.GetResultItems(e.m_handle, d, &n))
                        for (int i = 0; i < n; ++i)
                            items.Add(new SteamItem(d[i].m_itemId, d[i].m_iDefinition, d[i].m_unQuantity, (d[i].m_unFlags & 0x100) != 0));
            }
        }
        SteamInventory.DestroyResult(e.m_handle);
        done.TrySetResult(items);
    }

    internal static void Stop()
    {
        foreach (TaskCompletionSource<List<SteamItem>?> done in s_waiting.Values)
            done.TrySetResult(null);
        s_waiting.Clear();
        s_listening = null;
    }
}

/// <summary>An item the player owns: its instance, definition, how many, and whether it was used up.</summary>
public readonly record struct SteamItem(ulong Instance, int Definition, int Quantity, bool Consumed);

/// <summary>
/// Steam Input: controllers through Steam's action sets (any controller Steam supports, the player's
/// own bindings). Turned on by <see cref="Start"/> with the game's action manifest; read actions
/// here, or bind them to an <see cref="InputAction"/> (<see cref="InputAction.BindSteam"/>).
/// </summary>
public static unsafe class SteamControllers
{
    private static bool s_started;
    private static readonly Dictionary<string, ulong> s_digital = new(), s_analog = new(), s_sets = new();
    private static readonly ulong[] s_controllers = new ulong[16];
    private static int s_count;

    /// <summary>Steam Input is on (the game started it).</summary>
    public static bool IsActive => s_started && Steam.IsRunning;

    /// <summary>Turns Steam Input on, with the game's action manifest (its actions, sets and default
    /// bindings: a VDF file, absolute path; null: the one on the app's Steamworks page).</summary>
    public static bool Start(string? manifest = null)
    {
        if (!Steam.IsRunning)
            return false;
        if (!s_started)
            s_started = SteamInput.Init(true);
        if (s_started && manifest != null)
            SteamInput.SetInputActionManifestFilePath(System.IO.Path.GetFullPath(manifest));
        return s_started;
    }

    /// <summary>How many controllers Steam Input sees.</summary>
    public static int Count => IsActive ? s_count : 0;

    /// <summary>A digital action ("jump"), on any controller.</summary>
    public static bool Digital(string action)
    {
        if (!IsActive)
            return false;
        ulong handle = Handle(s_digital, action, true);
        for (int i = 0; i < s_count; ++i)
        {
            InputDigitalActionData_t d = SteamInput.GetDigitalActionData(s_controllers[i], handle);
            if (d.bActive && d.bState)
                return true;
        }
        return false;
    }

    /// <summary>An analog action ("move", "look"), the strongest controller's.</summary>
    public static Vector2 Analog(string action)
    {
        if (!IsActive)
            return default;
        ulong handle = Handle(s_analog, action, false);
        Vector2 best = default;
        for (int i = 0; i < s_count; ++i)
        {
            InputAnalogActionData_t d = SteamInput.GetAnalogActionData(s_controllers[i], handle);
            var v = new Vector2(d.x, d.y);
            if (d.bActive && v.LengthSquared() > best.LengthSquared())
                best = v;
        }
        return best;
    }

    /// <summary>Makes an action set ("menu", "driving") the one every controller uses.</summary>
    public static void ActivateSet(string set)
    {
        if (!IsActive)
            return;
        if (!s_sets.TryGetValue(set, out ulong h))
            s_sets[set] = h = SteamInput.GetActionSetHandle(set);
        for (int i = 0; i < s_count; ++i)
            SteamInput.ActivateActionSet(s_controllers[i], h);
    }

    /// <summary>The glyph (a PNG file) of what the player presses for an action on the first
    /// controller, for prompts ("" none).</summary>
    public static string Glyph(string action, string set, bool analog = false,
                               ESteamInputGlyphSize size = ESteamInputGlyphSize.k_ESteamInputGlyphSize_Medium)
    {
        if (!IsActive || s_count == 0)
            return "";
        if (!s_sets.TryGetValue(set, out ulong setHandle))
            s_sets[set] = setHandle = SteamInput.GetActionSetHandle(set);
        EInputActionOrigin* origins = stackalloc EInputActionOrigin[8];
        int n = analog ? SteamInput.GetAnalogActionOrigins(s_controllers[0], setHandle, Handle(s_analog, action, false), origins)
                       : SteamInput.GetDigitalActionOrigins(s_controllers[0], setHandle, Handle(s_digital, action, true), origins);
        return n > 0 ? SteamInput.GetGlyphPNGForActionOrigin(origins[0], size, 0) : "";
    }

    /// <summary>Steam's binding screen for the first controller.</summary>
    public static bool ShowBindings() => IsActive && s_count > 0 && SteamInput.ShowBindingPanel(s_controllers[0]);

    /// <summary>Rumbles a controller (0-65535 each side).</summary>
    public static void Vibrate(int controller, ushort left, ushort right)
    {
        if (IsActive && controller < s_count)
            SteamInput.TriggerVibration(s_controllers[controller], left, right);
    }

    private static ulong Handle(Dictionary<string, ulong> cache, string action, bool digital)
    {
        if (!cache.TryGetValue(action, out ulong h))
            cache[action] = h = digital ? SteamInput.GetDigitalActionHandle(action) : SteamInput.GetAnalogActionHandle(action);
        return h;
    }

    // Each frame (Steam.Frame): Steam Input's state, and the controllers.
    internal static void Frame()
    {
        if (!IsActive)
            return;
        SteamInput.RunFrame(false);
        fixed (ulong* p = s_controllers)
            s_count = SteamInput.GetConnectedControllers(p);
    }

    internal static void Stop()
    {
        if (s_started && Steam.IsRunning)
            SteamInput.Shutdown();
        s_started = false;
        s_digital.Clear();
        s_analog.Clear();
        s_sets.Clear();
        s_count = 0;
    }
}
