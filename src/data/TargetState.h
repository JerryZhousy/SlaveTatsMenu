#pragma once

#include "PCH.h"

namespace stm::data
{
    // The actor the menu is currently editing. Falls back to the player when
    // nothing else was picked, and survives the picked actor being unloaded
    // (ActorHandle just resolves to null and we fall back).
    class TargetState
    {
    public:
        static TargetState& Get();

        RE::Actor* Resolve() const;

        // Handle of the current target, suitable for posting to the game thread.
        RE::ActorHandle Handle() const;

        void Set(RE::Actor* a_actor);
        void ResetToPlayer();

        // Uses the crosshair pick data; game thread only. Returns false when the
        // crosshair is not on a valid actor.
        bool PickFromCrosshair();

        std::string DisplayName() const;
        std::uint32_t FormID() const;

    private:
        TargetState() = default;

        RE::ActorHandle _handle;
    };
}
