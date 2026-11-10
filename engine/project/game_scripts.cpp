#include "project/game_scripts.h"

#include "project/libraries.h"
#include "project/project.h"

#include "app/process.h"
#include "core/log.h"
#include "core/platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace fx {

namespace fs = std::filesystem;

namespace gamescripts {

namespace {

// "Zero Day 2" -> "ZeroDay2": a C# identifier (a leading digit gets a "Game" prefix).
std::string identifier(const std::string& name)
{
    std::string id;
    bool upper = true;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            id += upper ? char(std::toupper(static_cast<unsigned char>(c))) : c;
            upper = false;
        } else {
            upper = true;
        }
    }
    if (id.empty() || std::isdigit(static_cast<unsigned char>(id[0])))
        id = "Game" + id;
    return id;
}

bool write(const fs::path& path, const std::string& text, std::string& error)
{
    std::ofstream f(path, std::ios::binary);
    f << text;
    if (!f) {
        error = "Cannot write " + path.generic_string();
        return false;
    }
    return true;
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

const char* const kCsproj = R"(<Project Sdk="Microsoft.NET.Sdk">
  <!-- The game. The editor builds it into ../Cache/Scripts/bin/Game.dll whenever a source
       changes (and File > Build Scripts); Play starts it, Stop ends it. -->
  <PropertyGroup>
    <AssemblyName>Game</AssemblyName>
    <RootNamespace>$NAMESPACE$</RootNamespace>
    <TargetFramework>net10.0</TargetFramework>
    <LangVersion>latest</LangVersion>
    <Nullable>enable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <!-- Hosted by the engine: a runtimeconfig.json, and Falcon.Engine.dll copied next to the game. -->
    <EnableDynamicLoading>true</EnableDynamicLoading>
    <OutputPath>$(MSBuildThisFileDirectory)../Cache/Scripts/bin/</OutputPath>
    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>
    <!-- The game registers itself from a [ModuleInitializer] (Game.Register). -->
    <NoWarn>$(NoWarn);CA2255</NoWarn>
  </PropertyGroup>
  <ItemGroup>
    <Reference Include="Falcon.Engine">
      <HintPath>$(FalconEngineDir)Falcon.Engine.dll</HintPath>
    </Reference>
  </ItemGroup>
</Project>
)";

const char* const kProps = R"(<Project>
  <!-- Builds go to the project's Cache (out of the Project panel's way). FalconEngineDir: the
       engine's bin/scripts/jit/ (the editor passes its own when it builds; this one lets an IDE
       find Falcon.Engine.dll). -->
  <PropertyGroup>
    <BaseIntermediateOutputPath>$(MSBuildThisFileDirectory)../Cache/Scripts/obj/</BaseIntermediateOutputPath>
    <FalconEngineDir Condition="'$(FalconEngineDir)' == ''">$ENGINE$</FalconEngineDir>
  </PropertyGroup>
</Project>
)";

const char* const kGame = R"(using System.Numerics;
using System.Runtime.CompilerServices;
using Falcon;

namespace $NAMESPACE$;

/// <summary>
/// The game. Play in the editor starts it (OnStart), calls OnUpdate every frame, and Stop ends it
/// (OnStop) and puts the scene back: entities and components the game made are removed, and
/// what it moved returns to where it was.
/// Keep the game's state in ECS components (see Falcon.Ecs and Query): they survive a rebuild
/// while playing, fields don't.
/// </summary>
public sealed class $CLASS$ : Game
{
    [ModuleInitializer]
    internal static void Register() => Register<$CLASS$>();

    public override void OnStart()
    {
        Log.Info("$NAME$: started");
    }

    public override void OnUpdate(float dt)
    {
        PerformanceHud.Update(dt);  // frame time, fps, CPU, RAM and GPU over the view (F3 hides it)
    }

    public override void OnStop()
    {
        Log.Info("$NAME$: stopped");
    }
}
)";

const char* const kPerformanceHud = R"(using System.Globalization;
using Falcon;

namespace $NAMESPACE$;

/// <summary>
/// Frame time, fps, and CPU, RAM and GPU use, at the top left of the game view while playing
/// (refreshed four times a second; F3 hides and shows it). "game" is this process, the editor
/// included while playing in it; "system" is everything running. The game's RAM is Task
/// Manager's figure (its memory in physical RAM); "committed" is all it has allocated. Call
/// Update from OnUpdate.
/// </summary>
public static class PerformanceHud
{
    private const float Interval = 0.25f;
    private static bool s_hidden;
    private static float s_timer = Interval;

    public static void Update(float dt)
    {
        if (Input.Pressed(Key.F3))
        {
            s_hidden = !s_hidden;
            s_timer = Interval;
            if (s_hidden)
                Hud.Clear();
        }
        if (s_hidden || (s_timer += dt) < Interval)
            return;
        s_timer = 0f;
        PerformanceStats p = Performance.Current;
        Hud.Show(
            $"FPS   {Number(p.Fps, "0"),6}    {Number(p.FrameMsAvg, "0.00")} ms   (slowest {Number(p.FrameMsMax, "0.00")} ms)\n" +
            $"CPU   {Percent(p.CpuProcess)} game    {Percent(p.CpuSystem)} system\n" +
            $"GPU   {Percent(p.GpuProcess)} game    {Percent(p.GpuSystem)} system\n" +
            $"RAM   {Size(p.RamProcessMB),9} game ({Size(p.RamProcessCommittedMB)} committed)    {Size(p.RamUsedMB)} / {Size(p.RamTotalMB)} system\n" +
            $"VRAM  {Size(p.VramProcessMB),9} game    {Size(p.VramTotalMB)} on the GPU");
    }

    private static string Number(float v, string format) => v.ToString(format, CultureInfo.InvariantCulture);

    private static string Percent(float v) => v < 0f ? "   n/a" : (Number(v, "0") + "%").PadLeft(6);

    private static string Size(float mb) =>
        mb < 0f ? "n/a" : mb >= 1024f ? Number(mb / 1024f, "0.00") + " GB" : Number(mb, "0") + " MB";
}
)";

}  // namespace

std::string csprojPath(const std::string& projectDir) { return (fs::path(projectDir) / "Scripts" / "Game.csproj").generic_string(); }

std::string dllPath(const std::string& projectDir)
{
    return (fs::path(projectDir) / "Cache" / "Scripts" / "bin" / "Game.dll").generic_string();
}

std::string aotDllPath(const std::string& projectDir)
{
    return (fs::path(projectDir) / "Cache" / "Scripts" / "aot" / (std::string("Game") + platform::sharedLibrarySuffix())).generic_string();
}

std::string shippedProject()
{
    const char* base = SDL_GetBasePath();
    const fs::path game = fs::path(base ? base : "") / "Game";
    std::error_code ec;
    return fs::exists(game / "project.falcon", ec) ? game.generic_string() : std::string();
}

std::string engineScriptsDir()
{
    const char* base = SDL_GetBasePath();
    return (fs::path(base ? base : "") / "scripts" / "jit").generic_string() + "/";
}

bool exists(const std::string& projectDir)
{
    std::error_code ec;
    return fs::exists(csprojPath(projectDir), ec);
}

bool create(const std::string& projectDir, const std::string& projectName, std::string& error)
{
    const fs::path dir = fs::path(projectDir) / "Scripts";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error = "Cannot create " + dir.generic_string() + ": " + ec.message();
        return false;
    }
    const std::string ns = identifier(projectName);
    auto fill = [&](const char* text) {
        std::string s = replaceAll(text, "$NAMESPACE$", ns);
        s = replaceAll(s, "$CLASS$", ns + "Game");
        s = replaceAll(s, "$NAME$", projectName);
        return replaceAll(s, "$ENGINE$", engineScriptsDir());
    };
    if (!write(dir / "Game.csproj", fill(kCsproj), error) || !write(dir / "Directory.Build.props", fill(kProps), error))
        return false;
    if (!fs::exists(dir / "Game.cs", ec) && !write(dir / "Game.cs", fill(kGame), error))
        return false;
    if (!fs::exists(dir / "PerformanceHud.cs", ec) && !write(dir / "PerformanceHud.cs", fill(kPerformanceHud), error))
        return false;
    FX_INFO("scripts: created %s", dir.generic_string().c_str());
    return true;
}

bool builtForThisEngine(const std::string& projectDir)
{
    const auto read = [](const fs::path& path) {
        std::ifstream f(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    };
    const std::string built = read(fs::path(dllPath(projectDir)).parent_path() / "Falcon.Engine.dll");
    return !built.empty() && built == read(fs::path(engineScriptsDir()) / "Falcon.Engine.dll");
}

namespace {

// Each source under Scripts/ (.cs, .csproj, .props, .targets): the build's inputs. Folders
// starting with '.' (an IDE's) and bin/obj (a build of its own) are not looked into.
template <class F>
void forEachSource(const std::string& projectDir, F&& f)
{
    // Scripts/, and the libraries' Code/ (and their library.json: a version or a dependency changed).
    std::vector<fs::path> roots = {fs::path(projectDir) / "Scripts"};
    std::error_code ec;
    for (const fs::directory_entry& lib : fs::directory_iterator(fs::path(projectDir) / "Libraries", ec)) {
        roots.push_back(lib.path() / "Code");
        roots.push_back(lib.path() / "Editor");
        if (fs::is_regular_file(lib.path() / "library.json", ec))
            f(fs::directory_entry(lib.path() / "library.json"));
    }
    for (const fs::path& root : roots)
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path& path = it->path();
            if (it->is_directory(ec)) {
                const std::string name = path.filename().string();
                if (name.starts_with(".") || name == "bin" || name == "obj")
                    it.disable_recursion_pending();
                continue;
            }
            const std::string ext = path.extension().string();
            if (it->is_regular_file(ec) && (ext == ".cs" || ext == ".csproj" || ext == ".props" || ext == ".targets"))
                f(*it);
        }
}

}  // namespace

bool stale(const std::string& projectDir)
{
    std::error_code ec;
    const fs::path dll = dllPath(projectDir);
    if (!fs::exists(dll, ec) || !builtForThisEngine(projectDir))
        return true;
    const auto built = fs::last_write_time(dll, ec);
    bool newer = false;
    forEachSource(projectDir, [&](const fs::directory_entry& e) { newer = newer || e.last_write_time(ec) > built; });
    return newer;
}

uint64_t sourcesStamp(const std::string& projectDir)
{
    // FNV-1a over each source's path, size and time (the order the folder lists them in).
    uint64_t h = 14695981039346656037ull;
    const auto mix = [&](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i)
            h = (h ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
    };
    forEachSource(projectDir, [&](const fs::directory_entry& e) {
        std::error_code ec;
        const std::string path = e.path().generic_string();
        const uint64_t size = e.file_size(ec);
        const auto time = e.last_write_time(ec).time_since_epoch().count();
        mix(path.data(), path.size() + 1);
        mix(&size, sizeof(size));
        mix(&time, sizeof(time));
    });
    return h ? h : 1;
}

}  // namespace gamescripts

GameBuild::~GameBuild()
{
    stopFalcon(process_);
    if (thread_.joinable())
        thread_.join();
}

std::string buildDir(const std::string& projectDir, const std::string& name)
{
    std::string safe;
    for (const char c : name)  // (a file name: no separators or reserved characters)
        safe += std::strchr("<>:\"/\\|?*", c) || uint8_t(c) < 32 ? '_' : c;
    if (safe.empty())
        safe = "Game";
    return (fs::path(projectDir) / "Build" / safe).generic_string();
}

bool writeSteamPipe(const Project& project, const std::string& buildDir, const std::string& description, const std::string& branch,
                    std::string& vdf, std::string& error)
{
    const fs::path content = fs::path(buildDir).lexically_normal();
    const fs::path scripts = content.parent_path() / (content.filename().string() + " SteamPipe");
    std::error_code ec;
    fs::create_directories(scripts / "output", ec);
    const uint32_t app = project.steamAppId, depot = project.steamDepotId ? project.steamDepotId : project.steamAppId + 1;
    // (VDF strings: quotes and backslashes escaped; SteamPipe takes forward slashes too)
    const auto quoted = [](std::string text) {
        std::string out = "\"";
        for (const char c : text)
            out += c == '"' ? std::string("\\\"") : c == '\\' ? std::string("/") : std::string(1, c);
        return out + "\"";
    };
    const std::string depotFile = "depot_build_" + std::to_string(depot) + ".vdf";
    const std::string depotText = "\"DepotBuild\"\n{\n"
                                  "\t\"DepotID\" " + quoted(std::to_string(depot)) + "\n"
                                  "\t\"ContentRoot\" " + quoted(content.generic_string()) + "\n"
                                  "\t\"FileMapping\"\n\t{\n\t\t\"LocalPath\" \"*\"\n\t\t\"DepotPath\" \".\"\n\t\t\"Recursive\" \"1\"\n\t}\n"
                                  "\t\"FileExclusion\" \"*.pdb\"\n}\n";
    std::string appText = "\"AppBuild\"\n{\n"
                           "\t\"AppID\" " + quoted(std::to_string(app)) + "\n"
                           "\t\"Desc\" " + quoted(description.empty() ? project.name + " (Falcon)" : description) + "\n"
                           "\t\"ContentRoot\" " + quoted(content.generic_string()) + "\n"
                           "\t\"BuildOutput\" " + quoted((scripts / "output").generic_string()) + "\n";
    if (!branch.empty() && branch != "default")  // (Steam refuses to set default live from a build script)
        appText += "\t\"SetLive\" " + quoted(branch) + "\n";
    appText += "\t\"Depots\"\n\t{\n\t\t" + quoted(std::to_string(depot)) + " " + quoted(depotFile) + "\n\t}\n}\n";
    const fs::path appPath = scripts / ("app_build_" + std::to_string(app) + ".vdf");
    if (!gamescripts::write(scripts / depotFile, depotText, error) || !gamescripts::write(appPath, appText, error))
        return false;
    vdf = appPath.generic_string();
    return true;
}

bool GameBuild::start(const std::string& projectDir, const std::string& name)
{
    if (running())
        return false;
    if (thread_.joinable())
        thread_.join();
    projectDir_ = projectDir;
    output_ = buildDir(projectDir, name);
    name_ = fs::path(output_).filename().string();
    error_.clear();
    std::error_code ec;
    const fs::path cache = fs::path(projectDir) / "Cache" / "Scripts";
    fs::create_directories(cache, ec);
    fs::remove_all(cache / "aot", ec);
    log_ = (cache / "publish.log").generic_string();
    libraries::writeBuildProps(projectDir);
    const std::string c = cache.generic_string();
    // Its own intermediate and output folders: the editor's build (Debug, Cache/Scripts/bin) stays.
    const std::vector<std::string> args = {
        "publish", gamescripts::csprojPath(projectDir), "-c", "Release", "-r", platform::dotnetRid(), "--nologo", "-v:q", "-clp:NoSummary",
        "-p:PublishAot=true", "-p:NativeLib=Shared", "-p:FalconEngineDir=" + gamescripts::engineScriptsDir(),
        "-p:CustomAfterMicrosoftCommonTargets=" + gamescripts::engineScriptsDir() + "Falcon.Game.targets",
        "-p:BaseIntermediateOutputPath=" + c + "/publish-obj/", "-p:OutputPath=" + c + "/publish-bin/", "-o", c + "/aot"};
#if defined(_WIN32)
    // The NativeAOT link finds Visual Studio's tools through %ProgramFiles(x86)%, which a parent
    // without it (Git's bash: no parentheses in names) leaves out of the environment.
    if (!std::getenv("ProgramFiles(x86)")) {
        const char* programFiles = std::getenv("ProgramFiles");
        _putenv_s("ProgramFiles(x86)", programFiles ? (std::string(programFiles) + " (x86)").c_str() : "C:\\Program Files (x86)");
    }
    // ...and runs vcvarsall.bat, which breaks on PATH entries cmd can't take unquoted ("s&box
    // projects": '&' ends the command), its errors ending up in the linker's path: the publish
    // gets a PATH without them.
    const char* pathNow = std::getenv("PATH");
    const std::string path = pathNow ? pathNow : "";
    std::string cleanPath;
    for (size_t start = 0; start <= path.size();) {
        const size_t end = std::min(path.find(';', start), path.size());
        const std::string entry = path.substr(start, end - start);
        if (!entry.empty() && entry.find_first_of("&|<>^\"") == std::string::npos)
            cleanPath += (cleanPath.empty() ? "" : ";") + entry;
        start = end + 1;
    }
    // (vcvarsall runs vswhere.exe by name: Visual Studio's installer folder, which PATH may lack.)
    if (const char* x86 = std::getenv("ProgramFiles(x86)"))
        cleanPath += std::string(";") + x86 + "\\Microsoft Visual Studio\\Installer";
    _putenv_s("PATH", cleanPath.c_str());
    process_ = startProgramTracked("dotnet", args, log_);
    _putenv_s("PATH", path.c_str());
#else
    process_ = startProgramTracked("dotnet", args, log_);
#endif
    if (!process_) {
        error_ = "dotnet was not found: install the .NET 10 SDK";
        return false;
    }
    state_ = State::Publishing;
    FX_INFO("build game: publishing %s (NativeAOT)", gamescripts::csprojPath(projectDir).c_str());
    return true;
}

GameBuild::State GameBuild::poll()
{
    const State s = state_;
    if (s == State::Publishing) {
        int code = 0;
        if (!falconFinished(process_, code))
            return s;
        process_ = nullptr;
        std::ifstream f(log_);
        for (std::string line; std::getline(f, line);) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.find(": error ") != std::string::npos) {
                FX_ERROR("build game: %s", line.c_str());
                if (error_.empty())
                    error_ = line;
            } else if (!line.empty()) {
                FX_INFO("build game: %s", line.c_str());
            }
        }
        std::error_code ec;
        if (code != 0 || !fs::exists(gamescripts::aotDllPath(projectDir_), ec)) {
            if (error_.empty())
                error_ = "dotnet publish failed (exit code " + std::to_string(code) + ")";
            state_ = State::Idle;
            return State::Failed;
        }
        state_ = State::Copying;
        thread_ = std::thread([this] { copy(); });
        return State::Copying;
    }
    if (s == State::Succeeded || s == State::Failed) {
        if (thread_.joinable())
            thread_.join();
        state_ = State::Idle;
        if (s == State::Succeeded)
            FX_INFO("build game: %s", output_.c_str());
    }
    return s;
}

void GameBuild::copy()
{
    std::error_code ec;
    const fs::path out(output_), game = out / "Game", project(projectDir_);
    const char* base = SDL_GetBasePath();
    const fs::path engine(base ? base : "");
    std::string failed;
    const auto copyFile = [&](const fs::path& from, const fs::path& to) {
        fs::create_directories(to.parent_path(), ec);
        if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec) && failed.empty())
            failed = "Cannot copy " + from.generic_string() + " (" + ec.message() + ")";
    };
    // Every file under `from` but those `skip` turns down (by its path relative to `from`).
    const auto copyTree = [&](const fs::path& from, const fs::path& to, const auto& skip) {
        std::error_code walk;
        for (auto it = fs::recursive_directory_iterator(from, walk); !walk && it != fs::recursive_directory_iterator(); it.increment(walk)) {
            const std::string rel = it->path().lexically_relative(from).generic_string();
            if (skip(rel)) {
                if (it->is_directory(ec))
                    it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec))
                copyFile(it->path(), to / rel);
        }
    };
    fs::remove_all(out, ec);
    fs::create_directories(out, ec);
    // The engine: falcon.exe as <name>.exe (falcon as <name> on Linux and macOS); no debug files, no
    // script builds (the game is native), no material library (a project carries copies of what it
    // uses: Assets/Falcon).
    const std::string exe = falconExe();
    copyTree(engine, out, [&](const std::string& rel) {
        const std::string ext = fs::path(rel).extension().string();
        return rel == "scripts" || rel == "shaderc" || rel == "Game" || rel == "steamcmd" || rel == "content/library" || ext == ".pdb" ||
               ext == ".ilk" || ext == ".lib" || ext == ".dbg" ||
               ext == ".exp" ||
               rel == fs::path(exe).filename().generic_string();
    });
    const fs::path gameExe = out / (name_ + platform::executableSuffix());
    copyFile(exe, gameExe);
    // Its dedicated server: the same executable, headless (--server; its own arguments pass on).
#if defined(_WIN32)
    const fs::path server = out / (name_ + " Server.cmd");
    const std::string script = "@echo off\r\nrem " + name_ + "'s dedicated server: no window, hosting on port 27015 unless --host says\r\n"
                               "rem otherwise. Type status, kick <id>, say <text> or quit while it runs.\r\n\"%~dp0" +
                               name_ + ".exe\" --server %*\r\n";
#else
    // (.command: a double-click in Finder opens it in Terminal)
#if defined(__APPLE__)
    const fs::path server = out / (name_ + " Server.command");
#else
    const fs::path server = out / (name_ + " Server.sh");
#endif
    const std::string script = "#!/bin/sh\n# " + name_ + "'s dedicated server: no window, hosting on port 27015 unless --host says\n"
                               "# otherwise. Type status, kick <id>, say <text> or quit while it runs.\n"
                               "cd \"$(dirname \"$0\")\" && exec \"./" + name_ + "\" --server \"$@\"\n";
#endif
    if (FILE* f = platform::openFile(server, "wb")) {
        std::fwrite(script.data(), 1, script.size(), f);
        std::fclose(f);
    }
#if !defined(_WIN32)
    for (const fs::path& p : {gameExe, server})
        fs::permissions(p, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
#endif
    // The project: its content, not its sources or what the editor keeps for itself.
    copyTree(project, game, [&](const std::string& rel) {
        return rel == "Build" || rel == "Exports" || rel == "Scripts" || rel == ".git" || rel == ".vs" || rel == "Cache/Scripts" ||
               rel.ends_with("/Editor") || rel.ends_with(".lock") ||
               (rel.starts_with("Libraries/") && rel.ends_with("/Code"));  // (compiled into the game)
    });
    copyFile(gamescripts::aotDllPath(projectDir_), game / "Cache" / "Scripts" / "aot" / fs::path(gamescripts::aotDllPath(projectDir_)).filename());
    // A Steam game: SteamPipe's build scripts beside the build (Upload to Steam runs them).
    if (Project project; failed.empty() && loadProject(projectDir_, project, failed) && project.steamAppId) {
        std::string vdf;
        writeSteamPipe(project, output_, "", project.steamBranch, vdf, failed);
    }
    if (!failed.empty()) {
        error_ = failed;
        state_ = State::Failed;
        return;
    }
    state_ = State::Succeeded;
}

ScriptBuild::~ScriptBuild()
{
    stopFalcon(process_);
}

bool ScriptBuild::start(const std::string& projectDir)
{
    if (process_)
        return false;
    projectDir_ = projectDir;
    error_.clear();
    std::error_code ec;
    const fs::path cache = fs::path(projectDir) / "Cache" / "Scripts";
    fs::create_directories(cache, ec);
    log_ = (cache / "build.log").generic_string();
    libraries::writeBuildProps(projectDir);  // (the enabled, trusted libraries' code: Falcon.Game.targets)
    // -clp:NoSummary: each error once (MSBuild repeats them in a summary).
    std::vector<std::string> args = {"build", gamescripts::csprojPath(projectDir), "-c", "Debug", "--nologo", "-v:q", "-clp:NoSummary",
                                     "-p:FalconEngineDir=" + gamescripts::engineScriptsDir(),
                                     // (the script components' source generator: Falcon.Game.targets)
                                     "-p:CustomAfterMicrosoftCommonTargets=" + gamescripts::engineScriptsDir() + "Falcon.Game.targets",
                                     // (editor code built in: libraries' Editor/, Scripts/Editor/)
                                     "-p:FalconEditor=true"};
    // The restore (NuGet's, a few hundred ms) only when the project changed since the last one.
    const auto restored = fs::last_write_time(cache / "obj" / "project.assets.json", ec);
    bool restore = bool(ec);
    gamescripts::forEachSource(projectDir, [&](const fs::directory_entry& e) {
        restore = restore || (e.path().extension() != ".cs" && e.last_write_time(ec) > restored);
    });
    if (!restore)
        args.push_back("--no-restore");
    process_ = startProgramTracked("dotnet", args, log_);
    if (!process_) {
        error_ = "dotnet was not found: install the .NET 10 SDK";
        FX_ERROR("scripts: %s", error_.c_str());
        return false;
    }
    FX_INFO("scripts: building %s", gamescripts::csprojPath(projectDir).c_str());
    return true;
}

ScriptBuild::State ScriptBuild::poll()
{
    if (!process_)
        return State::Idle;
    int code = 0;
    if (!falconFinished(process_, code))
        return State::Running;
    process_ = nullptr;
    // Its output, a line each: "path(line,col): error CS0103: ... [project]".
    std::ifstream f(log_);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        const size_t first = line.find_first_not_of(' ');
        // MSBuild's closing lines ("Build succeeded.", the counts, the time): the result says it.
        if (first == std::string::npos || line.compare(first, 15, "Build succeeded") == 0 || line.compare(first, 12, "Time Elapsed") == 0 ||
            line.find(" Warning(s)") != std::string::npos || line.find(" Error(s)") != std::string::npos)
            continue;
        if (line.find(": error ") != std::string::npos) {
            FX_ERROR("scripts: %s", line.c_str());
            if (error_.empty())
                error_ = line;
        } else if (line.find(": warning ") != std::string::npos) {
            FX_WARN("scripts: %s", line.c_str());
        } else {
            FX_INFO("scripts: %s", line.c_str());
        }
    }
    std::error_code ec;
    if (code != 0 || !fs::exists(gamescripts::dllPath(projectDir_), ec)) {
        if (error_.empty())
            error_ = "dotnet build failed (exit code " + std::to_string(code) + ")";
        FX_ERROR("scripts: build failed");
        return State::Failed;
    }
    FX_INFO("scripts: built %s", gamescripts::dllPath(projectDir_).c_str());
    return State::Succeeded;
}

}  // namespace fx
