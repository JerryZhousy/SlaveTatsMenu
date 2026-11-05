#include "ops/LoadRepair.h"

#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Tasks.h"
#include "diag/OverlayState.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace stm::ops::loadrepair
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // OBody re-applies its presets a few seconds after a load - measured at
        // ~13 s on a real install - and that is the step that can take the
        // overlays skee just restored back off the body. A single pass would
        // race it, so the schedule samples twice: once after the body has
        // settled, once after OBody has had its turn.
        constexpr std::array<Clock::duration, 2> kPassOffsets{
            std::chrono::seconds(10),
            std::chrono::seconds(30),
        };

        constexpr std::array<const char*, 4> kAreas{ "Body", "Face", "Hands", "Feet" };

        // ---- timer -----------------------------------------------------------
        //
        // The pass has to run after the load screen is gone, and this plugin has
        // no per-frame hook outside the menu window. A thread that sleeps until
        // the next offset and posts the work onto the game thread is the same
        // pattern diag/Probe already uses, and it costs nothing while idle.
        std::mutex                 g_timerMutex;
        std::condition_variable    g_timerCv;
        std::thread                g_timerThread;
        bool                       g_stop = false;
        // Read from the pass without the timer lock, written by Arm().
        std::atomic<std::uint64_t> g_generation{ 0 };

        // ---- pass state ------------------------------------------------------
        //
        // Tasks are not guaranteed to run on one particular thread, so this is
        // guarded rather than assumed single-threaded. The pass walks one actor
        // per task: a character can carry 38 slots across four areas, and doing
        // a whole cell in one frame would be a visible hitch for a repair that
        // has no deadline.
        std::mutex                     g_passMutex;
        std::vector<RE::ActorHandle>   g_passActors;
        std::unordered_set<RE::FormID> g_gaveUp;  // sync did not visibly fix it
        std::size_t                    g_passIndex = 0;
        std::uint64_t                  g_passGeneration = 0;
        int                            g_passNumber = 0;
        int                            g_passChecked = 0;
        int                            g_passRepaired = 0;

        void StepPass();

        void FinishPassLocked()
        {
            SKSE::log::info("LoadRepair: pass {} done, {} actor(s) checked, {} resynced",
                g_passNumber + 1, g_passChecked, g_passRepaired);
            g_passActors.clear();
            g_passIndex = 0;
        }

        void BeginPass(int a_passNumber)
        {
            if (!Config::Get().resyncAfterLoad || !ng::Ready()) {
                return;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            // Still behind the load screen: every actor reports "no 3D", and a
            // pass here would decide nothing. The next offset tries again.
            if (!player || !player->Is3DLoaded()) {
                SKSE::log::info("LoadRepair: pass {} skipped, the player's 3D is not loaded yet", a_passNumber + 1);
                return;
            }

            std::vector<RE::ActorHandle> actors;
            actors.push_back(player->GetHandle());

            if (auto* lists = RE::ProcessLists::GetSingleton()) {
                const auto collect = [&actors, player](const RE::BSTArray<RE::ActorHandle>& a_handles) {
                    for (const auto& handle : a_handles) {
                        const auto ptr = handle.get();  // NiPointer, not a raw Actor*
                        if (!ptr || ptr.get() == player) {
                            continue;
                        }
                        actors.push_back(handle);
                    }
                };
                collect(lists->highActorHandles);
                collect(lists->lowActorHandles);
            }

            const auto count = actors.size();

            {
                std::scoped_lock lock(g_passMutex);
                g_passActors = std::move(actors);
                g_passIndex = 0;
                g_passGeneration = g_generation.load();
                g_passNumber = a_passNumber;
                g_passChecked = 0;
                g_passRepaired = 0;
            }

            SKSE::log::info("LoadRepair: pass {} checking {} actor(s) with 3D", a_passNumber + 1, count);
            PostToGameThread(StepPass);
        }

        // Ledger vs. body for one actor, slot by slot.
        //
        // Comparing counts would be simpler and wrong: RaceMenu writes its own
        // overlays into the same slot range, so a character with three of those
        // and one lost SlaveTats tattoo still has ledger <= render and would be
        // passed over. The ledger says which slot each tattoo sits in, so the
        // question can be asked per slot: "the ledger lists slot 3, does slot 3
        // show anything?" A slot that is absent, blank or alpha 0 is the answer
        // we are looking for.
        int MissingSlots(RE::Actor* a_actor, RE::NiAVObject* a_root, std::string& a_out)
        {
            int         missing = 0;
            int         recorded = 0;
            int         worstArea = 0;
            std::string worstDetail;

            for (const char* area : kAreas) {
                const int slots = ng::MaxSlots(area);
                if (slots <= 0) {
                    continue;
                }

                int areaMissing = 0;
                int areaRecorded = 0;

                for (int slot = 0; slot < slots; ++slot) {
                    if (ng::GetAppliedInSlot(a_actor, area, slot) <= 0) {
                        continue;
                    }
                    ++areaRecorded;
                    if (diag::overlay::LooksLikeBlankSlot(diag::overlay::ReadRenderState(a_root, area, slot))) {
                        ++areaMissing;
                    }
                }

                recorded += areaRecorded;
                missing += areaMissing;

                if (areaMissing > worstArea) {
                    worstArea = areaMissing;
                    worstDetail = std::string(area) + " " + std::to_string(areaMissing) + "/" +
                                  std::to_string(areaRecorded) + " slot(s) empty";
                }
            }

            if (missing > 0) {
                a_out = std::to_string(missing) + " of " + std::to_string(recorded) + " recorded slot(s) empty (" + worstDetail + ")";
            }
            return missing;
        }

        // Returns true when the actor was resynced.
        bool Inspect(RE::Actor* a_actor, int a_passNumber)
        {
            if (!a_actor || !a_actor->Is3DLoaded()) {
                return false;
            }

            auto* root = a_actor->Get3D();
            if (!root) {
                return false;
            }

            std::string detail;
            if (MissingSlots(a_actor, root, detail) == 0) {
                return false;
            }

            const auto formID = a_actor->GetFormID();
            {
                std::scoped_lock lock(g_passMutex);
                if (g_gaveUp.contains(formID)) {
                    return false;
                }
            }

            const char* name = a_actor->GetName();
            SKSE::log::warn("LoadRepair: '{}' ({:08X}) lost overlays the ledger still lists ({}), resyncing",
                name && name[0] != '\0' ? name : "<unnamed>", formID, detail);

            // Forced on purpose: synchronize_tattoos() cancels while
            // ".SlaveTats.updated" is 0, and the whole point here is that the
            // ledger is unchanged while the body is not. If the sync is dropped
            // again (lock held, 3D gone) the flag stays set and PendingSync
            // picks it up the next time the window is open.
            const auto status = ng::Synchronize(a_actor, /*a_forceUpdated=*/true);
            if (status != ng::Status::kOk) {
                SKSE::log::warn("LoadRepair: pass {} resync of '{}' returned {}, leaving it to PendingSync",
                    a_passNumber + 1, name ? name : "?", ng::ToString(status));
                return false;
            }

            // Read the body back. A slot that is still empty right after a sync
            // that reported success is not a lost overlay - it is this check
            // disagreeing with how that body names its slots, and retrying would
            // clear the slot range again on every pass for nothing.
            auto* after = a_actor->Is3DLoaded() ? a_actor->Get3D() : nullptr;
            std::string still;
            if (after && MissingSlots(a_actor, after, still) > 0) {
                SKSE::log::warn("LoadRepair: '{}' still reads as short after the sync ({}); not retrying this load",
                    name ? name : "?", still);
                std::scoped_lock lock(g_passMutex);
                g_gaveUp.insert(formID);
                return true;
            }

            SKSE::log::info("LoadRepair: pass {} resynced '{}'", a_passNumber + 1, name ? name : "?");
            return true;
        }

        void StepPass()
        {
            RE::ActorHandle handle;
            int             passNumber = 0;
            bool            more = false;

            {
                std::scoped_lock lock(g_passMutex);

                // A newer load replaced this pass; the work it queued is stale.
                if (g_passGeneration != g_generation.load()) {
                    g_passActors.clear();
                    g_passIndex = 0;
                    return;
                }

                if (g_passIndex >= g_passActors.size()) {
                    FinishPassLocked();
                    return;
                }

                handle = g_passActors[g_passIndex++];
                passNumber = g_passNumber;
                ++g_passChecked;
                more = g_passIndex < g_passActors.size();
            }

            bool repaired = false;
            if (const auto ptr = handle.get()) {
                repaired = Inspect(ptr.get(), passNumber);
            }

            {
                std::scoped_lock lock(g_passMutex);
                if (repaired) {
                    ++g_passRepaired;
                }
                if (!more) {
                    FinishPassLocked();
                }
            }

            if (more) {
                PostToGameThread(StepPass);
            }
        }

        void TimerMain()
        {
            std::uint64_t seen = 0;

            for (;;) {
                {
                    std::unique_lock lock(g_timerMutex);
                    g_timerCv.wait(lock, [&seen] { return g_stop || g_generation != seen; });
                    if (g_stop) {
                        return;
                    }
                    seen = g_generation;
                }

                const auto armed = Clock::now();

                for (int pass = 0; pass < static_cast<int>(kPassOffsets.size()); ++pass) {
                    {
                        std::unique_lock lock(g_timerMutex);
                        const bool superseded = g_timerCv.wait_until(lock, armed + kPassOffsets[pass], [&seen] {
                            return g_stop || g_generation != seen;
                        });
                        if (g_stop) {
                            return;
                        }
                        if (superseded) {
                            break;  // another load armed a fresh schedule
                        }
                    }

                    PostToGameThread([pass] { BeginPass(pass); });
                }
            }
        }
    }

    void Arm()
    {
        {
            std::scoped_lock lock(g_passMutex);
            g_gaveUp.clear();
        }
        {
            std::scoped_lock lock(g_timerMutex);
            ++g_generation;
            if (!g_timerThread.joinable()) {
                g_timerThread = std::thread(TimerMain);
            }
        }
        g_timerCv.notify_all();
    }

    void Shutdown()
    {
        {
            std::scoped_lock lock(g_timerMutex);
            g_stop = true;
        }
        g_timerCv.notify_all();

        if (g_timerThread.joinable()) {
            g_timerThread.join();
        }
    }
}
