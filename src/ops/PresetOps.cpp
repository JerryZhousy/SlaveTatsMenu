#include "ops/PresetOps.h"

#include "bridge/JcApi.h"
#include "bridge/KnownTargets.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Tasks.h"
#include "data/PresetStore.h"
#include "data/SlotSnapshot.h"

#include <ctime>

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

        std::string LocaleText(const char* a_zh, const char* a_en)
        {
            return Config::Get().IsChinese() ? std::string(a_zh) : std::string(a_en);
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

        int FindAppliedSlot(RE::Actor* a_actor, std::string_view a_section, std::string_view a_name)
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
                Push(OpResult::Fail("PresetSave", a_presetName, LocaleText("目标角色已失效", "target actor is gone")));
                return;
            }

            if (!ng::EnsureReady("preset-capture") || !jc::Ready()) {
                Push(OpResult::Fail("PresetSave", a_presetName, LocaleText("需要 SlaveTatsNG 与 JContainers 才能读取纹身", "SlaveTatsNG and JContainers are both required to read the tattoos")));
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

                    if (entry.name.empty()) {
                        continue;
                    }

                    preset.entries.push_back(std::move(entry));
                }
            }

            if (preset.Empty()) {
                Push(OpResult::Fail("PresetSave", a_presetName, LocaleText("目标身上没有可保存的纹身", "the target wears no tattoos to save")));
                return;
            }

            std::string error;
            if (!data::PresetStore::Get().Save(preset, &error)) {
                Push(OpResult::Fail("PresetSave", a_presetName, LocaleText("写入预设失败: ", "could not write the preset: ") + error));
                return;
            }

            Push(OpResult::Ok("PresetSave",
                data::SanitizePresetName(a_presetName),
                LocaleText("已保存 ", "saved ") + std::to_string(preset.Count()) + LocaleText(" 个纹身（来自 ", " tattoo(s) from ") + preset.sourceActor + "）"));
        });
    }

    void ApplyPreset(RE::ActorHandle a_actor, std::string a_presetName, bool a_clearFirst)
    {
        PostToGameThread([a_actor, a_presetName = std::move(a_presetName), a_clearFirst]() {
            auto* actor = ResolveActor(a_actor);
            if (!actor) {
                Push(OpResult::Fail("PresetApply", a_presetName, LocaleText("目标角色已失效", "target actor is gone")));
                return;
            }

            if (!ng::EnsureReady("preset-apply")) {
                Push(OpResult::Fail("PresetApply", a_presetName, LocaleText("SlaveTatsNG 接口不可用", "SlaveTatsNG interface unavailable")));
                return;
            }

            data::Preset preset;
            std::string  error;
            if (!data::PresetStore::Get().Load(a_presetName, preset, &error)) {
                Push(OpResult::Fail("PresetApply", a_presetName, LocaleText("读取预设失败: ", "could not read the preset: ") + error));
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
                            jc::MapSetFlt(fresh, "invertedAlpha", 1.0f - entry.alpha);
                            done = true;

                            // Report the truth: when the requested slot is already
                            // taken upstream silently places the tattoo elsewhere.
                            if (wantSlot && FindSlotOfHandle(actor, entry.area, added) == slot) {
                                ++exactSlot;
                            }
                        }
                    }
                }

                // Fallback: the high level API cannot target a slot, but it is the
                // only route left when the query returns nothing (cache not built).
                if (!done) {
                    if (ng::SimpleAddTattoo(actor, entry.section, entry.name, entry.color, entry.alpha, false) == ng::Status::kOk
                        && FindAppliedSlot(actor, entry.section, entry.name) >= 0) {
                        done = true;
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

            std::string detail = LocaleText("应用 ", "applied ") + std::to_string(applied) + "/" + std::to_string(preset.Count());
            if (cleared > 0) {
                detail += LocaleText("，先清空 ", ", cleared ") + std::to_string(cleared);
            }
            if (exactSlot < applied) {
                detail += LocaleText("（部分未落到原槽位）", " (some did not keep their original slot)");
            }

            if (failed == 0) {
                PushOutcome("PresetApply", preset.name, actor, detail, detail + LocaleText("；3D 未加载，同步被推迟", "; the 3D is not loaded so the sync was postponed"));
            } else if (applied > 0) {
                Push(OpResult::Fail("PresetApply", preset.name, detail + LocaleText("，", ", ") + std::to_string(failed) + LocaleText(" 个失败", " failed")));
            } else {
                Push(OpResult::Fail("PresetApply", preset.name, LocaleText("全部失败（纹身名称可能已不在当前清单里）", "every entry failed (the tattoo names may no longer be in the catalogue)")));
            }
        });
    }
}
