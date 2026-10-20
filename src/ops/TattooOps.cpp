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

        // Upstream's synchronize_tattoos_internal aborts on several paths
        // without doing any work - and without clearing .SlaveTats.updated:
        // 3D not loaded, the NiOverride lock already taken, AddOverlays or the
        // ledger upgrade failing. It reports those as an error (fail_t true),
        // so the call site can tell "the sync ran" from "the sync was dropped"
        // instead of claiming success for both. A dropped sync is not a
        // failure of the write, it is a postponed one: ops/PendingSync retries
        // it as soon as the body is there.
        bool SyncDeferred(RE::Actor* a_actor)
        {
            return a_actor != nullptr && !a_actor->Is3DLoaded();
        }

        // Upstream's own pending marker: every ledger write raises it, and only
        // a synchronisation that reached the end clears it. Reading it is how a
        // dropped sync is told apart from one that ran - and, when the call is
        // answered from the task queue rather than on the main thread, from one
        // that has been posted but has not run yet (both are "not on the body
        // yet", which is what the status line is about).
        bool SyncStillPending(RE::Actor* a_actor)
        {
            if (!a_actor || !jc::Ready()) {
                return true;  // cannot read the flag: trust the call's verdict
            }
            return jc::FormDbGetInt(a_actor, ".SlaveTats.updated", 0) != 0;
        }

        void PushOutcome(const char* a_action, std::string a_target, RE::Actor* a_actor, std::string a_okDetail, std::string a_deferredDetail,
            ng::Status a_syncStatus = ng::Status::kOk)
        {
            if (SyncDeferred(a_actor)) {
                Push(OpResult::Deferred(a_action, std::move(a_target), std::move(a_deferredDetail)));
            } else if (a_syncStatus == ng::Status::kFailed && SyncStillPending(a_actor)) {
                // The body is there, so this is not the 3D case: upstream
                // returned early for another reason (NiOverride lock, overlays
                // or upgrade) and left the pending flag set. Say "postponed",
                // not "ok", and let the watcher finish it.
                Push(OpResult::Deferred(a_action, std::move(a_target), Tr("$STM_Ops_SyncAbortedRetrying")));
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
        bool RestyleApplied(RE::Actor* a_actor, std::string_view a_area, int a_slot, std::uint32_t a_color, float a_alpha, std::uint32_t a_glow,
            ng::Status* a_outSync = nullptr)
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

            const auto syncStatus = ng::Synchronize(a_actor);
            if (a_outSync) {
                *a_outSync = syncStatus;
            }

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

                ng::Status syncStatus = ng::Status::kOk;
                const bool ok = RestyleApplied(actor, existingArea, existingSlot, a_request.color, a_request.alpha, a_request.glow, &syncStatus);
                data::SlotSnapshot::Get().Invalidate();

                if (!ok) {
                    Push(OpResult::Fail("Recolor", name, Tr("$STM_Ops_LedgerMismatch")));
                    return;
                }

                PushOutcome("Recolor",
                    name,
                    actor,
                    Tr("$STM_Ops_AlreadyOn") + existingArea + " #" + std::to_string(existingSlot) + Tr("$STM_Ops_ColorGlowOpacityUpdated"),
                    Tr("$STM_Ops_SyncPostponed3D"),
                    syncStatus);
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
                            const auto syncStatus = ng::Synchronize(actor);

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
                                    Tr("$STM_Ops_WrittenRequestedSlotPostponed"),
                                    syncStatus);
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
                                    Tr("$STM_Ops_WrittenAutoSlotPostponed"),
                                    syncStatus);
                                return;
                            }

                            Push(OpResult::Fail("Apply", name, Tr("$STM_Ops_SlotWriteDidNotAppear")));
                            return;
                        }
                    }

                    SKSE::log::warn("Ops: could not resolve tattoo '{}' / '{}' for slot placement, falling back to auto slot", section, name);
                }
            }

            // Auto slot: upstream picks the first free one and synchronises
            // internally (a_sync = true), so no extra synchronize call here.
            // Its return value is also the only report on that internal sync:
            // with the entry present in the ledger the add cannot have failed,
            // so "false" means the sync it was asked to run was dropped.
            //
            // Reaching here with a slot selected means the explicit-slot branch
            // was skipped - JContainers is unavailable, so the slot could not be
            // inspected - and the request is about to be ignored. Say so in the
            // result instead of reporting a plain "applied".
            const bool slotIgnored = a_request.targetSlot >= 0 && !jc::Ready();
            if (slotIgnored) {
                SKSE::log::warn("Ops: JContainers unavailable, requested slot {} in {} cannot be honoured - using the first free slot",
                    a_request.targetSlot, a_request.tattoo.area);
            }

            const auto status = ng::SimpleAddTattoo(actor, section, name, a_request.color, a_request.alpha, true);
            const int landed = FindAppliedSlot(actor, section, name, nullptr);
            data::SlotSnapshot::Get().Invalidate();

            if (landed >= 0) {
                ng::Status syncStatus = status;

                // If glow was requested, SimpleAddTattoo cannot set glow: write it
                // into the ledger and resynchronize.
                if (a_request.glow != 0) {
                    const auto handle = ng::GetAppliedInSlot(actor, a_request.tattoo.area, landed);
                    if (handle > 0) {
                        jc::MapSetInt(jc::ObjId{ handle }, "glow", static_cast<std::int32_t>(a_request.glow));
                        jc::FormDbSetInt(actor, ".SlaveTats.updated", 1);
                        syncStatus = ng::Synchronize(actor);
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
                    slotIgnored ? Tr("$STM_Ops_PlacedAutoSlot") : Tr("$STM_Ops_AppliedToTarget"),
                    Tr("$STM_Ops_RecordedLedgerPostponed"),
                    syncStatus);
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

            // Forced: upstream cancels the whole rebuild while .SlaveTats.updated
            // is 0, and a ledger written by a mod that never raised that flag
            // could not be pushed onto the body any other way. "Force sync" is
            // the one button that has to mean it.
            const auto status = ng::Synchronize(actor, /*a_forceUpdated=*/true);
            data::SlotSnapshot::Get().Invalidate();

            // 3D first: upstream reports "aborted because the body is not there"
            // as an error, and calling that a failed sync would be misleading -
            // the request is simply postponed.
            if (SyncDeferred(actor)) {
                Push(OpResult::Deferred("Sync", ActorLabel(actor), Tr("$STM_Ops_SyncSkipped3DNotLoaded")));
            } else if (status != ng::Status::kOk) {
                Push(OpResult::Fail("Sync", ActorLabel(actor), Tr("$STM_Ops_SyncCallFailed")));
            } else {
                Push(OpResult::Ok("Sync", ActorLabel(actor), Tr("$STM_Ops_SyncRequestedClean")));
            }
        });
    }
}
