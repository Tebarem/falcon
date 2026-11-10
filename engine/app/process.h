#pragma once

// Running falcon.exe again: the cooker as a child (new empty scenes), or another session
// (the launcher opening the editor on a project, the editor opening another scene). One
// session per process: Chromium initialises once per process, and a session sets up everything.

#include <cstdint>
#include <string>
#include <vector>

namespace fx {

// This executable's path.
std::string falconExe();
// falcon.exe with `args`, waited for; its exit code (-1: could not start). Its output is ours.
int runFalcon(const std::vector<std::string>& args);
// falcon.exe with `args` in the background (not waited for, outliving this process).
bool startFalcon(const std::vector<std::string>& args);
// falcon.exe with `args` in the background, to be polled (previews): null if it didn't start.
// With `errorFile`, its error output goes there instead of ours (the cooker's, for the message
// when it fails).
// A program in a console window of its own (the player types into it: steamcmd's sign-in), not
// tracked. False: it didn't start.
bool openConsole(const std::vector<std::string>& args);

void* startFalconTracked(const std::vector<std::string>& args, const std::string& errorFile = {});
// Another program (searched for on PATH without a path; dotnet), all its output into
// `outputFile`, to be polled like falcon's (falconFinished, stopFalcon).
void* startProgramTracked(const std::string& program, const std::vector<std::string>& args, const std::string& outputFile);
// Without waiting: true once it has exited (the handle is then closed; `code`: its exit code).
bool falconFinished(void* process, int& code);
// Ends it (it is not needed any more) and closes the handle.
void stopFalcon(void* process);
// Whether it shows a window yet (the launcher stays up until the editor's appears).
bool processShowsWindow(void* process);
// Closes the handle and leaves it running.
void releaseFalcon(void* process);
// This process's id, and whether process `pid` is still running (lock files).
uint32_t currentProcessId();
bool processRunning(uint32_t pid);

}  // namespace fx
