using System;
using System.Text.Json;
using Falcon.Editor;

namespace Falcon.Level;

/// <summary>
/// The last level operation, still open to change (Blender's Adjust Last Operation): the Level
/// window shows it with its amount and its command's settings; a change takes it back (undo) and
/// does it again at the new values, so it stays one undo step. It lasts while its step is the newest
/// in the undo history (anything done after it, an undo or a redo, ends it).
/// </summary>
internal static class LastOperation
{
    /// <summary>The menu path the Level window changes its amount through (editor.tools.option).</summary>
    internal const string Path = "Last";

    private sealed record Op(string Name, string? Menu, string? AmountName, string? Unit, float Amount, Action<float> Redo);
    private static Op? s_op;
    private static string s_top = "";

    /// <summary>Done: its name, its command (whose settings change it), its amount if it has one,
    /// and how to do it again at an amount (as one undo step, calling Done again).</summary>
    internal static void Done(string name, string? menu, string? amountName, string? unit, float amount, Action<float> redo)
    {
        s_op = new Op(name, menu, amountName, unit, amount, redo);
        s_top = Top();
    }

    // The newest undo step (an opaque token from the editor).
    private static string Top()
    {
        using JsonDocument d = EditorApp.Query("undo");
        return d.RootElement.GetRawText();
    }

    private static Op? Current()
    {
        if (s_op != null && Top() != s_top)
            s_op = null;
        return s_op;
    }

    /// <summary>Its amount changed in the Level window.</summary>
    internal static void Change(double amount)
    {
        if (Current() is Op op && op.AmountName != null && double.IsFinite(amount))
            Again(op, (float)amount);
    }

    /// <summary>A command's setting changed: when it is the last operation's, done again with it.</summary>
    internal static void SettingChanged(string menu)
    {
        if (Current() is Op op && op.Menu == menu)
            Again(op, op.Amount);
    }

    private static void Again(Op op, float amount)
    {
        s_op = null;  // (Redo calls Done again when it works)
        Undo.Perform();
        op.Redo(amount);
    }

    /// <summary>The Level window's view of it ("last": {name, menu, amount, amountName, unit}), if any.</summary>
    internal static void Write(Utf8JsonWriter w)
    {
        if (Current() is not Op op)
            return;
        w.WriteStartObject("last");
        w.WriteString("name", op.Name);
        if (op.Menu != null)
            w.WriteString("menu", op.Menu);
        if (op.AmountName != null)
        {
            w.WriteString("amountName", op.AmountName);
            w.WriteNumber("amount", Math.Round(op.Amount, 4));
            w.WriteString("unit", op.Unit ?? "");
        }
        w.WriteEndObject();
    }
}
