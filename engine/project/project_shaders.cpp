#include "project/project_shaders.h"

#include "app/process.h"
#include "core/log.h"
#include "core/platform.h"
#include "project/libraries.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace fx {

// DXIL (DXC beside falcon_shaderc) on Windows; SPIR-V and MSL only elsewhere.
#if defined(_WIN32)
constexpr const char* kDxilFlag = "--dxc";
#else
constexpr const char* kDxilFlag = "--no-dxil";
#endif

namespace fs = std::filesystem;

ProjectShaders::~ProjectShaders()
{
    stopFalcon(process_);
    for (EngineJob& j : engineJobs_)
        stopFalcon(j.process);
}

namespace {

// A compile's log into the console, and its first error (with where it is) into the status.
void readLog(const std::string& path, ProjectShaders::Status& status)
{
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty())
            continue;
        const bool isError = line.find("error") != std::string::npos;
        if (isError && status.error.empty())
            status.error = line;
        // (Slang puts where it is on a line of its own: "  --> path:line:column")
        if (const size_t arrow = line.find("--> "); arrow != std::string::npos && !status.error.empty() &&
                                                    status.error.find(" at ") == std::string::npos)
            status.error += " at " + line.substr(arrow + 4);
        if (isError)
            FX_ERROR("shaders: %s", line.c_str());
        else
            FX_INFO("shaders: %s", line.c_str());
    }
}

}  // namespace

void ProjectShaders::setProject(const std::string& dir)
{
    dir_ = dir;
    sources_.clear();
}

std::string ProjectShaders::compiler()
{
    const char* base = SDL_GetBasePath();
    const fs::path exe = fs::path(base ? base : "") / "shaderc" / (std::string("falcon_shaderc") + platform::executableSuffix());
    std::error_code ec;
    return fs::exists(exe, ec) ? exe.generic_string() : std::string();
}

std::string ProjectShaders::blobPath(const std::string& projectDir, const std::string& relative)
{
    std::string name = relative;
    for (char& c : name)
        if (c == '/' || c == '\\')
            c = '_';
    return (fs::path(projectDir) / "Cache" / "Shaders" / fs::path(name).replace_extension(".fshader")).generic_string();
}

std::vector<std::string> ProjectShaders::surfaceShaders(const std::string& projectDir)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fs::path(projectDir) / "Shaders", ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".slang")
            continue;
        std::ifstream f(it->path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (text.find("void surface(") != std::string::npos)
            out.push_back(it->path().lexically_relative(projectDir).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string ProjectShaders::resolvePath(const std::string& projectDir)
{
    // (resolve2: the layered surfaces' bindings; an older build's resolve is not read)
    return (fs::path(projectDir) / "Cache" / "Shaders" / "resolve2.fshader").generic_string();
}

std::string ProjectShaders::transparentPath(const std::string& projectDir)
{
    return (fs::path(projectDir) / "Cache" / "Shaders" / "transparent.fshader").generic_string();
}

std::string ProjectShaders::deformPath(const std::string& projectDir)
{
    return (fs::path(projectDir) / "Cache" / "Shaders" / "deform.fshader").generic_string();
}

bool ProjectShaders::hasVertexFunction(const std::string& projectDir, const std::string& relative)
{
    std::ifstream f(fs::path(projectDir) / relative, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return text.find("void vertex(") != std::string::npos;
}

std::vector<std::string> ProjectShaders::engineBlobs() const
{
    return {resolvePath(dir_), transparentPath(dir_), deformPath(dir_)};
}

bool ProjectShaders::startCompile(const std::vector<std::string>& args)
{
    const std::string exe = compiler();
    if (exe.empty())
        return false;
    std::error_code ec;
    fs::create_directories(fs::path(dir_) / "Cache" / "Shaders", ec);
    log_ = (fs::path(dir_) / "Cache" / "Shaders" / "compile.log").generic_string();
    process_ = startProgramTracked(exe, args, log_);
    return process_ != nullptr;
}

// The project's builds of the engine's shaders: a project_surfaces module calling each surface
// shader (each file's text in a namespace of its own, its imports of falcon_surface left out: the
// module has them), then the resolve and the transparent pass compiled against it, side by side.
void ProjectShaders::buildEngine(const std::vector<std::string>& surfaces)
{
    const fs::path tools = fs::path(compiler()).parent_path(), gen = fs::path(dir_) / "Cache" / "Shaders" / "gen";
    std::error_code ec;
    fs::create_directories(gen, ec);
    std::string m = "// Generated by the editor from the project's surface shaders (R4). Do not edit.\n\nimport falcon_surface;\n";
    std::string surfaceCases, lightCases, vertexCases;
    for (size_t i = 0; i < surfaces.size(); ++i) {
        std::ifstream f(fs::path(dir_) / surfaces[i], std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), body;
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);)
            body += (line.find("import falcon_surface") != std::string::npos ? std::string() : line) + "\n";  // (lines kept: #line holds)
        const std::string ns = "falcon_surface" + std::to_string(i + 1);
        m += "\nnamespace " + ns + "\n{\n#line 1 \"" + (fs::path(dir_) / surfaces[i]).generic_string() + "\"\n" + body + "}\n";
        surfaceCases += "    case " + std::to_string(i + 1) + ": " + ns + "::surface(i, o); break;\n";
        if (text.find("void light(") != std::string::npos)
            lightCases += "    case " + std::to_string(i + 1) + ": " + ns + "::light(l, color); break;\n";
        if (text.find("void vertex(") != std::string::npos)
            vertexCases += "    case " + std::to_string(i + 1) + ": " + ns + "::vertex(i, o); break;\n";
    }
    m += "\nvoid projectSurface(uint shader, SurfaceIn i, inout SurfaceOut o)\n{\n    switch (shader)\n    {\n" + surfaceCases +
         "    default: break;\n    }\n}\n";
    m += "\nvoid projectLight(uint shader, LightIn l, inout float3 color)\n{\n    switch (shader)\n    {\n" + lightCases +
         "    default: break;\n    }\n}\n";
    m += std::string("\nstatic const bool kProjectHasVertex = ") + (vertexCases.empty() ? "false" : "true") + ";\n";
    m += "\nvoid projectVertex(uint shader, VertexIn i, inout VertexOut o)\n{\n    switch (shader)\n    {\n" + vertexCases +
         "    default: break;\n    }\n}\n";
    std::ofstream(gen / "project_surfaces.slang", std::ios::binary) << m;
    const std::string exe = compiler();
    const std::pair<const char*, std::string> builds[] = {{"visbuffer_resolve.slang", resolvePath(dir_)},
                                                          {"transparent.slang", transparentPath(dir_)},
                                                          {"vertex_deform.slang", deformPath(dir_)}};
    engineJobs_.clear();
    for (const auto& [source, blob] : builds) {
        EngineJob j;
        j.blob = blob;
        j.log = (fs::path(dir_) / "Cache" / "Shaders" / (fs::path(source).stem().string() + ".log")).generic_string();
        const std::vector<std::string> args = {"--root",    (tools / "engine").generic_string(), "--source", source,
                                               "--blob",    blob,                                kDxilFlag,  kDxilFlag == std::string("--dxc") ? tools.generic_string() : "1",
                                               "--include", gen.generic_string(), "--include", (tools / "include").generic_string()};
        j.process = startProgramTracked(exe, args, j.log);
        if (j.process)
            engineJobs_.push_back(std::move(j));
    }
    if (engineJobs_.size() == std::size(builds)) {
        resolving_ = surfaces;
        FX_INFO("shaders: building the resolve, the transparent pass and the vertex pass with %zu surface shader%s", surfaces.size(),
                surfaces.size() == 1 ? "" : "s");
    } else {
        for (EngineJob& j : engineJobs_)
            stopFalcon(j.process);
        engineJobs_.clear();
        for (const std::string& s : surfaces)
            sources_[s].status = {false, "the shader compiler did not start"};
    }
}

void ProjectShaders::finishEngine()
{
    Status status;
    bool ok = true;
    for (const EngineJob& j : engineJobs_) {
        readLog(j.log, status);
        ok &= j.code == 0;
    }
    status.compiled = ok;
    engineFailed_ = !ok;  // (not built again until a surface shader changes)
    if (ok) {
        ++revision_;
        FX_INFO("shaders: compiled the resolve, the transparent pass and the vertex pass with the surface shaders");
    } else if (status.error.empty()) {
        status.error = "the engine's shaders: the compiler failed";
        FX_ERROR("shaders: %s", status.error.c_str());
    }
    for (const std::string& r : resolving_)  // (every surface shader shares the builds' result)
        sources_[r].status = status;
    resolving_.clear();
    engineJobs_.clear();
}

const ProjectShaders::Status* ProjectShaders::status(const std::string& relative) const
{
    const auto it = sources_.find(relative);
    return it == sources_.end() ? nullptr : &it->second.status;
}

void ProjectShaders::finish(int code)
{
    process_ = nullptr;
    Source& s = sources_[compiling_];
    s.status = {};
    readLog(log_, s.status);
    s.status.compiled = code == 0;
    s.stamp = compilingStamp_;
    if (code == 0) {
        ++revision_;
        FX_INFO("shaders: compiled %s", compiling_.c_str());
    } else if (s.status.error.empty()) {
        s.status.error = compiling_ + ": the compiler failed (exit code " + std::to_string(code) + ")";
        FX_ERROR("shaders: %s", s.status.error.c_str());
    }
}

void ProjectShaders::update(bool now)
{
    if (dir_.empty())
        return;
    if (!engineJobs_.empty()) {
        bool done = true;
        for (EngineJob& j : engineJobs_)
            if (j.process && falconFinished(j.process, j.code))
                j.process = nullptr;
            else
                done &= j.process == nullptr;
        if (done)
            finishEngine();
    }
    if (process_) {
        int code = 0;
        if (!falconFinished(process_, code))
            return;
        finish(code);
    }
    const uint64_t t = SDL_GetTicksNS();
    if (!now && t - scannedNs_ < 250'000'000ull)
        return;
    scannedNs_ = t;
    // The sources: the project's Shaders/ and its libraries'.
    std::vector<std::pair<std::string, int64_t>> found;
    std::error_code ec;
    std::vector<fs::path> roots = {fs::path(dir_) / "Shaders"};
    for (const libraries::Library& l : libraries::scan(dir_))
        if (l.enabled && l.trusted)
            roots.push_back(fs::path(dir_) / "Libraries" / l.name / "Shaders");
    for (const fs::path& r : roots)
        for (auto it = fs::recursive_directory_iterator(r, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".slang")
                found.push_back({it->path().lexically_relative(dir_).generic_string(),
                                 int64_t(it->last_write_time(ec).time_since_epoch().count())});
    pending_ = false;
    // (R4) Surface shaders: not compiled on their own; the resolve is built again with all of them
    // when any changes (and the engine's own is used again when the last goes).
    const std::vector<std::string> surfaces = surfaceShaders(dir_);
    std::string surfacesStamp;
    for (const auto& [relative, stamp] : found)
        if (std::find(surfaces.begin(), surfaces.end(), relative) != surfaces.end())
            surfacesStamp += relative + "@" + std::to_string(stamp) + ";";
    const std::vector<std::string> blobs = engineBlobs();
    const bool missing = std::any_of(blobs.begin(), blobs.end(), [&](const std::string& b) { return !fs::exists(b, ec); });
    if (surfacesStamp != surfacesStamp_ || (!surfaces.empty() && missing && !engineFailed_)) {
        if (!engineJobs_.empty()) {
            pending_ = true;
        } else if (surfaces.empty()) {
            surfacesStamp_ = surfacesStamp;
            bool removed = false;
            for (const std::string& b : blobs)
                removed |= fs::remove(b, ec);
            if (removed)
                ++revision_;
        } else if (!compiler().empty()) {
            surfacesStamp_ = surfacesStamp;
            engineFailed_ = false;
            buildEngine(surfaces);
        }
    }
    for (const auto& [relative, stamp] : found) {
        if (std::find(surfaces.begin(), surfaces.end(), relative) != surfaces.end())
            continue;
        Source& s = sources_[relative];
        const bool blob = fs::exists(blobPath(dir_, relative), ec);
        if (s.stamp == stamp && (blob || !s.status.compiled))
            continue;
        if (process_) {
            pending_ = true;
            continue;
        }
        const std::string exe = compiler();
        if (exe.empty()) {
            if (s.status.error.empty())
                FX_WARN("shaders: %s not compiled: the editor's shader compiler (bin/shaderc) is not there", relative.c_str());
            s.status.error = "no shader compiler";
            s.stamp = stamp;
            continue;
        }
        const fs::path tools = fs::path(exe).parent_path();
        std::vector<std::string> args = {"--root", dir_, "--source", relative, "--blob", blobPath(dir_, relative), kDxilFlag, kDxilFlag == std::string("--dxc") ? tools.generic_string() : "1"};
        if (fs::exists(tools / "include", ec))
            args.insert(args.end(), {"--include", (tools / "include").generic_string()});
        if (!startCompile(args)) {
            s.status.error = "the shader compiler did not start";
            s.stamp = stamp;
            continue;
        }
        compiling_ = relative;
        compilingStamp_ = stamp;
        pending_ = true;
    }
    // Sources gone: their status too.
    for (auto it = sources_.begin(); it != sources_.end();)
        it = std::none_of(found.begin(), found.end(), [&](const auto& f) { return f.first == it->first; }) && it->first != compiling_ &&
                     std::find(resolving_.begin(), resolving_.end(), it->first) == resolving_.end()
                 ? sources_.erase(it)
                 : std::next(it);
}

}  // namespace fx
