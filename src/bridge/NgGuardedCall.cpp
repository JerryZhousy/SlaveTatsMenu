#include "bridge/NgGuardedCall.h"

#include <Windows.h>

namespace stm::ng
{
    namespace
    {
        // Kept separate from the guarded frame on purpose: MSVC refuses __try in
        // a function that owns objects needing unwinding (C2712), and the
        // BSFixedString below is one. Here it is an ordinary call that unwinds
        // normally; the frame that installs the handler holds nothing but
        // pointers.
        bool CallRaw(ExternalSlotsFn a_fn, RE::Actor* a_actor, const char* a_area, int a_matches)
        {
            const RE::BSFixedString area(a_area ? a_area : "");
            return !a_fn(a_actor, area, a_matches);  // fail_t: true means error
        }
    }

    GuardedResult CallExternalSlotsGuarded(const slavetats::Addresses* a_table, ExternalSlotsFn a_fn,
        RE::Actor* a_actor, const char* a_area, int a_matches, unsigned long& a_exceptionCode)
    {
        (void)a_table;
        a_exceptionCode = 0;

        __try {
            return CallRaw(a_fn, a_actor, a_area, a_matches) ? GuardedResult::kOk : GuardedResult::kUpstream;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // The handler runs instead of the unwinding that would have released
            // the BSFixedString in the callee, so that one reference leaks. The
            // probe is disabled for the session after this, so it happens once.
            a_exceptionCode = ::GetExceptionCode();
            return GuardedResult::kFaulted;
        }
    }
}
