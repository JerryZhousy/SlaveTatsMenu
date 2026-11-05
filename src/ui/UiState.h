#pragma once

#include "PCH.h"

#include "data/TattooDatabase.h"

namespace stm::ui
{
    // Everything the menu remembers between frames.
    struct UiState
    {
        static UiState& Get()
        {
            static UiState s_state;
            return s_state;
        }

        // area filter: 0 = all, 1..4 = Body / Face / Hands / Feet (default: 1 = Body)
        int         areaIndex = 1;

        // Which view the left column is showing. That column is a two-step
        // drill-down: sectionChosen == false means the mod list is on screen,
        // sectionChosen == true means the mod list has been replaced by the
        // tattoo list of sectionFilter (an empty filter = the "[all mods]"
        // entry was picked). Going back only flips the flag, so the current
        // selection and its editor values survive the trip.
        bool        sectionChosen = false;
        std::string sectionFilter;
        char        modSearch[128] = {};
        char        tattooSearch[128] = {};

        // The selection is identified by section+name so it survives filtering
        // and re-sorting; the list index is never stored.
        bool              hasSelection = false;
        data::TattooEntry selected;

        // editor values for the selected tattoo
        float color[3] = { 0.0f, 0.0f, 0.0f };
        float alpha = 1.0f;
        int   targetSlot = -1;  // -1 = first free slot

        // glow (emissive)
        bool  glowOn = false;
        float glowColor[3] = { 1.0f, 1.0f, 1.0f };

        // pattern preview (owned by SKSEMenuFramework)
        std::string           previewPath;
        std::string           previewLoadedPath;
        ImGuiMCP::ImTextureID previewTexture = nullptr;

        // bottom status line
        std::string statusText;
        bool        statusOk = true;
        bool        statusDeferred = false;
        double      statusAt = 0.0;

        void SetStatus(bool a_ok, std::string a_text, double a_now)
        {
            SetStatus(a_ok, false, std::move(a_text), a_now);
        }

        void SetStatus(bool a_ok, bool a_deferred, std::string a_text, double a_now)
        {
            statusOk = a_ok;
            statusDeferred = a_deferred;
            statusText = std::move(a_text);
            statusAt = a_now;
        }
    };
}
