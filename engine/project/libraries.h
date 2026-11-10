#pragma once

// (Editor libraries, E1) Shareable libraries: folders of code, UI and assets a project uses, shared
// as a folder, a .zip or a git repository.
//
//   MyWorld/Libraries/<name>/
//     library.json   {"name", "version", "description", "author", "dependencies": [names]}
//     Code/          C# built into the game (script components, systems): ships with it
//     Editor/        C# for the editor only (E2): never shipped
//     UI/            HTML / JS / CSS for its editor panels (E3)
//     Assets/        models, materials, sounds, prefabs: the project's, as Assets/'s are
//
// project.falcon lists where each came from and whether it is on:
//   "libraries": [{"name", "source": "folder" | "zip" | "git" | "local", "ref", "enabled"}]
// A library in the folder that the list doesn't know is the project's too (copied in by hand, or
// part of a project made elsewhere). Code runs with the editor's and the game's full access, so a
// library's code is built only once this user trusts it: trust is the user's (the editor's app
// data, trusted-libraries.json), by name and version. Adding one through the Library Manager
// trusts it; the manager warns about that first.

#include <functional>
#include <string>
#include <vector>

namespace fx::libraries {

struct Library {
    std::string name;  // its folder under Libraries/ (library.json's "name" when it has one)
    std::string version, description, author;
    std::vector<std::string> dependencies;
    std::string source, ref;  // where it came from (project.falcon), "" when it doesn't say
    bool enabled = true;      // project.falcon; on unless turned off
    bool trusted = false;     // this user trusts its code (name@version)
    bool code = false, editor = false, ui = false, assets = false;  // the folders it has
    std::string error;        // what is wrong with it ("" when nothing): a bad library.json, a missing dependency
};

// The project's libraries (Libraries/*), in the order their dependencies need.
std::vector<Library> scan(const std::string& projectDir);
// What the game builds and the project has: the Code/ folders of enabled, trusted libraries
// (absolute), and the asset folders (relative to the project: "Assets", then each enabled library's).
std::vector<std::string> codeDirs(const std::string& projectDir);
std::vector<std::string> editorDirs(const std::string& projectDir);  // their Editor/ folders (the editor's builds only)
std::vector<std::string> assetDirs(const std::string& projectDir);
// Cache/Scripts/Libraries.props: the game build's library sources (Falcon.Game.targets imports it).
// Returns whether it changed.
bool writeBuildProps(const std::string& projectDir);

// Trust: this user's, by name@version. `file`: where it is kept instead of the editor's app data
// (--no-prefs: tests keep theirs in the project's Cache).
void setTrustFile(const std::string& file);
bool trusted(const std::string& name, const std::string& version);
void setTrusted(const std::string& name, const std::string& version, bool trust);

// project.falcon's entry for a library: where it came from, and whether it is on.
bool setEntry(const std::string& projectDir, const std::string& name, const std::string& source, const std::string& ref, bool enabled,
              std::string& error);
bool setEnabled(const std::string& projectDir, const std::string& name, bool enabled, std::string& error);
// Removes the library: its folder and its entry.
bool remove(const std::string& projectDir, const std::string& name, std::string& error);
// A new, empty library (its folders and a library.json, a component to start from), trusted.
bool create(const std::string& projectDir, const std::string& name, std::string& error);

// Installing: a library folder copied, a .zip extracted (Windows' tar), or a git repository cloned
// (git; `ref` a branch or tag, "" its default) into Libraries/<its name>. The library.json may be at
// the top or one folder down (a zip of the folder, a repository's subfolder). Trusted, entered in
// project.falcon. Runs on a thread (a clone takes a while): `done` is called on it.
struct InstallResult {
    bool ok = false;
    std::string name, error;
};
void install(const std::string& projectDir, const std::string& source, const std::string& location, const std::string& ref,
             std::function<void(InstallResult)> done);
// A library as a .zip to share (its folder's contents): `out` (default: Exports/<name>-<version>.zip
// in the project). Returns the zip's path, "" on failure.
std::string exportZip(const std::string& projectDir, const std::string& name, const std::string& out, std::string& error);

}  // namespace fx::libraries
