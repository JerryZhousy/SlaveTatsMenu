#pragma once

#include "PCH.h"

// Repairing what a load drops.
//
// SlaveTats keeps the authoritative list of tattoos per actor in JContainers and
// re-applies it to the body when it is told to. After a load that re-application
// is a no-op: SlaveTatsOnLoad walks its known targets and calls
// synchronize_tattoos(), and the native side cancels immediately while
// ".SlaveTats.updated" is 0 ("nothing has changed"), so nothing is put back on a
// body that lost its overlays in the meantime.
//
// Whatever does the dropping - a 3D rebuild, OBody re-applying its preset a few
// seconds after the load, a cell change - is outside this plugin. What this does
// is the narrow, safe half: once per load, look at the characters whose 3D is
// actually loaded, compare the ledger against the overlays that are really on
// the body, and force a synchronisation only where the body is missing entries
// the ledger claims.
//
// The comparison is what keeps this from being destructive. Forcing a sync on a
// character whose overlays are fine would clear the whole slot range and rebuild
// it from the ledger, taking any other mod's overlays with it; a character that
// is merely short of overlays gets exactly the re-apply it needs.
namespace stm::ops::loadrepair
{
    // Starts the post-load schedule. Call from kPostLoadGame on the game thread.
    // A second call while a schedule is running replaces it, which is what a
    // quick load-after-load needs.
    void Arm();

    // Stops and joins the timer thread. Call from the plugin's atexit handler,
    // before the DLL is unmapped.
    void Shutdown();
}
