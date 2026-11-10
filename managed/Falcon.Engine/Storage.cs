using System;
using System.IO;

namespace Falcon;

/// <summary>
/// A game's saves: files in the player's app data, a folder per game
/// (%APPDATA%/Falcon/Games/&lt;game&gt;/), made when first written. Plain System.IO works too; this
/// is where saves belong and keeps names safe. With Steam Cloud on (<see cref="SteamCloud"/>), saves
/// go there too and are read from there first: the same saves on every PC the player uses.
/// </summary>
public static class Storage
{
    private static string? s_game;

    /// <summary>Saves go to Steam Cloud too when it is on (default true).</summary>
    public static bool UseSteamCloud { get; set; } = true;

    private static bool Cloud => UseSteamCloud && SteamCloud.IsEnabled;

    /// <summary>The game's name for its folder (the game assembly's by default).</summary>
    public static string Game
    {
        get => s_game ??= Sanitize(System.Reflection.Assembly.GetEntryAssembly()?.GetName().Name ?? GameName());
        set => s_game = Sanitize(value);
    }

    /// <summary>The folder (made when first written).</summary>
    public static string Directory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Falcon", "Games", Game);

    private static string GameName()
    {
        Type? t = Falcon.Game.Factory?.Method.ReturnType;
        return t?.Assembly.GetName().Name ?? "Game";
    }

    private static string Sanitize(string name)
    {
        foreach (char c in Path.GetInvalidFileNameChars())
            name = name.Replace(c, '_');
        return name.Length == 0 ? "Game" : name;
    }

    private static string PathOf(string name) => Path.Combine(Directory, Sanitize(name));

    /// <summary>Writes a save (text: JSON, say), replacing it whole (written aside, then moved in).</summary>
    public static void Save(string name, string text)
    {
        System.IO.Directory.CreateDirectory(Directory);
        string path = PathOf(name), temp = path + ".tmp";
        File.WriteAllText(temp, text);
        File.Move(temp, path, overwrite: true);
        if (Cloud)
            SteamCloud.Write(Sanitize(name), System.Text.Encoding.UTF8.GetBytes(text));
    }

    public static void Save(string name, byte[] data)
    {
        System.IO.Directory.CreateDirectory(Directory);
        string path = PathOf(name), temp = path + ".tmp";
        File.WriteAllBytes(temp, data);
        File.Move(temp, path, overwrite: true);
        if (Cloud)
            SteamCloud.Write(Sanitize(name), data);
    }

    /// <summary>A save's text, or null when there is none.</summary>
    public static string? Load(string name) =>
        LoadBytes(name) is byte[] data ? System.Text.Encoding.UTF8.GetString(data) : null;

    public static byte[]? LoadBytes(string name)
    {
        if (Cloud && SteamCloud.Read(Sanitize(name)) is byte[] cloud)
            return cloud;  // (Steam's copy: the newest, from whichever PC)
        return File.Exists(PathOf(name)) ? File.ReadAllBytes(PathOf(name)) : null;
    }

    public static bool Exists(string name) => File.Exists(PathOf(name)) || (Cloud && SteamCloud.Exists(Sanitize(name)));

    public static void Delete(string name)
    {
        if (File.Exists(PathOf(name)))
            File.Delete(PathOf(name));
        if (Cloud)
            SteamCloud.Delete(Sanitize(name));
    }
}
