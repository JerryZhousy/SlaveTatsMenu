#pragma once

// Read-back verification for writes into the SlaveTats ledger.
//
// Every mutating path in this plugin follows the same contract: write through
// SlaveTatsNG, then re-read the JContainers ledger and believe only what is
// actually there. Upstream's return values are not trustworthy for colour and
// opacity - simple_add_tattoo reports success while leaving the entry at the
// default colour, and add_and_get_tattoo hands back an object id that the next
// synchronisation may already have replaced.
//
// The check used to be duplicated (and inconsistently: one path compared only
// the colour, so a failed alpha write reported "ok" and left the tattoo at the
// old opacity). It lives here so both TattooOps and PresetOps ask the same
// question and get the same answer.

#include "bridge/JcApi.h"
#include "bridge/SlaveTatsNg.h"

#include <cmath>
#include <cstdint>
#include <string_view>

namespace stm::ops
{
    // 1/1000 of a unit - the ledger stores alpha as a float, and a round trip
    // through 1.0f - invertedAlpha is not exact.
    constexpr float kLedgerAlphaEpsilon = 1e-3f;

    struct LedgerCheck
    {
        bool          present = false;  // a ledger entry exists in the slot at all
        bool          colorOk = false;
        bool          alphaOk = false;
        std::int32_t  actualColor = -1;
        float         actualAlpha = -1.0f;

        bool Ok() const { return present && colorOk && alphaOk; }

        // Human-readable reason, for the log line that accompanies a failure.
        const char* Reason() const
        {
            if (!present) { return "no ledger entry in that slot"; }
            if (!colorOk && !alphaOk) { return "colour and opacity both differ"; }
            if (!colorOk) { return "colour differs"; }
            if (!alphaOk) { return "opacity differs"; }
            return "";
        }
    };

    inline LedgerCheck CheckHandle(std::int32_t a_handle, std::uint32_t a_color, float a_alpha)
    {
        LedgerCheck result;
        if (a_handle <= 0 || !jc::Ready()) {
            return result;
        }

        const jc::ObjId obj{ a_handle };
        result.present = true;
        result.actualColor = jc::MapGetInt(obj, "color", -1);
        // The ledger stores the *inverse* of alpha; 0.0f means "not written",
        // which is why the default is 0 rather than 1.
        result.actualAlpha = 1.0f - jc::MapGetFlt(obj, "invertedAlpha", 0.0f);

        result.colorOk = result.actualColor == static_cast<std::int32_t>(a_color);
        result.alphaOk = std::fabs(result.actualAlpha - a_alpha) <= kLedgerAlphaEpsilon;
        return result;
    }

    inline LedgerCheck CheckSlot(RE::Actor* a_actor, std::string_view a_area, int a_slot, std::uint32_t a_color, float a_alpha)
    {
        if (!a_actor || a_slot < 0 || !jc::Ready()) {
            return {};
        }

        return CheckHandle(ng::GetAppliedInSlot(a_actor, a_area, a_slot), a_color, a_alpha);
    }

    inline bool VerifyApplied(RE::Actor* a_actor, std::string_view a_area, int a_slot, std::uint32_t a_color, float a_alpha)
    {
        return CheckSlot(a_actor, a_area, a_slot, a_color, a_alpha).Ok();
    }
}
