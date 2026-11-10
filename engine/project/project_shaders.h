#pragma once

// (R2) A project's shaders: Shaders/*.slang (and enabled libraries' Shaders/), compiled in the
// background with the editor's falcon_shaderc (bin/shaderc/) as they change, one at a time, into
// Cache/Shaders/<path, folders as _>.fshader (render/shader_file.h): the game loads those, and
// Build Game ships them (never the compiler). Errors go to the log (the editor's Console) with
// their file and line. revision() counts the shaders written: their users load them again.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fx {

class ProjectShaders {
public:
    ~ProjectShaders();
    void setProject(const std::string& dir);
    // A few times a second (now: at once): a changed source compiled, a finished compile taken.
    void update(bool now = false);
    bool busy() const { return process_ != nullptr || !engineJobs_.empty() || pending_; }
    uint64_t revision() const { return revision_; }
    struct Status {
        bool compiled = false;
        std::string error;  // the first error (empty: none)
    };
    const Status* status(const std::string& relative) const;  // by its path in the project
    static std::string blobPath(const std::string& projectDir, const std::string& relative);
    // (R4) The project's surface shaders (Shaders/*.slang with a `surface` function), in the order
    // their ids go (a material's id is its place here + 1), and the engine's shaders built with them
    // (none: the engine's own): the resolve, and (T2) the transparent pass. Compiled side by side;
    // revision() moves once all of them are written.
    static std::vector<std::string> surfaceShaders(const std::string& projectDir);
    static std::string resolvePath(const std::string& projectDir);
    static std::string transparentPath(const std::string& projectDir);
    static std::string deformPath(const std::string& projectDir);  // (V1) the vertex pass
    // (V1) Whether a surface shader has a vertex function (its text has `void vertex(`).
    static bool hasVertexFunction(const std::string& projectDir, const std::string& relative);
    static std::string compiler();  // falcon_shaderc next to the engine ("" when not shipped)

private:
    void finish(int code);
    bool startCompile(const std::vector<std::string>& args);
    void buildEngine(const std::vector<std::string>& surfaces);  // generated module + the compiles
    void finishEngine();
    std::vector<std::string> engineBlobs() const;
    std::string dir_;
    std::string surfacesStamp_;  // the surface shaders and their times as the resolve was built
    std::vector<std::string> resolving_;  // the surface shaders the running engine build is of
    struct EngineJob {
        std::string blob, log;
        void* process = nullptr;
        int code = -1;
    };
    std::vector<EngineJob> engineJobs_;
    bool engineFailed_ = false;  // the engine's shaders being built with the surface shaders
    struct Source {
        int64_t stamp = 0;  // the file's time when last compiled (0: never)
        Status status;
    };
    std::map<std::string, Source> sources_;
    void* process_ = nullptr;
    std::string compiling_, log_;
    int64_t compilingStamp_ = 0;
    uint64_t revision_ = 0, scannedNs_ = 0;
    bool pending_ = false;  // a source changed, not compiled yet
};

}  // namespace fx
