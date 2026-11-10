using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Falcon.Steamworks;

namespace Falcon;

/// <summary>
/// Steam stats (numbers Steam keeps for each player: games won, distance run) and achievements
/// (defined on the app's Steamworks page). Changes are local until <see cref="Store"/> sends them
/// (an achievement's popup shows then); the engine stores them when the game stops. Safe without
/// Steam: reads give 0 / false, writes do nothing.
/// </summary>
public static class SteamStats
{
    private static bool s_dirty;

    /// <summary>An integer stat (0: none, or no Steam).</summary>
    public static unsafe int GetInt(string name)
    {
        int v = 0;
        return Steam.IsRunning && SteamUserStats.GetStat(name, &v) ? v : 0;
    }

    /// <summary>A float stat.</summary>
    public static unsafe float GetFloat(string name)
    {
        float v = 0;
        return Steam.IsRunning && SteamUserStats.GetStat(name, &v) ? v : 0f;
    }

    /// <summary>Sets an integer stat (stored with <see cref="Store"/>).</summary>
    public static bool Set(string name, int value) => Steam.IsRunning && Dirty(SteamUserStats.SetStat(name, value));

    /// <summary>Sets a float stat.</summary>
    public static bool Set(string name, float value) => Steam.IsRunning && Dirty(SteamUserStats.SetStat(name, value));

    /// <summary>Adds to an integer stat.</summary>
    public static bool Add(string name, int amount) => Set(name, GetInt(name) + amount);

    /// <summary>A rate stat ("average per hour"): this session's count over its length.</summary>
    public static bool AddToAverage(string name, float count, double seconds) =>
        Steam.IsRunning && Dirty(SteamUserStats.UpdateAvgRateStat(name, count, seconds));

    /// <summary>Sends the changed stats and achievements to Steam (the achievements' popups show).</summary>
    public static bool Store()
    {
        if (!Steam.IsRunning || !s_dirty)
            return false;
        s_dirty = false;
        return SteamUserStats.StoreStats();
    }

    private static bool Dirty(bool changed)
    {
        s_dirty |= changed;
        return changed;
    }

    // ---- achievements

    /// <summary>The app's achievements: their API names, in Steamworks' order.</summary>
    public static List<string> Achievements
    {
        get
        {
            var list = new List<string>();
            if (!Steam.IsRunning)
                return list;
            uint n = SteamUserStats.GetNumAchievements();
            for (uint i = 0; i < n; ++i)
                list.Add(SteamUserStats.GetAchievementName(i));
            return list;
        }
    }

    /// <summary>The player has it.</summary>
    public static unsafe bool IsUnlocked(string achievement)
    {
        Bool8 done = false;
        return Steam.IsRunning && SteamUserStats.GetAchievement(achievement, &done) && done;
    }

    /// <summary>When the player got it (null: not yet).</summary>
    public static unsafe DateTime? UnlockedAt(string achievement)
    {
        Bool8 done = false;
        uint time = 0;
        return Steam.IsRunning && SteamUserStats.GetAchievementAndUnlockTime(achievement, &done, &time) && done
            ? DateTimeOffset.FromUnixTimeSeconds(time).UtcDateTime
            : null;
    }

    /// <summary>Its name, description or "hidden" ("1": hidden until unlocked) as players see them.</summary>
    public static string Display(string achievement, string key = "name") =>
        Steam.IsRunning ? SteamUserStats.GetAchievementDisplayAttribute(achievement, key) : "";

    /// <summary>Unlocks it (its popup shows at the next <see cref="Store"/>; stored now when
    /// <paramref name="store"/>).</summary>
    public static bool Unlock(string achievement, bool store = true)
    {
        if (!Steam.IsRunning || !Dirty(SteamUserStats.SetAchievement(achievement)))
            return false;
        if (store)
            Store();
        return true;
    }

    /// <summary>Takes it back (testing).</summary>
    public static bool Clear(string achievement) => Steam.IsRunning && Dirty(SteamUserStats.ClearAchievement(achievement));

    /// <summary>Shows its progress in a popup (it unlocks with its stat, at the max).</summary>
    public static bool ShowProgress(string achievement, int current, int max) =>
        Steam.IsRunning && SteamUserStats.IndicateAchievementProgress(achievement, (uint)current, (uint)max);

    /// <summary>The share of all players who have each achievement (Steam's global numbers), by name.</summary>
    public static async Task<Dictionary<string, float>> GlobalPercentagesAsync()
    {
        var all = new Dictionary<string, float>();
        if (!Steam.IsRunning)
            return all;
        GlobalAchievementPercentagesReady_t ready = await SteamUserStats.RequestGlobalAchievementPercentagesAsync();
        if (ready.m_eResult == EResult.k_EResultOK)
            ReadPercentages(all);
        return all;
    }

    private static unsafe void ReadPercentages(Dictionary<string, float> all)
    {
        byte* name = stackalloc byte[256];
        float percent;
        Bool8 achieved;
        for (int i = SteamUserStats.GetMostAchievedAchievementInfo(name, 256, &percent, &achieved); i >= 0;
             i = SteamUserStats.GetNextMostAchievedAchievementInfo(i, name, 256, &percent, &achieved))
            all[SteamText.Read(name, 256)] = percent;
    }

    /// <summary>How many people play the game now (Steam's count).</summary>
    public static async Task<int> PlayersNowAsync()
    {
        if (!Steam.IsRunning)
            return 0;
        NumberOfCurrentPlayers_t n = await SteamUserStats.GetNumberOfCurrentPlayersAsync();
        return n.m_bSuccess != 0 ? n.m_cPlayers : 0;
    }

    // The game stopped: what changed goes to Steam.
    internal static void Stop() => Store();
}

/// <summary>A Steam leaderboard (made on the app's Steamworks page, or by the game).</summary>
public sealed class SteamLeaderboard
{
    /// <summary>Its handle.</summary>
    public ulong Handle { get; }

    private SteamLeaderboard(ulong handle) => Handle = handle;

    /// <summary>Finds one by name; with <paramref name="create"/>, makes it when there is none
    /// (sorted that way, shown as numbers, seconds or milliseconds). Null: none, or no Steam.</summary>
    public static async Task<SteamLeaderboard?> FindAsync(string name, bool create = false,
                                                          ELeaderboardSortMethod sort = ELeaderboardSortMethod.k_ELeaderboardSortMethodDescending,
                                                          ELeaderboardDisplayType display = ELeaderboardDisplayType.k_ELeaderboardDisplayTypeNumeric)
    {
        if (!Steam.IsRunning)
            return null;
        LeaderboardFindResult_t found = create ? await SteamUserStats.FindOrCreateLeaderboardAsync(name, sort, display)
                                               : await SteamUserStats.FindLeaderboardAsync(name);
        return found.m_bLeaderboardFound != 0 ? new SteamLeaderboard(found.m_hSteamLeaderboard) : null;
    }

    public string Name => Steam.IsRunning ? SteamUserStats.GetLeaderboardName(Handle) : "";

    /// <summary>How many entries it has.</summary>
    public int EntryCount => Steam.IsRunning ? SteamUserStats.GetLeaderboardEntryCount(Handle) : 0;

    /// <summary>Uploads the player's score (with <paramref name="keepBest"/>, only if better than
    /// theirs) and up to 64 details. What happened: whether it changed, and the rank before and now.</summary>
    public async Task<(bool ok, bool changed, int rank, int rankBefore)> UploadAsync(int score, bool keepBest = true, int[]? details = null)
    {
        if (!Steam.IsRunning)
            return default;
        var method = keepBest ? ELeaderboardUploadScoreMethod.k_ELeaderboardUploadScoreMethodKeepBest
                              : ELeaderboardUploadScoreMethod.k_ELeaderboardUploadScoreMethodForceUpdate;
        LeaderboardScoreUploaded_t done = await StartUpload(method, score, details);
        return (done.m_bSuccess != 0, done.m_bScoreChanged != 0, done.m_nGlobalRankNew, done.m_nGlobalRankPrevious);
    }

    private unsafe Task<LeaderboardScoreUploaded_t> StartUpload(ELeaderboardUploadScoreMethod method, int score, int[]? details)
    {
        fixed (int* d = details)  // (Steam copies the details at the call)
            return SteamUserStats.UploadLeaderboardScoreAsync(Handle, method, score, d, details?.Length ?? 0);
    }

    /// <summary>Entries: the top ones (ranks <paramref name="start"/> to <paramref name="end"/>, from 1),
    /// around the player (offsets from them), or the player's friends'.</summary>
    public async Task<List<SteamLeaderboardEntry>> EntriesAsync(ELeaderboardDataRequest request = ELeaderboardDataRequest.k_ELeaderboardDataRequestGlobal,
                                                                 int start = 1, int end = 10)
    {
        var list = new List<SteamLeaderboardEntry>();
        if (!Steam.IsRunning)
            return list;
        LeaderboardScoresDownloaded_t got = await SteamUserStats.DownloadLeaderboardEntriesAsync(Handle, request, start, end);
        ReadEntries(got, list);
        return list;
    }

    private static unsafe void ReadEntries(LeaderboardScoresDownloaded_t got, List<SteamLeaderboardEntry> list)
    {
        int* details = stackalloc int[64];
        for (int i = 0; i < got.m_cEntryCount; ++i)
        {
            LeaderboardEntry_t e;
            if (!SteamUserStats.GetDownloadedLeaderboardEntry(got.m_hSteamLeaderboardEntries, i, &e, details, 64))
                continue;
            int[] d = new int[Math.Min(e.m_cDetails, 64)];
            for (int k = 0; k < d.Length; ++k)
                d[k] = details[k];
            list.Add(new SteamLeaderboardEntry(new SteamPlayer(e.m_steamIDUser), e.m_nGlobalRank, e.m_nScore, d));
        }
    }
}

/// <summary>A leaderboard's entry: who, their rank (from 1), score and details.</summary>
public readonly record struct SteamLeaderboardEntry(SteamPlayer Player, int Rank, int Score, int[] Details);

/// <summary>
/// Steam Cloud: the player's saves on Steam's servers, the same on each PC they play on (the app
/// must have Cloud space on its Steamworks page). <see cref="Storage"/> uses it by itself when it is on.
/// </summary>
public static unsafe class SteamCloud
{
    /// <summary>Cloud saves work now: Steam runs, and the player and the app both have them on.</summary>
    public static bool IsEnabled => Steam.IsRunning && SteamRemoteStorage.IsCloudEnabledForAccount() && SteamRemoteStorage.IsCloudEnabledForApp();

    /// <summary>The app's cloud space: all of it, and what's left (bytes).</summary>
    public static (ulong total, ulong available) Quota
    {
        get
        {
            ulong total = 0, available = 0;
            if (Steam.IsRunning)
                SteamRemoteStorage.GetQuota(&total, &available);
            return (total, available);
        }
    }

    public static bool Write(string name, ReadOnlySpan<byte> data)
    {
        if (!IsEnabled)
            return false;
        fixed (byte* p = data)
            return SteamRemoteStorage.FileWrite(name, p, data.Length);
    }

    public static byte[]? Read(string name)
    {
        if (!IsEnabled || !SteamRemoteStorage.FileExists(name))
            return null;
        byte[] data = new byte[SteamRemoteStorage.GetFileSize(name)];
        fixed (byte* p = data)
            return SteamRemoteStorage.FileRead(name, p, data.Length) == data.Length ? data : null;
    }

    public static bool Exists(string name) => IsEnabled && SteamRemoteStorage.FileExists(name);

    public static bool Delete(string name) => IsEnabled && SteamRemoteStorage.FileDelete(name);

    /// <summary>The files in the app's cloud: name and size.</summary>
    public static List<(string name, int size)> Files
    {
        get
        {
            var list = new List<(string, int)>();
            if (!Steam.IsRunning)
                return list;
            int n = SteamRemoteStorage.GetFileCount();
            for (int i = 0; i < n; ++i)
            {
                int size;
                string name = SteamRemoteStorage.GetFileNameAndSize(i, &size);
                list.Add((name, size));
            }
            return list;
        }
    }
}
