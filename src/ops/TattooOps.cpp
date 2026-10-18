#include "ops/TattooOps.h"

#include "bridge/JcApi.h"
#include "bridge/KnownTargets.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Translation.h"
#include "core/Log.h"
#include "core/Tasks.h"
#include "data/SlotSnapshot.h"
#include "ops/LedgerVerify.h"

#include <cmath>

namespace stm::ops
{
    namespace
    {
        constexpr std::string_view kAreas[] = { "Body", "Face", "Hands", "Feet" };
        constexpr std::string_view kPool = "SlaveTatsMenu-Ops";

        // Releases temporary JContainers objects on every exit path.
        struct PoolGuard
        {
            ~PoolGuard() { jc::CleanPool(kPool); }
        };

        RE::Actor* ResolveActor(const RE::ActorHandle& a_handle)
        {
            return a_handle.get().get();
        }

        void Push(stm::OpResult a_result)
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

        // Upstream's synchronize_tattoos_internal bails out without doing
        // anything - and without clearing .SlaveTats.updated - while the target's
        // 3D is not loaded. So a ledger write can be perfectly correct and still
        // not be visible until later; say so instead of claiming success.
        bool SyncDeferred(RE::Actor* a_actor)
        {
            return a_actor != nullptr && !a_actor->Is3DLoaded();
        }

        void PushOutcome(const char* a_action, std::string a_target, RE::Actor* a_actor, std::string a_okDetail, std::string a_deferredDetail)
        {
            if (SyncDeferred(a_actor)) {
                Push(OpResult::Deferred(a_action, std::move(a_target), std::move(a_deferredDetail)));
            } else {
                Push(OpResult::Ok(a_action, std::move(a_target), std::move(a_okDetail)));
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

        // Returns the slot index holding section+name, or -1. Also reports the area.
        int FindAppliedSlot(RE::Actor* a_actor, std::string_view a_section, std::string_view a_name, std::string* a_outArea)
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

        // A slot whose ledger entry carries no name belongs to another overlay mod
        // (RaceMenu, another tattoo mod, ...). Apply already refuses to overwrite
        // those; the clearing paths have to refuse as well, otherwise "clear
        // everything" silently deletes data this plugin does not own.
        //
        // Returns false when JContainers is unavailable: without it we cannot tell
        // an external overlay from our own, and blocking every slot would be worse
        // than the risk of clearing one.
        bool IsExternalSlot(RE::Actor* a_actor, std::string_view a_area, int a_slot)
        {
            if (!jc::Ready()) {
                return false;
            }

            const auto handle = ng::GetAppliedInSlot(a_actor, a_area, a_slot);
            if (handle <= 0) {
                return false;
            }

            return jc::MapGetStr(jc::ObjId{ handle }, "name").empty();
        }


        // Writes colour/glow/alpha into an applied tattoo's ledger entry and
        // forces a resynchronisation. This is the only way to restyle an existing
        // tattoo: upstream's simple_add_tattoo short-circuits when it is already
        // applied.
        bool RestyleApplied(RE::Actor* a_actor, std::string_view a_area, int a_slot, std::uint32_t a_color, float a_alpha, std::uint32_t a_glow)
        {
            const auto handle = ng::GetAppliedInSlot(a_actor, a_area, a_slot);
            if (handle <= 0) {
                return false;
            }

            const jc::ObjId obj{ handle };
            jc::MapSetInt(obj, "color", static_cast<std::int32_t>(a_color));
            jc::MapSetInt(obj, "glow", static_cast<std::int32_t>(a_glow));
            jc::MapSetFlt(obj, "invertedAlpha", 1.0f - a_alpha);
            jc::FormDbSetInt(a_actor, ".SlaveTats.updated", 1);

            ng::Synchronize(a_actor);

            // Read back: the ledger is the only authority we trust. All three
            // fields matter - verifying only the colour reported "ok" while a
            // failed alpha write left the tattoo at the old opacity, and the
            // glow is a plain int write with no other way to confirm it landed.
            return VerifyApplied(a_actor, a_area, a_slot, a_color, a_alpha, static_cast<std::int32_t>(a_glow));
        }
    }

    void Apply(const ApplyRequest& a_request)
    {
        PostToGameThread([a_request]() {
            const std::string name = a_request.tattoo.name;
            const std::string section = a_request.tattoo.section;

            auto* actor = ResolveActor(a_request.actor);
            if (!actor) {
                Push(OpResult::Fail("Apply", name, Tr("$STM_Ops_TargetActorGone")));
                return;
            }

            if (!ng::EnsureReady("apply")) {
                Push(OpResult::Fail("Apply", name, Tr("$STM_Ops_SlaveTatsUnavailable")));
                return;
            }

            // Keep the upstream save/load contract intact for every actor we touch.
            ng::RegisterKnownTarget(actor);

            std::string existingArea;
            const int existingSlot = FindAppliedSlot(actor, section, name, &existingArea);

            if (existingSlot >= 0) {
                if (!jc::Ready()) {
                    Push(OpResult::Fail("Recolor", name, Tr("$STM_Ops_JcUnavailableRestyle")));
                    return;
                }

                const bool ok = RestyleApplied(actor, existingArea, existingSlot, a_request.color, a_request.alpha, a_request.glow);
                data::SlotSnapshot::Get().Invalidate();

                if (!ok) {
                    Push(OpResult::Fail("Recolor", name, Tr("$STM_Ops_LedgerMismatch")));
                    return;
                }

                PushOutcome("Recolor",
                    name,
                    actor,
                    Tr("$STM_Ops_AlreadyOn") + existingArea + " #" + std::to_string(existingSlot) + Tr("$STM_Ops_ColorGlowOpacityUpdated"),
                    Tr("$STM_Ops_SyncPostponed3D"));
                return;
            }

            // --- brand new tattoo -----------------------------------------
            PoolGuard guard;

            // Defensive: the UI clamps this, but a stale selection could still
            // carry a slot number belonging to another area.
            const int  maxSlotsForArea = ng::MaxSlots(a_request.tattoo.area);
            const bool slotInRange = a_request.targetSlot >= 0 && a_request.targetSlot < maxSlotsForArea;
            if (a_request.targetSlot >= 0 && !slotInRange) {
                SKSE::log::warn("Ops: requested slot {} is out of range for {} (max {}), falling back to auto allocation", a_request.targetSlot, a_request.tattoo.area, maxSlotsForArea);
            }

            if (slotInRange && jc::Ready()) {
                const auto occupiedHandle = ng::GetAppliedInSlot(actor, a_request.tattoo.area, a_request.targetSlot);
                bool proceedWithSlot = true;
                if (occupiedHandle > 0) {
                    const jc::ObjId obj{ occupiedHandle };
                    const bool isExternal = jc::MapGetStr(obj, "name").empty();
                    if (isExternal) {
                        SKSE::log::warn("Ops: slot {} in {} is an external overlay, refusing to overwrite to protect mod data; falling back to auto allocation",
                            a_request.targetSlot, a_request.tattoo.area);
                        proceedWithSlot = false;
                    } else if (Config::Get().slotPolicy == SlotPolicy::kOverwrite) {
                        SKSE::log::info("Ops: slot {} occupied in {}, removing existing per overwrite policy", a_request.targetSlot, a_request.tattoo.area);
                        if (ng::RemoveFromSlot(actor, a_request.tattoo.area, a_request.targetSlot, true) != ng::Status::kOk) {
                            SKSE::log::warn("Ops: failed to remove existing tattoo from slot {} in {}, falling back to auto allocation", a_request.targetSlot, a_request.tattoo.area);
                            proceedWithSlot = false;
                        }
                    } else {
                        SKSE::log::info("Ops: slot {} occupied in {}, falling back to auto allocation per autofree policy", a_request.targetSlot, a_request.tattoo.area);
                        proceedWithSlot = false;
                    }
                }

                if (proceedWithSlot) {
                    const auto tmpl = jc::AddToPool(jc::CreateMap(), kPool);
                    const auto matches = jc::AddToPool(jc::CreateArray(), kPool);

                    jc::MapSetStr(tmpl, "section", section);
                    jc::MapSetStr(tmpl, "name", name);

                    if (tmpl && matches && ng::QueryAvailable(tmpl.id, matches.id) == ng::Status::kOk && jc::ArrayCount(matches) > 0) {
                        const auto first = jc::ArrayGetObj(matches, 0);
                        std::int32_t added = 0;

                        if (ng::AddAndGetTattoo(actor, first.id, a_request.targetSlot, added) == ng::Status::kOk) {
                            const jc::ObjId fresh{ added };
                            jc::MapSetInt(fresh, "color", static_cast<std::int32_t>(a_request.color));
                            jc::MapSetInt(fresh, "glow", static_cast<std::int32_t>(a_request.glow));
                            jc::MapSetFlt(fresh, "invertedAlpha", 1.0f - a_request.alpha);
                            jc::FormDbSetInt(actor, ".SlaveTats.updated", 1);
                            ng::Synchronize(actor);

                            const auto landed = ng::GetAppliedInSlot(actor, a_request.tattoo.area, a_request.targetSlot);
                            data::SlotSnapshot::Get().Invalidate();

                            if (landed == added && landed > 0) {
                                const std::string target = name + " -> " + a_request.tattoo.area + " #" + std::to_string(a_request.targetSlot);
                                const auto check = CheckSlot(actor, a_request.tattoo.area, a_request.targetSlot, a_request.color, a_request.alpha,
                                    static_cast<std::int32_t>(a_request.glow));
                                if (!check.Ok()) {
                                    SKSE::log::warn("Ops: '{}' landed in {} #{} but the ledger disagrees: {} (colour {} vs {}, opacity {:.3f} vs {:.3f}, glow {} vs {})",
                                        name, a_request.tattoo.area, a_request.targetSlot, check.Reason(),
                                        check.actualColor, static_cast<std::int32_t>(a_request.color),
                                        check.actualAlpha, a_request.alpha,
                                        check.actualGlow, static_cast<std::int32_t>(a_request.glow));
                                    Push(OpResult::Fail("Apply", target,
                                        std::string(Tr("$STM_Ops_PlacedRequestedSlot")) + Tr("$STM_Ops_Comma") + Tr("$STM_Ops_LedgerMismatch")));
                                    return;
                                }

                                PushOutcome("Apply",
                                    target,
                                    actor,
                                    Tr("$STM_Ops_PlacedRequestedSlot"),
                                    Tr("$STM_Ops_WrittenRequestedSlotPostponed"));
                                return;
                            }

                            // 若指定槽位未写入目标 handle，检查是否被自动分配到了其他槽位
                            const int actualSlot = FindSlotOfHandle(actor, a_request.tattoo.area, added);
                            if (actualSlot >= 0) {
                                const std::string target = name + " -> " + a_request.tattoo.area + " #" + std::to_string(actualSlot);
                                const auto check = CheckSlot(actor, a_request.tattoo.area, actualSlot, a_request.color, a_request.alpha,
                                    static_cast<std::int32_t>(a_request.glow));
                                if (!check.Ok()) {
                                    SKSE::log::warn("Ops: '{}' landed in {} #{} (requested #{}) but the ledger disagrees: {} (colour {} vs {}, opacity {:.3f} vs {:.3f}, glow {} vs {})",
                                        name, a_request.tattoo.area, actualSlot, a_request.targetSlot, check.Reason(),
                                        check.actualColor, static_cast<std::int32_t>(a_request.color),
                                        check.actualAlpha, a_request.alpha,
                                        check.actualGlow, static_cast<std::int32_t>(a_request.glow));
                                    Push(OpResult::Fail("Apply", target,
                                        std::string(Tr("$STM_Ops_PlacedAutoSlot")) + Tr("$STM_Ops_Comma") + Tr("$STM_Ops_LedgerMismatch")));
                                    return;
                                }

                                PushOutcome("Apply",
                                    target,
                                    actor,
                                    Tr("$STM_Ops_PlacedAutoSlot"),
                                    Tr("$STM_Ops_WrittenAutoSlotPostponed"));
                                return;
                            }

                            Push(OpResult::Fail("Apply", name, Tr("$STM_Ops_SlotWriteDidNotAppear")));
                            return;
                        }
                    }

                    SKSE::log::warn("Ops: could not resolve tattoo '{}' / '{}' for slot placement, falling back to auto slot", section, name);
                }
            }

            // Auto slot: upstream picks the first free one and synchronises internally
            // (a_sync = true), so no extra synchronize call here.
            const auto status = ng::SimpleAddTattoo(actor, section, name, a_request.color, a_request.alpha, true);
            const int landed = FindAppliedSlot(actor, section, name, nullptr);
            data::SlotSnapshot::Get().Invalidate();

            if (landed >= 0) {
                // If glow was requested, SimpleAddTattoo cannot set glow: write it
                // into the ledger and resynchronize.
                if (a_request.glow != 0) {
                    const auto handle = ng::GetAppliedInSlot(actor, a_request.tattoo.area, landed);
                    if (handle > 0) {
                        jc::MapSetInt(jc::ObjId{ handle }, "glow", static_cast<std::int32_t>(a_request.glow));
                        jc::FormDbSetInt(actor, ".SlaveTats.updated", 1);
                        ng::Synchronize(actor);
                        data::SlotSnapshot::Get().Invalidate();
                    }
                }

                // The high level API takes colour/alpha as arguments, so it can
                // also report success while quietly leaving the entry at its
                // defaults. Same read-back rule as every other write path.
                const auto check = CheckSlot(actor, a_request.tattoo.area, landed, a_request.color, a_request.alpha,
                    static_cast<std::int32_t>(a_request.glow));
                if (!check.Ok()) {
                    SKSE::log::warn("Ops: '{}' landed in {} #{} via the auto slot but the ledger disagrees: {} (colour {} vs {}, opacity {:.3f} vs {:.3f}, glow {} vs {})",
                        name, a_request.tattoo.area, landed, check.Reason(),
                        check.actualColor, static_cast<std::int32_t>(a_request.color),
                        check.actualAlpha, a_request.alpha,
                        check.actualGlow, static_cast<std::int32_t>(a_request.glow));
                    Push(OpResult::Fail("Apply", name,
                        std::string(Tr("$STM_Ops_AppliedToTarget")) + Tr("$STM_Ops_Comma") + Tr("$STM_Ops_LedgerMismatch")));
                    return;
                }

                PushOutcome("Apply",
                    name,
                    actor,
                    Tr("$STM_Ops_AppliedToTarget"),
                    Tr("$STM_Ops_RecordedLedgerPostponed"));
            } else if (status == ng::Status::kUnavailable) {
                Push(OpResult::Fail("Apply", name, Tr("$STM_Ops_SlaveTatsUnavailable")));
            } else {
                Push(OpResult::Fail("Apply", name, Tr("$STM_Ops_NotPresentInLedger")));
            }
        });
    }

    void Remove(RE::ActorHandle a_actor, std::string a_section, std::string a_name)
    {
        PostToGameThread([a_actor, a_section, a_name]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor || !ng::EnsureReady("remove")) {
                Push(OpResult::Fail("Remove", a_name, Tr("$STM_Ops_TargetOrInterfaceUnavailable")));
                return;
            }

            ng::RegisterKnownTarget(actor);

            const int  before = FindAppliedSlot(actor, a_section, a_name, nullptr);
            const bool wasPresent = before >= 0;

            ng::SimpleRemoveTattoo(actor, a_section, a_name, true);
            ng::Synchronize(actor);

            const bool stillPresent = FindAppliedSlot(actor, a_section, a_name, nullptr) >= 0;
            data::SlotSnapshot::Get().Invalidate();

            // Order matters: "not present afterwards" is only a success if it was
            // present to begin with.
            if (!wasPresent) {
                Push(OpResult::Fail("Remove", a_name, Tr("$STM_Ops_TargetDoesNotWearTattoo")));
            } else if (!stillPresent) {
                PushOutcome("Remove",
                    a_name,
                    actor,
                    Tr("$STM_Ops_RemovedFromTarget"),
                    Tr("$STM_Ops_RemovedLedgerPostponed"));
            } else {
                Push(OpResult::Fail("Remove", a_name, Tr("$STM_Ops_StillPresentAfterRemoval")));
            }
        });
    }

    void RemoveSlot(RE::ActorHandle a_actor, std::string a_area, int a_slot)
    {
        PostToGameThread([a_actor, a_area, a_slot]() {
            auto* actor = ResolveActor(a_actor);
            const std::string target = a_area + " #" + std::to_string(a_slot);

            if (!actor || !ng::EnsureReady("remove-slot")) {
                Push(OpResult::Fail("ClearSlot", target, Tr("$STM_Ops_TargetOrInterfaceUnavailable")));
                return;
            }

            ng::RegisterKnownTarget(actor);

            const auto before = ng::GetAppliedInSlot(actor, a_area, a_slot);

            if (before <= 0) {
                data::SlotSnapshot::Get().Invalidate();
                Push(OpResult::Fail("ClearSlot", target, Tr("$STM_Ops_SlotAlreadyEmpty")));
                return;
            }

            // Never clear an overlay another mod owns - same rule Apply follows.
            if (IsExternalSlot(actor, a_area, a_slot)) {
                Push(OpResult::Fail("ClearSlot", target, Tr("$STM_Ops_SlotIsExternal")));
                return;
            }

            ng::RemoveFromSlot(actor, a_area, a_slot, true);
            const auto after = ng::GetAppliedInSlot(actor, a_area, a_slot);

            if (after <= 0) {
                ng::Synchronize(actor);
                data::SlotSnapshot::Get().Invalidate();
                PushOutcome("ClearSlot",
                    target,
                    actor,
                    Tr("$STM_Ops_SlotCleared"),
                    Tr("$STM_Ops_SlotClearedPostponed"));
            } else {
                data::SlotSnapshot::Get().Invalidate();
                Push(OpResult::Fail("ClearSlot", target, Tr("$STM_Ops_SlotStillOccupied")));
            }
        });
    }

    void ClearArea(RE::ActorHandle a_actor, std::string a_area)
    {
        PostToGameThread([a_actor, a_area]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor || !ng::EnsureReady("clear-area")) {
                Push(OpResult::Fail("ClearArea", a_area, Tr("$STM_Ops_TargetOrInterfaceUnavailable")));
                return;
            }

            ng::RegisterKnownTarget(actor);

            const int maxSlots = ng::MaxSlots(a_area);
            int       cleared = 0;
            int       skipped = 0;
            for (int slot = 0; slot < maxSlots; ++slot) {
                if (ng::GetAppliedInSlot(actor, a_area, slot) > 0) {
                    if (IsExternalSlot(actor, a_area, slot)) {
                        ++skipped;
                        continue;
                    }
                    if (ng::RemoveFromSlot(actor, a_area, slot, true) == ng::Status::kOk) {
                        ++cleared;
                    }
                }
            }

            ng::Synchronize(actor);
            data::SlotSnapshot::Get().Invalidate();

            const std::string skippedNote = skipped > 0
                ? Tr("$STM_Ops_SkippedExternalPrefix") + std::to_string(skipped) + Tr("$STM_Ops_SkippedExternalSuffix")
                : std::string();

            if (cleared > 0) {
                PushOutcome("ClearArea",
                    a_area,
                    actor,
                    Tr("$STM_Ops_ClearedPrefix") + std::to_string(cleared) + Tr("$STM_Ops_SlotsSuffix") + skippedNote,
                    Tr("$STM_Ops_ClearedPrefix") + std::to_string(cleared) + Tr("$STM_Ops_ClearedSlotsPostponed") + skippedNote);
            } else if (skipped > 0) {
                Push(OpResult::Fail("ClearArea", a_area, Tr("$STM_Ops_OnlyExternalOverlays")));
            } else {
                Push(OpResult::Fail("ClearArea", a_area, Tr("$STM_Ops_NothingToClearInArea")));
            }
        });
    }

    void ClearAll(RE::ActorHandle a_actor)
    {
        PostToGameThread([a_actor]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor || !ng::EnsureReady("clear-all")) {
                Push(OpResult::Fail("ClearAll", "-", Tr("$STM_Ops_TargetOrInterfaceUnavailable")));
                return;
            }

            ng::RegisterKnownTarget(actor);

            int cleared = 0;
            int skipped = 0;
            for (const auto area : kAreas) {
                const int maxSlots = ng::MaxSlots(area);
                for (int slot = 0; slot < maxSlots; ++slot) {
                    if (ng::GetAppliedInSlot(actor, area, slot) > 0) {
                        if (IsExternalSlot(actor, area, slot)) {
                            ++skipped;
                            continue;
                        }
                        if (ng::RemoveFromSlot(actor, area, slot, true) == ng::Status::kOk) {
                            ++cleared;
                        }
                    }
                }
            }

            ng::Synchronize(actor);
            data::SlotSnapshot::Get().Invalidate();

            const std::string label = ActorLabel(actor);
            const std::string skippedNote = skipped > 0
                ? Tr("$STM_Ops_SkippedExternalPrefix") + std::to_string(skipped) + Tr("$STM_Ops_SkippedExternalSuffix")
                : std::string();

            if (cleared > 0) {
                PushOutcome("ClearAll",
                    label,
                    actor,
                    Tr("$STM_Ops_ClearedAllPrefix") + std::to_string(cleared) + Tr("$STM_Ops_TattoosSuffix") + skippedNote,
                    Tr("$STM_Cleared") + std::to_string(cleared) + Tr("$STM_Ops_ClearedAllPostponed") + skippedNote);
            } else if (skipped > 0) {
                Push(OpResult::Fail("ClearAll", label, Tr("$STM_Ops_OnlyExternalOverlays")));
            } else {
                Push(OpResult::Fail("ClearAll", label, Tr("$STM_Ops_TargetWearsNoTattoos")));
            }
        });
    }

    void Sync(RE::ActorHandle a_actor)
    {
        PostToGameThread([a_actor]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor || !ng::EnsureReady("sync")) {
                Push(OpResult::Fail("Sync", "-", Tr("$STM_Ops_TargetOrInterfaceUnavailable")));
                return;
            }

            ng::RegisterKnownTarget(actor);

            const auto status = ng::Synchronize(actor);
            data::SlotSnapshot::Get().Invalidate();

            if (status != ng::Status::kOk) {
                Push(OpResult::Fail("Sync", ActorLabel(actor), Tr("$STM_Ops_SyncCallFailed")));
            } else if (SyncDeferred(actor)) {
                Push(OpResult::Deferred("Sync", ActorLabel(actor), Tr("$STM_Ops_SyncSkipped3DNotLoaded")));
            } else {
                Push(OpResult::Ok("Sync", ActorLabel(actor), Tr("$STM_Ops_SyncRequestedClean")));
            }
        });
    }
}
