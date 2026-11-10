#pragma once
// The level API (level_api.h) over a ScriptHost's context: live meshes for scripts (a game's, the
// editor's tools). Kept apart from script_host.cpp so the level tools change without it.

#include "script/level_api.h"
#include "script/script_host.h"

namespace fx {

void fillLevelApi(FxLevelApi& api, ScriptHost::Context* context);
// Each frame before the rows stream (ScriptHost::extract): live meshes whose entity is gone are freed.
void levelUpdate(ScriptHost::Context& x);
// The game's end (the editor's Stop): its live meshes and their entities go.
void levelGameEnded(ScriptHost::Context& x);

}  // namespace fx
