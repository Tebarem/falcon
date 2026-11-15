#include "core/engine_files.h"

#include "core/log.h"
#include "core/platform.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <mutex>

namespace fx::engine_files {

namespace fs = std::filesystem;

namespace {

std::string generic(const fs::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

fs::path toPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

// falcon.exe's content unpacked into the user's cache (its folder), or the content folder beside
// it when it carries none. Empty on an error.
std::string unpackContent()
{
    const bundle::Bundle& b = packed();
    if (!b.has("content"))
        return generic(platform::executableDir() / "content");
    platform::setThreadName("Unpack");
    const fs::path cache = platform::localCacheDir();
    if (cache.empty()) {
        FX_ERROR("content: no folder for the engine's content (the user's local app data is unknown)");
        return {};
    }
    char id[17];
    std::snprintf(id, sizeof id, "%016llx", static_cast<unsigned long long>(b.idOf("content")));
    const fs::path root = cache / "Content", dir = root / id;
    std::error_code ec;
    // (each copy is made beside its place and moved there whole: a folder there is complete)
    if (!fs::is_directory(dir, ec)) {
        const auto start = std::chrono::steady_clock::now();
        const fs::path tmp = root / (std::string(id) + ".unpacking-" + std::to_string(platform::processId()));
        fs::remove_all(tmp, ec);
        std::string error;
        if (!b.extract("content", tmp, error)) {
            FX_ERROR("content: %s", error.c_str());
            fs::remove_all(tmp, ec);
            return {};
        }
        fs::rename(tmp, dir, ec);
        if (ec) {  // (another engine of this build moved its copy there first)
            fs::remove_all(tmp, ec);
            if (!fs::is_directory(dir, ec)) {
                FX_ERROR("content: cannot make %s", platform::utf8(dir).c_str());
                return {};
            }
        } else {
            FX_INFO("content: unpacked into %s (%.0f ms)", platform::utf8(dir).c_str(),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        // Other builds' copies go (and half-made ones an hour old: an engine stopped while unpacking).
        const auto hourAgo = fs::file_time_type::clock::now() - std::chrono::hours(1);
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string name = generic(it->path().filename());
            std::error_code e;
            if (it->path() == dir || (name.find(".unpacking-") != std::string::npos && it->last_write_time(e) > hourAgo))
                continue;
            fs::remove_all(it->path(), e);
        }
    }
    return generic(dir);
}

std::once_flag g_contentOnce;
std::shared_future<std::string> g_content;

}  // namespace

const bundle::Bundle& packed()
{
    static const bundle::Bundle b = [] {
        bundle::Bundle x;
        x.open(platform::executablePath());
        return x;
    }();
    return b;
}

std::string uiBundle(const std::string& name, bool orFolder)
{
    if (packed().find("ui/" + name + "/index.html"))
        return kUiPrefix + name;
    const fs::path folder = platform::executableDir() / "falcon-ui" / toPath(name);
    std::error_code ec;
    return orFolder || fs::is_directory(folder, ec) ? generic(folder) : std::string();
}

bool isPackedUi(const std::string& bundle) { return bundle.starts_with(kUiPrefix); }

bool copyUiFile(const std::string& path, const fs::path& to)
{
    std::error_code ec;
    if (std::string data; packed().read("ui/" + path, data)) {
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        out.write(data.data(), std::streamsize(data.size()));
        return bool(out);
    }
    return fs::copy_file(platform::executableDir() / "falcon-ui" / toPath(path), to, fs::copy_options::overwrite_existing, ec);
}

void prepareContent()
{
    std::call_once(g_contentOnce, [] {
        if (packed().has("content"))
            g_content = std::async(std::launch::async, unpackContent).share();
        else {
            std::promise<std::string> ready;
            ready.set_value(unpackContent());
            g_content = ready.get_future().share();
        }
    });
}

std::string contentDir(const std::string& name)
{
    prepareContent();
    const std::string& root = g_content.get();
    return root.empty() ? std::string() : root + "/" + name;
}

}  // namespace fx::engine_files
