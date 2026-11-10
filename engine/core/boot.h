#pragma once

namespace fx::boot {

// Milliseconds since the OS created this process (includes loader and DLL init),
// so boot timings reflect what a player actually waits for.
double msSinceProcessStart();

// Logs "<label>: <ms>" relative to process start. Cheap; meant for boot phases.
void mark(const char* label);

}  // namespace fx::boot
