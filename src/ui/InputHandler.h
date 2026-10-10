#pragma once

#include "PCH.h"

#include "core/Config.h"
#include "ui/Ui.h"

namespace stm::ui
{
    // Global hotkey to show/hide the floating window. Deliberately ignores the
    // key while the console is up or while another blocking menu owns input.
    class InputHandler : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static InputHandler* Get()
        {
            static InputHandler s_handler;
            return &s_handler;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }

            for (auto* event = *a_event; event; event = event->next) {
                if (event->eventType != RE::INPUT_EVENT_TYPE::kButton) {
                    continue;
                }

                const auto* button = event->AsButtonEvent();
                if (!button || !button->IsDown() || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                    continue;
                }

                if (button->GetIDCode() != Config::Get().toggleKey) {
                    continue;
                }

                if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::Console::MENU_NAME)) {
                    continue;
                }

                if (!IsWindowOpen() && SKSEMenuFramework::IsAnyBlockingWindowOpened()) {
                    continue;
                }

                ToggleWindow();
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        InputHandler() = default;
    };

    inline void InstallInputHandler()
    {
        auto* manager = RE::BSInputDeviceManager::GetSingleton();
        if (!manager) {
            SKSE::log::error("UI: no input device manager, the hotkey will not work");
            return;
        }

        manager->AddEventSink(InputHandler::Get());
        SKSE::log::info("UI: hotkey registered (scan code 0x{:X})", Config::Get().toggleKey);
    }
}
