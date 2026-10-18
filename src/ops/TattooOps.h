#pragma once

#include "PCH.h"

#include "data/TattooDatabase.h"

namespace stm::ops
{
    struct ApplyRequest
    {
        RE::ActorHandle   actor;
        data::TattooEntry tattoo;
        int               targetSlot = -1;  // -1 = first free slot
        std::uint32_t     color = 0x000000;
        float             alpha = 1.0f;
    };

    // All operations run on the game thread, verify their own result against
    // the SlaveTats ledger, and push exactly one OpResult describing what
    // really happened.
    void Apply(const ApplyRequest& a_request);
    void Remove(RE::ActorHandle a_actor, std::string a_section, std::string a_name);
    void RemoveSlot(RE::ActorHandle a_actor, std::string a_area, int a_slot);
    void ClearArea(RE::ActorHandle a_actor, std::string a_area);
    void ClearAll(RE::ActorHandle a_actor);
    void Sync(RE::ActorHandle a_actor);
}
