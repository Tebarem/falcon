#include "project/asset_library.h"

#include "assets/material_asset.h"
#include "core/json.h"
#include "core/platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace fx::asset_library {

namespace fs = std::filesystem;

namespace {

std::string falcon_, user_;

fs::path toPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string fromPath(const fs::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trimmed(std::string root)
{
    if (root.empty())
        return root;
    root = fromPath(toPath(root).lexically_normal());
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    return root;
}

bool under(const std::string& path, const char* prefix)
{
    const size_t n = std::strlen(prefix);
    return path.compare(0, n, prefix) == 0 && (path.size() == n || path[n] == '/');
}

// A name not taken in `folder` (absolute): "Oak", "Oak (2)"...
std::string uniqueName(const fs::path& folder, const std::string& stem, const std::string& ext)
{
    std::error_code ec;
    for (uint32_t n = 1;; ++n) {
        const std::string name = n == 1 ? stem + ext : stem + " (" + std::to_string(n) + ")" + ext;
        if (!fs::exists(folder / toPath(name), ec))
            return name;
    }
}

// One library file into the project (its copy's path), unless it is there already.
bool copyIn(const std::string& projectDir, const std::string& path, std::string& error)
{
    const std::string src = absolute(path);
    if (src.empty()) {
        error = "Not in a library: " + path;
        return false;
    }
    const fs::path dst = toPath(projectDir) / toPath(projectPathOf(path));
    std::error_code ec;
    if (fs::exists(dst, ec))
        return true;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(toPath(src), dst, ec);
    if (ec) {
        error = "Could not copy " + path + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

void setRoots(const std::string& falcon, const std::string& user)
{
    falcon_ = trimmed(falcon);
    user_ = trimmed(user);
}

const std::string& falconRoot() { return falcon_; }
const std::string& userRoot() { return user_; }

std::string defaultUserRoot()
{
    const fs::path documents = platform::documentsDir();
    return documents.empty() ? std::string() : fromPath(documents / "Falcon Library");
}

bool isLibrary(const std::string& path) { return under(path, kFalcon) || under(path, kUser); }
bool readOnly(const std::string& path) { return under(path, kFalcon); }

std::string absolute(const std::string& path)
{
    const bool falcon = under(path, kFalcon);
    if (!falcon && !under(path, kUser))
        return {};
    const std::string& root = falcon ? falcon_ : user_;
    const size_t n = std::strlen(falcon ? kFalcon : kUser);
    const fs::path rel = toPath(path.size() > n ? path.substr(n + 1) : std::string()).lexically_normal();
    if (root.empty() || rel.is_absolute() || rel.has_root_name() || (!rel.empty() && fromPath(*rel.begin()) == ".."))
        return {};
    return rel.empty() || rel == "." ? root : fromPath((toPath(root) / rel).lexically_normal());
}

std::string projectPathOf(const std::string& path)
{
    if (under(path, kFalcon))
        return "Assets/Falcon" + path.substr(std::strlen(kFalcon));
    if (under(path, kUser))
        return "Assets/Library" + path.substr(std::strlen(kUser));
    return path;
}

std::string useInProject(const std::string& projectDir, const std::string& path, bool& copied, std::string& error)
{
    copied = false;
    if (!isLibrary(path))
        return path;
    const std::string src = absolute(path);
    std::error_code ec;
    if (src.empty() || !fs::exists(toPath(src), ec)) {
        error = "Not in the library: " + path;
        return {};
    }
    if (fs::is_directory(toPath(src), ec)) {
        error = "A library folder can't be used as it is: use what is in it";
        return {};
    }
    const std::string rel = projectPathOf(path);
    const fs::path dst = toPath(projectDir) / toPath(rel);
    if (fs::exists(dst, ec))
        return rel;  // (copied before: the project's now, edits and all)
    const std::string ext = lower(fromPath(toPath(path).extension()));
    if (ext == ".fmat") {
        MaterialAsset mat;
        if (!readMaterialAsset(src, mat, error))
            return {};
        for (std::string* t : {&mat.textures[0], &mat.textures[1], &mat.textures[2], &mat.textures[3], &mat.height})
            if (isLibrary(*t)) {
                if (!copyIn(projectDir, *t, error))
                    return {};
                *t = projectPathOf(*t);
            }
        fs::create_directories(dst.parent_path(), ec);
        if (!writeMaterialAsset(fromPath(dst), mat, error))
            return {};
    } else {
        if (!copyIn(projectDir, path, error))
            return {};
        if (ext == ".gltf") {  // its buffers and images, beside it as it names them
            const std::string folder = fromPath(toPath(path).parent_path());
            for (const std::string& dep : gltfDependencies(src))
                if (!copyIn(projectDir, folder + "/" + dep, error))
                    return {};
        }
    }
    copied = true;
    return rel;
}

std::string addMaterial(const std::string& projectDir, const std::string& path, const std::string& into, std::string& error)
{
    const std::string intoAbsolute = under(into, kUser) ? absolute(into) : std::string();
    if (intoAbsolute.empty()) {
        error = user_.empty() ? "There is no library folder (set one in the Project panel's Library)" : "Not in My Library: " + into;
        return {};
    }
    MaterialAsset mat;
    if (!readMaterialAsset(fromPath(toPath(projectDir) / toPath(path)), mat, error))
        return {};
    std::error_code ec;
    const std::string stem = fromPath(toPath(path).stem());
    const std::string folder = into + "/" + uniqueName(toPath(intoAbsolute), stem, "");
    const fs::path dir = toPath(absolute(folder));
    fs::create_directories(dir / "Textures", ec);
    std::unordered_map<std::string, std::string> copies;  // (one texture in two slots: one copy)
    for (std::string* t : {&mat.textures[0], &mat.textures[1], &mat.textures[2], &mat.textures[3], &mat.height}) {
        if (t->empty() || isLibrary(*t))
            continue;
        if (const auto it = copies.find(*t); it != copies.end()) {
            *t = it->second;
            continue;
        }
        const fs::path src = toPath(projectDir) / toPath(*t);
        const std::string name = uniqueName(dir / "Textures", fromPath(src.stem()), fromPath(src.extension()));
        fs::copy_file(src, dir / "Textures" / toPath(name), ec);
        if (ec) {
            error = "Could not copy " + *t + ": " + ec.message();
            return {};
        }
        *t = copies[*t] = folder + "/Textures/" + name;
    }
    if (copies.empty())
        fs::remove(dir / "Textures", ec);
    const std::string file = stem + ".fmat";
    if (!writeMaterialAsset(fromPath(dir / toPath(file)), mat, error))
        return {};
    return folder + "/" + file;
}

std::vector<std::string> gltfDependencies(const std::string& gltf)
{
    std::vector<std::string> deps;
    size_t size = 0;
    void* text = SDL_LoadFile(gltf.c_str(), &size);
    if (!text)
        return deps;
    try {
        const Json root = Json::parse(std::string_view(static_cast<const char*>(text), size));
        for (const char* list : {"buffers", "images"})
            if (const Json* items = root.get(list))
                for (const Json& item : items->array) {
                    std::string uri = item.getString("uri");
                    if (uri.empty() || uri.starts_with("data:"))
                        continue;
                    std::string decoded;  // %20 and friends
                    for (size_t i = 0; i < uri.size(); ++i) {
                        if (uri[i] == '%' && i + 2 < uri.size()) {
                            decoded += char(std::strtol(uri.substr(i + 1, 2).c_str(), nullptr, 16));
                            i += 2;
                        } else {
                            decoded += uri[i];
                        }
                    }
                    deps.push_back(decoded);
                }
    } catch (const std::exception&) {
    }
    SDL_free(text);
    return deps;
}

}  // namespace fx::asset_library
