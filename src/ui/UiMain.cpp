#include "ui/Ui.h"

#include "bridge/JcApi.h"
#include "bridge/KnownTargets.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Tasks.h"
#include "data/SlotSnapshot.h"
#include "data/TargetState.h"
#include "data/PresetStore.h"
#include "data/TattooDatabase.h"
#include "diag/Probe.h"
#include "ops/PresetOps.h"
#include "ops/TattooOps.h"
#include "ui/UiState.h"

namespace stm::ui
{
    namespace
    {
        MENU_WINDOW g_window = nullptr;

        const char* kAreaIds[] = { "", "Body", "Face", "Hands", "Feet" };

        const char* L(const char* a_zh, const char* a_en)
        {
            return Config::Get().IsChinese() ? a_zh : a_en;
        }

        double NowSeconds()
        {
            static const auto s_start = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
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

            if (state.previewTexture) {
                SKSEMenuFramework::DisposeTexture(state.previewPath);
                state.previewTexture = nullptr;
            }

            state.previewPath = a_rawTexture;
            if (state.previewPath.empty()) {
                return;
            }

            std::string path = state.previewPath;
            std::replace(path.begin(), path.end(), '\\', '/');
            if (path.rfind("Textures/", 0) != 0 && path.rfind("textures/", 0) != 0) {
                path = "Textures/Actors/Character/slavetats/" + path;
            }

            state.previewTexture = SKSEMenuFramework::LoadTexture(path, state.previewSize);
            if (!state.previewTexture) {
                // Not fatal: older framework builds have no LoadTexture export.
                SKSE::log::warn("UI: preview for '{}' unavailable (missing file or framework has no LoadTexture)", path);
            }
        }

        // -------------------------------------------------------- helpers

        void SeedEditorFrom(const data::TattooEntry& a_entry)
        {
            auto& state = UiState::Get();

            state.selected = a_entry;
            state.hasSelection = true;
            state.targetSlot = -1;

            const std::uint32_t rgb = a_entry.defaultColor;
            state.color[0] = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
            state.color[1] = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
            state.color[2] = static_cast<float>(rgb & 0xFF) / 255.0f;
            state.alpha = a_entry.defaultAlpha;

            UpdatePreview(a_entry.texture);
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

            ImGui::Text("%s", L("当前目标:", "Target:"));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "[ %s ]", target.DisplayName().c_str());

            ImGui::SameLine();
            if (ImGui::SmallButton(L("拾取准星 NPC##pick_target", "Pick crosshair NPC##pick_target"))) {
                if (target.PickFromCrosshair()) {
                    state.SetStatus(true, std::string(L("已选定目标: ", "target: ")) + target.DisplayName(), NowSeconds());
                } else {
                    state.SetStatus(false, L("准星没有对准任何角色", "the crosshair is not on an actor"), NowSeconds());
                }
                data::SlotSnapshot::Get().Invalidate();
            }

            ImGui::SameLine();
            if (ImGui::SmallButton(L("设为主角##reset_target", "Reset to player##reset_target"))) {
                target.ResetToPlayer();
                state.SetStatus(true, L("已切换为主角自己", "target reset to the player"), NowSeconds());
                data::SlotSnapshot::Get().Invalidate();
            }

            // The two library buttons sit at the right edge of the row. Their
            // labels are measured rather than guessed: "重新扫描纹身库" needs far
            // more than the 144 px the old fixed box offered.
            const char*  rescanLabel = L("重新扫描纹身库##rescan", "Rescan library##rescan");
            const char*  syncLabel = L("强制同步##sync", "Force sync##sync");
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
                state.SetStatus(true, std::string(L("已重建纹身库: ", "library rebuilt: ")) + std::to_string(db.Size()) + " (" + db.SourceName() + ")", NowSeconds());
            }

            ImGui::SameLine();
            if (ImGui::Button(syncLabel, syncSize)) {
                ops::Sync(target.Handle());
            }
        }

        void DrawAreaRow()
        {
            auto& state = UiState::Get();

            ImGui::Text("%s", L("部位:", "Area:"));
            ImGui::SameLine();

            for (int i = 0; i < 5; ++i) {
                if (i > 0) {
                    ImGui::SameLine();
                }

                static const char* kZh[] = { "全部", "身体", "面部", "双手", "双脚" };
                static const char* kEn[] = { "All", "Body", "Face", "Hands", "Feet" };

                if (ImGui::RadioButton(L(kZh[i], kEn[i]), state.areaIndex == i)) {
                    state.areaIndex = i;
                    state.hasSelection = false;
                    UpdatePreview("");
                    data::SlotSnapshot::Get().Invalidate();
                }
            }
        }

        void DrawSectionPanel(float a_width, float a_height)
        {
            auto& state = UiState::Get();
            auto& db = data::TattooDatabase::Get();

            ImGui::BeginChild("section_panel", ImVec2(a_width, a_height), true);
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s", L("1. 模组 / 分类", "1. Mods / Sections"));
            ImGui::Separator();

            if (!state.sectionChosen) {
                ImGui::TextDisabled("%s", L("选一个模组后才会出现纹身列表", "pick a mod to reveal the tattoo list"));
            }

            // The clear button is as wide as its (large) glyph needs; the search
            // box takes whatever is left so the two never overlap.
            const float clearModWidth = ButtonWidth("x##clear_mod", 24.0f);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - clearModWidth - ImGui::GetStyle()->ItemSpacing.x - 2.0f);
            ImGui::InputTextWithHint("##mod_search", L("搜索模组...", "search mods..."), state.modSearch, sizeof(state.modSearch));
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
            if (ImGui::Selectable(L("[全部模组]##all_mods", "[All mods]##all_mods"), allSelected)) {
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
                ? std::string(L("2. 纹身列表", "2. Tattoos")) + " (" + std::to_string(tattoos.size()) + ")"
                : std::string("2. [") + state.sectionFilter + "] (" + std::to_string(tattoos.size()) + ")";

            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.7f, 1.0f), "%s", title.c_str());
            ImGui::Separator();

            const float clearTatWidth = ButtonWidth("x##clear_tat", 24.0f);
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - clearTatWidth - ImGui::GetStyle()->ItemSpacing.x - 2.0f);
            if (ImGui::InputTextWithHint("##tat_search", L("搜索纹身名称...", "search tattoos..."), state.tattooSearch, sizeof(state.tattooSearch))) {
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
                ImGui::TextDisabled("%s", L("当前筛选下没有纹身。", "no tattoos match the current filter."));
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
                ImGui::TextDisabled("%s", L("正在读取槽位...", "reading slots..."));
                return;
            }

            for (const auto& slot : snapshot.slots) {
                if (!slot.occupied) {
                    std::string label = std::string(L("槽位 ", "slot ")) + std::to_string(slot.slot) + ": " + L("[空闲]", "[empty]");
                    ImGui::TextDisabled("%s", label.c_str());
                    continue;
                }

                std::string label = std::string(L("槽位 ", "slot ")) + std::to_string(slot.slot) + ": ";
                if (slot.external) {
                    label += L("[外部占用] ", "[external] ");
                } else if (!slot.section.empty()) {
                    label += "[" + slot.section + "] ";
                }
                label += slot.name;

                ImGui::TextColored(slot.external ? ImVec4(0.9f, 0.7f, 0.3f, 1.0f) : ImVec4(0.3f, 0.9f, 0.6f, 1.0f), "%s", label.c_str());

                ImGui::SameLine();
                const std::string clearId = std::string("x##slot_") + std::to_string(slot.slot);
                if (ImGui::SmallButton(clearId.c_str())) {
                    ops::RemoveSlot(data::TargetState::Get().Handle(), a_area, slot.slot);
                }

                if (!slot.external) {
                    ImGui::SameLine();
                    const std::string editId = std::string(L("编辑##editslot_", "edit##editslot_")) + std::to_string(slot.slot);
                    if (ImGui::SmallButton(editId.c_str())) {
                        // Adopt this slot's current look into the editor.
                        float alpha = 1.0f;
                        std::uint32_t color = 0;
                        if (ops::ReadSlotAppearance(data::TargetState::Get().Resolve(), a_area, slot.slot, color, alpha)) {
                            state.color[0] = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
                            state.color[1] = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
                            state.color[2] = static_cast<float>(color & 0xFF) / 255.0f;
                            state.alpha = alpha;
                            state.selected.name = slot.name;
                            state.selected.section = slot.section;
                            state.selected.area = a_area;
                            state.selected.texture.clear();
                            state.hasSelection = true;
                            state.targetSlot = slot.slot;
                            UpdatePreview("");
                            state.SetStatus(true, std::string(L("已载入槽位 ", "loaded slot ")) + std::to_string(slot.slot), NowSeconds());
                        } else {
                            state.SetStatus(false, L("无法读取该槽位的属性", "could not read that slot"), NowSeconds());
                        }
                    }
                }
            }
        }

        void DrawInspector(float a_width, float a_height)
        {
            auto& state = UiState::Get();
            auto& db = data::TattooDatabase::Get();

            ImGui::BeginChild("inspector_panel", ImVec2(a_width, a_height), true);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", L("3. 属性与应用", "3. Properties & Actions"));
            ImGui::Separator();

            if (state.hasSelection) {
                ImGui::Text("%s: %s", L("纹身", "tattoo"), state.selected.name.c_str());
                ImGui::Text("%s: %s", L("模组", "mod"), state.selected.section.c_str());
                ImGui::Text("%s: %s", L("部位", "area"), state.selected.area.c_str());

                if (!state.selected.texture.empty()) {
                    ImGui::TextDisabled("%s: %s", L("贴图", "texture"), state.selected.texture.c_str());
                }

                if (state.previewTexture) {
                    ImGui::Spacing();
                    ImGui::Text("%s:", L("图案预览", "preview"));
                    ImGui::SameLine();
                    ImGui::Image(state.previewTexture, ImVec2(96.0f, 96.0f), ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, 1), ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                }

                ImGui::Spacing();
                ImGui::ColorEdit3(L("颜色", "colour"), state.color);

                ImGui::Text("%s:", L("快速调色", "quick colours"));
                ImGui::SameLine();
                if (ImGui::SmallButton(L("黑##c_black", "black##c_black"))) {
                    state.color[0] = state.color[1] = state.color[2] = 0.0f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(L("红##c_red", "red##c_red"))) {
                    state.color[0] = 0.8f;
                    state.color[1] = 0.1f;
                    state.color[2] = 0.1f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(L("金##c_gold", "gold##c_gold"))) {
                    state.color[0] = 0.9f;
                    state.color[1] = 0.7f;
                    state.color[2] = 0.2f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(L("白##c_white", "white##c_white"))) {
                    state.color[0] = state.color[1] = state.color[2] = 1.0f;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(L("蓝##c_blue", "blue##c_blue"))) {
                    state.color[0] = 0.2f;
                    state.color[1] = 0.4f;
                    state.color[2] = 0.9f;
                }

                ImGui::SliderFloat(L("透明度", "opacity"), &state.alpha, 0.05f, 1.0f, "%.2f");

                // Slot picker. The chosen slot is clamped against the area the
                // selected tattoo actually belongs to, so a stale value from
                // another area can never be submitted.
                const int maxSlots = ng::MaxSlots(state.selected.area);
                if (state.targetSlot >= maxSlots) {
                    state.targetSlot = -1;
                }

                const std::string comboLabel = state.targetSlot < 0
                    ? std::string(L("自动分配空槽", "first free slot"))
                    : std::string(L("指定槽位 ", "slot ")) + std::to_string(state.targetSlot);

                ImGui::Text("%s:", L("目标槽位", "target slot"));
                ImGui::SameLine();
                // Wide enough for the longest preview ("自动分配空槽"), clamped to
                // what is actually left on the line.
                ImVec2 slotText{};
                ImGui::CalcTextSize(&slotText, L("自动分配空槽", "first free slot"), nullptr, true, -1.0f);
                const auto* style = ImGui::GetStyle();
                const float comboWidth = std::min(slotText.x + ImGui::GetFrameHeight() + style->FramePadding.x * 2.0f + style->ItemInnerSpacing.x,
                    ImGui::GetContentRegionAvail().x);
                ImGui::PushItemWidth(comboWidth);
                if (ImGui::BeginCombo("##slot_combo", comboLabel.c_str())) {
                    if (ImGui::Selectable(L("自动分配空槽##slot_auto", "first free slot##slot_auto"), state.targetSlot < 0)) {
                        state.targetSlot = -1;
                    }
                    for (int slot = 0; slot < maxSlots; ++slot) {
                        const std::string option = std::string(L("指定槽位 ", "slot ")) + std::to_string(slot);
                        if (ImGui::Selectable((option + "##slot_opt_" + std::to_string(slot)).c_str(), state.targetSlot == slot)) {
                            state.targetSlot = slot;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();

                ImGui::Spacing();

                const char* applyLabel = L("应用 / 更新颜色##apply", "Apply / update##apply");
                const char* removeLabel = L("从目标移除##remove", "Remove##remove");
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
                    ? L("在中间的列表里挑一款纹身。", "pick a tattoo in the middle list.")
                    : L("先在左侧选择一个模组，纹身列表才会出现。", "pick a mod on the left to reveal the tattoo list."));
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const std::string area = CurrentArea().empty() ? std::string("Body") : CurrentArea();

            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s (%s):", L("槽位状态", "slot status"), area.c_str());

            data::SlotSnapshot::Get().Request(data::TargetState::Get().Handle(), area);
            DrawSlotList(area);

            ImGui::Spacing();
            const char* clearAreaLabel = L("清空当前部位##clear_area", "Clear area##clear_area");
            const char* clearAllLabel = L("清空全身##clear_all", "Clear everything##clear_all");
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
            if (ImGui::CollapsingHeader(L("预设##presets", "Presets##presets"))) {
                static char s_presetName[64] = "";
                static bool s_clearFirst = true;

                auto&       store = data::PresetStore::Get();
                static bool s_storeLoaded = false;
                if (!s_storeLoaded) {
                    s_storeLoaded = true;
                    store.Refresh();
                }

                ImGui::Text("%s:", L("预设名称", "preset name"));
                ImGui::SameLine();
                ImGui::PushItemWidth(std::max(200.0f, ImGui::GetContentRegionAvail().x - 4.0f));
                ImGui::InputTextWithHint("##preset_name", L("给这套纹身起个名字", "name this set"), s_presetName, sizeof(s_presetName));
                ImGui::PopItemWidth();

                const char* savePresetLabel = L("把当前目标存为预设##preset_save", "Save the current target##preset_save");
                if (ImGui::Button(savePresetLabel, ButtonSize(savePresetLabel, 240.0f, 28.0f))) {
                    if (s_presetName[0] == '\0') {
                        state.SetStatus(false, L("请先输入预设名称", "enter a preset name first"), NowSeconds());
                    } else {
                        ops::CapturePreset(data::TargetState::Get().Handle(), s_presetName);
                    }
                }

                ImGui::Checkbox(L("应用前先清空目标##preset_clear", "Clear the target before applying##preset_clear"), &s_clearFirst);

                // Copy the names: deleting one refreshes the store's list.
                const auto names = store.Names();
                if (names.empty()) {
                    ImGui::TextDisabled("%s", L("还没有保存过预设。", "no presets saved yet."));
                } else {
                    for (const auto& presetName : names) {
                        ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "%s", presetName.c_str());
                        ImGui::SameLine();
                        if (ImGui::SmallButton((std::string(L("应用##papply_", "Apply##papply_")) + presetName).c_str())) {
                            ops::ApplyPreset(data::TargetState::Get().Handle(), presetName, s_clearFirst);
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton((std::string(L("删除##pdel_", "Delete##pdel_")) + presetName).c_str())) {
                            if (store.Delete(presetName)) {
                                state.SetStatus(true, std::string(L("已删除预设: ", "deleted preset: ")) + presetName, NowSeconds());
                            } else {
                                state.SetStatus(false, std::string(L("删除预设失败: ", "could not delete preset: ")) + presetName, NowSeconds());
                            }
                        }
                    }
                }

                ImGui::TextDisabled("%s", L("预设存放在 Data/SKSE/Plugins/SlaveTatsMenu/Presets/", "presets live in Data/SKSE/Plugins/SlaveTatsMenu/Presets/"));
            }

            ImGui::Spacing();
            if (ImGui::CollapsingHeader(L("调试探针##probe", "Diagnostics##probe"))) {
                ImGui::Text("%s: %s", L("状态", "status"), diag::IsRunning() ? L("运行中", "running") : L("已停止", "stopped"));
                const char* probeToggleLabel = diag::IsRunning() ? L("停止##probe_toggle", "Stop##probe_toggle") : L("启动##probe_toggle", "Start##probe_toggle");
                if (ImGui::Button(probeToggleLabel, ButtonSize(probeToggleLabel, 110.0f, 26.0f))) {
                    if (diag::IsRunning()) {
                        diag::Stop();
                    } else {
                        diag::Start();
                    }
                }
                ImGui::SameLine();
                const char* probeSnapshotLabel = L("立即快照##probe_snap", "Snapshot now##probe_snap");
                if (ImGui::Button(probeSnapshotLabel, ButtonSize(probeSnapshotLabel, 156.0f, 26.0f))) {
                    diag::LogSnapshot();
                    state.SetStatus(true, L("已请求探针快照，结果写入 SlaveTatsMenu.log", "probe snapshot requested, see SlaveTatsMenu.log"), NowSeconds());
                }
                ImGui::TextDisabled("%s", L("探针对比 SlaveTats 账本与 3D overlay 节点，只在变化时写日志。", "the probe compares the SlaveTats ledger against live overlay nodes and logs only changes."));
            }

            ImGui::EndChild();
        }

        void DrawStatusLine()
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

            if (state.statusText.empty()) {
                return;
            }

            constexpr double kHoldSeconds = 15.0;
            if ((NowSeconds() - state.statusAt) > kHoldSeconds) {
                return;
            }

            ImGui::Separator();
            const char*  tag = !state.statusOk ? L("失败", "FAILED") : (state.statusDeferred ? L("延后", "DEFERRED") : L("成功", "OK"));
            const ImVec4 colour = !state.statusOk      ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                                  : state.statusDeferred ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f)
                                                         : ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
            ImGui::TextColored(colour, "[%s] %s", tag, state.statusText.c_str());
        }

        void DrawMain()
        {
            // One-off verification that the framework calls us on the game's main
            // thread - every direct engine access below assumes exactly that.
            static const std::uint32_t s_renderThread = ThisThreadId();
            static const bool          s_threadLogged = [] {
                SKSE::log::info("UI: rendering on thread {} (plugin load thread {})", s_renderThread, LoadThreadId());
                if (LoadThreadId() != 0 && s_renderThread != LoadThreadId()) {
                    SKSE::log::warn("UI: render thread differs from the load thread - direct engine access from here needs review");
                }
                return true;
            }();
            (void)s_threadLogged;

            auto& db = data::TattooDatabase::Get();
            auto& state = UiState::Get();

            if (!ng::Ready()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::Text("%s", L("警告: 没有检测到 SlaveTatsNG 接口，所有操作都会失败。", "warning: SlaveTatsNG interface not found, every action will fail."));
                ImGui::PopStyleColor();
                ImGui::Separator();
            }

            ImGui::TextDisabled("%s: %zu (%s)   %s: %s   %s: %d",
                L("纹身库", "library"),
                db.Size(),
                db.SourceName(),
                L("JContainers", "JContainers"),
                jc::Ready() ? L("可用", "ok") : L("不可用", "unavailable"),
                L("已登记角色", "registered actors"),
                ng::KnownTargetCount());

            DrawTargetRow();
            DrawAreaRow();
            ImGui::Separator();

            // Panel 2 only exists once a mod has been picked in panel 1. While
            // it is hidden, the inspector keeps the width it has in the
            // three-column layout and the mod list absorbs the freed space.
            const float available = ImGui::GetContentRegionAvail().x;
            const float panelHeight = 460.0f;

            float col3 = available - 240.0f - 280.0f - 16.0f;
            if (col3 < 400.0f) {
                col3 = 400.0f;
            }

            const float gaps = state.sectionChosen ? 16.0f : 8.0f;
            const float col2 = state.sectionChosen ? 280.0f : 0.0f;
            const float col1 = state.sectionChosen
                ? 240.0f
                : std::max(240.0f, available - gaps - col3);

            DrawSectionPanel(col1, panelHeight);
            if (state.sectionChosen) {
                ImGui::SameLine();
                DrawTattooPanel(col2, panelHeight);
            }
            ImGui::SameLine();
            DrawInspector(col3, panelHeight);
            DrawStatusLine();
        }
    }

    void ToggleWindow()
    {
        if (g_window) {
            g_window->IsOpen = !g_window->IsOpen;
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
        SKSEMenuFramework::AddSectionItem(L("纹身管理器", "Tattoo Manager"), RenderMenu);
        g_window = SKSEMenuFramework::AddWindow(RenderWindow);

        if (!g_window) {
            SKSE::log::error("UI: the menu framework did not provide a window (framework {:.2f}) - the hotkey will do nothing", frameworkVersion);
        } else {
            SKSE::log::info("UI: window registered (framework {:.2f})", frameworkVersion);
        }
    }

    void __stdcall RenderMenu()
    {
        DrawMain();
    }

    void __stdcall RenderWindow()
    {
        if (!IsWindowOpen()) {
            return;
        }

        if (auto* viewport = ImGui::GetMainViewport()) {
            const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f);
            ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        }

        ImGui::SetNextWindowSize(ImVec2(Config::Get().windowWidth, Config::Get().windowHeight), ImGuiCond_FirstUseEver);

        if (ImGui::Begin(L("SlaveTats 纹身管理器##stm_window", "SlaveTats Manager##stm_window"), nullptr, ImGuiWindowFlags_NoCollapse)) {
            DrawMain();
            ImGui::Spacing();
            ImGui::Separator();
            const char* closeLabel = L("关闭窗口##close", "Close window##close");
            if (ImGui::Button(closeLabel, ButtonSize(closeLabel, 180.0f, 34.0f))) {
                g_window->IsOpen = false;
            }
        }
        ImGui::End();
    }
}
