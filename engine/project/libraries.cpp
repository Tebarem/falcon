#include "project/libraries.h"

#include "app/process.h"
#include "core/json.h"
#include "core/log.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace fx::libraries {

namespace fs = std::filesystem;

namespace {

bool readFile(const fs::path& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::stringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

bool writeFile(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    f << text;
    return bool(f);
}

std::string generic(const fs::path& p) { return p.generic_string(); }

// A library name: letters, digits, '.', '-', '_' (a folder name everywhere, a C# namespace part).
bool validName(const std::string& name)
{
    return !name.empty() && name.size() <= 64 && name != "." && name != ".." &&
           std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'; });
}

// ---- project.falcon's "libraries"

Json readProject(const std::string& projectDir)
{
    std::string text;
    if (!readFile(fs::path(projectDir) / "project.falcon", text))
        return Json::makeObject();
    try {
        Json j = Json::parse(text);
        return j.isObject() ? j : Json::makeObject();
    } catch (const std::exception&) {
        return Json::makeObject();
    }
}

bool writeProject(const std::string& projectDir, const Json& root, std::string& error)
{
    if (!writeFile(fs::path(projectDir) / "project.falcon", root.dump(2) + "\n")) {
        error = "Cannot write project.falcon";
        return false;
    }
    return true;
}

const Json* entryOf(const Json& root, const std::string& name)
{
    if (const Json* list = root.get("libraries"); list && list->type == Json::Type::Array)
        for (const Json& e : list->array)
            if (e.getString("name") == name)
                return &e;
    return nullptr;
}

// ---- trust: the user's, in the editor's app data

std::string g_trustFile;  // (setTrustFile)

std::string trustFile()
{
    if (!g_trustFile.empty())
        return g_trustFile;
    std::string path;
    if (char* dir = SDL_GetPrefPath("Falcon", "Editor")) {
        path = std::string(dir) + "trusted-libraries.json";
        SDL_free(dir);
    }
    return path;
}

std::mutex g_trustMutex;

std::set<std::string> loadTrust()
{
    std::set<std::string> out;
    std::string text;
    const std::string file = trustFile();
    if (file.empty() || !readFile(file, text))
        return out;
    try {
        const Json j = Json::parse(text);
        if (const Json* list = j.get("trusted"))
            for (const Json& t : list->array)
                out.insert(t.str);
    } catch (const std::exception&) {
    }
    return out;
}

std::string trustKey(const std::string& name, const std::string& version) { return name + "@" + (version.empty() ? "0" : version); }

// ---- running git and tar (on the install thread)

int runProgram(const std::string& program, const std::vector<std::string>& args, const std::string& log)
{
    void* p = startProgramTracked(program, args, log);
    if (!p)
        return -1;
    int code = -1;
    while (!falconFinished(p, code))
        SDL_Delay(20);
    return code;
}

std::string lastLines(const std::string& log)
{
    std::string text;
    readFile(log, text);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    const size_t at = text.size() > 400 ? text.rfind('\n', text.size() - 400) : std::string::npos;
    return at == std::string::npos ? text : text.substr(at + 1);
}

// The folder in `root` holding library.json: root itself, or its only folder that has one.
fs::path libraryRoot(const fs::path& root)
{
    std::error_code ec;
    if (fs::exists(root / "library.json", ec))
        return root;
    fs::path found;
    for (const fs::directory_entry& e : fs::directory_iterator(root, ec))
        if (e.is_directory(ec) && fs::exists(e.path() / "library.json", ec)) {
            if (!found.empty())
                return {};  // (several: which one?)
            found = e.path();
        }
    return found;
}

}  // namespace

std::vector<Library> scan(const std::string& projectDir)
{
    std::vector<Library> found;
    const Json project = readProject(projectDir);
    std::set<std::string> trust;
    {
        std::lock_guard<std::mutex> lock(g_trustMutex);
        trust = loadTrust();
    }
    std::error_code ec;
    const fs::path dir = fs::path(projectDir) / "Libraries";
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_directory(ec) || e.path().filename().string().starts_with("."))
            continue;
        Library lib;
        lib.name = e.path().filename().string();
        std::string text;
        if (!readFile(e.path() / "library.json", text)) {
            lib.error = "No library.json";
        } else {
            try {
                const Json j = Json::parse(text);
                lib.version = j.getString("version", "0.1.0");
                lib.description = j.getString("description");
                lib.author = j.getString("author");
                if (const Json* d = j.get("dependencies"))
                    for (const Json& n : d->array)
                        if (n.type == Json::Type::String)
                            lib.dependencies.push_back(n.str);
            } catch (const std::exception& ex) {
                lib.error = std::string("library.json: ") + ex.what();
            }
        }
        if (const Json* entry = entryOf(project, lib.name)) {
            lib.source = entry->getString("source");
            lib.ref = entry->getString("ref");
            lib.enabled = entry->getBool("enabled", true);
        }
        lib.trusted = trust.count(trustKey(lib.name, lib.version)) != 0;
        lib.code = fs::is_directory(e.path() / "Code", ec);
        lib.editor = fs::is_directory(e.path() / "Editor", ec);
        lib.ui = fs::is_directory(e.path() / "UI", ec);
        lib.assets = fs::is_directory(e.path() / "Assets", ec);
        found.push_back(std::move(lib));
    }
    // Dependencies first (a missing one is an error on the library needing it).
    std::sort(found.begin(), found.end(), [](const Library& a, const Library& b) { return a.name < b.name; });
    std::vector<Library> ordered;
    std::set<std::string> placed;
    const auto has = [&](const std::string& n) {
        return std::any_of(found.begin(), found.end(), [&](const Library& l) { return l.name == n; });
    };
    for (size_t pass = 0; pass <= found.size() && ordered.size() < found.size(); ++pass)
        for (Library& lib : found) {
            if (placed.count(lib.name))
                continue;
            bool ready = true;
            for (const std::string& d : lib.dependencies)
                ready &= placed.count(d) != 0 || !has(d);
            if (ready || pass == found.size()) {
                for (const std::string& d : lib.dependencies)
                    if (!has(d) && lib.error.empty())
                        lib.error = "Needs the library " + d;
                placed.insert(lib.name);
                ordered.push_back(lib);
            }
        }
    return ordered;
}

std::vector<std::string> codeDirs(const std::string& projectDir)
{
    std::vector<std::string> out;
    for (const Library& lib : scan(projectDir))
        if (lib.enabled && lib.trusted && lib.code && lib.error.empty())
            out.push_back(generic(fs::path(projectDir) / "Libraries" / lib.name / "Code"));
    return out;
}

std::vector<std::string> editorDirs(const std::string& projectDir)
{
    std::vector<std::string> out;
    for (const Library& lib : scan(projectDir))
        if (lib.enabled && lib.trusted && lib.editor && lib.error.empty())
            out.push_back(generic(fs::path(projectDir) / "Libraries" / lib.name / "Editor"));
    return out;
}

std::vector<std::string> assetDirs(const std::string& projectDir)
{
    std::vector<std::string> out = {"Assets"};
    for (const Library& lib : scan(projectDir))
        if (lib.enabled && lib.assets)
            out.push_back("Libraries/" + lib.name + "/Assets");
    return out;
}

bool writeBuildProps(const std::string& projectDir)
{
    std::string text =
        "<Project>\n  <!-- Written by the editor before each build: the enabled, trusted libraries' code (project/libraries.h);\n"
        "       their Editor/ code only in the editor's builds (FalconEditor). -->\n"
        "  <ItemGroup>\n";
    for (const std::string& dir : codeDirs(projectDir))
        text += "    <Compile Include=\"" + dir + "/**/*.cs\" Exclude=\"" + dir + "/**/bin/**;" + dir + "/**/obj/**\" />\n";
    text += "  </ItemGroup>\n  <ItemGroup Condition=\"'$(FalconEditor)' == 'true'\">\n";
    for (const std::string& dir : editorDirs(projectDir))
        text += "    <Compile Include=\"" + dir + "/**/*.cs\" Exclude=\"" + dir + "/**/bin/**;" + dir + "/**/obj/**\" />\n";
    text += "  </ItemGroup>\n</Project>\n";
    const fs::path path = fs::path(projectDir) / "Cache" / "Scripts" / "Libraries.props";
    std::string was;
    if (readFile(path, was) && was == text)
        return false;
    writeFile(path, text);
    return true;
}

void setTrustFile(const std::string& file) { g_trustFile = file; }

bool trusted(const std::string& name, const std::string& version)
{
    std::lock_guard<std::mutex> lock(g_trustMutex);
    return loadTrust().count(trustKey(name, version)) != 0;
}

void setTrusted(const std::string& name, const std::string& version, bool trust)
{
    std::lock_guard<std::mutex> lock(g_trustMutex);
    std::set<std::string> all = loadTrust();
    if (trust)
        all.insert(trustKey(name, version));
    else
        all.erase(trustKey(name, version));
    JsonWriter w;
    w.beginObject().beginArray("trusted");
    for (const std::string& t : all)
        w.value(nullptr, t);
    w.endArray().endObject();
    if (const std::string file = trustFile(); !file.empty())
        writeFile(file, Json::parse(w.str()).dump(2) + "\n");
}

bool setEntry(const std::string& projectDir, const std::string& name, const std::string& source, const std::string& ref, bool enabled,
              std::string& error)
{
    Json root = readProject(projectDir);
    Json list;
    list.type = Json::Type::Array;
    if (const Json* old = root.get("libraries"); old && old->type == Json::Type::Array)
        for (const Json& e : old->array)
            if (e.getString("name") != name)
                list.array.push_back(e);
    Json entry = Json::makeObject();
    entry.set("name", Json::makeString(name));
    if (!source.empty())
        entry.set("source", Json::makeString(source));
    if (!ref.empty())
        entry.set("ref", Json::makeString(ref));
    entry.set("enabled", Json::makeBool(enabled));
    list.array.push_back(std::move(entry));
    root.set("libraries", std::move(list));
    return writeProject(projectDir, root, error);
}

bool setEnabled(const std::string& projectDir, const std::string& name, bool enabled, std::string& error)
{
    const Json root = readProject(projectDir);
    const Json* e = entryOf(root, name);
    return setEntry(projectDir, name, e ? e->getString("source") : "", e ? e->getString("ref") : "", enabled, error);
}

bool remove(const std::string& projectDir, const std::string& name, std::string& error)
{
    if (!validName(name)) {
        error = "No library " + name;
        return false;
    }
    std::error_code ec;
    fs::remove_all(fs::path(projectDir) / "Libraries" / name, ec);
    if (ec) {
        error = "Cannot remove Libraries/" + name + ": " + ec.message();
        return false;
    }
    Json root = readProject(projectDir);
    if (Json* list = const_cast<Json*>(root.get("libraries")); list && list->type == Json::Type::Array)
        std::erase_if(list->array, [&](const Json& e) { return e.getString("name") == name; });
    return writeProject(projectDir, root, error);
}

// falcon-panel.js (the editor's page bundle: falcon-ui/editor/) beside a library's pages.
static bool copyPanelHelper(const fs::path& uiDir)
{
    std::error_code ec;
    const char* base = SDL_GetBasePath();
    const fs::path helper = fs::path(base ? base : "") / "falcon-ui" / "editor" / "falcon-panel.js";
    fs::copy_file(helper, uiDir / "falcon-panel.js", fs::copy_options::overwrite_existing, ec);
    return true;  // (no helper next to this engine: the page can still be written by hand)
}

bool create(const std::string& projectDir, const std::string& name, std::string& error)
{
    if (!validName(name)) {
        error = "A library's name is letters, digits, '.', '-' and '_'";
        return false;
    }
    const fs::path dir = fs::path(projectDir) / "Libraries" / name;
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        error = "There is a library " + name + " already";
        return false;
    }
    std::string ns;
    for (char c : name)
        ns += std::isalnum(static_cast<unsigned char>(c)) || c == '.' ? c : '_';
    if (std::isdigit(static_cast<unsigned char>(ns[0])))
        ns = "_" + ns;
    const bool ok =
        writeFile(dir / "library.json", "{\n  \"name\": \"" + name + "\",\n  \"version\": \"0.1.0\",\n  \"description\": \"\",\n  \"author\": \"\",\n"
                                        "  \"dependencies\": []\n}\n") &&
        writeFile(dir / "Code" / "Example.cs",
                  "using System.Numerics;\nusing Falcon;\n\nnamespace " + ns +
                      ";\n\n// A script component this library brings to every project using it (Add Component).\n"
                      "public sealed class Example : Component\n{\n    [Property] public float Speed = 1f;\n\n"
                      "    public override void OnUpdate(float dt) { }\n}\n") &&
        writeFile(dir / "Assets" / ".keep", "") &&
        writeFile(dir / "Editor" / "ExampleWindow.cs",
                  "using Falcon;\nusing Falcon.Editor;\n\nnamespace " + ns +
                      ".Editor;\n\n// An editor window: UI/example.html in a floating window (Tools > " + name + " > Example).\n"
                      "[EditorWindow(\"Tools/" + name + "/Example\", Page = \"example.html\")]\n"
                      "public sealed class ExampleWindow : EditorWindow\n{\n"
                      "    public override void OnMessage(string json) => Send(\"\\\"Hello from C#\\\"\");\n}\n") &&
        writeFile(dir / "UI" / "example.html",
                  "<!doctype html>\n<html>\n<head>\n  <meta charset=\"utf-8\">\n  <link rel=\"stylesheet\" href=\"example.css\">\n</head>\n<body>\n"
                  "  <button id=\"hello\">Say hello</button>\n  <p id=\"reply\"></p>\n"
                  "  <script src=\"falcon-panel.js\"></script>\n  <script src=\"example.js\"></script>\n</body>\n</html>\n") &&
        writeFile(dir / "UI" / "example.js",
                  "document.getElementById('hello').onclick = () => FalconPanel.send({ hello: true });\n"
                  "FalconPanel.onMessage = v => { document.getElementById('reply').textContent = v; };\n") &&
        writeFile(dir / "UI" / "example.css",
                  "body { background: var(--panel); color: var(--text); font: 13px system-ui, sans-serif; margin: 12px; }\n") &&
        copyPanelHelper(dir / "UI");
    if (!ok) {
        error = "Cannot write Libraries/" + name;
        return false;
    }
    setTrusted(name, "0.1.0", true);  // (made here: this user's own code)
    return setEntry(projectDir, name, "local", "", true, error);
}

void install(const std::string& projectDir, const std::string& source, const std::string& location, const std::string& ref,
             std::function<void(InstallResult)> done)
{
    std::thread([projectDir, source, location, ref, done = std::move(done)] {
        InstallResult r;
        std::error_code ec;
        const fs::path staging = fs::path(projectDir) / "Cache" / "Libraries" / ("install-" + std::to_string(SDL_GetTicksNS()));
        fs::create_directories(staging, ec);
        const std::string log = generic(staging.parent_path() / (staging.filename().string() + ".log"));
        fs::path from;
        if (source == "folder") {
            from = libraryRoot(location);
        } else if (source == "zip") {
            if (runProgram("tar", {"-xf", location, "-C", generic(staging)}, log) != 0)
                r.error = "The zip couldn't be extracted: " + lastLines(log);
            else
                from = libraryRoot(staging);
        } else if (source == "git") {
            std::vector<std::string> args = {"clone", "--depth", "1"};
            if (!ref.empty())
                args.insert(args.end(), {"--branch", ref});
            args.insert(args.end(), {location, generic(staging / "repo")});
            const int code = runProgram("git", args, log);
            if (code != 0)
                r.error = code == -1 ? "git was not found: install Git" : "git clone failed: " + lastLines(log);
            else {
                fs::remove_all(staging / "repo" / ".git", ec);
                from = libraryRoot(staging / "repo");
            }
        } else {
            r.error = "Unknown source " + source;
        }
        if (r.error.empty() && from.empty())
            r.error = "No library there (a library.json at its top, or one folder down)";
        std::string version;
        if (r.error.empty()) {
            std::string text;
            readFile(from / "library.json", text);
            try {
                const Json j = Json::parse(text);
                r.name = j.getString("name", from.filename().string().c_str());
                version = j.getString("version", "0.1.0");
            } catch (const std::exception& ex) {
                r.error = std::string("library.json: ") + ex.what();
            }
            if (r.error.empty() && !validName(r.name))
                r.error = "Its name (" + r.name + ") isn't a folder name";
        }
        const fs::path to = fs::path(projectDir) / "Libraries" / r.name;
        if (r.error.empty() && fs::exists(to, ec))
            r.error = "There is a library " + r.name + " already (remove it first to replace it)";
        if (r.error.empty()) {
            fs::create_directories(to.parent_path(), ec);
            fs::copy(from, to, fs::copy_options::recursive, ec);
            if (ec)
                r.error = "Cannot copy it into Libraries/" + r.name + ": " + ec.message();
        }
        if (r.error.empty()) {
            setTrusted(r.name, version, true);  // (added by this user, who was told what that means)
            r.ok = setEntry(projectDir, r.name, source, source == "zip" || source == "folder" ? generic(location) : ref.empty() ? location : location + "#" + ref,
                            true, r.error);
        }
        fs::remove_all(staging, ec);
        fs::remove(log, ec);
        done(std::move(r));
    }).detach();
}

std::string exportZip(const std::string& projectDir, const std::string& name, const std::string& out, std::string& error)
{
    const fs::path dir = fs::path(projectDir) / "Libraries" / name;
    std::error_code ec;
    if (!validName(name) || !fs::exists(dir / "library.json", ec)) {
        error = "No library " + name;
        return {};
    }
    std::string version = "0.1.0", text;
    if (readFile(dir / "library.json", text))
        try {
            version = Json::parse(text).getString("version", "0.1.0");
        } catch (const std::exception&) {
        }
    const fs::path zip = out.empty() ? fs::path(projectDir) / "Exports" / (name + "-" + version + ".zip") : fs::path(out);
    fs::create_directories(zip.parent_path(), ec);
    fs::remove(zip, ec);
    const std::string log = generic(fs::path(projectDir) / "Cache" / "Libraries" / "export.log");
    fs::create_directories(fs::path(log).parent_path(), ec);
    // The library's folder in the zip (so it extracts as one folder); bin/obj left out.
    if (runProgram("tar", {"-a", "-cf", generic(zip), "--exclude", "bin", "--exclude", "obj", "-C", generic(dir.parent_path()), name}, log) != 0) {
        error = "The zip couldn't be made: " + lastLines(log);
        return {};
    }
    fs::remove(log, ec);
    return generic(zip);
}

}  // namespace fx::libraries
