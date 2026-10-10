#include "ops/TattooOps.h"

#include "bridge/JcApi.h"
#include "bridge/KnownTargets.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Tasks.h"
#include "data/SlotSnapshot.h"

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

        std::string LocaleText(const char* a_zh, const char* a_en)
        {
            return Config::Get().IsChinese() ? std::string(a_zh) : std::string(a_en);
        }

        // Writes colour/alpha into an applied tattoo's ledger entry and forces a
        // resynchronisation. This is the only way to restyle an existing tattoo:
        // upstream's simple_add_tattoo short-circuits when it is already applied.
        bool RestyleApplied(RE::Actor* a_actor, std::string_view a_area, int a_slot, std::uint32_t a_color, float a_alpha)
        {
            const auto handle = ng::GetAppliedInSlot(a_actor, a_area, a_slot);
            if (handle <= 0) {
                return false;
            }

            const jc::ObjId obj{ handle };
            jc::MapSetInt(obj, "color", static_cast<std::int32_t>(a_color));
            jc::MapSetFlt(obj, "invertedAlpha", 1.0f - a_alpha);
            jc::FormDbSetInt(a_actor, ".SlaveTats.updated", 1);

            ng::Synchronize(a_actor);

            // Read back: the ledger is the only authority we trust.
            const auto reread = ng::GetAppliedInSlot(a_actor, a_area, a_slot);
            if (reread <= 0) {
                return false;
            }

            const jc::ObjId after{ reread };
            return jc::MapGetInt(after, "color", -1) == static_cast<std::int32_t>(a_color);
        }
    }

    void Apply(const ApplyRequest& a_request)
    {
        PostToGameThread([a_request]() {
            const std::string name = a_request.tattoo.name;
            const std::string section = a_request.tattoo.section;

            auto* actor = ResolveActor(a_request.actor);
            if (!actor) {
                Push(OpResult::Fail("Apply", name, LocaleText("目标角色已失效", "target actor is gone")));
                return;
            }

            if (!ng::EnsureReady("apply")) {
                Push(OpResult::Fail("Apply", name, LocaleText("SlaveTatsNG 接口不可用", "SlaveTatsNG interface unavailable")));
                return;
            }

            // Keep the upstream save/load contract intact for every actor we touch.
            ng::RegisterKnownTarget(actor);

            std::string existingArea;
            const int existingSlot = FindAppliedSlot(actor, section, name, &existingArea);

            if (existingSlot >= 0) {
                if (!jc::Ready()) {
                    Push(OpResult::Fail("Recolor", name, LocaleText("JContainers 不可用，无法修改已应用的纹身", "JContainers unavailable, cannot restyle an applied tattoo")));
                    return;
                }

                const bool ok = RestyleApplied(actor, existingArea, existingSlot, a_request.color, a_request.alpha);
                data::SlotSnapshot::Get().Invalidate();

                if (!ok) {
                    Push(OpResult::Fail("Recolor", name, LocaleText("写入账本后回查不一致", "ledger read-back mismatch")));
                    return;
                }

                PushOutcome("Recolor",
                    name,
                    actor,
                    LocaleText("已在 ", "already on ") + existingArea + " #" + std::to_string(existingSlot) + LocaleText(" 更新颜色/透明度", " - colour and opacity updated"),
                    LocaleText("账本已更新，但目标 3D 未加载，同步被 SlaveTatsNG 推迟", "ledger updated, but the target's 3D is not loaded so the sync was postponed"));
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
                        jc::MapSetFlt(fresh, "invertedAlpha", 1.0f - a_request.alpha);
                        jc::FormDbSetInt(actor, ".SlaveTats.updated", 1);
                        ng::Synchronize(actor);

                        const auto landed = ng::GetAppliedInSlot(actor, a_request.tattoo.area, a_request.targetSlot);
                        data::SlotSnapshot::Get().Invalidate();

                        if (landed > 0) {
                            const std::string target = name + " -> " + a_request.tattoo.area + " #" + std::to_string(a_request.targetSlot);
                            PushOutcome("Apply",
                                target,
                                actor,
                                LocaleText("已写入指定槽位", "placed into the requested slot"),
                                LocaleText("账本已写入指定槽位，但 3D 未加载，同步被推迟", "written to the requested slot, but the 3D is not loaded so the sync was postponed"));
                            return;
                        }

                        Push(OpResult::Fail("Apply", name, LocaleText("指定槽位写入后回查不到，可能被占用", "slot write did not appear - the slot may be taken")));
                        return;
                    }
                }

                SKSE::log::warn("Ops: could not resolve tattoo '{}' / '{}' for slot placement, falling back to auto slot", section, name);
            }

            // Auto slot: upstream picks the first free one and synchronises internally
            // (a_sync = true), so no extra synchronize call here.
            const auto status = ng::SimpleAddTattoo(actor, section, name, a_request.color, a_request.alpha, true);
            const int landed = FindAppliedSlot(actor, section, name, nullptr);
            data::SlotSnapshot::Get().Invalidate();

            if (landed >= 0) {
                PushOutcome("Apply",
                    name,
                    actor,
                    LocaleText("已应用到目标", "applied to the target"),
                    LocaleText("账本已记录，但 3D 未加载，同步被推迟", "recorded in the ledger, but the 3D is not loaded so the sync was postponed"));
            } else if (status == ng::Status::kUnavailable) {
                Push(OpResult::Fail("Apply", name, LocaleText("SlaveTatsNG 接口不可用", "SlaveTatsNG interface unavailable")));
            } else {
                Push(OpResult::Fail("Apply", name, LocaleText("调用后账本里找不到该纹身（可能名称不匹配或槽位已满）", "not present in the ledger afterwards (name mismatch or no free slot)")));
            }
        });
    }

    void Remove(RE::ActorHandle a_actor, std::string a_section, std::string a_name)
    {
        PostToGameThread([a_actor, a_section, a_name]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor || !ng::EnsureReady("remove")) {
                Push(OpResult::Fail("Remove", a_name, LocaleText("目标或接口不可用", "target or interface unavailable")));
                return;
            }

            ng::RegisterKnownTarget(actor);

            const int  before = FindAppliedSlot(actor, a_section, a_name, nullptr);
            const bool wasPresent = before >= 0;

            ng::SimpleRemoveTattoo(actor, a_section, a_name, true);

            const bool stillPresent = FindAppliedSlot(actor, a_section, a_name, nullptr) >= 0;
            data::SlotSnapshot::Get().Invalidate();

            // Order matters: "not present afterwards" is only a success if it was
            // present to begin with.
            if (!wasPresent) {
                Push(OpResult::Fail("Remove", a_name, LocaleText("目标身上没有这个纹身", "the target does not wear this tattoo")));
            } else if (!stillPresent) {
                PushOutcome("Remove",
                    a_name,
                    actor,
                    LocaleText("已从目标移除", "removed from the target"),
                    LocaleText("账本已移除，但 3D 未加载，同步被推迟", "removed from the ledger, but the 3D is not loaded so the sync was postponed"));
            } else {
                Push(OpResult::Fail("Remove", a_name, LocaleText("移除后账本里仍然存在", "still present in the ledger after removal")));
            }
        });
    }

    void RemoveSlot(RE::ActorHandle a_actor, std::string a_area, int a_slot)
    {
        PostToGameThread([a_actor, a_area, a_slot]() {
            auto* actor = ResolveActor(a_actor);
            const std::string target = a_area + " #" + std::to_string(a_slot);

            if (!actor || !ng::EnsureReady("remove-slot")) {
                Push(OpResult::Fail("ClearSlot", target, LocaleText("目标或接口不可用", "target or interface unavailable")));
                return;
            }

            ng::RegisterKnownTarget(actor);

            const auto before = ng::GetAppliedInSlot(actor, a_area, a_slot);
            ng::RemoveFromSlot(actor, a_area, a_slot, true);
            const auto after = ng::GetAppliedInSlot(actor, a_area, a_slot);
            data::SlotSnapshot::Get().Invalidate();

            if (before <= 0) {
                Push(OpResult::Fail("ClearSlot", target, LocaleText("该槽位本来就是空的", "that slot was already empty")));
            } else if (after <= 0) {
                PushOutcome("ClearSlot",
                    target,
                    actor,
                    LocaleText("槽位已清空", "slot cleared"),
                    LocaleText("槽位已清空，但 3D 未加载，同步被推迟", "slot cleared in the ledger, but the 3D is not loaded so the sync was postponed"));
            } else {
                Push(OpResult::Fail("ClearSlot", target, LocaleText("清除后槽位仍被占用", "slot is still occupied")));
            }
        });
    }

    void ClearArea(RE::ActorHandle a_actor, std::string a_area)
    {
        PostToGameThread([a_actor, a_area]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor || !ng::EnsureReady("clear-area")) {
                Push(OpResult::Fail("ClearArea", a_area, LocaleText("目标或接口不可用", "target or interface unavailable")));
                return;
            }

            ng::RegisterKnownTarget(actor);

            const int maxSlots = ng::MaxSlots(a_area);
            int       cleared = 0;
            for (int slot = 0; slot < maxSlots; ++slot) {
                if (ng::GetAppliedInSlot(actor, a_area, slot) > 0) {
                    if (ng::RemoveFromSlot(actor, a_area, slot, true) == ng::Status::kOk) {
                        ++cleared;
                    }
                }
            }

            ng::Synchronize(actor);
            data::SlotSnapshot::Get().Invalidate();

            if (cleared > 0) {
                PushOutcome("ClearArea",
                    a_area,
                    actor,
                    LocaleText("已清空 ", "cleared ") + std::to_string(cleared) + LocaleText(" 个槽位", " slot(s)"),
                    LocaleText("已清空 ", "cleared ") + std::to_string(cleared) + LocaleText(" 个槽位，但 3D 未加载，同步被推迟", " slot(s) in the ledger, but the 3D is not loaded so the sync was postponed"));
            } else {
                Push(OpResult::Fail("ClearArea", a_area, LocaleText("该部位没有可清空的纹身", "nothing to clear in this area")));
            }
        });
    }

    void ClearAll(RE::ActorHandle a_actor)
    {
        PostToGameThread([a_actor]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor || !ng::EnsureReady("clear-all")) {
                Push(OpResult::Fail("ClearAll", "-", LocaleText("目标或接口不可用", "target or interface unavailable")));
                return;
            }

            ng::RegisterKnownTarget(actor);

            int cleared = 0;
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

            ng::Synchronize(actor);
            data::SlotSnapshot::Get().Invalidate();

            const std::string label = ActorLabel(actor);
            if (cleared > 0) {
                PushOutcome("ClearAll",
                    label,
                    actor,
                    LocaleText("已清除全身 ", "cleared ") + std::to_string(cleared) + LocaleText(" 个纹身", " tattoo(s)"),
                    LocaleText("账本已清除 ", "cleared ") + std::to_string(cleared) + LocaleText(" 个纹身，但 3D 未加载，同步被推迟", " tattoo(s) in the ledger, but the 3D is not loaded so the sync was postponed"));
            } else {
                Push(OpResult::Fail("ClearAll", label, LocaleText("目标身上没有纹身", "the target wears no tattoos")));
            }
        });
    }

    void Sync(RE::ActorHandle a_actor)
    {
        PostToGameThread([a_actor]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor || !ng::EnsureReady("sync")) {
                Push(OpResult::Fail("Sync", "-", LocaleText("目标或接口不可用", "target or interface unavailable")));
                return;
            }

            ng::RegisterKnownTarget(actor);

            const auto status = ng::Synchronize(actor);
            data::SlotSnapshot::Get().Invalidate();

            if (status != ng::Status::kOk) {
                Push(OpResult::Fail("Sync", ActorLabel(actor), LocaleText("同步调用返回失败", "synchronize call reported failure")));
            } else if (SyncDeferred(actor)) {
                Push(OpResult::Deferred("Sync", ActorLabel(actor), LocaleText("目标 3D 未加载，SlaveTatsNG 会直接放弃这次同步", "the target's 3D is not loaded, SlaveTatsNG will skip this sync entirely")));
            } else {
                Push(OpResult::Ok("Sync", ActorLabel(actor), LocaleText("已请求强制同步（账本无变化时会立即跳过）", "synchronisation requested (skipped immediately when the ledger is clean)")));
            }
        });
    }

    bool ReadSlotAppearance(RE::Actor* a_actor, std::string_view a_area, int a_slot, std::uint32_t& a_color, float& a_alpha)
    {
        if (!a_actor || !jc::Ready()) {
            return false;
        }

        const auto handle = ng::GetAppliedInSlot(a_actor, a_area, a_slot);
        if (handle <= 0) {
            return false;
        }

        const jc::ObjId obj{ handle };
        a_color = static_cast<std::uint32_t>(jc::MapGetInt(obj, "color", 0));
        a_alpha = 1.0f - jc::MapGetFlt(obj, "invertedAlpha", 0.0f);
        return true;
    }
}
