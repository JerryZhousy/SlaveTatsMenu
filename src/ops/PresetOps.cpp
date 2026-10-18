#include "ops/PresetOps.h"

#include "bridge/JcApi.h"
#include "bridge/KnownTargets.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Translation.h"
#include "core/Log.h"
#include "core/Tasks.h"
#include "data/PresetStore.h"
#include "data/SlotSnapshot.h"
#include "ops/LedgerVerify.h"

#include <ctime>
#include <vector>

namespace stm::ops
{
    namespace
    {
        constexpr std::string_view kAreas[] = { "Body", "Face", "Hands", "Feet" };
        constexpr std::string_view kPool = "SlaveTatsMenu-Preset";

        // Releases the temporary JContainers objects on every exit path.
        struct PoolGuard
        {
            ~PoolGuard() { jc::CleanPool(kPool); }
        };

        RE::Actor* ResolveActor(const RE::ActorHandle& a_handle)
        {
            return a_handle.get().get();
        }



        void Push(OpResult a_result)
        {
            if (!a_result.ok) {
                SKSE::log::warn("Op {} FAILED on '{}': {}", a_result.action, a_result.target, a_result.detail);
            } else if (a_result.deferred) {
                SKSE::log::warn("Op {} DEFERRED on '{}': {}", a_result.action, a_result.target, a_result.detail);
            } else {
                SKSE::log::info("Op {} ok on '{}': {}", a_result.action, a_result.target, a_result.detail);
            }
            ResultBoard::Get().Push(std::move(a_result));
        }

        void PushOutcome(const char* a_action, std::string a_target, RE::Actor* a_actor, std::string a_ok, std::string a_deferred)
        {
            if (a_actor && !a_actor->Is3DLoaded()) {
                Push(OpResult::Deferred(a_action, std::move(a_target), std::move(a_deferred)));
            } else {
                Push(OpResult::Ok(a_action, std::move(a_target), std::move(a_ok)));
            }
        }

        std::string ActorLabel(RE::Actor* a_actor)
        {
            if (!a_actor) {
                return "<null>";
            }
            const char* name = a_actor->GetName();
            return (name && name[0] != '\0') ? std::string(name) : std::string("<unnamed>");
        }

        std::string NowStamp()
        {
            const auto        now = std::chrono::system_clock::now();
            const std::time_t stamp = std::chrono::system_clock::to_time_t(now);

            std::tm tm{};
            localtime_s(&tm, &stamp);

            char buffer[32]{};
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
            return buffer;
        }

        int FindAppliedSlot(RE::Actor* a_actor, std::string_view a_section, std::string_view a_name, std::string* a_outArea = nullptr)
        {
            for (const auto area : kAreas) {
                const int maxSlots = ng::MaxSlots(area);
                for (int slot = 0; slot < maxSlots; ++slot) {
                    const auto handle = ng::GetAppliedInSlot(a_actor, area, slot);
                    if (handle <= 0) {
                        continue;
                    }

                    const jc::ObjId obj{ handle };
                    if (jc::MapGetStr(obj, "name") == a_name && jc::MapGetStr(obj, "section") == a_section) {
                        if (a_outArea) {
                            *a_outArea = std::string(area);
                        }
                        return slot;
                    }
                }
            }
            return -1;
        }

        // Which slot actually holds this tattoo handle in the given area, or -1.
        int FindSlotOfHandle(RE::Actor* a_actor, std::string_view a_area, std::int32_t a_handle)
        {
            const int maxSlots = ng::MaxSlots(a_area);
            for (int slot = 0; slot < maxSlots; ++slot) {
                if (ng::GetAppliedInSlot(a_actor, a_area, slot) == a_handle) {
                    return slot;
                }
            }
            return -1;
        }
    }

    void CapturePreset(RE::ActorHandle a_actor, std::string a_presetName)
    {
        PostToGameThread([a_actor, a_presetName = std::move(a_presetName)]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor) {
                Push(OpResult::Fail("PresetSave", a_presetName, Tr("$STM_Ops_TargetActorGone")));
                return;
            }

            if (!ng::EnsureReady("preset-capture") || !jc::Ready()) {
                Push(OpResult::Fail("PresetSave", a_presetName, Tr("$STM_Ops_ReqSlaveTatsAndJc")));
                return;
            }

            data::Preset preset;
            preset.name = a_presetName;
            preset.sourceActor = ActorLabel(actor);
            preset.savedAt = NowStamp();

            for (const auto area : kAreas) {
                const int maxSlots = ng::MaxSlots(area);
                for (int slot = 0; slot < maxSlots; ++slot) {
                    const auto handle = ng::GetAppliedInSlot(actor, area, slot);
                    if (handle <= 0) {
                        continue;
                    }

                    const jc::ObjId obj{ handle };

                    data::PresetEntry entry;
                    entry.area = std::string(area);
                    entry.slot = slot;
                    entry.section = jc::MapGetStr(obj, "section");
                    entry.name = jc::MapGetStr(obj, "name");
                    entry.color = static_cast<std::uint32_t>(jc::MapGetInt(obj, "color", 0));
                    entry.alpha = 1.0f - jc::MapGetFlt(obj, "invertedAlpha", 0.0f);
                    entry.glow = static_cast<std::uint32_t>(jc::MapGetInt(obj, "glow", 0));

                    if (entry.name.empty()) {
                        continue;
                    }

                    preset.entries.push_back(std::move(entry));
                }
            }

            if (preset.Empty()) {
                Push(OpResult::Fail("PresetSave", a_presetName, Tr("$STM_Ops_NoTattoosToSave")));
                return;
            }

            std::string error;
            if (!data::PresetStore::Get().Save(preset, &error)) {
                Push(OpResult::Fail("PresetSave", a_presetName, Tr("$STM_Ops_PresetWriteFailed") + error));
                return;
            }

            Push(OpResult::Ok("PresetSave",
                data::SanitizePresetName(a_presetName),
                Tr("$STM_Saved") + std::to_string(preset.Count()) + Tr("$STM_Ops_TattoosFromSection") + preset.sourceActor + Tr("$STM_Ops_CloseParen")));
        });
    }

    void ApplyPreset(RE::ActorHandle a_actor, std::string a_presetName, bool a_clearFirst)
    {
        PostToGameThread([a_actor, a_presetName = std::move(a_presetName), a_clearFirst]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor) {
                Push(OpResult::Fail("PresetApply", a_presetName, Tr("$STM_Ops_TargetActorGone")));
                return;
            }

            if (!ng::EnsureReady("preset-apply")) {
                Push(OpResult::Fail("PresetApply", a_presetName, Tr("$STM_Ops_SlaveTatsUnavailable")));
                return;
            }

            data::Preset preset;
            std::string  error;
            if (!data::PresetStore::Get().Load(a_presetName, preset, &error)) {
                Push(OpResult::Fail("PresetApply", a_presetName, Tr("$STM_Ops_PresetReadFailed") + error));
                return;
            }

            // Same save/load contract as every other write path.
            ng::RegisterKnownTarget(actor);

            int cleared = 0;
            if (a_clearFirst) {
                for (const auto area : kAreas) {
                    const int maxSlots = ng::MaxSlots(area);
                    for (int slot = 0; slot < maxSlots; ++slot) {
                        if (ng::GetAppliedInSlot(actor, area, slot) > 0) {
                            if (ng::RemoveFromSlot(actor, area, slot, true) == ng::Status::kOk) {
                                ++cleared;
                            }
                        }
                    }
                }
            }

            PoolGuard guard;

            int applied = 0;
            int failed = 0;
            int exactSlot = 0;

            // Where each entry ended up, so the read-back after the single batch
            // synchronisation can confirm colour and opacity actually stuck.
            // add_and_get_tattoo reports success without writing either field, and
            // the batch sync rebuilds the ledger objects, so the object id handed
            // back by the add call is useless for verification.
            struct Placed
            {
                std::string   section;
                std::string   name;
                std::string   area;
                int           slot = -1;
                std::uint32_t color = 0;
                float         alpha = 1.0f;
                std::uint32_t glow = 0;
            };
            std::vector<Placed> placed;

            for (const auto& entry : preset.entries) {
                const int  maxSlots = ng::MaxSlots(entry.area);
                const int  slot = entry.slot;
                const bool wantSlot = slot >= 0 && slot < maxSlots;

                bool done = false;

                // Preferred path: resolve the tattoo and place it in the exact
                // slot it occupied in the source preset.
                if (jc::Ready()) {
                    const auto tmpl = jc::AddToPool(jc::CreateMap(), kPool);
                    const auto matches = jc::AddToPool(jc::CreateArray(), kPool);

                    jc::MapSetStr(tmpl, "section", entry.section);
                    jc::MapSetStr(tmpl, "name", entry.name);

                    if (tmpl && matches && ng::QueryAvailable(tmpl.id, matches.id) == ng::Status::kOk && jc::ArrayCount(matches) > 0) {
                        const auto   first = jc::ArrayGetObj(matches, 0);
                        std::int32_t added = 0;

                        if (ng::AddAndGetTattoo(actor, first.id, wantSlot ? slot : -1, added) == ng::Status::kOk) {
                            const jc::ObjId fresh{ added };
                            jc::MapSetInt(fresh, "color", static_cast<std::int32_t>(entry.color));
                            jc::MapSetInt(fresh, "glow", static_cast<std::int32_t>(entry.glow));
                            jc::MapSetFlt(fresh, "invertedAlpha", 1.0f - entry.alpha);
                            done = true;

                            // Report the truth: when the requested slot is already
                            // taken upstream silently places the tattoo elsewhere.
                            const int landedSlot = FindSlotOfHandle(actor, entry.area, added);
                            if (landedSlot >= 0) {
                                placed.push_back({ entry.section, entry.name, entry.area, landedSlot, entry.color, entry.alpha, entry.glow });
                                if (wantSlot && landedSlot == slot) {
                                    ++exactSlot;
                                }
                            }
                        }
                    }
                }

                // Fallback: the high level API cannot target a slot, but it is the
                // only route left when the query returns nothing (cache not built).
                if (!done) {
                    if (ng::SimpleAddTattoo(actor, entry.section, entry.name, entry.color, entry.alpha, false) == ng::Status::kOk) {
                        std::string landedArea;
                        const int   landedSlot = FindAppliedSlot(actor, entry.section, entry.name, &landedArea);
                        if (landedSlot >= 0) {
                            done = true;
                            if (entry.glow != 0) {
                                const auto handle = ng::GetAppliedInSlot(actor, landedArea, landedSlot);
                                if (handle > 0) {
                                    jc::MapSetInt(jc::ObjId{ handle }, "glow", static_cast<std::int32_t>(entry.glow));
                                }
                            }
                            placed.push_back({ entry.section, entry.name, landedArea, landedSlot, entry.color, entry.alpha, entry.glow });
                        }
                    }
                }

                if (done) {
                    ++applied;
                } else {
                    ++failed;
                    SKSE::log::warn("PresetApply: '{}' / '{}' could not be applied", entry.section, entry.name);
                }
            }

            // One ledger flag and one sync for the whole batch.
            jc::FormDbSetInt(actor, ".SlaveTats.updated", 1);
            ng::Synchronize(actor);
            data::SlotSnapshot::Get().Invalidate();

            // Read back every entry we think we wrote. "The add call returned
            // success" is not evidence: this is the same ledger-authority rule the
            // single-tattoo path follows, and without it a preset could report
            // "12/12 applied" while every tattoo sat at the default colour.
            int mismatched = 0;
            for (const auto& check : placed) {
                const auto result = CheckSlot(actor, check.area, check.slot, check.color, check.alpha, static_cast<std::int32_t>(check.glow));
                if (result.Ok()) {
                    continue;
                }

                ++mismatched;
                SKSE::log::warn("PresetApply: '{}' / '{}' landed in {} #{} but the ledger disagrees: {} (colour {} vs {}, opacity {:.3f} vs {:.3f}, glow {} vs {})",
                    check.section, check.name, check.area, check.slot, result.Reason(),
                    result.actualColor, static_cast<std::int32_t>(check.color),
                    result.actualAlpha, check.alpha,
                    result.actualGlow, static_cast<std::int32_t>(check.glow));
            }

            std::string detail = Tr("$STM_Ops_PresetAppliedPrefix") + std::to_string(applied) + "/" + std::to_string(preset.Count());
            if (cleared > 0) {
                detail += Tr("$STM_Ops_PresetClearedPrefix") + std::to_string(cleared);
            }
            if (exactSlot < applied) {
                detail += Tr("$STM_Ops_PresetSomeSlotsShifted");
            }
            if (mismatched > 0) {
                detail += Tr("$STM_Ops_PresetLedgerMismatchPrefix") + std::to_string(mismatched) + Tr("$STM_Ops_PresetLedgerMismatchSuffix");
            }

            if (failed == 0 && mismatched == 0) {
                PushOutcome("PresetApply", preset.name, actor, detail, detail + Tr("$STM_Ops_Postponed3D"));
            } else if (applied > 0) {
                Push(OpResult::Fail("PresetApply", preset.name, detail + Tr("$STM_Ops_Comma") + std::to_string(failed) + Tr("$STM_Ops_FailedSuffix")));
            } else {
                Push(OpResult::Fail("PresetApply", preset.name, Tr("$STM_Ops_PresetAllFailed")));
            }
        });
    }
}
