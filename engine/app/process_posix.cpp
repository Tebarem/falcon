// app/process.h on Linux and macOS: posix_spawn, waitpid, kill.
#include "app/process.h"

#include "core/log.h"
#include "core/platform.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

extern char** environ;

namespace fx {

namespace {

// A started process, for falconFinished / stopFalcon / releaseFalcon.
struct Child {
    pid_t pid = 0;
};

// `program` with `args`, its output going where ours does. The child gets our stdin, stdout and
// stderr and no other file descriptors (Chromium's IPC sockets stay ours). `errorFile`: its
// error output goes there instead; with `allToFile`, its standard output too. A program without
// a path is searched for (PATH). 0: it didn't start.
pid_t spawnProgram(const std::string& program, const std::vector<std::string>& args, const std::string& errorFile = {},
                   bool allToFile = false)
{
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (!errorFile.empty()) {
        posix_spawn_file_actions_addopen(&actions, 2, errorFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (allToFile)
            posix_spawn_file_actions_adddup2(&actions, 2, 1);
    }
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
#if defined(__APPLE__)
    // Nothing inherited but what is named.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    for (int fd = 0; fd < 3; ++fd)
        if (errorFile.empty() || (fd != 2 && !(allToFile && fd == 1)))
            posix_spawn_file_actions_addinherit_np(&actions, fd);
#elif defined(__GLIBC__)
    posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
    pid_t pid = 0;
    const int error = program.find('/') == std::string::npos
                          ? posix_spawnp(&pid, program.c_str(), &actions, &attributes, argv.data(), environ)
                          : posix_spawn(&pid, program.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (error != 0) {
        FX_ERROR("process: cannot start %s (%s)", program.c_str(), std::strerror(error));
        return 0;
    }
    return pid;
}

void* track(pid_t pid) { return pid ? new Child{pid} : nullptr; }

}  // namespace

std::string falconExe() { return platform::utf8(platform::executableDir() / "falcon"); }

int runFalcon(const std::vector<std::string>& args)
{
    const pid_t pid = spawnProgram(falconExe(), args);
    if (!pid)
        return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void* startFalconTracked(const std::vector<std::string>& args, const std::string& errorFile)
{
    return track(spawnProgram(falconExe(), args, errorFile));
}

bool openConsole(const std::vector<std::string>& args)
{
    if (args.empty())
        return false;
    // A terminal window of its own, through a script that runs the command.
    std::string line;
    for (const std::string& a : args) {
        std::string quoted = "'";
        for (char c : a)
            quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        line += (line.empty() ? "" : " ") + quoted + "'";
    }
    std::error_code ec;
    const std::filesystem::path script =
        std::filesystem::temp_directory_path(ec) / ("falcon-console-" + std::to_string(getpid()) + ".command");
    {
        std::ofstream out(script);
        out << "#!/bin/sh\n" << line << "\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
#if defined(__APPLE__)
    const pid_t pid = spawnProgram("open", {"-a", "Terminal", script.string()});
#else
    const pid_t pid = spawnProgram("x-terminal-emulator", {"-e", script.string()});
#endif
    if (pid)
        waitpid(pid, nullptr, WNOHANG);
    return pid != 0;
}

void* startProgramTracked(const std::string& program, const std::vector<std::string>& args, const std::string& outputFile)
{
    return track(spawnProgram(program, args, outputFile, true));
}

uint32_t currentProcessId() { return uint32_t(getpid()); }

bool processRunning(uint32_t pid)
{
    if (!pid)
        return false;
    return kill(pid_t(pid), 0) == 0 || errno == EPERM;  // (EPERM: it exists, someone else's)
}

bool falconFinished(void* process, int& code)
{
    if (!process)
        return false;
    auto* child = static_cast<Child*>(process);
    int status = 0;
    const pid_t done = waitpid(child->pid, &status, WNOHANG);
    if (done == 0 || (done < 0 && errno == EINTR))
        return false;
    code = done > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    delete child;
    return true;
}

void stopFalcon(void* process)
{
    if (!process)
        return;
    auto* child = static_cast<Child*>(process);
    kill(child->pid, SIGKILL);
    waitpid(child->pid, nullptr, 0);
    delete child;
}

bool processShowsWindow(void* process)
{
    // (no portable way to ask another process for its windows: running is taken as shown)
    if (!process)
        return false;
    return kill(static_cast<Child*>(process)->pid, 0) == 0;
}

void releaseFalcon(void* process) { delete static_cast<Child*>(process); }

bool startFalcon(const std::vector<std::string>& args) { return spawnProgram(falconExe(), args) != 0; }

}  // namespace fx
