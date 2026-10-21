#pragma once
// Tests only, not part of the C ABI: lets a test give the players dc_load builds a fake output and memory
// decoders (tests/fake_output.h), so the C API's threading can be tested without a device. The players are
// still built inside the library; only their PlayerDeps come from here. NULL goes back to the platform's.
#include "daphne_core.h"
#include "daphne/engine/player.h"

extern "C" DC_API void dc_internal_player_deps(const daphne::PlayerDeps* deps);
