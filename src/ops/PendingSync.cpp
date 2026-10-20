#include "ops/PendingSync.h"

#include "bridge/JcApi.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Tasks.h"
#include "core/Translation.h"
#include "data/TargetState.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace stm::ops::pending
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // The UI calls Tick() every frame; this is what keeps the game thread
        // off the JContainers path more than four times a second.
        constexpr auto kSampleInterval = std::chrono::milliseconds(250);

        // A flag that survives this many samples (~1.5 s) is not a sync that is
        // still running - it is one that never ran. An apply from this menu
        // clears it in the same frame.
        constexpr int kStaleSamples = 6;

        // Never retry the same actor more often than this, and give up after
        // this many attempts, so a permanently stuck NiOverride lock cannot turn
        // into a resync every frame for the rest of the session.
        constexpr auto kRetryInterval = std::chrono::seconds(2);
        constexpr int  kMaxAttempts = 5;

        // A watch for an actor nobody is looking at any more is noise.
        constexpr auto kForgetAfter = std::chrono::seconds(60);

        struct Watch
        {
            RE::FormID        formID = 0;
            std::string       label;
            int               staleSamples = 0;
            int               attempts = 0;
            bool              gaveUp = false;
            Clock::time_point firstStale{};
            Clock::time_point lastTry{};
            Clock::time_point lastSeen{};
        };

        std::mutex         g_mutex;
        std::vector<Watch> g_watches;
        std::atomic_bool   g_inFlight{ false };
        Clock::time_point  g_lastSample{};
        bool               g_warnedNoJc = false;

        std::string ActorLabel(RE::Actor* a_actor)
        {
            if (!a_actor) {
                return "<null>";
            }
            const char* name = a_actor->GetName();
            return (name && name[0] != '\0') ? std::string(name) : std::string("<unnamed>");
        }

        Watch* Find(RE::FormID a_formID)
        {
            for (auto& watch : g_watches) {
                if (watch.formID == a_formID) {
                    return &watch;
                }
            }
            return nullptr;
        }

        Watch* FindOrAdd(RE::FormID a_formID, const std::string& a_label)
        {
            const auto now = Clock::now();

            if (auto* existing = Find(a_formID)) {
                existing->label = a_label;
                existing->lastSeen = now;
                return existing;
            }

            Watch watch;
            watch.formID = a_formID;
            watch.label = a_label;
            watch.lastSeen = now;
            g_watches.push_back(std::move(watch));
            return &g_watches.back();
        }

        void Forget(RE::FormID a_formID)
        {
            std::erase_if(g_watches, [a_formID](const Watch& a_watch) { return a_watch.formID == a_formID; });
        }

        // Game thread. Reads the flag the engine never re-reads, and acts on it.
        void Sample()
        {
            struct FlightGuard
            {
                ~FlightGuard() { g_inFlight.store(false); }
            } guard;

            if (!ng::Ready()) {
                return;
            }

            if (!jc::Ready()) {
                if (!g_warnedNoJc) {
                    g_warnedNoJc = true;
                    SKSE::log::warn("PendingSync: JContainers unavailable, cannot read .SlaveTats.updated - automatic sync completion is off");
                }
                return;
            }

            std::vector<RE::Actor*> candidates;
            if (auto* target = data::TargetState::Get().Resolve()) {
                candidates.push_back(target);
            }
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                // TargetState already falls back to the player; do not sample
                // the same actor twice.
                if (candidates.empty() || candidates.front() != player) {
                    candidates.push_back(player);
                }
            }

            std::scoped_lock lock(g_mutex);

            const auto now = Clock::now();

            for (auto* actor : candidates) {
                if (!actor) {
                    continue;
                }

                const auto formID = actor->GetFormID();
                const auto label = ActorLabel(actor);

                if (jc::FormDbGetInt(actor, ".SlaveTats.updated", 0) == 0) {
                    if (auto* watch = Find(formID); watch && watch->attempts > 0) {
                        // The retry landed. Say so once: the apply that started
                        // this reported "deferred", and this is its ending.
                        SKSE::log::info("PendingSync: '{}' is in sync now (after {} attempt(s))", label, watch->attempts);
                        ResultBoard::Get().Push(OpResult::Ok("AutoSync", label, Tr("$STM_Ops_AutoSyncFinished")));
                    }
                    Forget(formID);
                    continue;
                }

                // A target whose 3D is still loading is not stranded, it is
                // early: the flag is supposed to be set and the body will pick
                // it up. Only start counting once the body exists.
                if (!actor->Is3DLoaded()) {
                    auto* watch = FindOrAdd(formID, label);
                    watch->staleSamples = 0;
                    watch->firstStale = {};
                    continue;
                }

                auto* watch = FindOrAdd(formID, label);

                if (watch->staleSamples == 0) {
                    watch->firstStale = now;
                }
                if (watch->staleSamples < kStaleSamples) {
                    ++watch->staleSamples;
                    continue;
                }

                if (watch->gaveUp) {
                    continue;
                }

                if (watch->attempts > 0 && (now - watch->lastTry) < kRetryInterval) {
                    continue;
                }

                if (watch->attempts >= kMaxAttempts) {
                    watch->gaveUp = true;
                    SKSE::log::warn("PendingSync: giving up on '{}' after {} attempts - .SlaveTats.updated is still 1, so the NiOverride lock is probably held by something that never releases it", label, watch->attempts);
                    continue;
                }

                ++watch->attempts;
                watch->lastTry = now;

                const auto staleFor = std::chrono::duration<double>(now - watch->firstStale).count();
                SKSE::log::warn("PendingSync: '{}' still has .SlaveTats.updated = 1 {:.1f}s after its ledger changed - SlaveTatsNG skipped that sync (3D was not loaded, or the NiOverride lock was taken); retrying (attempt {}/{})",
                    label, staleFor, watch->attempts, kMaxAttempts);

                ng::Synchronize(actor);
            }

            // Drop watches for actors that stopped being the target a while ago,
            // so a session that switches targets does not accumulate them.
            std::erase_if(g_watches, [now](const Watch& a_watch) { return (now - a_watch.lastSeen) > kForgetAfter; });
        }
    }

    void Tick()
    {
        if (!Config::Get().autoFinishSync) {
            return;
        }

        const auto now = Clock::now();
        if (g_inFlight.load() || (now - g_lastSample) < kSampleInterval) {
            return;
        }

        g_lastSample = now;
        g_inFlight.store(true);

        if (!PostToGameThread([] { Sample(); })) {
            g_inFlight.store(false);
        }
    }

    void Reset()
    {
        std::scoped_lock lock(g_mutex);
        g_watches.clear();
    }
}
