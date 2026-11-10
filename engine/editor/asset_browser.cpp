#include "editor/asset_browser.h"

#include "app/process.h"
#include "assets/acoustic_material.h"
#include "assets/material_asset.h"
#include "assets/scene_pak.h"
#include "assets/sound_asset.h"
#include "audio/clip_cache.h"
#include "core/json.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/profile.h"
#include "project/asset_library.h"
#include "ui/ui_layer.h"

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace fx {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxItems = 5000;    // keeps editor.assets well under cef-lite's 1 MB message limit
constexpr size_t kMaxLibraryItems = 1500;  // each library's (on top)
constexpr uint64_t kRescanNs = 1'000'000'000ull;
constexpr int kImageSize = 256;       // editor.assets.read: the longest side

// The page's strings are UTF-8; std::filesystem would read a plain std::string in the ANSI code page.
fs::path toPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string fromPath(const fs::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}
std::string nativeUtf8(const fs::path& p)
{
    const std::u8string s = p.lexically_normal().make_preferred().u8string();
    return std::string(s.begin(), s.end());
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

const char* kindOf(const fs::path& p, bool folder)
{
    if (folder)
        return "folder";
    const std::string ext = lower(fromPath(p.extension()));
    if (ext == ".fpak") return "scene";
    if (ext == ".gltf" || ext == ".glb") return "model";
    if (ext == ".fmat") return "material";
    if (ext == ".fprefab") return "prefab";
    if (ext == ".fsound") return "sound";
    if (ext == ".fgraph") return "animgraph";
    if (ext == ".wav" || ext == ".ogg") return "clip";
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") return "image";
    if (ext == ".cs") return "script";
    if (ext == ".falcon") return "project";
    if (ext == ".json") return "json";
    if (ext == ".fvt" || ext == ".bin") return "data";
    return "file";
}

int64_t secondsOf(fs::file_time_type t)
{
    using namespace std::chrono;
#if defined(_LIBCPP_VERSION)
    // (libc++ has no clock_cast; its file_clock counts from the Unix epoch, as system_clock does)
    return duration_cast<seconds>(t.time_since_epoch()).count();
#else
    return duration_cast<seconds>(clock_cast<system_clock>(t).time_since_epoch()).count();
#endif
}

uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

std::string base64(const uint8_t* data, size_t size)
{
    static const char* const kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t n = uint32_t(data[i]) << 16 | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0u) | (i + 2 < size ? data[i + 2] : 0u);
        out += kTable[n >> 18 & 63];
        out += kTable[n >> 12 & 63];
        out += i + 1 < size ? kTable[n >> 6 & 63] : '=';
        out += i + 2 < size ? kTable[n & 63] : '=';
    }
    return out;
}

// The companions of scene `stem` in a folder: what Terrain::dirFor / SceneEdits::pathFor /
// audioBakeDirFor / navMeshPathFor / cutBakePathFor and the cooker put next to its .fpak.
const char* const kCompanionSuffixes[] = {".fvt", ".edits.json", ".terrain", ".audio", ".nav", ".cuts", ".fpak.tmp", ".fvt.tmp"};

}  // namespace

void AssetBrowser::init(const Init& init)
{
    ui_ = init.ui;
    pak_ = init.pak;
    project_ = init.project;
    root_ = project_ ? project_->dir : fromPath(toPath(init.sceneFolder).lexically_normal());
    while (root_.size() > 1 && root_.back() == '/')
        root_.pop_back();
    currentScene_ = init.currentScene;
    recycle_ = init.recycle;
    notice_ = init.notice;
    scenePath_ = init.scenePath;
    cameraOf_ = init.cameraOf;
    window_ = init.window;
    libraryFolderChanged_ = init.libraryFolderChanged;
}

AssetBrowser::~AssetBrowser()
{
    stopFalcon(previewProcess_);
    std::error_code ec;
    if (previewProcess_)
        fs::remove(toPath(jobsFile_), ec);
}

AssetBrowser::ScanInput AssetBrowser::scanInput() const
{
    ScanInput in;
    in.root = root_;
    in.project = project_ != nullptr;
    if (project_) {
        in.startup = project_->startupScene;
        in.scenes = project_->scenes;
    }
    in.current = currentScene_;
    for (const char* prefix : {asset_library::kFalcon, asset_library::kUser})
        if (const std::string folder = asset_library::absolute(prefix); !folder.empty())
            in.libraries.push_back({prefix, folder});
    return in;
}

AssetBrowser::Listing AssetBrowser::scan(const ScanInput& in)
{
    FX_ZONE_CAT("AssetBrowser::scan", Editor);
    const std::unordered_set<std::string> projectScenes(in.scenes.begin(), in.scenes.end());
    struct Entry {
        fs::path path;  // relative
        bool folder = false;
        uint64_t size = 0;
        int64_t modified = 0;
    };
    std::vector<Entry> entries;
    Listing out;
    std::error_code ec;
    const fs::path root = toPath(in.root);
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        Entry e;
        e.path = it->path().lexically_relative(root);
        std::error_code entryError;  // (one unreadable entry must not end the walk)
        e.folder = it->is_directory(entryError);
        e.size = e.folder ? 0 : it->file_size(entryError);
        const fs::file_time_type written = it->last_write_time(entryError);
        e.modified = entryError ? 0 : secondsOf(written);
        out.signature = mix(mix(mix(out.signature, std::hash<std::string>()(fromPath(e.path))), e.size), uint64_t(e.modified));
        entries.push_back(std::move(e));
        if (entries.size() > kMaxItems * 4)  // a huge folder (a whole drive?): stop walking
            break;
    }
    // A scene's companions are part of it; temporary files are nobody's.
    std::unordered_set<std::string> companions;
    std::unordered_map<std::string, uint64_t> sceneData;  // .fpak -> its .fvt's size
    for (const Entry& e : entries) {
        if (e.folder || lower(fromPath(e.path.extension())) != ".fpak")
            continue;
        const std::string base = fromPath(e.path.parent_path() / e.path.stem());
        for (const char* suffix : kCompanionSuffixes)
            companions.insert(base + suffix);
    }
    for (const Entry& e : entries)
        if (fromPath(e.path).ends_with(".fvt") && companions.contains(fromPath(e.path)))
            sceneData[fromPath(e.path.parent_path() / e.path.stem()) + ".fpak"] = e.size;
    for (const Entry& e : entries) {
        const std::string rel = fromPath(e.path);
        // Inside a companion folder (a scene's .terrain/)?
        bool skip = companions.contains(rel) || rel.ends_with(".tmp");
        for (fs::path p = e.path.parent_path(); !skip && !p.empty(); p = p.parent_path())
            skip = companions.contains(fromPath(p));
        if (skip)
            continue;
        if (out.items.size() >= kMaxItems) {
            out.truncated = true;
            break;
        }
        Item item;
        item.path = rel;
        item.kind = kindOf(e.path, e.folder);
        item.size = e.size;
        item.modified = e.modified;
        const std::string first = fromPath(*e.path.begin());
        if ((in.project && first == "Cache") || first.starts_with(".") || fromPath(e.path.filename()).starts_with("."))
            item.flags |= kHidden;
        if (std::strcmp(item.kind, "scene") == 0) {
            if (auto it = sceneData.find(rel); it != sceneData.end())
                item.size += it->second;
            if (rel == in.startup)
                item.flags |= kStartup;
            if (rel == in.current)
                item.flags |= kCurrent;
            if (projectScenes.contains(rel))
                item.flags |= kInProject;
        }
        out.items.push_back(std::move(item));
    }
    // The libraries: their own folders, then what is in them ("@falcon/Wood/Oak.fmat").
    for (const auto& [prefix, folder] : in.libraries) {
        const uint32_t flags = kLibrary | (asset_library::readOnly(prefix) ? kReadOnly : 0u);
        out.items.push_back({prefix, "folder", 0, 0, flags});
        out.signature = mix(out.signature, std::hash<std::string>()(prefix + folder));
        const fs::path lib = toPath(folder);
        size_t count = 0;
        std::error_code walkError;
        for (fs::recursive_directory_iterator it(lib, fs::directory_options::skip_permission_denied, walkError), end;
             !walkError && it != end; it.increment(walkError)) {
            if (++count > kMaxLibraryItems) {
                out.truncated = true;
                break;
            }
            const fs::path rel = it->path().lexically_relative(lib);
            std::error_code entryError;
            Item item;
            const bool folderItem = it->is_directory(entryError);
            item.path = prefix + "/" + fromPath(rel);
            item.kind = kindOf(rel, folderItem);
            item.size = folderItem ? 0 : it->file_size(entryError);
            const fs::file_time_type written = it->last_write_time(entryError);
            item.modified = entryError ? 0 : secondsOf(written);
            item.flags = flags | (fromPath(rel.filename()).starts_with(".") || item.path.ends_with(".tmp") ? kHidden : 0u);
            out.signature = mix(mix(mix(out.signature, std::hash<std::string>()(item.path)), item.size), uint64_t(item.modified));
            out.items.push_back(std::move(item));
        }
    }
    out.stale = in.project && !in.current.empty() && modelsChanged(in.root, in.current);
    out.signature = mix(out.signature, out.stale);
    return out;
}

void AssetBrowser::send(const Listing& listing)
{
    sentSignature_ = listing.signature;
    JsonWriter w;
    w.beginObject().value("type", "editor.assets").value("root", root_).value("project", project_ != nullptr)
        .value("truncated", listing.truncated).value("stale", listing.stale).beginArray("items");
    for (const Item& i : listing.items) {
        w.beginArray().value(nullptr, i.path).value(nullptr, i.kind).value(nullptr, i.size).value(nullptr, i.modified)
            .value(nullptr, i.flags).endArray();
    }
    w.endArray().beginArray("meshes");
    if (pak_) {
        std::unordered_set<uint32_t> builtin(std::begin(pak_->header().builtinMeshes), std::end(pak_->header().builtinMeshes));
        for (uint32_t m = 0; m < pak_->cookedMeshCount(); ++m) {
            const std::string name = pak_->meshName(m);
            if (name.find('~') != std::string::npos)
                continue;  // (a mesh displaced by a material: drawn for entities with both, not placed itself)
            const std::string source = pak_->meshSource(m);
            const char* group = builtin.contains(m) ? "shape" : !source.empty() ? "model" : name.starts_with("rock_") ? "rock" : "scene";
            w.beginArray().value(nullptr, name).value(nullptr, group).value(nullptr, source).endArray();
        }
    }
    w.endArray().beginArray("libraries");
    for (const char* prefix : {asset_library::kFalcon, asset_library::kUser})
        if (const std::string folder = asset_library::absolute(prefix); !folder.empty())
            w.beginObject().value("prefix", prefix).value("name", asset_library::readOnly(prefix) ? "Falcon" : "My Library")
                .value("folder", nativeUtf8(toPath(folder))).value("readOnly", asset_library::readOnly(prefix)).endObject();
    w.endArray().endObject();
    ui_->send(w.str());
}

void AssetBrowser::rescanNow()
{
    scanning_ = {};  // an older scan's answer would undo what an operation just did (waits for it)
    lastScanNs_ = SDL_GetTicksNS();
    send(scan(scanInput()));
}

void AssetBrowser::dropped(const std::vector<std::string>& paths) { done("import", importFiles(paths, folder_)); }

using asset_library::gltfDependencies;

std::vector<std::string> AssetBrowser::importFiles(const std::vector<std::string>& sources, const std::string& folder)
{
    std::vector<std::string> made;
    // Into the page's folder when it is in Assets/ (or My Library), else Assets/ itself.
    const bool library = asset_library::isLibrary(folder) && !asset_library::readOnly(folder);
    if (!project_ && !library) {
        fail("Files can be imported into a project (or My Library) only");
        return made;
    }
    const std::string dest = library || folder == "Assets" || folder.starts_with("Assets/") ? folder : "Assets";
    const std::string destAbsolute = resolve(dest);
    if (destAbsolute.empty())
        return made;
    std::error_code ec;
    const fs::path destDir = toPath(destAbsolute);
    fs::create_directories(destDir, ec);
    for (const std::string& source : sources) {
        const fs::path src = toPath(source);
        if (!fs::exists(src, ec)) {
            fail("Nothing to import at " + source);
            continue;
        }
        const std::string stem = fromPath(src.stem()), ext = fromPath(src.extension());
        std::string relative;
        if (fs::is_directory(src, ec)) {
            const std::string name = uniqueName(fromPath(destDir), fromPath(src.filename()), "");
            fs::copy(src, destDir / toPath(name), fs::copy_options::recursive, ec);
            relative = dest + "/" + name;
        } else if (lower(ext) == ".gltf") {
            const std::vector<std::string> deps = gltfDependencies(fromPath(src));
            if (deps.empty()) {
                const std::string name = uniqueName(fromPath(destDir), stem, ext);
                fs::copy_file(src, destDir / toPath(name), ec);
                relative = dest + "/" + name;
            } else {  // a folder of its own, its files beside it as the .gltf names them
                const std::string dir = uniqueName(fromPath(destDir), stem, "");
                fs::create_directories(destDir / toPath(dir), ec);
                fs::copy_file(src, destDir / toPath(dir) / src.filename(), ec);
                for (const std::string& dep : deps) {
                    std::error_code depError;
                    const fs::path to = destDir / toPath(dir) / toPath(dep);
                    fs::create_directories(to.parent_path(), depError);
                    fs::copy_file(src.parent_path() / toPath(dep), to, fs::copy_options::overwrite_existing, depError);
                    if (depError)
                        fail("The model's file " + dep + " could not be copied: " + depError.message());
                }
                relative = dest + "/" + dir + "/" + fromPath(src.filename());
            }
        } else {
            const std::string name = uniqueName(fromPath(destDir), stem, ext);
            fs::copy_file(src, destDir / toPath(name), ec);
            relative = dest + "/" + name;
        }
        if (ec) {
            fail("Could not import " + source + ": " + ec.message());
            ec.clear();
            continue;
        }
        made.push_back(relative);
    }
    if (!made.empty() && notice_)
        notice_("Imported " + std::to_string(made.size()) + " item" + (made.size() == 1 ? "" : "s") + " into " + dest, false);
    rescanNow();
    return made;
}

void AssetBrowser::update()
{
    // The file picker's answer.
    {
        std::vector<std::string> picked;
        std::string library;
        {
            std::lock_guard<std::mutex> lock(pickedMutex_);
            if (pickedDone_) {
                picked.swap(picked_);
                pickedDone_ = false;
            }
            library.swap(pickedLibrary_);
        }
        if (!picked.empty())
            done("import", importFiles(picked, folder_));
        if (!library.empty())
            setLibraryFolder(library);
    }
    // Previews: a finished run, the next batch, a saved scene's new picture.
    if (int code = 0; previewProcess_ && falconFinished(previewProcess_, code)) {
        previewProcess_ = nullptr;
        finishPreviews(code == 0);
    }
    if (!sceneCapture_.empty()) {
        const std::string key = "scene:" + currentScene_;
        std::error_code ec;
        if (fs::exists(toPath(sceneCapture_), ec)) {
            index_[key] = {fromPath(toPath(sceneCapture_).filename()), ""};
            saveIndex();
            sendPreview(key, sceneCapture_);
        }
        sceneCapture_.clear();
    }
    startPreviews();

    if (scanning_.valid() && scanning_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Listing listing = scanning_.get();
        if (listing.signature != sentSignature_)
            send(listing);
    }
    const uint64_t now = SDL_GetTicksNS();
    if (watching_ && !scanning_.valid() && now - lastScanNs_ > kRescanNs) {
        lastScanNs_ = now;
        scanning_ = std::async(std::launch::async, [in = scanInput()] { return scan(in); });
    }
}

void AssetBrowser::done(const char* op, const std::vector<std::string>& paths)
{
    JsonWriter w;
    w.beginObject().value("type", "editor.assets.done").value("op", op).beginArray("paths");
    for (const std::string& p : paths)
        w.value(nullptr, p);
    w.endArray().endObject();
    ui_->send(w.str());
}

void AssetBrowser::fail(const std::string& message)
{
    if (notice_)
        notice_(message, true);
}

std::string AssetBrowser::resolve(const std::string& relative)
{
    if (asset_library::isLibrary(relative)) {
        std::string path = asset_library::absolute(relative);
        if (path.empty())
            fail("Not in the library: " + relative);
        return path;
    }
    const fs::path rel = toPath(relative).lexically_normal();
    if (rel.is_absolute() || rel.has_root_name() || (!rel.empty() && fromPath(*rel.begin()) == "..")) {
        fail("Not in the project: " + relative);
        return {};
    }
    return fromPath((toPath(root_) / rel).lexically_normal());
}

bool AssetBrowser::exists(const std::string& path) const
{
    std::error_code ec;
    if (asset_library::isLibrary(path))
        return fs::exists(toPath(asset_library::absolute(path)), ec);
    return fs::exists(toPath(root_) / toPath(path), ec);
}

bool AssetBrowser::isScene(const std::string& relative) const { return lower(relative).ends_with(".fpak"); }

bool AssetBrowser::canWriteIn(const std::string& folder)
{
    if (asset_library::isLibrary(folder)) {
        if (asset_library::readOnly(folder)) {
            fail("Falcon's library is read-only: use what is in it, or add your own to My Library");
            return false;
        }
        return !resolve(folder).empty();
    }
    if (!project_) {
        fail("Files can be changed in a project only (open the scene from the launcher)");
        return false;
    }
    return !resolve(folder).empty();
}

void AssetBrowser::setLibraryFolder(const std::string& folder)
{
    const std::string absolute = fromPath(toPath(folder).lexically_normal());
    asset_library::setRoots(asset_library::falconRoot(), absolute);
    if (libraryFolderChanged_)
        libraryFolderChanged_(absolute);
    if (notice_)
        notice_("My Library is now " + nativeUtf8(toPath(absolute)), false);
    rescanNow();
}

bool AssetBrowser::editable(const std::string& relative)
{
    if (asset_library::isLibrary(relative)) {
        if (relative == asset_library::kFalcon || relative == asset_library::kUser) {
            fail("A library's own folder can't be changed here");
            return false;
        }
        return canWriteIn(relative);
    }
    if (!project_) {
        fail("Files can be changed in a project only (open the scene from the launcher)");
        return false;
    }
    if (relative.empty() || relative == "project.falcon") {
        fail(relative.empty() ? "The project's folder itself can't be changed here" : "project.falcon is the project itself");
        return false;
    }
    if (relative == currentScene_ || currentScene_.starts_with(relative + "/")) {
        fail(relative == currentScene_ ? "The open scene can't be changed: open another scene first"
                                       : relative + " holds the open scene: open another scene first");
        return false;
    }
    return !resolve(relative).empty();
}

std::vector<std::string> AssetBrowser::companions(const std::string& fpakAbsolute) const
{
    std::vector<std::string> out;
    const fs::path p = toPath(fpakAbsolute);
    std::error_code ec;
    for (const char* suffix : kCompanionSuffixes) {
        const fs::path c = p.parent_path() / (p.stem().u8string() + std::u8string(suffix, suffix + std::strlen(suffix)));
        if (fs::exists(c, ec))
            out.push_back(fromPath(c));
    }
    return out;
}

void AssetBrowser::projectRenamed(const std::string& from, const std::string& to, bool folder, bool& projectChanged)
{
    if (!project_)
        return;
    const auto map = [&](std::string& s) {
        if (!folder && s == from)
            s = to;
        else if (folder && s.starts_with(from + "/"))
            s = to + s.substr(from.size());
        else
            return false;
        return true;
    };
    bool changed = false;
    for (std::string& s : project_->scenes)
        changed |= map(s);
    changed |= map(project_->startupScene);
    if (changed) {
        std::string error;
        if (!saveProject(*project_, error))
            fail(error);
        projectChanged = true;
    }
}

bool AssetBrowser::relocate(const std::string& from, const std::string& to, bool& projectChanged)
{
    const std::string a = resolve(from), b = resolve(to);
    if (a.empty() || b.empty())
        return false;
    std::error_code ec;
    const bool caseOnly = lower(a) == lower(b);  // Windows: "rock.png" -> "Rock.png" is the same file
    if (fs::exists(toPath(b), ec) && !caseOnly) {
        fail("There is a " + fromPath(toPath(to).filename()) + " there already");
        return false;
    }
    const bool folder = fs::is_directory(toPath(a), ec);
    // A scene's companions take its new name; all of them are checked before anything moves.
    std::vector<std::pair<fs::path, fs::path>> moves{{toPath(a), toPath(b)}};
    if (!folder && isScene(from)) {
        const std::u8string fromStem = toPath(a).stem().u8string(), toStem = toPath(b).stem().u8string();
        for (const std::string& c : companions(a)) {
            const std::u8string name = toPath(c).filename().u8string();
            const fs::path dest = toPath(b).parent_path() / (toStem + name.substr(fromStem.size()));
            if (fs::exists(dest, ec) && !caseOnly) {
                fail("There is a " + fromPath(dest.filename()) + " there already");
                return false;
            }
            moves.push_back({toPath(c), dest});
        }
    }
    for (const auto& [src, dest] : moves) {
        fs::rename(src, dest, ec);
        if (ec) {
            fail("Could not move " + fromPath(src.filename()) + ": " + ec.message());
            return false;
        }
    }
    projectRenamed(from, to, folder, projectChanged);
    return true;
}

std::string AssetBrowser::uniqueName(const std::string& folderAbsolute, const std::string& stem, const std::string& ext) const
{
    std::error_code ec;
    for (uint32_t n = 1;; ++n) {
        const std::string name = n == 1 ? stem + ext : stem + " (" + std::to_string(n) + ")" + ext;
        if (!fs::exists(toPath(folderAbsolute) / toPath(name), ec))
            return name;
    }
}

bool AssetBrowser::remove(const std::vector<std::string>& absolutePaths)
{
    if (!recycle_) {  // tests: inside their own folder, outright
        std::error_code ec;
        for (const std::string& p : absolutePaths)
            fs::remove_all(toPath(p), ec);
        return !ec;
    }
    // The Recycle Bin / Trash.
    std::vector<fs::path> paths;
    for (const std::string& p : absolutePaths)
        paths.push_back(toPath(p));
    return platform::moveToTrash(paths);
}

void AssetBrowser::readImage(const std::string& relative)
{
    std::string path = resolve(relative);
    if (path.empty())
        return;
    if (lower(relative).ends_with(".fmat")) {  // (a library's: its colour texture is its picture)
        MaterialAsset mat;
        std::string error;
        if (!readMaterialAsset(path, mat, error) || mat.textures[0].empty())
            return;
        const std::string& colour = mat.textures[0];
        path = asset_library::isLibrary(colour) ? asset_library::absolute(colour) : fromPath(toPath(root_) / toPath(colour));
    }
    size_t size = 0;
    void* file = SDL_LoadFile(path.c_str(), &size);
    int w = 0, h = 0, n = 0;
    stbi_uc* px = file ? stbi_load_from_memory(static_cast<const stbi_uc*>(file), int(size), &w, &h, &n, 4) : nullptr;
    SDL_free(file);
    if (!px)
        return fail("Can't read the image " + relative);
    const float scale = std::min(1.0f, float(kImageSize) / float(std::max(w, h)));
    const int ow = std::max(1, int(std::lround(w * scale))), oh = std::max(1, int(std::lround(h * scale)));
    std::vector<uint8_t> small(size_t(ow) * oh * 4);
    stbir_resize_uint8_srgb(px, w, h, w * 4, small.data(), ow, oh, ow * 4, STBIR_RGBA);
    stbi_image_free(px);
    std::string url;
    if (SDL_Surface* s = SDL_CreateSurfaceFrom(ow, oh, SDL_PIXELFORMAT_RGBA32, small.data(), ow * 4)) {
        if (SDL_IOStream* io = SDL_IOFromDynamicMem()) {
            if (SDL_SavePNG_IO(s, io, false)) {
                const auto* bytes = static_cast<const uint8_t*>(
                    SDL_GetPointerProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr));
                if (bytes)
                    url = "data:image/png;base64," + base64(bytes, size_t(SDL_GetIOSize(io)));
            }
            SDL_CloseIO(io);
        }
        SDL_DestroySurface(s);
    }
    JsonWriter jw;
    jw.beginObject().value("type", "editor.assets.image").value("path", relative).value("url", url)
        .value("width", uint32_t(w)).value("height", uint32_t(h)).endObject();
    ui_->send(jw.str());
}

bool AssetBrowser::handle(const std::string& type, const Json& m)
{
    const std::string op = type.substr(std::strlen("editor.assets."));
    std::vector<std::string> paths;
    if (const Json* list = m.get("paths"))
        for (const Json& p : list->array)
            paths.push_back(p.str);
    bool projectChanged = false;
    std::vector<std::string> made;
    std::error_code ec;

    if (op == "watch") {
        watching_ = m.getBool("on");
        if (watching_) {
            sentSignature_ = 0;
            rescanNow();
        }
        return false;
    }
    if (op == "refresh") {
        rescanNow();
        return false;
    }
    if (op == "read") {
        readImage(m.getString("path"));
        return false;
    }
    if (op == "folder") {
        folder_ = m.getString("path");
        return false;
    }
    if (op == "readMaterial") {
        const std::string path = m.getString("path");
        const std::string file = resolve(path);
        MaterialAsset mat;
        std::string error;
        if (file.empty() || !readMaterialAsset(file, mat, error)) {
            fail(error.empty() ? "Not a material: " + path : error);
            return false;
        }
        JsonWriter w;
        w.beginObject().value("type", "editor.assets.material").value("path", path).beginObject("material")
            .value("name", mat.name).beginArray("baseColor");
        for (const float c : mat.baseColor)
            w.value(nullptr, double(c));
        w.endArray().value("metallic", double(mat.metallic)).value("roughness", double(mat.roughness))
            .value("roughnessMin", double(mat.roughnessMin)).beginArray("emissive");
        for (const float c : mat.emissive)
            w.value(nullptr, double(c));
        w.endArray().value("emissiveStrength", double(mat.emissiveStrength)).value("normalScale", double(mat.normalScale))
            .value("occlusionStrength", double(mat.occlusionStrength)).value("alpha", mat.alpha).value("alphaCutoff", double(mat.alphaCutoff))
            .value("doubleSided", mat.doubleSided).value("displacement", double(mat.displacement))
            .value("heightCenter", double(mat.heightCenter)).value("displacementDetail", double(mat.displacementDetail))
            .value("parallax", double(mat.parallax)).value("refraction", double(mat.refraction)).value("ior", double(mat.ior))
            .beginArray("size").value(nullptr, double(mat.size[0])).value(nullptr, double(mat.size[1])).endArray()
            .value("readOnly", asset_library::readOnly(path))
            .value("shader", mat.shader).value("vertexBound", double(mat.vertexBound)).value("vertexDistance", double(mat.vertexDistance))
            .beginObject("textures");
        for (int s = 0; s < 4; ++s)
            w.value(MaterialAsset::kTextureSlots[s], mat.textures[s]);
        w.value("height", mat.height);
        w.endObject();
        writeAcousticJson(w, "acoustic", mat.acousticPreset, mat.acoustic);
        w.beginObject("surface");
        writeSurfaceFields(w, mat.surfacePreset, mat.surface);
        w.endObject().endObject();
        writeAcousticPresets(w, "acousticPresets");
        writeSurfacePresets(w, "surfacePresets");
        w.endObject();
        ui_->send(w.str());
        return false;
    }
    if (op == "readSound") {  // a .fsound's fields, clips and the field table
        const std::string path = m.getString("path");
        const std::string file = resolve(path);
        SoundAsset sound;
        std::string error;
        if (file.empty() || !readSoundAsset(file, sound, error)) {
            fail(error.empty() ? "Not a sound: " + path : error);
            return false;
        }
        JsonWriter w;
        w.beginObject().value("type", "editor.assets.sound").value("path", path).beginObject("sound");
        writeSoundAsset(w, sound);
        w.endObject();
        writeSoundSchema(w, "schema");
        w.beginArray("presets");
        for (const auto& [name, display] : soundPresets())
            w.beginObject().value("value", name).value("label", display).endObject();
        w.endArray().endObject();
        ui_->send(w.str());
        return false;
    }
    if (op == "readClip") {  // a clip's length and channels (decoded once to know)
        const std::string path = m.getString("path");
        const std::string file = resolve(path);
        AudioClip clip;
        std::string error;
        JsonWriter w;
        w.beginObject().value("type", "editor.assets.clip").value("path", path);
        if (file.empty() || !decodeAudioFile(file, clip, error))
            w.value("error", error.empty() ? std::string("Not a clip") : error);
        else
            w.value("seconds", double(clip.frames) / double(kAudioSampleRate)).value("channels", clip.channels);
        w.endObject();
        ui_->send(w.str());
        return false;
    }
    if (op == "importFiles") {
        done("import", importFiles(paths, m.getString("folder", folder_.c_str())));
        return false;
    }
    if (op == "import") {
        folder_ = m.getString("folder", folder_.c_str());
        if (!project_ && !(asset_library::isLibrary(folder_) && !asset_library::readOnly(folder_))) {
            fail("Files can be imported into a project (or My Library) only");
            return false;
        }
        static const SDL_DialogFileFilter kFilters[] = {
            {"Models, images and sounds", "gltf;glb;png;jpg;jpeg;tga;bmp;wav;ogg"}, {"Models (glTF)", "gltf;glb"}, {"Images", "png;jpg;jpeg;tga;bmp"},
            {"Sounds", "wav;ogg"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(
            [](void* user, const char* const* files, int) {
                auto* self = static_cast<AssetBrowser*>(user);
                std::lock_guard<std::mutex> lock(self->pickedMutex_);
                for (const char* const* f = files; f && *f; ++f)
                    self->picked_.push_back(*f);
                self->pickedDone_ = true;
            },
            this, window_, kFilters, int(std::size(kFilters)), nullptr, true);
        return false;
    }
    if (op == "previews" || op == "regenerate") {
        std::vector<std::string> keys;
        if (const Json* list = m.get("keys"))
            for (const Json& k : list->array)
                keys.push_back(k.str);
        if (op == "regenerate") {
            loadIndex();
            for (const std::string& k : keys) {
                index_.erase(k);
                failed_.erase(k);
            }
        }
        requestPreviews(keys);
        return false;
    }
    if (op == "reveal" || op == "copyPath") {
        const std::string path = resolve(m.getString("path"));
        if (path.empty())
            return false;
        if (m.getString("path") == asset_library::kUser)
            fs::create_directories(toPath(path), ec);  // (My Library before anything is in it)
        if (op == "copyPath") {
            SDL_SetClipboardText(nativeUtf8(toPath(path)).c_str());
            if (notice_)
                notice_("Copied " + nativeUtf8(toPath(path)), false);
        } else {
            platform::revealInFileBrowser(toPath(path));
        }
        return false;
    }

    if (op == "libraryFolder") {
        if (const std::string path = m.getString("path"); !path.empty()) {
            setLibraryFolder(path);
            return false;
        }
        const std::string start = nativeUtf8(toPath(asset_library::userRoot()));
        SDL_ShowOpenFolderDialog(
            [](void* user, const char* const* files, int) {
                auto* self = static_cast<AssetBrowser*>(user);
                std::lock_guard<std::mutex> lock(self->pickedMutex_);
                if (files && *files)
                    self->pickedLibrary_ = *files;
            },
            this, window_, start.empty() ? nullptr : start.c_str(), false);
        return false;
    }
    if (op == "use") {  // a library item's copy in the project (made now unless it is there)
        const std::string path = m.getString("path");
        JsonWriter w;
        w.beginObject().value("type", "editor.assets.used").value("id", m.getNumber("id")).value("path", path);
        if (!project_) {
            fail("Library items are used in a project");
        } else {
            bool copied = false;
            std::string error;
            const std::string rel = asset_library::useInProject(root_, path, copied, error);
            if (rel.empty()) {
                fail(error);
            } else {
                w.value("project", rel).value("copied", copied);
                if (copied && notice_)
                    notice_("Copied " + fromPath(toPath(path).filename()) + " into " + fromPath(toPath(rel).parent_path()) +
                                (lower(rel).ends_with(".fmat") ? " (its textures show after Reload Scene)" : ""), false);
                if (copied)
                    rescanNow();
            }
        }
        w.endObject();
        ui_->send(w.str());
        return false;
    }
    if (op == "addToLibrary") {
        std::string into = m.getString("folder", asset_library::kUser);
        if (!asset_library::isLibrary(into) || asset_library::readOnly(into))
            into = asset_library::kUser;
        std::vector<std::string> files;  // (images, models, clips: copied as they are)
        for (const std::string& path : paths) {
            const std::string kind = kindOf(toPath(path), false);
            if (asset_library::isLibrary(path)) {
                fail(path + " is in a library already");
            } else if (!project_) {
                fail("Items are added to the library from a project");
            } else if (resolve(path).empty()) {
            } else if (kind == "material") {
                std::string error;
                const std::string added = asset_library::addMaterial(root_, path, into, error);
                if (added.empty())
                    fail(error);
                else
                    made.push_back(added);
            } else if (kind == "image" || kind == "model" || kind == "clip") {
                files.push_back(resolve(path));
            } else {
                fail(fromPath(toPath(path).filename()) + " can't go into the library (materials, images, models and clips can)");
            }
        }
        if (!files.empty())
            for (const std::string& p : importFiles(files, into))
                made.push_back(p);
        if (!made.empty() && notice_)
            notice_("Added " + std::to_string(made.size()) + " item" + (made.size() == 1 ? "" : "s") + " to My Library", false);
        rescanNow();
        done("addToLibrary", made);
        return false;
    }

    if (op == "newMaterial") {
        const std::string f = m.getString("folder", folder_.c_str());
        const bool library = asset_library::isLibrary(f) && !asset_library::readOnly(f);
        if (!project_ && !library) {
            fail("Materials are made in a project (or My Library)");
            return false;
        }
        const std::string dest = library || f == "Assets" || f.starts_with("Assets/") ? f : "Assets";
        const std::string dir = resolve(dest);
        if (dir.empty())
            return false;
        fs::create_directories(toPath(dir), ec);
        const std::string name = uniqueName(dir, m.getString("name", "New Material"), ".fmat");
        MaterialAsset mat;
        mat.name = fromPath(toPath(name).stem());
        std::string error;
        if (!writeMaterialAsset(dir + "/" + name, mat, error))
            fail(error);
        else
            made.push_back(dest + "/" + name);
    } else if (op == "newSound") {
        if (!project_) {
            fail("Sounds are made in a project");
            return false;
        }
        const std::string f = m.getString("folder", folder_.c_str());
        const std::string dest = f == "Assets" || f.starts_with("Assets/") ? f : "Assets";
        const std::string dir = resolve(dest);
        if (dir.empty())
            return false;
        fs::create_directories(toPath(dir), ec);
        const std::string name = uniqueName(dir, m.getString("name", "New Sound"), ".fsound");
        SoundAsset sound;
        sound.name = fromPath(toPath(name).stem());
        // Made from clips (selected in the panel): they are its variations.
        if (const Json* clips = m.get("clips"))
            for (const Json& c : clips->array)
                if (isAudioClipPath(c.str))
                    sound.clips.push_back(c.str);
        std::string error;
        if (!writeSoundAsset(dir + "/" + name, sound, error))
            fail(error);
        else
            made.push_back(dest + "/" + name);
    } else if (op == "newFolder") {
        const std::string parent = m.getString("parent");
        if (!canWriteIn(parent))
            return false;
        const std::string dir = resolve(parent);
        const std::string name = uniqueName(dir, m.getString("name", "New Folder"), "");
        fs::create_directories(toPath(dir) / toPath(name), ec);
        if (ec)
            fail("Could not create the folder: " + ec.message());
        else
            made.push_back(parent.empty() ? name : parent + "/" + name);
    } else if (op == "rename") {
        const std::string path = m.getString("path");
        std::string name = m.getString("name");
        while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
            name.pop_back();
        while (!name.empty() && name.front() == ' ')
            name.erase(0, 1);
        if (name.empty() || name.find_first_of("<>:\"/\\|?*") != std::string::npos)
            fail("A name can't be empty or contain < > : \" / \\ | ? *");
        else if (editable(path)) {
            if (isScene(path) && !isScene(name))
                name += ".fpak";
            const std::string parent = fromPath(toPath(path).parent_path());
            const std::string to = parent.empty() ? name : parent + "/" + name;
            if (to != path && relocate(path, to, projectChanged))
                made.push_back(to);
        }
    } else if (op == "move") {
        const std::string to = m.getString("to");
        const std::string dest = resolve(to);
        if (!dest.empty() && (to.empty() || fs::is_directory(toPath(dest), ec))) {
            for (const std::string& path : paths) {
                const std::string name = fromPath(toPath(path).filename());
                const std::string target = to.empty() ? name : to + "/" + name;
                const auto rootOf = [](const std::string& p) {
                    return !asset_library::isLibrary(p) ? 0 : asset_library::readOnly(p) ? 1 : 2;
                };
                if (rootOf(path) != rootOf(to)) {
                    fail(rootOf(to) == 2 && rootOf(path) == 0 ? "Add to Library copies a project's item into My Library"
                                                              : "Items move within the project or one library");
                    continue;
                }
                if (target == path || !editable(path))
                    continue;
                if (to == path || to.starts_with(path + "/")) {
                    fail("A folder can't go into itself");
                    continue;
                }
                if (relocate(path, target, projectChanged))
                    made.push_back(target);
            }
        }
    } else if (op == "duplicate") {
        for (const std::string& path : paths) {
            if (!editable(path))
                continue;
            const fs::path src = toPath(resolve(path));
            const bool folder = fs::is_directory(src, ec);
            const std::string ext = folder ? "" : fromPath(src.extension());
            const std::string name = uniqueName(fromPath(src.parent_path()), fromPath(folder ? src.filename() : src.stem()), ext);
            const fs::path dest = src.parent_path() / toPath(name);
            fs::copy(src, dest, fs::copy_options::recursive, ec);
            if (!ec && isScene(path)) {  // with its companions, and into the project
                for (const std::string& c : companions(fromPath(src))) {
                    const std::u8string cname = toPath(c).filename().u8string();
                    fs::copy(toPath(c), dest.parent_path() / (dest.stem().u8string() + cname.substr(src.stem().u8string().size())),
                             fs::copy_options::recursive, ec);
                }
                const std::string parent = fromPath(toPath(path).parent_path());
                project_->scenes.push_back(parent.empty() ? name : parent + "/" + name);
                std::string error;
                if (!saveProject(*project_, error))
                    fail(error);
                projectChanged = true;
            }
            if (ec)
                fail("Could not duplicate " + path + ": " + ec.message());
            else {
                const std::string parent = fromPath(toPath(path).parent_path());
                made.push_back(parent.empty() ? name : parent + "/" + name);
            }
        }
    } else if (op == "delete") {
        std::vector<std::string> doomed;
        std::vector<std::string> scenes = project_ ? project_->scenes : std::vector<std::string>();
        for (const std::string& path : paths) {
            if (!editable(path))
                continue;
            const std::string abs = resolve(path);
            doomed.push_back(abs);
            if (isScene(path))
                for (const std::string& c : companions(abs))
                    doomed.push_back(c);
            std::erase_if(scenes, [&](const std::string& s) { return s == path || s.starts_with(path + "/"); });
        }
        if (project_ && scenes.empty() && !project_->scenes.empty()) {
            fail("A project needs at least one scene");
        } else if (!doomed.empty()) {
            if (!remove(doomed))
                fail("Some files could not be deleted");
            if (project_ && scenes.size() != project_->scenes.size()) {
                project_->scenes = scenes;
                if (std::find(scenes.begin(), scenes.end(), project_->startupScene) == scenes.end())
                    project_->startupScene = scenes.front();
                std::string error;
                if (!saveProject(*project_, error))
                    fail(error);
                projectChanged = true;
            }
        }
    } else {
        FX_WARN("editor: unknown message %s", type.c_str());
        return false;
    }
    rescanNow();
    done(op.c_str(), made);
    return projectChanged;
}

// ---------------------------------------------------------------- previews

namespace {

uint64_t fnv(const void* data, size_t size, uint64_t h = 0xcbf29ce484222325ull)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

std::string hex(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

std::string AssetBrowser::thumbnails() const { return project_ ? root_ + "/Cache/Thumbnails" : std::string(); }

std::string AssetBrowser::previewFile(const std::string& key) const { return thumbnails() + "/" + hex(fnv(key.data(), key.size())) + ".png"; }

std::string AssetBrowser::expectedHash(const std::string& key) const
{
    if (key.starts_with("prefab:")) {  // the file (its meshes and materials: as named there)
        size_t size = 0;
        void* bytes = SDL_LoadFile((root_ + "/" + key.substr(7)).c_str(), &size);
        if (!bytes)
            return {};
        const uint64_t h = fnv(bytes, size);
        SDL_free(bytes);
        return hex(h);
    }
    if (key.starts_with("material:")) {
        // The file and its textures (their sizes and dates).
        const std::string file = root_ + "/" + key.substr(9);
        size_t size = 0;
        void* bytes = SDL_LoadFile(file.c_str(), &size);
        if (!bytes)
            return {};
        uint64_t h = fnv(bytes, size);
        SDL_free(bytes);
        MaterialAsset mat;
        std::string error;
        std::error_code ec;
        if (readMaterialAsset(file, mat, error))
            for (const std::string& t : mat.textures)
                if (!t.empty()) {
                    const fs::path p = toPath(root_ + "/" + t);
                    const uint64_t stamp[2] = {uint64_t(fs::file_size(p, ec)), uint64_t(fs::last_write_time(p, ec).time_since_epoch().count())};
                    h = fnv(stamp, sizeof(stamp), h);
                }
        return hex(h);
    }
    if (key.starts_with("model:") && pak_) {
        // The model's files as they are (a .gltf with its buffers and images); only once it is
        // cooked into the open scene (until then there is nothing to render it from).
        const std::string source = key.substr(6);
        if (!pak_->library(source))
            return {};
        const std::string path = root_ + "/" + source;
        std::vector<std::string> files = {path};
        if (lower(source).ends_with(".gltf"))
            for (const std::string& dep : gltfDependencies(path))
                files.push_back(fromPath(toPath(path).parent_path() / toPath(dep)));
        uint64_t h = 0xcbf29ce484222325ull;
        for (const std::string& f : files) {
            size_t size = 0;
            if (void* bytes = SDL_LoadFile(f.c_str(), &size)) {
                h = fnv(bytes, size, h);
                SDL_free(bytes);
            }
        }
        return hex(h);
    }
    if (!key.starts_with("mesh:") || !pak_)
        return {};
    // The mesh's geometry and material as cooked: unchanged, its preview stands.
    const uint32_t mesh = pak_->findMesh(key.substr(5));
    uint32_t meshCount = 0, clusterCount = 0, materialCount = 0;
    const auto* meshes = pak_->view<fpak::MeshGpu>(fpak::Section::Meshes, &meshCount);
    const auto* clusters = pak_->view<fpak::ClusterGpu>(fpak::Section::Clusters, &clusterCount);
    const auto* materials = pak_->view<fpak::MaterialGpu>(fpak::Section::Materials, &materialCount);
    if (mesh >= meshCount)
        return {};
    const fpak::MeshGpu& m = meshes[mesh];
    uint64_t h = fnv(&m, sizeof(m));
    if (m.clusterOffset + m.clusterCount <= clusterCount) {
        h = fnv(clusters + m.clusterOffset, sizeof(fpak::ClusterGpu) * m.clusterCount, h);
        const uint32_t material = m.clusterCount ? clusters[m.clusterOffset].counts >> 16 : ~0u;
        if (material < materialCount)
            h = fnv(&materials[material], sizeof(fpak::MaterialGpu), h);
    }
    return hex(h);
}

void AssetBrowser::loadIndex()
{
    if (indexLoaded_ || !project_)
        return;
    indexLoaded_ = true;
    size_t size = 0;
    void* text = SDL_LoadFile((thumbnails() + "/index.json").c_str(), &size);
    if (!text)
        return;
    try {
        const Json root = Json::parse(std::string_view(static_cast<const char*>(text), size));
        if (const Json* entries = root.get("entries"))
            for (const auto& [key, e] : entries->object)
                index_[key] = {e.getString("png"), e.getString("hash")};
    } catch (const std::exception& e) {
        FX_WARN("editor: the preview index is malformed (%s); previews render again", e.what());
    }
    SDL_free(text);
}

void AssetBrowser::saveIndex() const
{
    if (!project_)
        return;
    std::error_code ec;
    fs::create_directories(toPath(thumbnails()), ec);
    JsonWriter w;
    w.beginObject().value("format", 1u).beginObject("entries");
    std::vector<std::string> keys;
    for (const auto& [key, e] : index_)
        keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    for (const std::string& key : keys)
        w.beginObject(key.c_str()).value("png", index_.at(key).png).value("hash", index_.at(key).hash).endObject();
    w.endObject().endObject();
    const std::string path = thumbnails() + "/index.json";
    if (SDL_SaveFile((path + ".tmp").c_str(), w.str().data(), w.str().size()))
        fs::rename(toPath(path + ".tmp"), toPath(path), ec);
}

void AssetBrowser::sendPreview(const std::string& key, const std::string& png)
{
    size_t size = 0;
    void* bytes = SDL_LoadFile(png.c_str(), &size);
    if (!bytes)
        return;
    JsonWriter w;
    w.beginObject().value("type", "editor.assets.preview").value("key", key)
        .value("url", "data:image/png;base64," + base64(static_cast<const uint8_t*>(bytes), size)).endObject();
    SDL_free(bytes);
    ui_->send(w.str());
}

void AssetBrowser::requestPreviews(const std::vector<std::string>& keys)
{
    if (!project_)
        return;
    loadIndex();
    std::error_code ec;
    for (const std::string& key : keys) {
        if (!key.starts_with("mesh:") && !key.starts_with("model:") && !key.starts_with("material:") && !key.starts_with("prefab:") &&
            !key.starts_with("scene:"))
            continue;
        const std::string hash = expectedHash(key);
        if (!key.starts_with("scene:") && hash.empty())
            continue;  // not a mesh (or model) of this scene
        const auto it = index_.find(key);
        const std::string png = it == index_.end() ? std::string() : thumbnails() + "/" + it->second.png;
        if (it != index_.end() && it->second.hash == hash && fs::exists(toPath(png), ec)) {
            sendPreview(key, png);
            continue;
        }
        if (queued_.contains(key) || failed_.contains(key))
            continue;
        queued_.insert(key);
        queue_.push_back({key, hash});
    }
}

// One run at a time: the meshes asked for (rendered from the open scene), or one scene.
void AssetBrowser::startPreviews()
{
    if (previewProcess_ || queue_.empty() || !project_)
        return;
    std::error_code ec;
    fs::create_directories(toPath(thumbnails()), ec);
    running_.clear();
    JsonWriter w;
    w.beginObject();
    const auto onScene = [](const std::string& key) {
        return key.starts_with("mesh:") || key.starts_with("model:") || key.starts_with("material:") || key.starts_with("prefab:");
    };
    const bool meshes = std::any_of(queue_.begin(), queue_.end(), [&](const PreviewRequest& r) { return onScene(r.key); });
    std::string scene;
    if (meshes) {
        scene = scenePath_;
        w.value("scene", scene).value("edits", false).value("terrain", false).value("root", root_);
    } else {
        scene = queue_.front().key.substr(6);
        w.value("scene", root_ + "/" + scene).value("edits", true).value("terrain", true);
    }
    w.beginArray("jobs");
    for (auto it = queue_.begin(); it != queue_.end();) {
        const bool mine = meshes ? onScene(it->key) : it->key == "scene:" + scene;
        if (!mine) {
            ++it;
            continue;
        }
        const bool model = it->key.starts_with("model:"), material = it->key.starts_with("material:"), prefab = it->key.starts_with("prefab:");
        w.beginObject().value("kind", !meshes ? "scene" : model ? "model" : material ? "material" : prefab ? "prefab" : "mesh")
            .value("out", previewFile(it->key)).value("size", 256u);
        if (meshes && prefab)
            w.value("prefab", it->key.substr(7));
        else if (meshes && model)
            w.value("model", it->key.substr(6));
        else if (meshes && material)
            w.value("material", it->key.substr(9));
        else if (meshes)
            w.value("mesh", it->key.substr(5));
        else if (const std::string pose = cameraOf_ ? cameraOf_(scene) : std::string(); !pose.empty())
            w.raw("pose", pose);
        w.endObject();
        running_.push_back(*it);
        it = queue_.erase(it);
    }
    w.endArray().endObject();
    jobsFile_ = thumbnails() + "/jobs.json";
    if (!SDL_SaveFile(jobsFile_.c_str(), w.str().data(), w.str().size())) {
        finishPreviews(false);
        return;
    }
    previewProcess_ = startFalconTracked({"--preview", jobsFile_});
    if (!previewProcess_) {
        finishPreviews(false);
        return;
    }
    ++previewRuns_;
    FX_INFO("editor: rendering %zu preview%s", running_.size(), running_.size() == 1 ? "" : "s");
}

void AssetBrowser::finishPreviews(bool ok)
{
    std::error_code ec;
    for (const PreviewRequest& r : running_) {
        queued_.erase(r.key);
        const std::string png = previewFile(r.key);
        if (fs::exists(toPath(png), ec)) {
            index_[r.key] = {fromPath(toPath(png).filename()), r.hash};
            sendPreview(r.key, png);
        } else {
            failed_.insert(r.key);  // not again this session (Regenerate Preview asks again)
        }
    }
    if (!ok)
        FX_WARN("editor: the preview run failed (%zu previews)", running_.size());
    running_.clear();
    saveIndex();
    fs::remove(toPath(jobsFile_), ec);
}

std::string AssetBrowser::sceneSaved()
{
    if (!project_ || currentScene_.empty())
        return {};
    std::error_code ec;
    fs::create_directories(toPath(thumbnails()), ec);
    loadIndex();
    sceneCapture_ = previewFile("scene:" + currentScene_);
    return sceneCapture_;
}

}  // namespace fx
