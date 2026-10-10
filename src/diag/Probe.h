#pragma once

#include "PCH.h"

namespace stm::diag
{
    // Diagnostic probe for the "my tattoos vanished" class of problems.
    //
    // Upstream code has several paths that leave the SlaveTats ledger and the
    // live NiOverride overlay nodes out of sync, and the game gives no feedback
    // about any of them:
    //   * synchronize_tattoos silently aborts (without clearing .updated) when
    //     the actor's 3D is not loaded yet;
    //   * the sync is cancelled outright while .SlaveTats.updated is 0, so the
    //     two sides can drift apart and never recover;
    //   * when .SlaveTats.version reads empty, upgrade_tattoos wipes the ledger
    //     and rebuilds it from the 3D nodes, losing colour/glow details;
    //   * slots that hold another mod's overlay are skipped by SlaveTats.
    //
    // The probe samples both sides on a timer and logs only the differences,
    // plus a one-line verdict that names the most likely cause.
    //
    // The sampler thread only posts work to the game thread - every engine and
    // JContainers call happens there.

    void Start();
    void Stop();
    bool IsRunning();

    // Queues a full one-off dump on the game thread.
    void LogSnapshot();
}
