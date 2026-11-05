#pragma once

#include "PCH.h"

#include <string>
#include <string_view>

// Reading the *live* overlay state off an actor's 3D.
//
// SlaveTats keeps its own ledger in JContainers, and that ledger says what
// should be on the body - not what is. The only way to tell "the ledger is
// right" from "the ledger is right and the body lost it" is to look at the
// scene graph, which is what this does: for "<Area> [OvlN]" it reports the
// node's type, the geometry's shader alpha, the tint and the diffuse texture
// path.
//
// Two callers, one implementation:
//   - diag/Probe: samples the player and diffs consecutive samples;
//   - ops/LoadRepair: decides whether a character needs its overlays re-applied
//     after a load.
//
// Everything here must run on the game thread: it walks live NiAVObject trees.
namespace stm::diag::overlay
{
    // "<Area> [OvlN]" state, or "absent" when the node does not exist.
    std::string ReadRenderState(RE::NiAVObject* a_root, std::string_view a_area, int a_slot);

    // RaceMenu parks its own blank overlay texture in unused slots, so "a node
    // exists" is not the same as "a tattoo is showing".
    bool LooksLikeBlankSlot(std::string_view a_state);

    // Number of slots that currently show something other than the blank
    // texture. a_slots is the area's slot count (ng::MaxSlots).
    int RenderCount(RE::NiAVObject* a_root, std::string_view a_area, int a_slots);
}
