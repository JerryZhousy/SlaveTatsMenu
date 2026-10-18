#include "ui/Ui.h"

#include "bridge/JcApi.h"
#include "bridge/KnownTargets.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Translation.h"
#include "core/Tasks.h"
#include "data/SlotSnapshot.h"
#include "data/TargetState.h"
#include "data/PresetStore.h"
#include "data/TattooDatabase.h"
#include "diag/Probe.h"
#include "ops/PresetOps.h"
#include "ops/TattooOps.h"
#include "render/MannequinRenderer.h"
#include "render/TextureLoader.h"
#include "ui/UiState.h"

#include <shellapi.h>
#include <cmath>

namespace stm::ui
{
    namespace
    {
        MENU_WINDOW g_window = nullptr;

        const char* kAreaIds[] = { "", "Body", "Face", "Hands", "Feet" };

        inline const char* L(const char* a_key)
        {
            return Tr(a_key);
        }

        struct FontScaleScope
        {
            float prevScale = 1.0f;
            bool  active = false;

            explicit FontScaleScope(float s)
            {
                if (auto* win = ImGui::GetCurrentWindowRead()) {
                    prevScale = win->FontWindowScale;
                }
                if (s > 0.01f && std::abs(s - prevScale) > 0.001f) {
                    ImGui::SetWindowFontScale(s);
                    active = true;
                }
            }
            ~FontScaleScope()
            {
                if (active) {
                    ImGui::SetWindowFontScale(prevScale);
                }
            }
        };

        double NowSeconds()
        {
            static const auto s_start = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
        }

        // Everything below this point touches the engine, JContainers or the
        // task-queue-shared state directly, which is only legal on the game's main
        // thread. The framework does call us there today, but if that ever stops
        // being true, doing the work anyway is a silent data race - so refuse and
        // say so once, instead of logging a warning and carrying on.
        bool OnGameThread()
        {
            static const std::uint32_t s_renderThread = ThisThreadId();
            static const bool          s_sameThread = LoadThreadId() == 0 || s_renderThread == LoadThreadId();
            static const bool          s_logged = [] {
                SKSE::log::info("UI: rendering on thread {} (plugin load thread {})", s_renderThread, LoadThreadId());
                return true;
            }();
            (void)s_logged;

            if (!s_sameThread) {
                static bool s_warned = false;
                if (!s_warned) {
                    s_warned = true;
                    SKSE::log::error("UI: render callback runs on thread {} but the game thread is {} - skipping the frame, direct engine access from here is unsafe", s_renderThread, LoadThreadId());
                }
            }
            return s_sameThread;
        }

        // ---------------------------------------------------------------- list

        // Wraps the framework's list clipper. Falls back to drawing everything
        // when the framework build does not expose the clipper API.
        class ListClipper
        {
        public:
            explicit ListClipper(int a_count) : _count(a_count)
            {
                _clipper = ImGuiMCP::ImGuiListClipperManager::Create();
                if (_clipper) {
                    ImGuiMCP::ImGuiListClipperManager::Begin(_clipper, a_count, -1.0f);
                }
            }

            ~ListClipper()
            {
                if (_clipper) {
                    ImGuiMCP::ImGuiListClipperManager::End(_clipper);
                    ImGuiMCP::ImGuiListClipperManager::Destroy(_clipper);
                }
            }

            ListClipper(const ListClipper&) = delete;
            ListClipper& operator=(const ListClipper&) = delete;

            bool Step(int& a_start, int& a_end)
            {
                if (_clipper) {
                    if (ImGuiMCP::ImGuiListClipperManager::Step(_clipper)) {
                        a_start = _clipper->DisplayStart;
                        a_end = _clipper->DisplayEnd;
                        return true;
                    }
                    return false;
                }

                if (!_fallbackDone) {
                    _fallbackDone = true;
                    a_start = 0;
                    a_end = _count;
                    return true;
                }
                return false;
            }

        private:
            ImGuiMCP::ImGuiListClipper* _clipper = nullptr;
            int                         _count = 0;
            bool                        _fallbackDone = false;
        };

        // ------------------------------------------------------------- preview

        void UpdatePreview(const std::string& a_rawTexture)
        {
            auto& state = UiState::Get();
            if (a_rawTexture == state.previewPath) {
                return;
            }

            state.previewPath = a_rawTexture;
            state.previewLoadedPath.clear();
            if (state.previewPath.empty()) {
                state.previewTexture = nullptr;
                return;
            }

            state.previewLoadedPath = render::TextureLoader::NormalizeVfsPath(state.previewPath);
            auto* srv = render::TextureLoader::LoadTattooTexture(state.previewPath);
            state.previewTexture = reinterpret_cast<ImGuiMCP::ImTextureID>(srv);

            if (!state.previewTexture) {
                // Not fatal: missing from both loose Data/ and BSA archives
                SKSE::log::warn("UI: preview for '{}' unavailable (VFS path: '{}')", state.previewPath, state.previewLoadedPath);
            }
        }

        // -------------------------------------------------------- helpers

        bool IsBackTattoo(const data::TattooEntry& a_entry)
        {
            if (a_entry.area == "back") return true;
            auto containsCaseInsensitive = [](std::string_view str, std::string_view sub) {
                if (sub.empty()) return true;
                auto it = std::search(str.begin(), str.end(), sub.begin(), sub.end(),
                    [](char ch1, char ch2) { return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2)); });
                return it != str.end();
            };
            return containsCaseInsensitive(a_entry.name, "back") ||
                   containsCaseInsensitive(a_entry.name, "spine") ||
                   containsCaseInsensitive(a_entry.section, "back") ||
                   containsCaseInsensitive(a_entry.texture, "back");
        }

        render::PreviewPart AreaToPreviewPart(std::string_view a_area)
        {
            if (a_area.empty()) return render::PreviewPart::kBody;

            std::string lower(a_area);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (lower.find("face") != std::string::npos || lower.find("head") != std::string::npos) {
                return render::PreviewPart::kFace;
            }
            if (lower.find("hand") != std::string::npos) {
                return render::PreviewPart::kHands;
            }
            if (lower.find("feet") != std::string::npos || lower.find("foot") != std::string::npos) {
                return render::PreviewPart::kFeet;
            }
            return render::PreviewPart::kBody;
        }

        // 当前预览部位的名字, 只用于显示(部位开关已并入上方「部位」一行)。
        const char* PreviewPartName(render::PreviewPart a_part)
        {
            switch (a_part) {
            case render::PreviewPart::kFace:
                return Tr("$STM_Part_FaceHead");
            case render::PreviewPart::kHands:
                return Tr("$STM_Part_Hands");
            case render::PreviewPart::kFeet:
                return Tr("$STM_Part_Feet");
            default:
                return Tr("$STM_Area_Body");
            }
        }

        void SeedEditorFrom(const data::TattooEntry& a_entry)
        {
            auto& state = UiState::Get();

            state.selected = a_entry;
            state.hasSelection = true;
            state.targetSlot = -1;

            // A pack that does not author a colour/alpha leaves the built-in
            // defaults (black / fully opaque) in the entry, and that is exactly
            // where the user's configured starting ink applies. Authored values
            // always win, so coloured tattoo packs keep their own palette.
            const auto&         config = Config::Get();
            const std::uint32_t rgb = (a_entry.defaultColor == 0x000000) ? config.defaultColor : a_entry.defaultColor;

            state.color[0] = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
            state.color[1] = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
            state.color[2] = static_cast<float>(rgb & 0xFF) / 255.0f;
            state.alpha = (a_entry.defaultAlpha >= 1.0f) ? config.defaultAlpha : a_entry.defaultAlpha;

            if (a_entry.defaultGlow > 0) {
                state.glowOn = true;
                state.glowColor[0] = static_cast<float>((a_entry.defaultGlow >> 16) & 0xFF) / 255.0f;
                state.glowColor[1] = static_cast<float>((a_entry.defaultGlow >> 8) & 0xFF) / 255.0f;
                state.glowColor[2] = static_cast<float>(a_entry.defaultGlow & 0xFF) / 255.0f;
            } else {
                state.glowOn = false;
            }

            UpdatePreview(a_entry.texture);

            // 根据纹身所属区域自动切换 3D 模特到对应的身体/面部/手部/脚部
            render::MannequinRenderer::Get().SetCurrentPart(AreaToPreviewPart(a_entry.area));

            if (IsBackTattoo(a_entry)) {
                render::MannequinRenderer::Get().SetBackView();
            }
        }

        std::uint32_t PackColor(const float a_color[3])
        {
            const auto r = static_cast<std::uint32_t>(std::clamp(a_color[0], 0.0f, 1.0f) * 255.0f) & 0xFF;
            const auto g = static_cast<std::uint32_t>(std::clamp(a_color[1], 0.0f, 1.0f) * 255.0f) & 0xFF;
            const auto b = static_cast<std::uint32_t>(std::clamp(a_color[2], 0.0f, 1.0f) * 255.0f) & 0xFF;
            return (r << 16) | (g << 8) | b;
        }

        std::string CurrentArea()
        {
            const int index = UiState::Get().areaIndex;
            if (index <= 0 || index > 4) {
                return {};
            }
            return kAreaIds[index];
        }

        // ------------------------------------------------------ button sizing

        // The menu framework draws with a large CJK font (26-32 px depending on
        // the installed font/theme mods) while ImGui's frame padding stays at
        // (10, 5). A button whose box is smaller than its label gets its glyphs
        // cropped: vertically the text is pushed up and cut off at the top, and
        // horizontally it is clipped on both sides - which reads exactly like
        // "the text is not centred and not fully visible". So every button is
        // sized from the live font metrics instead of a hardcoded box.
        float ButtonWidth(const char* a_label, float a_minWidth = 0.0f)
        {
            ImVec2 text{};
            ImGui::CalcTextSize(&text, a_label, nullptr, true, -1.0f);

            const float padding = ImGui::GetStyle()->FramePadding.x * 2.0f;
            return std::max(a_minWidth, text.x + padding + 6.0f);
        }

        float ButtonHeight(float a_minHeight = 0.0f)
        {
            const float padding = ImGui::GetStyle()->FramePadding.y * 2.0f;
            return std::max(a_minHeight, ImGui::GetTextLineHeight() + padding + 2.0f);
        }

        ImVec2 ButtonSize(const char* a_label, float a_minWidth = 0.0f, float a_minHeight = 0.0f)
        {
            return ImVec2(ButtonWidth(a_label, a_minWidth), ButtonHeight(a_minHeight));
        }

        // Put the cursor at a_window_x on the current row, never closer than the
        // natural same-line position, so a right-aligned button can never be
        // placed back on top of what the row already contains.
        void SameLineAt(float a_window_x)
        {
            const float spacing = ImGui::GetStyle()->ItemSpacing.x;
            const float natural = ImGui::GetCursorPosX() + spacing;
            if (a_window_x < natural) {
                a_window_x = natural;
            }
            ImGui::SameLine(a_window_x - spacing);
        }

        // A pair of buttons that belong together. Each one gets at least its
        // natural width; when both labels cannot fit on one row the second is
        // stacked underneath rather than cropped.
        struct ButtonPair
        {
            ImVec2 first;
            ImVec2 second;
            bool   stacked = false;
        };

        ButtonPair LayoutButtonPair(const char* a_first, const char* a_second, float a_firstWidth, float a_secondWidth)
        {
            const float available = ImGui::GetContentRegionAvail().x;
            const float spacing = ImGui::GetStyle()->ItemSpacing.x;
            const float height = ButtonHeight();

            ButtonPair pair;
            pair.first = ImVec2(std::max(a_firstWidth, ButtonWidth(a_first)), height);
            pair.second = ImVec2(std::max(a_secondWidth, ButtonWidth(a_second)), height);

            if (pair.first.x + pair.second.x + spacing > available) {
                pair.stacked = true;
                pair.first.x = std::min(pair.first.x, available);
                pair.second.x = std::min(pair.second.x, available);
            }

            return pair;
        }

        // ----------------------------------------------------------- panels

        void DrawTargetRow()
        {
            auto& state = UiState::Get();
            auto& target = data::TargetState::Get();

            ImGui::Text("%s", Tr("$STM_Target_2"));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "[ %s ]", target.DisplayName().c_str());

            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("$STM_PickCrosshairNpc"))) {
                if (target.PickFromCrosshair()) {
                    state.SetStatus(true, std::string(Tr("$STM_Target")) + target.DisplayName(), NowSeconds());
                } else {
                    state.SetStatus(false, Tr("$STM_Target_CrosshairEmpty"), NowSeconds());
                }
                data::SlotSnapshot::Get().Invalidate();
            }

            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("$STM_ResetToPlayer"))) {
                target.ResetToPlayer();
                state.SetStatus(true, Tr("$STM_TargetResetToThe"), NowSeconds());
                data::SlotSnapshot::Get().Invalidate();
            }

            // The two library buttons sit at the right edge of the row. Their
            // labels are measured rather than guessed: "重新扫描纹身库" needs far
            // more than the 144 px the old fixed box offered.
            const char*  rescanLabel = Tr("$STM_Btn_Rescan");
            const char*  syncLabel = Tr("$STM_Btn_ForceSync");
            const ImVec2 rescanSize = ButtonSize(rescanLabel, 144.0f);
            const ImVec2 syncSize = ButtonSize(syncLabel, 144.0f);
            const float  spacing = ImGui::GetStyle()->ItemSpacing.x;

            const float rowRight = ImGui::GetWindowContentRegionMax().x;
            const float buttonsWidth = rescanSize.x + spacing + syncSize.x;
            if (rowRight - buttonsWidth > ImGui::GetCursorPosX() + spacing) {
                SameLineAt(rowRight - buttonsWidth);
            } else {
                // A long target name eats the row: drop to the next line and
                // still keep the pair at the right edge instead of cropping it.
                ImGui::NewLine();
                ImGui::SetCursorPosX(std::max(0.0f, rowRight - buttonsWidth));
            }

            if (ImGui::Button(rescanLabel, rescanSize)) {
                auto& db = data::TattooDatabase::Get();
                db.Rebuild();
                state.hasSelection = false;
                UpdatePreview("");
                state.SetStatus(true, std::string(Tr("$STM_LibraryRebuilt")) + std::to_string(db.Size()) + " (" + db.SourceName() + ")", NowSeconds());
            }

            ImGui::SameLine();
            if (ImGui::Button(syncLabel, syncSize)) {
                ops::Sync(target.Handle());
            }
        }

        void DrawAreaRow()
        {
            auto& state = UiState::Get();

            ImGui::Text("%s", Tr("$STM_Area"));
            ImGui::SameLine();

            for (int i = 0; i < 5; ++i) {
                if (i > 0) {
                    ImGui::SameLine();
                }

                static const char* kAreaKeys[] = {
                    "$STM_Area_All",
                    "$STM_Area_Body",
                    "$STM_Area_Face",
                    "$STM_Area_Hands",
                    "$STM_Area_Feet"
                };

                if (ImGui::RadioButton(Tr(kAreaKeys[i]), state.areaIndex == i)) {
                    state.areaIndex = i;
                    state.hasSelection = false;
                    UpdatePreview("");
                    data::SlotSnapshot::Get().Invalidate();
                    // 3D 预览的部位直接跟随这一行的选择(「全部」= 全身), 不再有第二套部位开关
                    render::MannequinRenderer::Get().SetCurrentPart(AreaToPreviewPart(kAreaIds[i]));
                }
            }
        }

        void DrawSectionPanel(float a_width, float a_height)
        {
            auto& state = UiState::Get();
            auto& db = data::TattooDatabase::Get();

            ImGui::BeginChild("section_panel", ImVec2(a_width, a_height), true);
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", Tr("$STM_Heading_Mods"));
            ImGui::Separator();

            if (!state.sectionChosen) {
                ImGui::TextDisabled("%s", Tr("$STM_Hint_PickMod"));
            }

            // The clear button is as wide as its (large) glyph needs; the search
            // box takes whatever is left so the two never overlap.
            const float clearModWidth = ButtonWidth("x##clear_mod", 24.0f);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - clearModWidth - ImGui::GetStyle()->ItemSpacing.x - 2.0f);
            ImGui::InputTextWithHint("##mod_search", Tr("$STM_SearchMods"), state.modSearch, sizeof(state.modSearch));
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("x##clear_mod", ImVec2(clearModWidth, ButtonHeight()))) {
                state.modSearch[0] = '\0';
            }
            ImGui::Spacing();

            const auto& sections = db.QuerySections(CurrentArea(), state.modSearch);

            // Picking an entry selects it; clicking the current pick again
            // deselects it and takes the tattoo list away with it.
            const bool allSelected = state.sectionChosen && state.sectionFilter.empty();
            if (ImGui::Selectable(Tr("$STM_Filter_AllMods"), allSelected)) {
                state.sectionChosen = !allSelected;
                state.sectionFilter.clear();
                state.hasSelection = false;
                UpdatePreview("");
            }

            ListClipper clipper(static_cast<int>(sections.size()));
            int start = 0;
            int end = 0;
            while (clipper.Step(start, end)) {
                for (int i = start; i < end; ++i) {
                    const auto& section = sections[static_cast<std::size_t>(i)];
                    const std::string label = section.name + " (" + std::to_string(section.count) + ")";
                    const bool selected = state.sectionChosen && state.sectionFilter == section.name;

                    if (ImGui::Selectable((label + "##sec_" + section.name).c_str(), selected)) {
                        state.sectionChosen = !selected;
                        state.sectionFilter = selected ? std::string() : section.name;
                        state.hasSelection = false;
                        UpdatePreview("");
                    }
                }
            }

            ImGui::EndChild();
        }

        void DrawTattooPanel(float a_width, float a_height)
        {
            auto& state = UiState::Get();
            auto& db = data::TattooDatabase::Get();

            ImGui::BeginChild("tattoo_panel", ImVec2(a_width, a_height), true);

            const auto& tattoos = db.Query(CurrentArea(), state.sectionFilter, state.tattooSearch);

            const std::string title = state.sectionFilter.empty()
                ? std::string(Tr("$STM_Heading_Tattoos")) + " (" + std::to_string(tattoos.size()) + ")"
                : std::string("2. [") + state.sectionFilter + "] (" + std::to_string(tattoos.size()) + ")";

            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.7f, 1.0f), "%s", title.c_str());
            ImGui::Separator();

            const float clearTatWidth = ButtonWidth("x##clear_tat", 24.0f);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - clearTatWidth - ImGui::GetStyle()->ItemSpacing.x - 2.0f);
            if (ImGui::InputTextWithHint("##tat_search", Tr("$STM_SearchTattoos"), state.tattooSearch, sizeof(state.tattooSearch))) {
                state.hasSelection = false;
                UpdatePreview("");
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            if (ImGui::Button("x##clear_tat", ImVec2(clearTatWidth, ButtonHeight()))) {
                state.tattooSearch[0] = '\0';
                state.hasSelection = false;
                UpdatePreview("");
            }
            ImGui::Spacing();

            if (tattoos.empty()) {
                ImGui::TextDisabled("%s", Tr("$STM_NoTattoosMatchThe"));
            } else {
                ListClipper clipper(static_cast<int>(tattoos.size()));
                int start = 0;
                int end = 0;
                while (clipper.Step(start, end)) {
                    for (int i = start; i < end; ++i) {
                        const auto& item = tattoos[static_cast<std::size_t>(i)];

                        std::string label = "[" + item.area + "] ";
                        if (state.sectionFilter.empty()) {
                            label += "[" + item.section + "] ";
                        }
                        label += item.name;

                        const bool selected = state.hasSelection && state.selected.section == item.section && state.selected.name == item.name;

                        if (ImGui::Selectable((label + "##tat_" + item.section + "_" + item.name).c_str(), selected)) {
                            SeedEditorFrom(item);
                        }
                    }
                }
            }

            ImGui::EndChild();
        }

        void DrawSlotList(const std::string& a_area)
        {
            auto& state = UiState::Get();

            // Take a copy: the snapshot is rebuilt on the task queue, which is a
            // different thread from this render callback.
            const auto snapshot = data::SlotSnapshot::Get().Copy();

            if (!snapshot.valid || snapshot.area != a_area) {
                ImGui::TextDisabled("%s", Tr("$STM_ReadingSlots"));
                return;
            }

            for (const auto& slot : snapshot.slots) {
                if (!slot.occupied) {
                    std::string label = std::string(Tr("$STM_Slot_2")) + std::to_string(slot.slot) + ": " + Tr("$STM_Slot_Empty");
                    ImGui::TextDisabled("%s", label.c_str());
                    continue;
                }

                std::string label = std::string(Tr("$STM_Slot_2")) + std::to_string(slot.slot) + ": ";
                if (slot.external) {
                    label += Tr("$STM_Slot_ExternalTrailing");
                } else if (!slot.section.empty()) {
                    label += "[" + slot.section + "] ";
                }
                label += slot.name;

                ImGui::TextColored(slot.external ? ImVec4(0.9f, 0.7f, 0.3f, 1.0f) : ImVec4(0.3f, 0.9f, 0.6f, 1.0f), "%s", label.c_str());

                // Both actions belong to tattoos this plugin owns. An external
                // overlay is listed (in orange) but never touched - the same rule
                // ops::Apply follows when it refuses to overwrite one.
                if (!slot.external) {
                    ImGui::SameLine();
                    const std::string clearId = std::string("x##slot_") + std::to_string(slot.slot);
                    if (ImGui::SmallButton(clearId.c_str())) {
                        ops::RemoveSlot(data::TargetState::Get().Handle(), a_area, slot.slot);
                    }

                    ImGui::SameLine();
                    const std::string editId = std::string(Tr("$STM_Edit")) + std::to_string(slot.slot);
                    if (ImGui::SmallButton(editId.c_str())) {
                        // The snapshot already carries the ledger values, so this
                        // needs no extra JContainers round trip from the render
                        // callback (it used to re-read them through ReadSlotAppearance).
                        const std::uint32_t color = slot.color;
                        const float         alpha = slot.alpha;
                        const std::uint32_t glow = slot.glow;

                        state.color[0] = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
                        state.color[1] = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
                        state.color[2] = static_cast<float>(color & 0xFF) / 255.0f;
                        state.alpha = alpha;

                        if (glow > 0) {
                            state.glowOn = true;
                            state.glowColor[0] = static_cast<float>((glow >> 16) & 0xFF) / 255.0f;
                            state.glowColor[1] = static_cast<float>((glow >> 8) & 0xFF) / 255.0f;
                            state.glowColor[2] = static_cast<float>(glow & 0xFF) / 255.0f;
                        } else {
                            state.glowOn = false;
                        }

                        const auto* matched = data::TattooDatabase::Get().Find(slot.section, slot.name);
                        if (matched) {
                            state.selected = *matched;
                        } else {
                            state.selected.name = slot.name;
                            state.selected.section = slot.section;
                            state.selected.area = a_area;
                            state.selected.texture.clear();
                        }
                        state.hasSelection = true;
                        state.targetSlot = slot.slot;
                        UpdatePreview(state.selected.texture);

                        // 自动同步部位与后背视角
                        render::MannequinRenderer::Get().SetCurrentPart(AreaToPreviewPart(state.selected.area));

                        if (IsBackTattoo(state.selected)) {
                            render::MannequinRenderer::Get().SetBackView();
                        }

                        state.SetStatus(true, std::string(Tr("$STM_LoadedSlot")) + std::to_string(slot.slot), NowSeconds());
                    }
                }
            }
        }

        void DrawEnlargedPreviewModal(const char* a_modalTitle)
        {
            auto& state = UiState::Get();

            auto* viewport = ImGui::GetMainViewport();
            const ImVec2 center = viewport ? ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f) : ImVec2(640.0f, 360.0f);
            const float modalW = viewport ? std::min(std::max(600.0f, viewport->Size.x * 0.70f), 860.0f) : 760.0f;
            const float modalH = viewport ? std::min(std::max(500.0f, viewport->Size.y * 0.80f), 860.0f) : 760.0f;

            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(modalW, modalH), ImGuiCond_Appearing);

            bool modalOpen = true;
            if (ImGui::BeginPopupModal(a_modalTitle, &modalOpen, ImGuiWindowFlags_NoCollapse)) {
                if (!modalOpen || !state.previewTexture || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    return;
                }

                // 顶部信息与快速控制
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s: %s", Tr("$STM_Tattoo"), state.selected.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("| %s: %s", Tr("$STM_Mod"), state.selected.section.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("| %s: %s", Tr("$STM_Part"), PreviewPartName(render::MannequinRenderer::Get().GetCurrentPart()));
                ImGui::SameLine();
                ImGui::TextDisabled("| %s: %s", Tr("$STM_Mesh"), render::MannequinRenderer::Get().GetMeshSourceInfo().c_str());

                ImGui::Text("%s:", Tr("$STM_Sex"));
                ImGui::SameLine();
                int sexChoice = render::MannequinRenderer::Get().GetCurrentSex();
                if (ImGui::RadioButton(Tr("$STM_Female"), sexChoice == 0)) {
                    render::MannequinRenderer::Get().InitMeshWithSex(0);
                }
                ImGui::SameLine();
                if (ImGui::RadioButton(Tr("$STM_Male"), sexChoice == 1)) {
                    render::MannequinRenderer::Get().InitMeshWithSex(1);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr("$STM_SyncTarget"))) {
                    render::MannequinRenderer::Get().ReloadForPlayerSex();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr("$STM_ResetCamera"))) {
                    render::MannequinRenderer::Get().ResetCamera();
                }
                ImGui::SameLine();
                ImGui::Text(" | %s:", Tr("$STM_Color"));
                ImGui::SameLine();
                int curCol = (Config::Get().mannequinColor == MannequinColor::kFlesh) ? 1 : 0;
                if (ImGui::RadioButton(Tr("$STM_Flesh"), curCol == 1)) {
                    Config::Get().mannequinColor = MannequinColor::kFlesh;
                    Config::Get().Save();
                }
                ImGui::SameLine();
                if (ImGui::RadioButton(Tr("$STM_Holo"), curCol == 0)) {
                    Config::Get().mannequinColor = MannequinColor::kTransparent;
                    Config::Get().Save();
                }

                ImGui::Separator();

                // 3D 渲染画面 (填满窗口剩余主要空间)
                const float availW = std::max(180.0f, ImGui::GetContentRegionAvail().x);
                const float availH = std::max(180.0f, ImGui::GetContentRegionAvail().y - 36.0f);

                if (render::MannequinRenderer::Get().GetWidth() != static_cast<std::uint32_t>(availW) ||
                    render::MannequinRenderer::Get().GetHeight() != static_cast<std::uint32_t>(availH)) {
                    render::MannequinRenderer::Get().Resize(static_cast<std::uint32_t>(availW), static_cast<std::uint32_t>(availH));
                }

                auto* bigSrv = render::MannequinRenderer::Get().RenderFrame(
                    reinterpret_cast<ID3D11ShaderResourceView*>(state.previewTexture),
                    state.color,
                    state.alpha,
                    false,
                    Config::Get().mannequinColor,
                    0.0f,
                    state.glowOn ? state.glowColor : nullptr
                );

                if (bigSrv) {
                    ImGui::Image(reinterpret_cast<ImGuiMCP::ImTextureID>(bigSrv), ImVec2(availW, availH),
                        ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, 1), ImVec4(0.35f, 0.40f, 0.50f, 1.0f));

                    static bool s_bigDraggingLeft = false;
                    static bool s_bigDraggingRight = false;
                    static bool s_bigDraggingMiddle = false;
                    static double s_bigLeftDownTime = 0.0;
                    static float s_bigLeftDragDist = 0.0f;

                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", Tr("$STM_LeftDragRotateRight"));

                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            s_bigDraggingLeft = true;
                            s_bigLeftDownTime = NowSeconds();
                            s_bigLeftDragDist = 0.0f;
                        }
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                            s_bigDraggingRight = true;
                        }
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
                            s_bigDraggingMiddle = true;
                        }
                    }

                    if (s_bigDraggingLeft) {
                        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                            auto* io = ImGui::GetIO();
                            if (io) {
                                s_bigLeftDragDist += std::hypot(io->MouseDelta.x, io->MouseDelta.y);
                                render::MannequinRenderer::Get().Rotate(io->MouseDelta.x * 0.012f, io->MouseDelta.y * 0.012f);
                            }
                        } else {
                            s_bigDraggingLeft = false;
                            const double elapsed = NowSeconds() - s_bigLeftDownTime;
                            if (elapsed < 0.35 && s_bigLeftDragDist < 6.0f) {
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    }

                    if (s_bigDraggingRight) {
                        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                            auto* io = ImGui::GetIO();
                            if (io && io->MouseDelta.x != 0.0f) {
                                render::MannequinRenderer::Get().Zoom(io->MouseDelta.x * 0.012f);
                            }
                        } else {
                            s_bigDraggingRight = false;
                        }
                    }

                    if (s_bigDraggingMiddle) {
                        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                            auto* io = ImGui::GetIO();
                            if (io && io->MouseDelta.y != 0.0f) {
                                render::MannequinRenderer::Get().Pan(-io->MouseDelta.y * 0.005f);
                            }
                        } else {
                            s_bigDraggingMiddle = false;
                        }
                    }
                }

                // 底部操作栏与提示
                ImGui::Spacing();
                const float btnW = 150.0f;
                if (ImGui::Button(Tr("$STM_RestoreEsc"), ImVec2(btnW, 0.0f))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", Tr("$STM_TipLeftDragRotate"));

                ImGui::EndPopup();
            }
        }

        void DrawInspector(float a_width, float a_height)
        {
            auto& state = UiState::Get();
            auto& db = data::TattooDatabase::Get();

            ImGui::BeginChild("inspector_panel", ImVec2(a_width, a_height), true);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", Tr("$STM_Heading_Properties"));
            ImGui::Separator();

            if (state.hasSelection) {
                ImGui::Text("%s: %s", Tr("$STM_TattooLower"), state.selected.name.c_str());
                ImGui::Text("%s: %s", Tr("$STM_ModLower"), state.selected.section.c_str());
                ImGui::Text("%s: %s", Tr("$STM_AreaLower"), state.selected.area.c_str());

                if (!state.selected.texture.empty()) {
                    ImGui::TextDisabled("%s: %s", Tr("$STM_Texture"), state.selected.texture.c_str());
                }

                if (Config::Get().showPreview && state.previewTexture) {
                    ImGui::Spacing();

                    static bool s_view3D = true;

                    ImGui::Text("%s:", Tr("$STM_Preview"));
                    ImGui::SameLine();
                    if (ImGui::RadioButton(Tr("$STM_Tab_3DMannequin"), s_view3D)) {
                        s_view3D = true;
                        render::MannequinRenderer::Get().ClearInitFailed();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(Tr("$STM_Tab_2DTexture"), !s_view3D)) {
                        s_view3D = false;
                    }

                    if (s_view3D) {
                        ImGui::Text("%s:", Tr("$STM_ModelColor"));
                        ImGui::SameLine();
                        int curCol = (Config::Get().mannequinColor == MannequinColor::kFlesh) ? 1 : 0;
                        // 顺序: 先自然肉色, 再透明全息蓝
                        if (ImGui::RadioButton(Tr("$STM_NaturalFlesh"), curCol == 1)) {
                            Config::Get().mannequinColor = MannequinColor::kFlesh;
                            Config::Get().Save();
                        }
                        ImGui::SameLine();
                        if (ImGui::RadioButton(Tr("$STM_TransparentHologram"), curCol == 0)) {
                            Config::Get().mannequinColor = MannequinColor::kTransparent;
                            Config::Get().Save();
                        }

                        if (!render::MannequinRenderer::Get().IsInitialized() && !render::MannequinRenderer::Get().IsInitFailed()) {
                            auto* bsDev = RE::BSGraphics::Renderer::GetDevice();
                            if (bsDev) {
                                auto* d3dDev = reinterpret_cast<ID3D11Device*>(bsDev);
                                ID3D11DeviceContext* ctx = nullptr;
                                d3dDev->GetImmediateContext(&ctx);
                                if (ctx) {
                                    render::MannequinRenderer::Get().Initialize(d3dDev, ctx);
                                    ctx->Release();
                                }
                            }
                        }

                        if (render::MannequinRenderer::Get().IsInitFailed()) {
                            ImGui::Spacing();
                            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", Tr("$STM_Preview_3DInitFailed"));
                            if (ImGui::Button(Tr("$STM_ClickToRetry"))) {
                                render::MannequinRenderer::Get().ClearInitFailed();
                            }
                        } else if (render::MannequinRenderer::Get().IsInitialized()) {
                            const float pWidth = std::max(180.0f, a_width - 30.0f);
                            const float pHeight = 220.0f;

                            static const char* kEnlargedModalId = "###stm_3d_preview_modal";
                            const std::string modalTitle = std::string(Tr("$STM_Preview_3DEnlarged")) + kEnlargedModalId;
                            const bool isModalOpen = ImGui::IsPopupOpen(modalTitle.c_str());

                            ID3D11ShaderResourceView* srv = nullptr;
                            if (!isModalOpen) {
                                if (render::MannequinRenderer::Get().GetWidth() != static_cast<std::uint32_t>(pWidth) ||
                                    render::MannequinRenderer::Get().GetHeight() != static_cast<std::uint32_t>(pHeight)) {
                                    render::MannequinRenderer::Get().Resize(static_cast<std::uint32_t>(pWidth), static_cast<std::uint32_t>(pHeight));
                                }

                                srv = render::MannequinRenderer::Get().RenderFrame(
                                    reinterpret_cast<ID3D11ShaderResourceView*>(state.previewTexture),
                                    state.color,
                                    state.alpha,
                                    false,
                                    Config::Get().mannequinColor,
                                    0.0f,
                                    state.glowOn ? state.glowColor : nullptr
                                );
                            } else {
                                srv = render::MannequinRenderer::Get().GetShaderResourceView();
                            }

                            if (srv) {
                                ImGui::Image(reinterpret_cast<ImGuiMCP::ImTextureID>(srv), ImVec2(pWidth, pHeight),
                                    ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, 1), ImVec4(0.35f, 0.40f, 0.50f, 1.0f));

                                static bool s_isDraggingLeft = false;
                                static bool s_isDraggingRight = false;
                                static bool s_isDraggingMiddle = false;
                                static double s_leftClickDownTime = 0.0;
                                static float s_leftDragDistance = 0.0f;

                                if (ImGui::IsItemHovered()) {
                                    ImGui::SetTooltip("%s", Tr("$STM_LeftDragRotateRight_2"));

                                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                        s_isDraggingLeft = true;
                                        s_leftClickDownTime = NowSeconds();
                                        s_leftDragDistance = 0.0f;
                                    }
                                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                                        s_isDraggingRight = true;
                                    }
                                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
                                        s_isDraggingMiddle = true;
                                    }
                                }

                                if (s_isDraggingLeft) {
                                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                        auto* io = ImGui::GetIO();
                                        if (io) {
                                            s_leftDragDistance += std::hypot(io->MouseDelta.x, io->MouseDelta.y);
                                            render::MannequinRenderer::Get().Rotate(io->MouseDelta.x * 0.012f, io->MouseDelta.y * 0.012f);
                                        }
                                    } else {
                                        s_isDraggingLeft = false;
                                        const double elapsed = NowSeconds() - s_leftClickDownTime;
                                        if (elapsed < 0.35 && s_leftDragDistance < 6.0f) {
                                            ImGui::OpenPopup(modalTitle.c_str());
                                        }
                                    }
                                }

                                if (s_isDraggingRight) {
                                    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                                        auto* io = ImGui::GetIO();
                                        if (io && io->MouseDelta.x != 0.0f) {
                                            // 按住右键鼠标往左缩小(负值增加相机距离)，往右放大(正值减小相机距离)
                                            render::MannequinRenderer::Get().Zoom(io->MouseDelta.x * 0.012f);
                                        }
                                    } else {
                                        s_isDraggingRight = false;
                                    }
                                }

                                if (s_isDraggingMiddle) {
                                    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                                        auto* io = ImGui::GetIO();
                                        if (io && io->MouseDelta.y != 0.0f) {
                                            render::MannequinRenderer::Get().Pan(-io->MouseDelta.y * 0.005f);
                                        }
                                    } else {
                                        s_isDraggingMiddle = false;
                                    }
                                }

                                // 部位不再单独提供开关: 预览部位完全跟随上方「部位」一行,
                                // 这里只显示当前正在预览哪一部分。
                                ImGui::TextDisabled("%s: %s%s", Tr("$STM_Part"),
                                    PreviewPartName(render::MannequinRenderer::Get().GetCurrentPart()),
                                    Tr("$STM_Hint_FollowsArea"));

                                ImGui::TextDisabled("%s: %s", Tr("$STM_Mesh"), render::MannequinRenderer::Get().GetMeshSourceInfo().c_str());

                                ImGui::Text("%s:", Tr("$STM_Sex"));
                                ImGui::SameLine();
                                int sexChoice = render::MannequinRenderer::Get().GetCurrentSex();
                                if (ImGui::RadioButton(Tr("$STM_Female_2"), sexChoice == 0)) {
                                    render::MannequinRenderer::Get().InitMeshWithSex(0);
                                }
                                ImGui::SameLine();
                                if (ImGui::RadioButton(Tr("$STM_Male_2"), sexChoice == 1)) {
                                    render::MannequinRenderer::Get().InitMeshWithSex(1);
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton(Tr("$STM_SyncTarget_2"))) {
                                    render::MannequinRenderer::Get().ReloadForPlayerSex();
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton(Tr("$STM_ResetCamera_2"))) {
                                    render::MannequinRenderer::Get().ResetCamera();
                                }
                                ImGui::SameLine();
                                if (ImGui::SmallButton(Tr("$STM_Enlarge"))) {
                                    ImGui::OpenPopup(modalTitle.c_str());
                                }
                            }

                            DrawEnlargedPreviewModal(modalTitle.c_str());
                        }
                    } else {
                        const float pSize = Config::Get().previewSize;
                        ImGui::Image(state.previewTexture, ImVec2(pSize, pSize), ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, 1), ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                    }
                }

                ImGui::Spacing();
                ImGui::ColorEdit3(Tr("$STM_Colour"), state.color);

                ImGui::Text("%s:", Tr("$STM_QuickColours"));
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr("$STM_Black"))) {
                    state.color[0] = state.color[1] = state.color[2] = 0.0f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr("$STM_Red"))) {
                    state.color[0] = 1.0f;
                    state.color[1] = 0.0f;
                    state.color[2] = 0.0f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr("$STM_Gold"))) {
                    state.color[0] = 1.0f;
                    state.color[1] = 0.843f;
                    state.color[2] = 0.0f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr("$STM_White"))) {
                    state.color[0] = state.color[1] = state.color[2] = 1.0f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr("$STM_Blue"))) {
                    state.color[0] = 0.0f;
                    state.color[1] = 0.5f;
                    state.color[2] = 1.0f;
                }

                ImGui::SliderFloat(Tr("$STM_Opacity"), &state.alpha, 0.05f, 1.0f, "%.2f");

                ImGui::Spacing();
                ImGui::Checkbox(Tr("$STM_GlowOn", "Glow (Emissive)##glow_on"), &state.glowOn);
                if (state.glowOn) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(Tr("$STM_GlowMatchInk", "Match Ink##glow_match"))) {
                        state.glowColor[0] = state.color[0];
                        state.glowColor[1] = state.color[1];
                        state.glowColor[2] = state.color[2];
                    }
                    ImGui::ColorEdit3(Tr("$STM_GlowColor", "Glow Color##glow_col"), state.glowColor);
                }

                // Slot picker. The chosen slot is clamped against the area the
                // selected tattoo actually belongs to, so a stale value from
                // another area can never be submitted.
                const int maxSlots = ng::MaxSlots(state.selected.area);
                if (state.targetSlot >= maxSlots) {
                    state.targetSlot = -1;
                }

                const std::string comboLabel = state.targetSlot < 0
                    ? std::string(Tr("$STM_FirstFreeSlot"))
                    : std::string(Tr("$STM_Slot")) + std::to_string(state.targetSlot);

                ImGui::Text("%s:", Tr("$STM_TargetSlot"));
                ImGui::SameLine();
                // Wide enough for the longest preview ("自动分配空槽"), clamped to
                // what is actually left on the line.
                ImVec2 slotText{};
                ImGui::CalcTextSize(&slotText, Tr("$STM_FirstFreeSlot"), nullptr, true, -1.0f);
                const auto* style = ImGui::GetStyle();
                const float comboWidth = std::min(slotText.x + ImGui::GetFrameHeight() + style->FramePadding.x * 2.0f + style->ItemInnerSpacing.x,
                    ImGui::GetContentRegionAvail().x);
                ImGui::PushItemWidth(comboWidth);
                if (ImGui::BeginCombo("##slot_combo", comboLabel.c_str())) {
                    if (ImGui::Selectable(Tr("$STM_FirstFreeSlot_2"), state.targetSlot < 0)) {
                        state.targetSlot = -1;
                    }
                    const auto snap = data::SlotSnapshot::Get().Copy();
                    for (int slot = 0; slot < maxSlots; ++slot) {
                        std::string statusText = Tr("$STM_Slot_Empty");
                        if (snap.valid && snap.area == state.selected.area) {
                            for (const auto& s : snap.slots) {
                                if (s.slot == slot && s.occupied) {
                                    statusText = s.external ? Tr("$STM_Slot_External") : ("[" + s.name + "]");
                                    break;
                                }
                            }
                        }
                        const std::string option = std::string(Tr("$STM_Slot_2")) + std::to_string(slot) + ": " + statusText;
                        if (ImGui::Selectable((option + "##slot_opt_" + std::to_string(slot)).c_str(), state.targetSlot == slot)) {
                            state.targetSlot = slot;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                ImGui::Spacing();

                const char* applyLabel = Tr("$STM_ApplyUpdate");
                const char* removeLabel = Tr("$STM_Btn_Remove");
                const float available = ImGui::GetContentRegionAvail().x;
                const auto  actions = LayoutButtonPair(applyLabel, removeLabel,
                    (available - 12.0f) * 0.58f, (available - 12.0f) * 0.42f);

                if (ImGui::Button(applyLabel, actions.first)) {
                    ops::ApplyRequest request;
                    request.actor = data::TargetState::Get().Handle();
                    request.tattoo = state.selected;
                    request.targetSlot = state.targetSlot;
                    request.color = PackColor(state.color);
                    request.alpha = state.alpha;
                    request.glow = state.glowOn ? PackColor(state.glowColor) : 0x000000;
                    ops::Apply(request);
                }

                if (!actions.stacked) {
                    ImGui::SameLine();
                }
                if (ImGui::Button(removeLabel, actions.second)) {
                    ops::Remove(data::TargetState::Get().Handle(), state.selected.section, state.selected.name);
                }
            } else {
                ImGui::TextDisabled("%s", state.sectionChosen
                    ? Tr("$STM_PickATattooIn")
                    : Tr("$STM_Hint_SelectModFirst"));
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const std::string area = CurrentArea().empty() ? std::string("Body") : CurrentArea();

            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s (%s):", Tr("$STM_SlotStatus"), area.c_str());

            data::SlotSnapshot::Get().Request(data::TargetState::Get().Handle(), area);
            DrawSlotList(area);

            ImGui::Spacing();
            const char* clearAreaLabel = Tr("$STM_ClearArea");
            const char* clearAllLabel = Tr("$STM_ClearEverything");
            const float half = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
            const auto  clears = LayoutButtonPair(clearAreaLabel, clearAllLabel, half, half);

            if (ImGui::Button(clearAreaLabel, clears.first)) {
                ops::ClearArea(data::TargetState::Get().Handle(), area);
            }
            if (!clears.stacked) {
                ImGui::SameLine();
            }
            if (ImGui::Button(clearAllLabel, clears.second)) {
                ops::ClearAll(data::TargetState::Get().Handle());
            }

            ImGui::Spacing();
            if (ImGui::CollapsingHeader(Tr("$STM_Presets"))) {
                static char s_presetName[64] = "";
                static bool s_clearFirst = true;

                // Overwriting and deleting both destroy a capture with no undo,
                // and both used to happen on a single click with nothing but a
                // status line afterwards. They now share one confirmation popup.
                static std::string s_pendingPreset;
                static bool        s_pendingIsDelete = false;

                auto&       store = data::PresetStore::Get();
                static bool s_storeLoaded = false;
                if (!s_storeLoaded) {
                    s_storeLoaded = true;
                    store.Refresh();
                }

                ImGui::Text("%s:", Tr("$STM_PresetName"));
                ImGui::SameLine();
                ImGui::PushItemWidth(std::max(200.0f, ImGui::GetContentRegionAvail().x - 4.0f));
                ImGui::InputTextWithHint("##preset_name", Tr("$STM_NameThisSet"), s_presetName, sizeof(s_presetName));
                ImGui::PopItemWidth();

                const char* savePresetLabel = Tr("$STM_SaveTheCurrentTarget");
                if (ImGui::Button(savePresetLabel, ButtonSize(savePresetLabel, 200.0f, 28.0f))) {
                    if (s_presetName[0] == '\0') {
                        state.SetStatus(false, Tr("$STM_EnterAPresetName"), NowSeconds());
                    } else if (store.Exists(s_presetName)) {
                        s_pendingPreset = s_presetName;
                        s_pendingIsDelete = false;
                        ImGui::OpenPopup("##stm_preset_confirm");
                    } else {
                        ops::CapturePreset(data::TargetState::Get().Handle(), s_presetName);
                    }
                }
                ImGui::SameLine();
                const char* openPresetFolderLabel = Tr("$STM_OpenFolder");
                if (ImGui::Button(openPresetFolderLabel, ButtonSize(openPresetFolderLabel, 0.0f, 28.0f))) {
                    const auto dir = data::PresetStore::Get().Directory();
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }

                ImGui::Checkbox(Tr("$STM_ClearTheTargetBefore"), &s_clearFirst);

                // Copy the names: deleting one refreshes the store's list.
                const auto names = store.Names();
                if (names.empty()) {
                    ImGui::TextDisabled("%s", Tr("$STM_NoPresetsSavedYet"));
                } else {
                    for (const auto& presetName : names) {
                        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "%s", presetName.c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton((std::string(Tr("$STM_Apply")) + presetName).c_str())) {
                            ops::ApplyPreset(data::TargetState::Get().Handle(), presetName, s_clearFirst);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton((std::string(Tr("$STM_Btn_DeletePreset")) + presetName).c_str())) {
                            s_pendingPreset = presetName;
                            s_pendingIsDelete = true;
                            ImGui::OpenPopup("##stm_preset_confirm");
                        }
                    }
                }

                if (ImGui::BeginPopupModal("##stm_preset_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    const char* question = s_pendingIsDelete ? Tr("$STM_ConfirmDeletePreset") : Tr("$STM_ConfirmOverwritePreset");
                    ImGui::TextUnformatted(question);
                    ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "%s", s_pendingPreset.c_str());
                    ImGui::Spacing();

                    const char* actionLabel = s_pendingIsDelete ? Tr("$STM_Btn_DeletePreset") : Tr("$STM_Btn_Overwrite");
                    if (ImGui::Button(actionLabel, ButtonSize(actionLabel, 120.0f, 26.0f))) {
                        if (s_pendingIsDelete) {
                            if (store.Delete(s_pendingPreset)) {
                                state.SetStatus(true, std::string(Tr("$STM_DeletedPreset")) + s_pendingPreset, NowSeconds());
                            } else {
                                state.SetStatus(false, std::string(Tr("$STM_Status_PresetDeleteFailed")) + s_pendingPreset, NowSeconds());
                            }
                        } else {
                            ops::CapturePreset(data::TargetState::Get().Handle(), s_pendingPreset);
                        }
                        s_pendingPreset.clear();
                        ImGui::CloseCurrentPopup();
                    }

                    ImGui::SameLine();
                    const char* cancelLabel = Tr("$STM_Common_Cancel");
                    if (ImGui::Button(cancelLabel, ButtonSize(cancelLabel, 120.0f, 26.0f))) {
                        s_pendingPreset.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }

                ImGui::TextDisabled("%s", Tr("$STM_PresetsLiveInData"));
            }

            ImGui::Spacing();
            if (ImGui::CollapsingHeader(Tr("$STM_Diagnostics"))) {
                ImGui::Text("%s: %s", Tr("$STM_Status"), diag::IsRunning() ? Tr("$STM_Probe_Running") : Tr("$STM_Probe_Stopped"));
                const char* probeToggleLabel = diag::IsRunning() ? Tr("$STM_Probe_BtnStop") : Tr("$STM_Probe_BtnStart");
                if (ImGui::Button(probeToggleLabel, ButtonSize(probeToggleLabel, 110.0f, 26.0f))) {
                    if (diag::IsRunning()) {
                        diag::Stop();
                    } else {
                        diag::Start();
                    }
                }
                ImGui::SameLine();
                const char* probeSnapshotLabel = Tr("$STM_SnapshotNow");
                if (ImGui::Button(probeSnapshotLabel, ButtonSize(probeSnapshotLabel, 156.0f, 26.0f))) {
                    diag::LogSnapshot();
                    state.SetStatus(true, Tr("$STM_ProbeSnapshotRequestedSee"), NowSeconds());
                }
                ImGui::TextDisabled("%s", Tr("$STM_TheProbeComparesThe"));
            }

            ImGui::EndChild();
        }

        // Results arrive from the game thread and are consumed on the render
        // thread. This has to run every frame regardless of which tab is
        // selected: it used to live inside DrawStatusLine, which only DrawMain
        // called, so every operation started from the Settings tab sat in the
        // board unseen and then flooded the status line the moment the user
        // switched back.
        void PumpResults()
        {
            auto& state = UiState::Get();

            std::vector<OpResult> results;
            ResultBoard::Get().Drain(results);
            for (auto& result : results) {
                std::string text = result.action + ": " + result.target;
                if (!result.detail.empty()) {
                    text += " - " + result.detail;
                }
                state.SetStatus(result.ok, result.deferred, std::move(text), NowSeconds());
            }
        }

        void DrawStatusLine()
        {
            auto& state = UiState::Get();

            if (state.statusText.empty()) {
                return;
            }

            constexpr double kHoldSeconds = 15.0;
            if ((NowSeconds() - state.statusAt) > kHoldSeconds) {
                return;
            }

            ImGui::Separator();
            const char*  tag = !state.statusOk ? Tr("$STM_Failed") : (state.statusDeferred ? Tr("$STM_Deferred") : Tr("$STM_Ok"));
            const ImVec4 colour = !state.statusOk      ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                                  : state.statusDeferred ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f)
                                                         : ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
            ImGui::TextColored(colour, "[%s] %s", tag, state.statusText.c_str());
        }

        void DrawMain()
        {
            if (!OnGameThread()) {
                return;
            }

            FontScaleScope fontScope(Config::Get().fontScale);

            auto& db = data::TattooDatabase::Get();
            auto& state = UiState::Get();

            if (!ng::Ready()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::Text("%s", Tr("$STM_WarningSlavetatsngInterfaceNot"));
                ImGui::PopStyleColor();
                ImGui::Separator();
            }

            ImGui::TextDisabled("%s: %zu (%s)   %s: %s   %s: %d",
                Tr("$STM_Library"),
                db.Size(),
                db.SourceName(),
                Tr("$STM_Jcontainers"),
                jc::Ready() ? Tr("$STM_Status_Ok") : Tr("$STM_Status_Unavailable"),
                Tr("$STM_RegisteredActors"),
                ng::KnownTargetCount());

            DrawTargetRow();
            DrawAreaRow();
            ImGui::Separator();

            // Panel 2 only exists once a mod has been picked in panel 1. While
            // it is hidden, the inspector keeps the width it has in the
            // three-column layout and the mod list absorbs the freed space.
            const float available = ImGui::GetContentRegionAvail().x;
            const float availY = ImGui::GetContentRegionAvail().y;
            const float panelHeight = std::max(380.0f, availY - 35.0f);

            // Three columns need 240 + 280 + 380 + gaps. On a narrower window the
            // tattoo list gives up width first (down to 180) - the old fixed 400 px
            // inspector minimum pushed the whole right column out of the window.
            constexpr float col1Fixed = 240.0f;
            constexpr float col2Max = 280.0f;
            constexpr float col2Min = 180.0f;
            constexpr float minInspector = 380.0f;

            const float gaps = state.sectionChosen ? 16.0f : 8.0f;
            float       col2 = state.sectionChosen ? col2Max : 0.0f;
            float       col3 = available - col1Fixed - col2 - gaps;

            if (state.sectionChosen && col3 < minInspector && col2 > col2Min) {
                col2 = std::max(col2Min, col2 - (minInspector - col3));
                col3 = available - col1Fixed - col2 - gaps;
            }
            col3 = std::max(240.0f, col3);

            const float col1 = state.sectionChosen
                ? col1Fixed
                : std::max(col1Fixed, available - gaps - col3);

            DrawSectionPanel(col1, panelHeight);
            if (state.sectionChosen) {
                ImGui::SameLine();
                DrawTattooPanel(col2, panelHeight);
            }
            ImGui::SameLine();
            DrawInspector(col3, panelHeight);
            DrawStatusLine();
        }
        ImGuiKey DirectInputToImGuiKey(std::uint32_t a_dik)
        {
            switch (a_dik) {
            case 0x01: return ImGuiKey_Escape;
            case 0x02: return ImGuiKey_1;
            case 0x03: return ImGuiKey_2;
            case 0x04: return ImGuiKey_3;
            case 0x05: return ImGuiKey_4;
            case 0x06: return ImGuiKey_5;
            case 0x07: return ImGuiKey_6;
            case 0x08: return ImGuiKey_7;
            case 0x09: return ImGuiKey_8;
            case 0x0A: return ImGuiKey_9;
            case 0x0B: return ImGuiKey_0;
            case 0x0C: return ImGuiKey_Minus;
            case 0x0D: return ImGuiKey_Equal;
            case 0x0E: return ImGuiKey_Backspace;
            case 0x0F: return ImGuiKey_Tab;
            case 0x10: return ImGuiKey_Q;
            case 0x11: return ImGuiKey_W;
            case 0x12: return ImGuiKey_E;
            case 0x13: return ImGuiKey_R;
            case 0x14: return ImGuiKey_T;
            case 0x15: return ImGuiKey_Y;
            case 0x16: return ImGuiKey_U;
            case 0x17: return ImGuiKey_I;
            case 0x18: return ImGuiKey_O;
            case 0x19: return ImGuiKey_P;
            case 0x1A: return ImGuiKey_LeftBracket;
            case 0x1B: return ImGuiKey_RightBracket;
            case 0x1C: return ImGuiKey_Enter;
            case 0x1D: return ImGuiKey_LeftCtrl;
            case 0x1E: return ImGuiKey_A;
            case 0x1F: return ImGuiKey_S;
            case 0x20: return ImGuiKey_D;
            case 0x21: return ImGuiKey_F;
            case 0x22: return ImGuiKey_G;
            case 0x23: return ImGuiKey_H;
            case 0x24: return ImGuiKey_J;
            case 0x25: return ImGuiKey_K;
            case 0x26: return ImGuiKey_L;
            case 0x27: return ImGuiKey_Semicolon;
            case 0x28: return ImGuiKey_Apostrophe;
            case 0x29: return ImGuiKey_GraveAccent;
            case 0x2A: return ImGuiKey_LeftShift;
            case 0x2B: return ImGuiKey_Backslash;
            case 0x2C: return ImGuiKey_Z;
            case 0x2D: return ImGuiKey_X;
            case 0x2E: return ImGuiKey_C;
            case 0x2F: return ImGuiKey_V;
            case 0x30: return ImGuiKey_B;
            case 0x31: return ImGuiKey_N;
            case 0x32: return ImGuiKey_M;
            case 0x33: return ImGuiKey_Comma;
            case 0x34: return ImGuiKey_Period;
            case 0x35: return ImGuiKey_Slash;
            case 0x36: return ImGuiKey_RightShift;
            case 0x37: return ImGuiKey_KeypadMultiply;
            case 0x38: return ImGuiKey_LeftAlt;
            case 0x39: return ImGuiKey_Space;
            case 0x3A: return ImGuiKey_CapsLock;
            case 0x3B: return ImGuiKey_F1;
            case 0x3C: return ImGuiKey_F2;
            case 0x3D: return ImGuiKey_F3;
            case 0x3E: return ImGuiKey_F4;
            case 0x3F: return ImGuiKey_F5;
            case 0x40: return ImGuiKey_F6;
            case 0x41: return ImGuiKey_F7;
            case 0x42: return ImGuiKey_F8;
            case 0x43: return ImGuiKey_F9;
            case 0x44: return ImGuiKey_F10;
            case 0x45: return ImGuiKey_NumLock;
            case 0x46: return ImGuiKey_ScrollLock;
            case 0x47: return ImGuiKey_Keypad7;
            case 0x48: return ImGuiKey_Keypad8;
            case 0x49: return ImGuiKey_Keypad9;
            case 0x4A: return ImGuiKey_KeypadSubtract;
            case 0x4B: return ImGuiKey_Keypad4;
            case 0x4C: return ImGuiKey_Keypad5;
            case 0x4D: return ImGuiKey_Keypad6;
            case 0x4E: return ImGuiKey_KeypadAdd;
            case 0x4F: return ImGuiKey_Keypad1;
            case 0x50: return ImGuiKey_Keypad2;
            case 0x51: return ImGuiKey_Keypad3;
            case 0x52: return ImGuiKey_Keypad0;
            case 0x53: return ImGuiKey_KeypadDecimal;
            case 0x57: return ImGuiKey_F11;
            case 0x58: return ImGuiKey_F12;
            case 0x9C: return ImGuiKey_KeypadEnter;
            case 0x9D: return ImGuiKey_RightCtrl;
            case 0xB5: return ImGuiKey_KeypadDivide;
            case 0xB8: return ImGuiKey_RightAlt;
            case 0xC7: return ImGuiKey_Home;
            case 0xC8: return ImGuiKey_UpArrow;
            case 0xC9: return ImGuiKey_PageUp;
            case 0xCB: return ImGuiKey_LeftArrow;
            case 0xCD: return ImGuiKey_RightArrow;
            case 0xCF: return ImGuiKey_End;
            case 0xD0: return ImGuiKey_DownArrow;
            case 0xD1: return ImGuiKey_PageDown;
            case 0xD2: return ImGuiKey_Insert;
            case 0xD3: return ImGuiKey_Delete;
            default: {
                const UINT vk = ::MapVirtualKeyA(a_dik & 0xFF, MAPVK_VSC_TO_VK);
                if (vk >= VK_F1 && vk <= VK_F12) {
                    return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - VK_F1));
                }
                return ImGuiKey_None;
            }
            }
        }

        std::uint32_t ImGuiKeyToDirectInput(ImGuiKey a_key)
        {
            switch (a_key) {
            case ImGuiKey_Escape: return 0x01;
            case ImGuiKey_1: return 0x02;
            case ImGuiKey_2: return 0x03;
            case ImGuiKey_3: return 0x04;
            case ImGuiKey_4: return 0x05;
            case ImGuiKey_5: return 0x06;
            case ImGuiKey_6: return 0x07;
            case ImGuiKey_7: return 0x08;
            case ImGuiKey_8: return 0x09;
            case ImGuiKey_9: return 0x0A;
            case ImGuiKey_0: return 0x0B;
            case ImGuiKey_Minus: return 0x0C;
            case ImGuiKey_Equal: return 0x0D;
            case ImGuiKey_Backspace: return 0x0E;
            case ImGuiKey_Tab: return 0x0F;
            case ImGuiKey_Q: return 0x10;
            case ImGuiKey_W: return 0x11;
            case ImGuiKey_E: return 0x12;
            case ImGuiKey_R: return 0x13;
            case ImGuiKey_T: return 0x14;
            case ImGuiKey_Y: return 0x15;
            case ImGuiKey_U: return 0x16;
            case ImGuiKey_I: return 0x17;
            case ImGuiKey_O: return 0x18;
            case ImGuiKey_P: return 0x19;
            case ImGuiKey_LeftBracket: return 0x1A;
            case ImGuiKey_RightBracket: return 0x1B;
            case ImGuiKey_Enter: return 0x1C;
            case ImGuiKey_LeftCtrl: return 0x1D;
            case ImGuiKey_A: return 0x1E;
            case ImGuiKey_S: return 0x1F;
            case ImGuiKey_D: return 0x20;
            case ImGuiKey_F: return 0x21;
            case ImGuiKey_G: return 0x22;
            case ImGuiKey_H: return 0x23;
            case ImGuiKey_J: return 0x24;
            case ImGuiKey_K: return 0x25;
            case ImGuiKey_L: return 0x26;
            case ImGuiKey_Semicolon: return 0x27;
            case ImGuiKey_Apostrophe: return 0x28;
            case ImGuiKey_GraveAccent: return 0x29;
            case ImGuiKey_LeftShift: return 0x2A;
            case ImGuiKey_Backslash: return 0x2B;
            case ImGuiKey_Z: return 0x2C;
            case ImGuiKey_X: return 0x2D;
            case ImGuiKey_C: return 0x2E;
            case ImGuiKey_V: return 0x2F;
            case ImGuiKey_B: return 0x30;
            case ImGuiKey_N: return 0x31;
            case ImGuiKey_M: return 0x32;
            case ImGuiKey_Comma: return 0x33;
            case ImGuiKey_Period: return 0x34;
            case ImGuiKey_Slash: return 0x35;
            case ImGuiKey_RightShift: return 0x36;
            case ImGuiKey_KeypadMultiply: return 0x37;
            case ImGuiKey_LeftAlt: return 0x38;
            case ImGuiKey_Space: return 0x39;
            case ImGuiKey_CapsLock: return 0x3A;
            case ImGuiKey_F1: return 0x3B;
            case ImGuiKey_F2: return 0x3C;
            case ImGuiKey_F3: return 0x3D;
            case ImGuiKey_F4: return 0x3E;
            case ImGuiKey_F5: return 0x3F;
            case ImGuiKey_F6: return 0x40;
            case ImGuiKey_F7: return 0x41;
            case ImGuiKey_F8: return 0x42;
            case ImGuiKey_F9: return 0x43;
            case ImGuiKey_F10: return 0x44;
            case ImGuiKey_NumLock: return 0x45;
            case ImGuiKey_ScrollLock: return 0x46;
            case ImGuiKey_Keypad7: return 0x47;
            case ImGuiKey_Keypad8: return 0x48;
            case ImGuiKey_Keypad9: return 0x49;
            case ImGuiKey_KeypadSubtract: return 0x4A;
            case ImGuiKey_Keypad4: return 0x4B;
            case ImGuiKey_Keypad5: return 0x4C;
            case ImGuiKey_Keypad6: return 0x4D;
            case ImGuiKey_KeypadAdd: return 0x4E;
            case ImGuiKey_Keypad1: return 0x4F;
            case ImGuiKey_Keypad2: return 0x50;
            case ImGuiKey_Keypad3: return 0x51;
            case ImGuiKey_Keypad0: return 0x52;
            case ImGuiKey_KeypadDecimal: return 0x53;
            case ImGuiKey_F11: return 0x57;
            case ImGuiKey_F12: return 0x58;
            case ImGuiKey_KeypadEnter: return 0x9C;
            case ImGuiKey_RightCtrl: return 0x9D;
            case ImGuiKey_KeypadDivide: return 0xB5;
            case ImGuiKey_RightAlt: return 0xB8;
            case ImGuiKey_Home: return 0xC7;
            case ImGuiKey_UpArrow: return 0xC8;
            case ImGuiKey_PageUp: return 0xC9;
            case ImGuiKey_LeftArrow: return 0xCB;
            case ImGuiKey_RightArrow: return 0xCD;
            case ImGuiKey_End: return 0xCF;
            case ImGuiKey_DownArrow: return 0xD0;
            case ImGuiKey_PageDown: return 0xD1;
            case ImGuiKey_Insert: return 0xD2;
            case ImGuiKey_Delete: return 0xD3;
            default: return 0;
            }
        }

        std::string GetDirectInputKeyName(std::uint32_t a_dik)
        {
            switch (a_dik) {
            case 0x01: return "Escape";
            case 0x02: return "1";
            case 0x03: return "2";
            case 0x04: return "3";
            case 0x05: return "4";
            case 0x06: return "5";
            case 0x07: return "6";
            case 0x08: return "7";
            case 0x09: return "8";
            case 0x0A: return "9";
            case 0x0B: return "0";
            case 0x0C: return "Minus (-)";
            case 0x0D: return "Equals (=)";
            case 0x0E: return "Backspace";
            case 0x0F: return "Tab";
            case 0x10: return "Q";
            case 0x11: return "W";
            case 0x12: return "E";
            case 0x13: return "R";
            case 0x14: return "T";
            case 0x15: return "Y";
            case 0x16: return "U";
            case 0x17: return "I";
            case 0x18: return "O";
            case 0x19: return "P";
            case 0x1A: return "[";
            case 0x1B: return "]";
            case 0x1C: return "Enter";
            case 0x1D: return "Left Ctrl";
            case 0x1E: return "A";
            case 0x1F: return "S";
            case 0x20: return "D";
            case 0x21: return "F";
            case 0x22: return "G";
            case 0x23: return "H";
            case 0x24: return "J";
            case 0x25: return "K";
            case 0x26: return "L";
            case 0x27: return ";";
            case 0x28: return "'";
            case 0x29: return "` (Tilde)";
            case 0x2A: return "Left Shift";
            case 0x2B: return "\\";
            case 0x2C: return "Z";
            case 0x2D: return "X";
            case 0x2E: return "C";
            case 0x2F: return "V";
            case 0x30: return "B";
            case 0x31: return "N";
            case 0x32: return "M";
            case 0x33: return ",";
            case 0x34: return ".";
            case 0x35: return "/";
            case 0x36: return "Right Shift";
            case 0x37: return "Num *";
            case 0x38: return "Left Alt";
            case 0x39: return "Space";
            case 0x3A: return "Caps Lock";
            case 0x3B: return "F1";
            case 0x3C: return "F2";
            case 0x3D: return "F3";
            case 0x3E: return "F4";
            case 0x3F: return "F5";
            case 0x40: return "F6";
            case 0x41: return "F7";
            case 0x42: return "F8";
            case 0x43: return "F9";
            case 0x44: return "F10";
            case 0x45: return "Num Lock";
            case 0x46: return "Scroll Lock";
            case 0x47: return "Num 7";
            case 0x48: return "Num 8";
            case 0x49: return "Num 9";
            case 0x4A: return "Num -";
            case 0x4B: return "Num 4";
            case 0x4C: return "Num 5";
            case 0x4D: return "Num 6";
            case 0x4E: return "Num +";
            case 0x4F: return "Num 1";
            case 0x50: return "Num 2";
            case 0x51: return "Num 3";
            case 0x52: return "Num 0";
            case 0x53: return "Num .";
            case 0x57: return "F11";
            case 0x58: return "F12";
            case 0x9C: return "Num Enter";
            case 0x9D: return "Right Ctrl";
            case 0xB5: return "Num /";
            case 0xB8: return "Right Alt";
            case 0xC7: return "Home";
            case 0xC8: return "Up Arrow";
            case 0xC9: return "Page Up";
            case 0xCB: return "Left Arrow";
            case 0xCD: return "Right Arrow";
            case 0xCF: return "End";
            case 0xD0: return "Down Arrow";
            case 0xD1: return "Page Down";
            case 0xD2: return "Insert";
            case 0xD3: return "Delete";
            default: {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "Key 0x%02X", a_dik);
                return buf;
            }
            }
        }

        struct KeyChoice
        {
            const char* key;
            std::uint32_t dik;
        };

        const KeyChoice kPopularKeys[] = {
            { "$STM_Key_F10Default", 0x44 },
            { "$STM_Key_F9", 0x43 },
            { "$STM_Key_F8", 0x42 },
            { "$STM_Key_F7", 0x41 },
            { "$STM_Key_F6", 0x40 },
            { "$STM_Key_F5", 0x3F },
            { "$STM_Key_F4", 0x3E },
            { "$STM_Key_F3", 0x3D },
            { "$STM_Key_F2", 0x3C },
            { "$STM_Key_F1", 0x3B },
            { "$STM_Key_F11", 0x57 },
            { "$STM_Key_F12", 0x58 },
            { "$STM_Key_Insert", 0xD2 },
            { "$STM_Key_Delete", 0xD3 },
            { "$STM_Key_Home", 0xC7 },
            { "$STM_Key_End", 0xCF },
            { "$STM_Key_PageUp", 0xC9 },
            { "$STM_Key_PageDown", 0xD1 },
            { "$STM_Key_Tilde", 0x29 },
            { "$STM_Key_Num0", 0x52 },
            { "$STM_Key_Num1", 0x4F },
            { "$STM_Key_Num2", 0x50 },
            { "$STM_Key_Num3", 0x51 },
            { "$STM_Key_Num4", 0x4B },
            { "$STM_Key_Num5", 0x4C },
            { "$STM_Key_Num6", 0x4D },
            { "$STM_Key_Num7", 0x47 },
            { "$STM_Key_Num8", 0x48 },
            { "$STM_Key_Num9", 0x49 },
            { "$STM_Key_NumMul", 0x37 },
            { "$STM_Key_NumSub", 0x4A },
            { "$STM_Key_NumAdd", 0x4E },
            { "$STM_Key_NumDiv", 0xB5 },
            { "$STM_Key_NumEnter", 0x9C },
        };

        static bool        s_forceResizeWindow = false;
        static bool        s_bindingHotkey = false;
        static bool        s_bindingJustStarted = false;
        static ImVec2      s_lastFloatingWindowSize{ 1280.0f, 720.0f };
        static float       s_w = 0.0f;
        static float       s_h = 0.0f;
        static std::string s_settingsStatus;
        static double      s_settingsStatusTime = 0.0;

        void DrawSettings()
        {
            if (!OnGameThread()) {
                return;
            }

            FontScaleScope fontScope(Config::Get().fontScale);
            auto&          config = Config::Get();

            auto SetStatus = [&](const char* a_msg) {
                s_settingsStatus = a_msg;
                s_settingsStatusTime = NowSeconds();
            };

            ImGui::TextDisabled("%s", Tr("$STM_Settings_BannerText"));
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 1. 界面与显示 (Interface & Display)
            if (ImGui::CollapsingHeader(Tr("$STM_Settings_InterfaceDisplay"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();

                // 语言 (选择框/下拉选项)
                ImGui::Text("%s:", Tr("$STM_Settings_InterfaceLanguage"));
                ImGui::SameLine();

                const auto availableLangs = Translation::Get().GetAvailableLanguages();
                // The loaded language, not the raw INI value: an INI pointing at the
                // template (or any invalid code) is coerced during Load(), and the
                // combo has to agree with the strings actually being displayed.
                const auto currentNorm = Translation::Get().GetCurrentLanguage();
                std::string currentDisplay = Translation::GetLanguageDisplayName(currentNorm);

                ImGui::SetNextItemWidth(220.0f);
                if (ImGui::BeginCombo("##language_select", currentDisplay.c_str())) {
                    for (const auto& lang : availableLangs) {
                        const bool isSelected = (lang == currentNorm);
                        std::string label = Translation::GetLanguageDisplayName(lang);
                        if (ImGui::Selectable(label.c_str(), isSelected)) {
                            if (lang != currentNorm) {
                                config.language = (lang == "CHINESE") ? "zh" : (lang == "ENGLISH" ? "en" : lang);
                                Translation::Get().Load(config.language);
                                config.Save();
                                SetStatus(Tr("$STM_Status_LanguageUpdated"));
                            }
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::Spacing();

                // 文字大小
                float fScale = config.fontScale;
                ImGui::PushItemWidth(260.0f);
                if (ImGui::SliderFloat(Tr("$STM_Settings_FontScaleSlider"), &fScale, 0.75f, 1.50f, "%.2f x")) {
                    config.fontScale = fScale;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    config.Save();
                }
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button(Tr("$STM_Settings_ResetFontBtn"), ButtonSize(Tr("$STM_Settings_ResetFontLabel")))) {
                    config.fontScale = 1.0f;
                    config.Save();
                    SetStatus(Tr("$STM_Status_FontReset"));
                }

                ImGui::Spacing();

                // 窗口背景不透明度
                float winAlpha = config.windowAlpha;
                ImGui::PushItemWidth(260.0f);
                if (ImGui::SliderFloat(Tr("$STM_Settings_WindowAlphaSlider"), &winAlpha, 0.20f, 1.0f, "%.2f")) {
                    config.windowAlpha = winAlpha;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    config.Save();
                }
                ImGui::PopItemWidth();
                ImGui::TextDisabled("%s", Tr("$STM_Settings_WindowAlphaNote"));
            }

            ImGui::Spacing();

            // 2. 默认窗口大小设置 (Default Menu Size)
            if (ImGui::CollapsingHeader(Tr("$STM_Settings_WindowSizeHeader"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();

                if (s_w <= 0.0f) s_w = config.windowWidth;
                if (s_h <= 0.0f) s_h = config.windowHeight;

                ImGui::Text("%s:", Tr("$STM_QuickPresets"));
                ImGui::SameLine();

                struct SizePreset { const char* label; float w; float h; };
                const SizePreset presets[] = {
                    { "1024x600", 1024.0f, 600.0f },
                    { "1280x720", 1280.0f, 720.0f },
                    { "1440x810", 1440.0f, 810.0f },
                    { "1600x900", 1600.0f, 900.0f },
                    { "1920x1080", 1920.0f, 1080.0f }
                };

                for (int i = 0; i < 5; ++i) {
                    if (i > 0) ImGui::SameLine();
                    char pbuf[32];
                    std::snprintf(pbuf, sizeof(pbuf), "%s##preset_%d", presets[i].label, i);
                    if (ImGui::Button(pbuf, ButtonSize(presets[i].label))) {
                        s_w = presets[i].w;
                        s_h = presets[i].h;
                        config.windowWidth = s_w;
                        config.windowHeight = s_h;
                        config.Save();
                        s_forceResizeWindow = true;
                        SetStatus(Tr("$STM_Status_PresetSizeApplied"));
                    }
                }

                ImGui::Spacing();

                ImGui::PushItemWidth(260.0f);
                if (ImGui::SliderFloat(Tr("$STM_DefaultWidth"), &s_w, 800.0f, 2560.0f, "%.0f px")) {
                    config.windowWidth = std::round(s_w);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    config.Save();
                }
                if (ImGui::SliderFloat(Tr("$STM_DefaultHeight"), &s_h, 500.0f, 1440.0f, "%.0f px")) {
                    config.windowHeight = std::round(s_h);
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    config.Save();
                }
                ImGui::PopItemWidth();

                ImGui::Spacing();

                if (ImGui::Button(Tr("$STM_ApplyResizeWindow_2"), ButtonSize(Tr("$STM_ApplyResizeWindow")))) {
                    config.windowWidth = std::round(s_w);
                    config.windowHeight = std::round(s_h);
                    config.Save();
                    s_forceResizeWindow = true;
                    SetStatus(Tr("$STM_Status_WindowSizeApplied"));
                }

                if (s_lastFloatingWindowSize.x >= 600.0f && s_lastFloatingWindowSize.y >= 400.0f) {
                    ImGui::SameLine();
                    char grabLabel[64];
                    std::snprintf(grabLabel, sizeof(grabLabel), "%s (%.0fx%.0f)##grab_sz",
                                  Tr("$STM_GrabCurrentWindowSize"),
                                  s_lastFloatingWindowSize.x, s_lastFloatingWindowSize.y);
                    if (ImGui::Button(grabLabel, ButtonSize(grabLabel))) {
                        s_w = std::round(s_lastFloatingWindowSize.x);
                        s_h = std::round(s_lastFloatingWindowSize.y);
                        config.windowWidth = s_w;
                        config.windowHeight = s_h;
                        config.Save();
                        SetStatus(Tr("$STM_CurrentWindowSizeSaved"));
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", Tr("$STM_SaveTheCurrentFloating"));
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button(Tr("$STM_ResetTo1280x720_2"), ButtonSize(Tr("$STM_ResetTo1280x720")))) {
                    s_w = 1280.0f;
                    s_h = 720.0f;
                    config.windowWidth = 1280.0f;
                    config.windowHeight = 720.0f;
                    config.Save();
                    s_forceResizeWindow = true;
                    SetStatus(Tr("$STM_Status_ResetDefaultSize"));
                }

                ImGui::TextDisabled("%s", Tr("$STM_NoteTakesEffectOn"));
                ImGui::Spacing();
            }

            ImGui::Spacing();

            // 3. 纹身预览与槽位策略 (Preview & Slot Policy)
            if (ImGui::CollapsingHeader(Tr("$STM_PreviewSlotPolicy"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();

                // 槽位冲突策略
                ImGui::Text("%s:", Tr("$STM_SlotConflictPolicy"));
                ImGui::SameLine();
                int policyChoice = (config.slotPolicy == SlotPolicy::kOverwrite) ? 1 : 0;
                bool policyChanged = false;
                if (ImGui::RadioButton(Tr("$STM_AutoAllocateToFirst"), &policyChoice, 0)) {
                    policyChanged = true;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton(Tr("$STM_OverwriteSelectedSlot"), &policyChoice, 1)) {
                    policyChanged = true;
                }
                if (policyChanged) {
                    config.slotPolicy = (policyChoice == 1) ? SlotPolicy::kOverwrite : SlotPolicy::kAutoFreeSlot;
                    config.Save();
                    SetStatus(Tr("$STM_SlotPolicyUpdated"));
                }
                ImGui::TextDisabled("%s", Tr("$STM_NoteChooseWhetherTo"));

                ImGui::Spacing();

                // 图案预览
                bool showPrev = config.showPreview;
                if (ImGui::Checkbox(Tr("$STM_ShowTattooTexturePreview"), &showPrev)) {
                    config.showPreview = showPrev;
                    config.Save();
                    SetStatus(Tr("$STM_PreviewToggleUpdated"));
                }

                if (config.showPreview) {
                    ImGui::SameLine();
                    ImGui::Text("%s:", Tr("$STM_PreviewSizes"));
                    const float sizes[] = { 64.0f, 96.0f, 128.0f, 192.0f };
                    for (int i = 0; i < 4; ++i) {
                        ImGui::SameLine();
                        char sbuf[32];
                        std::snprintf(sbuf, sizeof(sbuf), "%.0fpx##prev_sz_%d", sizes[i], i);
                        if (ImGui::Button(sbuf)) {
                            config.previewSize = sizes[i];
                            config.Save();
                            SetStatus(Tr("$STM_Status_PreviewSizeUpdated"));
                        }
                    }

                    ImGui::PushItemWidth(260.0f);
                    float prevSz = config.previewSize;
                    if (ImGui::SliderFloat(Tr("$STM_PreviewDimension"), &prevSz, 64.0f, 256.0f, "%.0f px")) {
                        config.previewSize = prevSz;
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        config.Save();
                    }
                    ImGui::PopItemWidth();
                }

                ImGui::Spacing();
            }

            ImGui::Spacing();

            // 4. 预设管理与实用工具 (Presets & Utilities)
            if (ImGui::CollapsingHeader(Tr("$STM_Settings_PresetsManagement"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();

                const char* openExplorerLabel = Tr("$STM_Settings_OpenPresetsDirBtn");
                if (ImGui::Button(openExplorerLabel, ButtonSize(openExplorerLabel, 260.0f, 28.0f))) {
                    const auto dir = data::PresetStore::Get().Directory();
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    SetStatus(Tr("$STM_Status_OpenedPresetsDir"));
                }
                ImGui::TextDisabled("%s", Tr("$STM_Settings_OpenPresetsDirNote"));

                ImGui::Spacing();
            }

            ImGui::Spacing();

            // 5. 快捷键设置 (Hotkey)
            if (ImGui::CollapsingHeader(Tr("$STM_Settings_HotkeyHeader"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();
                ImGui::Text("%s:", Tr("$STM_Settings_FloatingHotkeyLabel"));
                ImGui::SameLine();

                const auto currentDik = config.toggleKey;
                const auto keyName = GetDirectInputKeyName(currentDik);

                if (s_bindingHotkey) {
                    if (s_bindingJustStarted) {
                        s_bindingJustStarted = false;
                    } else {
                        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
                            const auto imguiKey = static_cast<ImGuiKey>(k);
                            if (ImGui::IsKeyPressed(imguiKey, false)) {
                                if (imguiKey == ImGuiKey_Escape) {
                                    s_bindingHotkey = false;
                                    break;
                                }
                                const auto dik = ImGuiKeyToDirectInput(imguiKey);
                                if (dik != 0) {
                                    config.toggleKey = dik;
                                    config.Save();
                                    s_bindingHotkey = false;
                                    SetStatus(Tr("$STM_Status_HotkeyUpdated"));
                                    break;
                                }
                            }
                        }
                    }

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.1f, 1.0f));
                    ImGui::Button(Tr("$STM_Settings_PressAnyKeyBtn"), ButtonSize("", 240.0f));
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (ImGui::Button(Tr("$STM_Settings_BindCancelBtn"), ButtonSize(Tr("$STM_Common_Cancel")))) {
                        s_bindingHotkey = false;
                    }
                } else {
                    char btnLabel[64];
                    std::snprintf(btnLabel, sizeof(btnLabel), "%s (0x%02X)##bind_btn", keyName.c_str(), currentDik);
                    if (ImGui::Button(btnLabel, ButtonSize(btnLabel, 180.0f))) {
                        s_bindingHotkey = true;
                        s_bindingJustStarted = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", Tr("$STM_Settings_RecordKeyTooltip"));
                    }

                    ImGui::SameLine();
                    if (ImGui::Button(Tr("$STM_Settings_QuickSelectKeyBtn"), ButtonSize(Tr("$STM_Settings_QuickSelectKeyLabel")))) {
                        ImGui::OpenPopup("QuickKeyPopup");
                    }

                    if (ImGui::BeginPopup("QuickKeyPopup")) {
                        for (const auto& choice : kPopularKeys) {
                            const bool isSelected = (config.toggleKey == choice.dik);
                            if (ImGui::Selectable(Tr(choice.key), isSelected)) {
                                config.toggleKey = choice.dik;
                                config.Save();
                                SetStatus(Tr("$STM_Status_HotkeyChanged"));
                            }
                        }
                        ImGui::EndPopup();
                    }

                    ImGui::SameLine();
                    if (ImGui::Button(Tr("$STM_Settings_ResetHotkeyBtn"), ButtonSize(Tr("$STM_Settings_ResetHotkeyLabel")))) {
                        config.toggleKey = 0x44;
                        config.Save();
                        SetStatus(Tr("$STM_Status_HotkeyResetF10"));
                    }
                }

                ImGui::TextDisabled("%s", Tr("$STM_Settings_HotkeyNote"));
                ImGui::Spacing();
            }

            ImGui::Spacing();

            // 6. 其他常规设置 (Other General Settings)
            if (ImGui::CollapsingHeader(Tr("$STM_Settings_OtherHeader"))) {
                ImGui::Spacing();

                float inkColor[3] = {
                    static_cast<float>((config.defaultColor >> 16) & 0xFF) / 255.0f,
                    static_cast<float>((config.defaultColor >> 8) & 0xFF) / 255.0f,
                    static_cast<float>(config.defaultColor & 0xFF) / 255.0f
                };
                if (ImGui::ColorEdit3(Tr("$STM_Settings_DefaultInkColor"), inkColor)) {
                    config.defaultColor = PackColor(inkColor);
                    config.Save();
                }

                if (ImGui::SliderFloat(Tr("$STM_DefaultTattooOpacity"), &config.defaultAlpha, 0.05f, 1.0f, "%.2f")) {
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    config.Save();
                }

                if (ImGui::SliderInt(Tr("$STM_AppliedSlotsRefreshInterval"), &config.slotRefreshMs, 50, 1000, "%d ms")) {
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    config.Save();
                }

                ImGui::Spacing();
                if (ImGui::Button(Tr("$STM_Settings_RestoreAllBtn"), ButtonSize(Tr("$STM_Settings_RestoreAllLabel")))) {
                    config.toggleKey = 0x44;
                    // 保留当前界面语言，不强制覆盖
                    config.windowWidth = 1280.0f;
                    config.windowHeight = 720.0f;
                    config.windowAlpha = 0.95f;
                    config.fontScale = 1.0f;
                    config.showPreview = true;
                    config.previewSize = 96.0f;
                    config.mannequinColor = MannequinColor::kFlesh;
                    config.slotPolicy = SlotPolicy::kAutoFreeSlot;
                    config.defaultAlpha = 1.0f;
                    config.defaultColor = 0x000000;
                    config.slotRefreshMs = 250;
                    config.probeEnabled = false;
                    config.probeIntervalMs = 1000;
                    config.probeArea = "Body";
                    config.probeSlots = 12;
                    config.callTraceEnabled = false;
                    s_w = 1280.0f;
                    s_h = 720.0f;
                    config.Save();
                    s_forceResizeWindow = true;
                    SetStatus(Tr("$STM_Status_AllDefaultsRestored"));
                }
                ImGui::Spacing();
            }

            // 状态提示条
            if (!s_settingsStatus.empty() && (NowSeconds() - s_settingsStatusTime) < 8.0) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[OK] %s", s_settingsStatus.c_str());
            }
        }

        bool IsTextInputKey(ImGuiKey a_key)
        {
            return (a_key >= ImGuiKey_0 && a_key <= ImGuiKey_9) ||
                   (a_key >= ImGuiKey_A && a_key <= ImGuiKey_Z) ||
                   (a_key >= ImGuiKey_Keypad0 && a_key <= ImGuiKey_Keypad9) ||
                   (a_key >= ImGuiKey_Apostrophe && a_key <= ImGuiKey_GraveAccent) ||
                   a_key == ImGuiKey_Space;
        }
    }

    bool IsBindingHotkey()
    {
        return s_bindingHotkey && IsWindowOpen();
    }

    void ToggleWindow()
    {
        if (g_window) {
            g_window->IsOpen = !g_window->IsOpen;
            if (!g_window->IsOpen) {
                s_bindingHotkey = false;
            }
        }
    }

    bool IsWindowOpen()
    {
        return g_window && g_window->IsOpen;
    }

    void Initialize()
    {
        const float frameworkVersion = SKSEMenuFramework::GetMenuFrameworkVersion();

        SKSEMenuFramework::SetSection("SlaveTats");
        SKSEMenuFramework::AddSectionItem(Tr("$STM_Menu_TattooManager"), RenderMenu);
        SKSEMenuFramework::AddSectionItem(Tr("$STM_Menu_Settings"), RenderSettings);
        g_window = SKSEMenuFramework::AddWindow(RenderWindow);

        if (!g_window) {
            SKSE::log::error("UI: the menu framework did not provide a window (framework {:.2f}) - the hotkey will do nothing", frameworkVersion);
        } else {
            SKSE::log::info("UI: window registered (framework {:.2f})", frameworkVersion);
        }
    }

    void __stdcall RenderMenu()
    {
        PumpResults();
        DrawMain();
    }

    void __stdcall RenderSettings()
    {
        PumpResults();
        DrawSettings();
    }

    void __stdcall RenderWindow()
    {
        static bool s_wasOpen = false;
        static bool s_canCloseWithToggleKey = false;

        if (!IsWindowOpen()) {
            s_wasOpen = false;
            s_canCloseWithToggleKey = false;
            return;
        }

        // Drain before any tab is drawn, so a result produced while the Settings
        // tab was active is not stuck in the board until the user switches back.
        PumpResults();

        const bool justOpened = !s_wasOpen;
        s_wasOpen = true;

        const auto toggleKey = Config::Get().toggleKey;
        const auto toggleImKey = DirectInputToImGuiKey(toggleKey);

        if (justOpened) {
            s_canCloseWithToggleKey = false;
            if (!UiState::Get().hasSelection) {
                render::MannequinRenderer::Get().SetCurrentPart(render::PreviewPart::kBody);
            }
        } else if (toggleImKey != ImGuiKey_None && !ImGui::IsKeyDown(toggleImKey)) {
            s_canCloseWithToggleKey = true;
        }

        const bool isTyping = ImGui::GetIO() && ImGui::GetIO()->WantTextInput;

        // Toggle hotkey closes the window once released and pressed again
        if (!s_bindingHotkey && toggleImKey != ImGuiKey_None && toggleImKey != ImGuiKey_Escape &&
            s_canCloseWithToggleKey && ImGui::IsKeyPressed(toggleImKey, false)) {
            if (!isTyping || !IsTextInputKey(toggleImKey)) {
                g_window->IsOpen = false;
                s_wasOpen = false;
                s_canCloseWithToggleKey = false;
                s_bindingHotkey = false;
                return;
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (s_bindingHotkey) {
                s_bindingHotkey = false;
            } else {
                const bool hasPopup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);

                if (hasPopup) {
                    // Handled by ImGui to close the popup/combo first.
                } else if (isTyping) {
                    // Defocus active text input so subsequent ESC closes the window.
                    ImGui::ClearActiveID();
                    ImGui::SetWindowFocus();
                } else {
                    g_window->IsOpen = false;
                    s_wasOpen = false;
                    s_canCloseWithToggleKey = false;
                    s_bindingHotkey = false;
                    return;
                }
            }
        }

        if (auto* viewport = ImGui::GetMainViewport()) {
            const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
            ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        }

        if (s_forceResizeWindow) {
            ImGui::SetNextWindowSize(ImVec2(Config::Get().windowWidth, Config::Get().windowHeight), ImGuiCond_Always);
            s_forceResizeWindow = false;
        } else {
            ImGui::SetNextWindowSize(ImVec2(Config::Get().windowWidth, Config::Get().windowHeight), ImGuiCond_FirstUseEver);
        }

        ImGui::SetNextWindowBgAlpha(Config::Get().windowAlpha);

        bool open = true;
        if (ImGui::Begin(Tr("$STM_Title"), &open, ImGuiWindowFlags_NoCollapse)) {
            FontScaleScope fontScope(Config::Get().fontScale);
            s_lastFloatingWindowSize = ImGui::GetWindowSize();
            if (ImGui::BeginTabBar("##stm_tabs")) {
                if (ImGui::BeginTabItem(Tr("$STM_TattooManager"))) {
                    DrawMain();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(Tr("$STM_Menu_Settings"))) {
                    DrawSettings();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();

        if (!open) {
            g_window->IsOpen = false;
            s_wasOpen = false;
            s_canCloseWithToggleKey = false;
            s_bindingHotkey = false;
        }
    }
}
