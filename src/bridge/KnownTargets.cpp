#include "bridge/KnownTargets.h"

#include "bridge/JcApi.h"
#include "core/Log.h"

namespace stm::ng
{
    namespace
    {
        constexpr std::string_view kPath = ".SlaveTatsMenu.known_targets";
    }

    void RegisterKnownTarget(RE::Actor* a_actor)
    {
        if (!a_actor) {
            return;
        }

        if (!jc::Ready()) {
            log::Once("known-targets-no-jc", spdlog::level::warn, "KnownTargets: JContainers unavailable, cannot register {} - tattoos on this actor will not survive a load", a_actor->GetName());
            return;
        }

        auto targets = jc::JdbSolveObj(kPath);

        if (!targets) {
            // The list does not exist yet (the player never opened the MCM).
            // Create it, then ASK FOR IT AGAIN: solveObjSetter reports success
            // as a bool, it is not an object id.
            const auto fresh = jc::CreateArray();
            if (!fresh) {
                SKSE::log::error("KnownTargets: could not allocate the known_targets array");
                return;
            }

            jc::JdbSolveObjSetter(kPath, fresh, true);
            targets = jc::JdbSolveObj(kPath);

            if (!targets) {
                SKSE::log::error("KnownTargets: creating {} failed", kPath);
                return;
            }

            SKSE::log::info("KnownTargets: created {} (id {})", kPath, targets.id);
        }

        if (jc::ArrayFindForm(targets, a_actor) >= 0) {
            // Already on the list - the MCM puts the player there the first time it
            // is opened. Say so once, otherwise "no KnownTargets line in the log"
            // looks like the contract failed when it is actually working.
            log::Once("known-targets-present", spdlog::level::info, "KnownTargets: '{}' is already on the list (listed actors: {})", a_actor->GetName(), jc::ArrayCount(targets));
            return;
        }

        jc::ArrayAddForm(targets, a_actor);

        // Read back so the log tells the truth even if the write silently failed.
        const bool present = jc::ArrayFindForm(targets, a_actor) >= 0;
        if (present) {
            SKSE::log::info("KnownTargets: registered '{}' ({:08X}), list now has {} entries", a_actor->GetName(), a_actor->GetFormID(), jc::ArrayCount(targets));
        } else {
            SKSE::log::error("KnownTargets: registering '{}' ({:08X}) did NOT take effect", a_actor->GetName(), a_actor->GetFormID());
        }
    }

    int KnownTargetCount()
    {
        if (!jc::Ready()) {
            return -1;
        }

        const auto targets = jc::JdbSolveObj(kPath);
        if (!targets) {
            return 0;
        }

        return jc::ArrayCount(targets);
    }
}
