#pragma once

#include "PCH.h"

namespace stm::diag
{
    // Attribution tracer for SlaveTatsNG's published C++ interface.
    //
    // SlaveTatsNG hands every interested plugin a pointer to a static table of
    // function pointers (SlaveTatsNG_Interface.h). A plugin that mutates tattoos
    // through that table leaves no other trace: the ledger changes silently and
    // the only visible side effect is whatever the SlaveTatsNG DLL itself logs.
    // This tracer swaps the mutating entries of the table for shims that record
    // the return address of the call site, resolve it to a module + offset and
    // then forward to the real function, which names the exact caller DLL.
    //
    // Scope: C++ consumers only. Papyrus never goes through this table - the VM
    // calls the registered natives directly - so script driven changes are
    // invisible here; the Probe covers those.
    //
    // SKSE delivers a sender's message to one listener per plugin, so this module
    // deliberately does not register its own SlaveTatsNG listener: bridge/
    // SlaveTatsNg owns that listener and calls InstallCallTrace() the moment it
    // stores the published pointer. UninstallCallTrace() writes the original
    // entries back (used when the INI disables the tracer).

    void InstallCallTrace();
    void UninstallCallTrace();
    bool CallTraceInstalled();
}
