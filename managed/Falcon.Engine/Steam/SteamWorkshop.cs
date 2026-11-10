using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Falcon.Steamworks;

namespace Falcon;

/// <summary>
/// The Steam Workshop: players' items for the game (levels, prefabs, mods). Find them (ranked,
/// searched, by tag), subscribe (Steam downloads them; <see cref="Installed"/> has their folders),
/// and publish one: a folder of files, a title, a preview, tags. Safe without Steam (no items).
/// </summary>
public static class SteamWorkshop
{
    /// <summary>The game's items, as Steam ranks them (most popular, newest, by trend...), one page
    /// of 50 (from 1), optionally searched and with all these tags.</summary>
    public static async Task<List<SteamWorkshopItem>> FindAsync(EUGCQuery rank = EUGCQuery.k_EUGCQuery_RankedByVote, int page = 1,
                                                                string? search = null, IEnumerable<string>? tags = null, uint appId = 0)
    {
        var items = new List<SteamWorkshopItem>();
        if (!Steam.IsRunning)
            return items;
        uint app = appId != 0 ? appId : Steam.AppId;
        ulong query = SteamUGC.CreateQueryAllUGCRequest(rank, EUGCMatchingUGCType.k_EUGCMatchingUGCType_Items_ReadyToUse, app, app, (uint)Math.Max(page, 1));
        if (search != null)
            SteamUGC.SetSearchText(query, search);
        if (tags != null)
            foreach (string tag in tags)
                SteamUGC.AddRequiredTag(query, tag);
        return await Run(query, items);
    }

    /// <summary>Particular items, by their ids.</summary>
    public static async Task<List<SteamWorkshopItem>> DetailsAsync(IReadOnlyList<ulong> ids)
    {
        var items = new List<SteamWorkshopItem>();
        if (!Steam.IsRunning || ids.Count == 0)
            return items;
        return await Run(DetailsQuery(ids), items);
    }

    private static unsafe ulong DetailsQuery(IReadOnlyList<ulong> ids)
    {
        ulong[] array = new ulong[ids.Count];
        for (int i = 0; i < array.Length; ++i)
            array[i] = ids[i];
        fixed (ulong* p = array)
            return SteamUGC.CreateQueryUGCDetailsRequest(p, (uint)array.Length);
    }

    private static async Task<List<SteamWorkshopItem>> Run(ulong query, List<SteamWorkshopItem> items)
    {
        try
        {
            SteamUGCQueryCompleted_t done = await SteamUGC.SendQueryUGCRequestAsync(query);
            if (done.m_eResult == EResult.k_EResultOK)
                Read(done, items);
        }
        finally
        {
            SteamUGC.ReleaseQueryUGCRequest(query);
        }
        return items;
    }

    private static unsafe void Read(SteamUGCQueryCompleted_t done, List<SteamWorkshopItem> items)
    {
        SteamUGCDetails_t d;
        byte* url = stackalloc byte[1024];
        for (uint i = 0; i < done.m_unNumResultsReturned; ++i)
        {
            if (!SteamUGC.GetQueryUGCResult(done.m_handle, i, &d))
                continue;
            string preview = SteamUGC.GetQueryUGCPreviewURL(done.m_handle, i, url, 1024) ? SteamText.Read(url, 1024) : "";
            items.Add(new SteamWorkshopItem(d.m_nPublishedFileId, d.Title, d.Description, d.Tags, new SteamPlayer(d.m_ulSteamIDOwner),
                                            d.m_unVotesUp, d.m_unVotesDown, d.m_flScore, preview, d.m_ulTotalFilesSize));
        }
    }

    // ---- subscribing

    /// <summary>Subscribes the player (Steam downloads it, and keeps it updated).</summary>
    public static async Task<bool> SubscribeAsync(ulong item)
    {
        if (!Steam.IsRunning)
            return false;
        RemoteStorageSubscribePublishedFileResult_t r = await SteamUGC.SubscribeItemAsync(item);
        return r.m_eResult == EResult.k_EResultOK;
    }

    public static async Task<bool> UnsubscribeAsync(ulong item)
    {
        if (!Steam.IsRunning)
            return false;
        RemoteStorageUnsubscribePublishedFileResult_t r = await SteamUGC.UnsubscribeItemAsync(item);
        return r.m_eResult == EResult.k_EResultOK;
    }

    /// <summary>The items the player subscribes to.</summary>
    public static unsafe List<ulong> Subscribed
    {
        get
        {
            var list = new List<ulong>();
            if (!Steam.IsRunning)
                return list;
            uint n = SteamUGC.GetNumSubscribedItems(false);
            ulong[] ids = new ulong[n];
            fixed (ulong* p = ids)
                n = SteamUGC.GetSubscribedItems(p, n, false);
            for (int i = 0; i < n; ++i)
                list.Add(ids[i]);
            return list;
        }
    }

    /// <summary>Subscribed items on disk: each one's id and folder (its files).</summary>
    public static unsafe List<(ulong item, string folder)> Installed
    {
        get
        {
            var list = new List<(ulong, string)>();
            byte* folder = stackalloc byte[1024];
            foreach (ulong id in Subscribed)
            {
                ulong size;
                uint stamp;
                if ((SteamUGC.GetItemState(id) & 4) != 0 &&  // (k_EItemStateInstalled)
                    SteamUGC.GetItemInstallInfo(id, &size, folder, 1024, &stamp))
                    list.Add((id, SteamText.Read(folder, 1024)));
            }
            return list;
        }
    }

    /// <summary>Asks Steam to download (or update) an item now.</summary>
    public static bool Download(ulong item, bool highPriority = true) => Steam.IsRunning && SteamUGC.DownloadItem(item, highPriority);

    /// <summary>An item's download: bytes so far and all of them.</summary>
    public static unsafe (ulong done, ulong total) DownloadProgress(ulong item)
    {
        ulong done = 0, total = 0;
        if (Steam.IsRunning)
            SteamUGC.GetItemDownloadInfo(item, &done, &total);
        return (done, total);
    }

    // ---- publishing

    /// <summary>
    /// Publishes a folder's files as an item (<paramref name="existing"/>: updates that one instead),
    /// with a title, description, preview image (a file, under 1 MB) and tags, then visible to
    /// <paramref name="visibility"/>. <paramref name="progress"/> hears the upload (0..1). Returns the
    /// item's id (0: failed), and whether the player must first accept the Workshop's legal agreement
    /// (Steam then shows it on the item's page).
    /// </summary>
    public static async Task<(ulong item, bool needsAgreement)> PublishAsync(string folder, string title, string description = "",
                                                                             string? preview = null, IReadOnlyList<string>? tags = null,
                                                                             ERemoteStoragePublishedFileVisibility visibility =
                                                                                 ERemoteStoragePublishedFileVisibility.k_ERemoteStoragePublishedFileVisibilityPublic,
                                                                             ulong existing = 0, string changeNote = "", Action<float>? progress = null)
    {
        if (!Steam.IsRunning)
            return (0, false);
        ulong item = existing;
        bool agreement = false;
        if (item == 0)
        {
            CreateItemResult_t made = await SteamUGC.CreateItemAsync(Steam.AppId, EWorkshopFileType.k_EWorkshopFileTypeCommunity);
            if (made.m_eResult != EResult.k_EResultOK)
            {
                Log.Warning($"Steam Workshop: the item wasn't made ({made.m_eResult})");
                return (0, false);
            }
            item = made.m_nPublishedFileId;
            agreement = made.m_bUserNeedsToAcceptWorkshopLegalAgreement;
        }
        ulong update = SteamUGC.StartItemUpdate(Steam.AppId, item);
        SteamUGC.SetItemTitle(update, title);
        SteamUGC.SetItemDescription(update, description);
        SteamUGC.SetItemContent(update, System.IO.Path.GetFullPath(folder));
        if (preview != null)
            SteamUGC.SetItemPreview(update, System.IO.Path.GetFullPath(preview));
        if (tags != null && tags.Count > 0)
            SetTags(update, tags);
        SteamUGC.SetItemVisibility(update, visibility);
        Task<SubmitItemUpdateResult_t> submit = SteamUGC.SubmitItemUpdateAsync(update, changeNote);
        IDisposable? watching = progress == null ? null : WatchProgress(update, submit, progress);
        SubmitItemUpdateResult_t result = await submit;
        watching?.Dispose();
        if (result.m_eResult != EResult.k_EResultOK)
        {
            Log.Warning($"Steam Workshop: the item's update failed ({result.m_eResult})");
            return (0, agreement);
        }
        return (item, agreement || result.m_bUserNeedsToAcceptWorkshopLegalAgreement);
    }

    private static unsafe void SetTags(ulong update, IReadOnlyList<string> tags)
    {
        // SteamParamStringArray_t: an array of NUL-ended strings and its count.
        var utf8 = new byte[tags.Count][];
        var handles = new System.Runtime.InteropServices.GCHandle[tags.Count];
        nint* pointers = stackalloc nint[tags.Count];
        try
        {
            for (int i = 0; i < tags.Count; ++i)
            {
                utf8[i] = SteamText.Utf8(tags[i])!;
                handles[i] = System.Runtime.InteropServices.GCHandle.Alloc(utf8[i], System.Runtime.InteropServices.GCHandleType.Pinned);
                pointers[i] = handles[i].AddrOfPinnedObject();
            }
            SteamParamStringArray_t array = default;
            array.m_ppStrings = (byte**)pointers;
            array.m_nNumStrings = tags.Count;
            SteamUGC.SetItemTags(update, &array, false);
        }
        finally
        {
            foreach (var h in handles)
                if (h.IsAllocated)
                    h.Free();
        }
    }

    // The upload's progress, each frame until it's done.
    private static IDisposable WatchProgress(ulong update, Task done, Action<float> progress)
    {
        var watch = new ProgressWatch(update, done, progress);
        s_watches.Add(watch);
        return watch;
    }

    private static readonly List<ProgressWatch> s_watches = new();

    private sealed class ProgressWatch(ulong update, Task done, Action<float> progress) : IDisposable
    {
        public unsafe void Frame()
        {
            ulong sent = 0, total = 0;
            SteamUGC.GetItemUpdateProgress(update, &sent, &total);
            if (total > 0)
                progress((float)sent / total);
            if (done.IsCompleted)
                Dispose();
        }

        public void Dispose() => s_watches.Remove(this);
    }

    // Each frame (Steam.Frame): uploads' progress.
    internal static void Frame()
    {
        for (int i = s_watches.Count - 1; i >= 0; --i)
            s_watches[i].Frame();
    }

    internal static void Stop() => s_watches.Clear();
}

/// <summary>A Workshop item as a query found it.</summary>
public sealed record SteamWorkshopItem(ulong Id, string Title, string Description, string Tags, SteamPlayer Owner, uint VotesUp, uint VotesDown,
                                       float Score, string PreviewUrl, ulong Size);
