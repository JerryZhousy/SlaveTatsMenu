#pragma once

#include "PCH.h"

namespace stm::ops
{
    // Saves every tattoo currently applied to the target as a named preset.
    // Game thread, reports through the result board like every other operation.
    void CapturePreset(RE::ActorHandle a_actor, std::string a_presetName);

    // Re-applies a stored preset to the target. When a_clearFirst is set the
    // target's existing tattoos are removed first, so the result matches the
    // preset exactly instead of piling on top of whatever is already there.
    void ApplyPreset(RE::ActorHandle a_actor, std::string a_presetName, bool a_clearFirst);
}
