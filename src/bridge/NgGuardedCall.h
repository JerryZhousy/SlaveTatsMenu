#pragma once

#include "PCH.h"

// The real Addresses struct: a forward declaration here would declare a second
// slavetats::Addresses next to the one the interface header publishes through
// `using namespace slavetats::interface`, and hide it.
#include "SlaveTatsNG_Interface.h"

namespace stm::ng
{
    // SlaveTatsNG's external_slots(). It walks the NiOverride node of every slot
    // in an area, through skee64's address table, and it is the only upstream
    // entry point this plugin calls that touches the engine's node graph rather
    // than JContainers.
    using ExternalSlotsFn = slavetats::fail_t (*)(RE::Actor*, RE::BSFixedString, int);

    enum class GuardedResult
    {
        kOk,        // upstream answered; the array holds the slot numbers
        kUpstream,  // upstream reported an error (fail_t true)
        kFaulted,   // the call raised a hardware exception; a_exceptionCode is set
    };

    // Calls external_slots() with a hardware-exception guard around it.
    //
    // A mod calling another mod's internals has no way to know whether the
    // build it is talking to still behaves the way its header says: this one
    // resolves NiOverride through skee64's hard-coded address table, matched by
    // DLL hash. When that does not line up, the failure is an access violation
    // inside someone else's code, and without a guard it takes the game down
    // from a callback that only wanted to know which slots are taken.
    //
    // Implemented in NgGuardedCall.cpp, which is compiled with /EHa so __try may
    // appear next to objects that need unwinding.
    GuardedResult CallExternalSlotsGuarded(const slavetats::Addresses* a_table, ExternalSlotsFn a_fn,
        RE::Actor* a_actor, const char* a_area, int a_matches, unsigned long& a_exceptionCode);
}
