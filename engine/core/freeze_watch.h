#pragma once

#include <cstdint>
#include <string>

// Freeze reports: a watchdog thread that notices the main thread stopping (no heartbeat() for
// `seconds`) and writes what it was doing next to the executable, in FreezeReports/ (or under
// %LOCALAPPDATA%\Falcon Engine\FreezeReports when that folder can't be written):
//   freeze-<date>-<time>.txt  every thread's call stack (with function names and lines when
//                             falcon.pdb is beside falcon.exe), the main thread's again 12 s later
//                             if it is still stuck, and the last log lines;
//   freeze-<date>-<time>.dmp  a minidump of the process (open it in Visual Studio with the pdb).
// Once the main thread runs again it logs a warning naming the report (the editor's Log tab shows
// it). Not while a debugger is attached (a breakpoint stops the main thread too).
namespace fx::freezewatch {

// Keeps the last log lines for reports (chains to the log output set before it). Call early.
void keepLog();
// Starts the watchdog for the calling (main) thread; it begins watching at the first heartbeat().
// `reportDir`: "" for the default above.
void start(float seconds, const std::string& reportDir = {});
void stop();
// Once a frame on the main thread.
void heartbeat(uint64_t frame);
// Where the last report went ("" when none was written).
std::string lastReport();
// Tests: the main thread stops for `ms` here (--test-freeze).
void testFreeze(uint32_t ms);

}  // namespace fx::freezewatch
