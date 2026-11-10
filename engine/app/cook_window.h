#pragma once

// A project scene cooked before a session opens it, when nothing cooked it first (the editor
// and the launcher cook with their own progress): opened from the command line, or models
// changed by hand. Cooking 100+ models takes minutes, so it gets a small window, its title the
// stage and its body a progress bar (closing it cancels), and a failure gets a message box.
// Hidden sessions (tests) only log.

#include "project/project.h"

#include <string>

namespace fx {

bool cookSceneWithProgress(const Project& project, const std::string& relative, bool hidden, std::string& error);

}  // namespace fx
