#include "data/TargetState.h"

#include "core/Config.h"

#include <RE/C/CrosshairPickData.h>

namespace stm::data
{
    namespace
    {
        const char* L(const char* a_zh, const char* a_en)
        {
            return Config::Get().IsChinese() ? a_zh : a_en;
        }
    }

    TargetState& TargetState::Get()
    {
        static TargetState s_state;
        return s_state;
    }

    RE::Actor* TargetState::Resolve() const
    {
        if (_handle) {
            if (auto* actor = _handle.get().get()) {
                return actor;
            }
        }
        return RE::PlayerCharacter::GetSingleton();
    }

    RE::ActorHandle TargetState::Handle() const
    {
        if (auto* actor = Resolve()) {
            return actor->GetHandle();
        }
        return {};
    }

    void TargetState::Set(RE::Actor* a_actor)
    {
        if (a_actor) {
            _handle = a_actor->GetHandle();
        } else {
            _handle = {};
        }
    }

    void TargetState::ResetToPlayer()
    {
        _handle = {};
    }

    bool TargetState::PickFromCrosshair()
    {
        auto* crosshair = RE::CrosshairPickData::GetSingleton();
        if (!crosshair) {
            return false;
        }

        auto       handle = crosshair->GetActiveTarget();
        const auto ref = handle.get();
        if (!ref || !ref->Is(RE::FormType::ActorCharacter)) {
            return false;
        }

        auto* actor = ref->As<RE::Actor>();
        if (!actor) {
            return false;
        }

        Set(actor);
        return true;
    }

    std::string TargetState::DisplayName() const
    {
        auto* actor = Resolve();
        if (!actor) {
            return L("无目标", "none");
        }

        if (actor->IsPlayerRef()) {
            return L("主角 (Player)", "Player");
        }

        if (auto* base = actor->GetActorBase()) {
            if (const char* name = base->GetName(); name && name[0] != '\0') {
                return name;
            }
        }

        return L("<未命名 NPC>", "<unnamed NPC>");
    }

    std::uint32_t TargetState::FormID() const
    {
        auto* actor = Resolve();
        return actor ? actor->GetFormID() : 0;
    }
}
