#pragma once

#include "PCH.h"

namespace stm::ops::pending
{
    // Finishes the syncs SlaveTatsNG drops on the floor.
    //
    // SlaveTatsNG sets ".SlaveTats.updated = 1" whenever an actor's ledger
    // changes, and synchronize_tattoos_internal clears that flag - but only on
    // the path that reaches the end. Every abort path returns early and leaves
    // the flag set:
    //
    //   * the actor's 3D is not loaded (the common one: a tattoo applied while
    //     the target is out of view, or before its body finished loading);
    //   * ni_node_override_lock::lock() failed because something else holds it;
    //   * NiOverride::AddOverlays() or upgrade_tattoos() failed.
    //
    // Nothing in SlaveTatsNG ever reads the flag again - the plugin has no
    // maintenance loop - so one aborted sync strands the actor for the rest of
    // the session. The ledger says the tattoo is there, the body never gets it,
    // and the only way out is a manual "force sync". That is exactly the
    // report this exists for: "the tattoo shows in the menu but not on the
    // body, and only syncing in the menu makes it appear".
    //
    // It is not specific to this menu, and it is why applying the tattoo
    // through the menu cannot reproduce it: an apply from here always ends in
    // an explicit synchronise on an actor whose 3D is loaded. A mod that
    // applies tattoos from an animation event, on an actor it has not looked
    // at, hits the deferred path instead.
    //
    // While the window is open this samples the current target and the player,
    // and when the flag has stayed set for longer than a real sync takes, it
    // issues the sync the game never got around to. Rate limited, capped, and
    // limited to the actors this menu is looking at, so it cannot fight another
    // mod's own sync schedule.
    void Tick();

    // Forgets every pending actor (feature switched off, game load).
    void Reset();
}
