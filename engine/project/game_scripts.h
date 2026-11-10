#pragma once

// A project's C# game (see docs/scripting.md, "Project scripts"):
//   Scripts/                   the sources: Game.csproj, Game.cs and whatever else the game adds
//     Directory.Build.props    where the build goes, and the engine's Falcon.Engine.dll by default
//   Cache/Scripts/bin/Game.dll the build the editor loads (JIT, hot-reloadable), with
//                              Falcon.Engine.dll and the runtimeconfig next to it
// The editor builds it with the .NET SDK (dotnet build) in the background, whenever a source
// changes (and at startup when the build is out of date), so Play starts the build there is.

#include <cstdint>
#include <atomic>
#include <string>
#include <thread>

namespace fx {

namespace gamescripts {

std::string csprojPath(const std::string& projectDir);
std::string dllPath(const std::string& projectDir);
// The game published with NativeAOT (File > Build Game): what a game started on its own prefers.
std::string aotDllPath(const std::string& projectDir);
// A shipped game's project (Game/ beside the executable, with its project.falcon), else empty.
std::string shippedProject();
// The engine's Falcon.Engine.dll folder (bin/scripts/jit/, next to falcon.exe), '/'-separated
// and ending with one.
std::string engineScriptsDir();
bool exists(const std::string& projectDir);
// Writes the game's sources (a new project's, or a project without scripts yet). The game
// class is named after the project.
bool create(const std::string& projectDir, const std::string& projectName, std::string& error);
// No build yet, a source (.cs, .csproj, .props, .targets under Scripts/) newer than it, or a build
// for another engine (below).
bool stale(const std::string& projectDir);
// The sources as they are now (a hash of the paths, sizes and times of the .cs, .csproj, .props
// and .targets files under Scripts/): another value, another set of sources. Never 0.
uint64_t sourcesStamp(const std::string& projectDir);
// The build's copy of Falcon.Engine.dll is this engine's (byte for byte). Another one (an engine
// rebuilt since) must not be loaded: it would hold the process's one Falcon.Engine, and fail the
// API version check or, worse, pass it with another layout.
bool builtForThisEngine(const std::string& projectDir);

}  // namespace gamescripts

// One `dotnet build` of a project's game at a time, polled without waiting. Its output goes to
// Cache/Scripts/build.log, and to the log (the editor's Console) when it is done: errors as
// errors, warnings as warnings.
// File > Build Game: the game shipped. dotnet publish compiles it (its libraries' code too, no
// editor code) with NativeAOT into Cache/Scripts/aot/Game.dll; then, on a thread, Build/<name>/
// gets the engine (this executable as <name>.exe, its DLLs and folders, without debug files and
// the engine's own script builds) and Game/: the project (scenes, assets, libraries, its UI, the
// cooked Cache with the published game) without its sources and the editor's builds. <name>.exe
// alone plays it (gamescripts::shippedProject).
struct Project;

// A built game's folder: <project>/Build/<name> (the name made safe for a file name).
std::string buildDir(const std::string& projectDir, const std::string& name);

// SteamPipe's scripts for a built game (Build/<name>/): app_build_<app>.vdf and
// depot_build_<depot>.vdf in Build/<name> SteamPipe/ (beside it, so they aren't uploaded), the
// build's description and the branch to set it live on ("": none). `vdf`: the app build's path.
bool writeSteamPipe(const Project& project, const std::string& buildDir, const std::string& description, const std::string& branch,
                    std::string& vdf, std::string& error);

class GameBuild {
public:
    enum class State { Idle, Publishing, Copying, Succeeded, Failed };
    ~GameBuild();
    bool start(const std::string& projectDir, const std::string& name);
    State poll();  // Succeeded and Failed once, then Idle
    bool running() const { return state_ != State::Idle; }
    const std::string& error() const { return error_; }
    const std::string& output() const { return output_; }  // the Build/<name>/ folder

private:
    void copy();
    void* process_ = nullptr;
    std::thread thread_;
    std::atomic<State> state_ = State::Idle;
    std::string projectDir_, name_, log_, error_, output_;
};

class ScriptBuild {
public:
    enum class State { Idle, Running, Succeeded, Failed };

    ~ScriptBuild();
    bool start(const std::string& projectDir);
    // Running until the build ends; then Succeeded or Failed once, and Idle after that.
    State poll();
    bool running() const { return process_ != nullptr; }
    const std::string& error() const { return error_; }  // the first compiler error, or why it failed

private:
    void* process_ = nullptr;
    std::string projectDir_, log_, error_;
};

}  // namespace fx
