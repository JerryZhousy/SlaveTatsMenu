#pragma once

#include "PCH.h"

namespace stm::ng
{
    // Upstream typedefs "fail_t" as bool where TRUE MEANS ERROR. Every wrapper
    // here converts that into an explicit status so call sites cannot
    // accidentally read "true" as success.
    enum class Status
    {
        kOk,
        kFailed,
        kUnavailable,
    };

    const char* ToString(Status a_status);

    // Registers the SKSE message listener for SlaveTatsNG's interface. Call
    // once, from kPostLoad.
    void InstallListener();

    bool Ready();

    // Logs (at most once per reason) when the interface is still missing.
    bool EnsureReady(std::string_view a_reason);

    Status SimpleAddTattoo(RE::Actor* a_actor, std::string_view a_section, std::string_view a_name, std::uint32_t a_color, float a_alpha, bool a_sync = true);
    Status SimpleRemoveTattoo(RE::Actor* a_actor, std::string_view a_section, std::string_view a_name, bool a_sync = true);

    // a_forceUpdated raises ".SlaveTats.updated" before calling upstream, which
    // otherwise cancels the whole synchronisation the moment that flag is 0
    // ("nothing has changed"). Use it for a user-requested force sync; internal
    // calls after a ledger write do not need it, the write already set it.
    Status Synchronize(RE::Actor* a_actor, bool a_forceUpdated = false);
    Status RemoveFromSlot(RE::Actor* a_actor, std::string_view a_area, int a_slot, bool a_ignoreLock);
    Status AddAndGetTattoo(RE::Actor* a_actor, std::int32_t a_template, int a_slot, std::int32_t& a_outHandle);
    Status QueryAvailable(std::int32_t a_templateHandle, std::int32_t a_matchesHandle);

    // Asks SlaveTatsNG to rebuild its tattoo cache. It normally compiles that
    // cache only once a game is loaded, which is why query_available_tattoos
    // returns an empty list at kDataLoaded. This walks every pack file, so it
    // costs a moment - call it only when the player is actually in game.
    // Returns whatever upstream returns, or -1 when unavailable.
    int CompileCache();

    // Returns 0 when the slot is empty (the upstream contract never returns -1).
    // Non-zero values are JContainers object ids: never cache them, never treat
    // them as slot numbers.
    std::int32_t GetAppliedInSlot(RE::Actor* a_actor, std::string_view a_area, int a_slot);

    // Slots in a_area whose overlay was written by something other than
    // SlaveTats. RaceMenu's own tattoo page writes the same "<Area> [OvlN]"
    // NiOverride nodes, and so do other overlay mods, so those slots are taken
    // even though the ledger says nothing about them - GetAppliedInSlot()
    // returns 0 for every one of them. Upstream answers this by reading the
    // node itself, which is the only way to see it.
    //
    // Returns false when upstream could not answer (interface or JContainers
    // unavailable). Callers must then treat the slot list as unknown rather
    // than as "nothing is external".
    bool ExternalSlots(RE::Actor* a_actor, std::string_view a_area, std::vector<int>& a_out);

    // Slot count for an area, read from skee64.ini exactly like SlaveTatsNG does,
    // with the same fallback to the historic defaults.
    int MaxSlots(std::string_view a_area);
}
