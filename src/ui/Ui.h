#pragma once

#include "PCH.h"

namespace stm::ui
{
    // Public surface used by main.cpp and the hotkey handler.
    void Initialize();

    void __stdcall RenderMenu();
    void __stdcall RenderWindow();

    void ToggleWindow();
    bool IsWindowOpen();
}
