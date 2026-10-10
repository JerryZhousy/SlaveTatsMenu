#pragma once

#include "PCH.h"

namespace stm::ng
{
    // Upstream contract (SlaveTatsNG/papyrus/Source/Scripts/SlaveTatsOnLoad.psc):
    //
    //   event OnPlayerLoadGame()
    //       int targets = JDB.solveObj(".SlaveTatsMenu.known_targets")
    //       ... SlaveTats.synchronize_tattoos(who) for every entry ...
    //
    // The matching writer upstream is SlaveTatsMCMMenu, which appends the
    // current target every time the MCM is closed. Because this plugin tattoos
    // actors without going through the MCM, it has to maintain the same list -
    // otherwise a save/load cycle drops every tattoo we applied to followers.
    //
    // Must be called on the game thread.
    void RegisterKnownTarget(RE::Actor* a_actor);

    // Diagnostics only: current length of the list, -1 when unavailable.
    int KnownTargetCount();
}
