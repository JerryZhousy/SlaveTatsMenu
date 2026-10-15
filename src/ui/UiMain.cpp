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
#include "render/MannequinRenderer.h"
#include "ui/UiState.h"

#include <shellapi.h>

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

        struct FontScaleScope
        {
            float scale;
            explicit FontScaleScope(float s) : scale(s)
            {
                if (scale > 0.01f && std::abs(scale - 1.0f) > 0.001f) {
                    ImGui::SetWindowFontScale(scale);
                }
            }
            ~FontScaleScope()
            {
                if (scale > 0.01f && std::abs(scale - 1.0f) > 0.001f) {
                    ImGui::SetWindowFontScale(1.0f);
                }
            }
        };

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
                if (!state.previewLoadedPath.empty()) {
                    SKSEMenuFramework::DisposeTexture(state.previewLoadedPath);
                }
                state.previewTexture = nullptr;
            }

            state.previewPath = a_rawTexture;
            state.previewLoadedPath.clear();
            if (state.previewPath.empty()) {
                return;
            }

            std::string path = state.previewPath;
            std::replace(path.begin(), path.end(), '\\', '/');
            if (path.rfind("Textures/", 0) != 0 && path.rfind("textures/", 0) != 0 &&
                path.rfind("Data/Textures/", 0) != 0 && path.rfind("data/textures/", 0) != 0) {
                path = "Data/Textures/Actors/Character/slavetats/" + path;
            } else if (path.rfind("Data/", 0) != 0 && path.rfind("data/", 0) != 0) {
                path = "Data/" + path;
            }

            // Ensure extension is lower-case .dds for SKSEMenuFramework case-sensitive check
            if (path.size() >= 4) {
                std::string ext = path.substr(path.size() - 4);
                if (ext == ".DDS" || ext == ".Dds") {
                    path.replace(path.size() - 4, 4, ".dds");
                }
            }

            state.previewLoadedPath = path;
            state.previewTexture = SKSEMenuFramework::LoadTexture(path, state.previewSize);
            if (!state.previewTexture) {
                // Not fatal: loose file missing, packed in BSA, or framework error.
                SKSE::log::warn("UI: preview for '{}' unavailable (missing loose file, packed in BSA, or framework error)", path);
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

                if (Config::Get().showPreview && state.previewTexture) {
                    ImGui::Spacing();

                    static bool s_view3D = true;
                    static bool s_autoSpin = false;

                    ImGui::Text("%s:", L("预览模式", "Preview"));
                    ImGui::SameLine();
                    if (ImGui::RadioButton(L("3D 模特##v3d", "3D Mannequin##v3d"), s_view3D)) {
                        s_view3D = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(L("2D 贴图##v2d", "2D Texture##v2d"), !s_view3D)) {
                        s_view3D = false;
                    }

                    if (s_view3D) {
                        if (!render::MannequinRenderer::Get().IsInitialized()) {
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

                        if (render::MannequinRenderer::Get().IsInitialized()) {
                            if (s_autoSpin) {
                                render::MannequinRenderer::Get().Rotate(0.015f, 0.0f);
                            }

                            const float pWidth = std::max(180.0f, a_width - 30.0f);
                            const float pHeight = 220.0f;

                            if (render::MannequinRenderer::Get().GetWidth() != static_cast<std::uint32_t>(pWidth) ||
                                render::MannequinRenderer::Get().GetHeight() != static_cast<std::uint32_t>(pHeight)) {
                                render::MannequinRenderer::Get().Resize(static_cast<std::uint32_t>(pWidth), static_cast<std::uint32_t>(pHeight));
                            }

                            auto* srv = render::MannequinRenderer::Get().RenderFrame(
                                reinterpret_cast<ID3D11ShaderResourceView*>(state.previewTexture),
                                state.color,
                                state.alpha,
                                false
                            );

                            if (srv) {
                                ImGui::Image(reinterpret_cast<ImGuiMCP::ImTextureID>(srv), ImVec2(pWidth, pHeight),
                                    ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, 1), ImVec4(0.35f, 0.40f, 0.50f, 1.0f));

                                if (ImGui::IsItemHovered()) {
                                    auto* io = ImGui::GetIO();
                                    if (io) {
                                        if (io->MouseWheel != 0.0f) {
                                            render::MannequinRenderer::Get().Zoom(io->MouseWheel * 0.18f);
                                        }
                                        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                                            render::MannequinRenderer::Get().Rotate(io->MouseDelta.x * 0.012f, io->MouseDelta.y * 0.012f);
                                        }
                                        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                                            render::MannequinRenderer::Get().Pan(-io->MouseDelta.y * 0.005f);
                                        }
                                    }
                                }

                                ImGui::TextDisabled("%s", L("左键拖动旋转 / 滚轮缩放 / 右键拖动视角", "Left-drag to rotate / Wheel to zoom / Right-drag"));
                                ImGui::SameLine();
                                if (ImGui::SmallButton(L("复位##cam_rst", "Reset##cam_rst"))) {
                                    render::MannequinRenderer::Get().ResetCamera();
                                }
                                ImGui::SameLine();
                                ImGui::Checkbox(L("自转##cam_spin", "Spin##cam_spin"), &s_autoSpin);

                                ImGui::TextDisabled("%s: %s", L("身形模型", "Body Mesh"), render::MannequinRenderer::Get().GetMeshSourceInfo().c_str());
                            }
                        }
                    } else {
                        const float pSize = Config::Get().previewSize;
                        ImGui::Image(state.previewTexture, ImVec2(pSize, pSize), ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, 1), ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                    }
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
                if (ImGui::Button(savePresetLabel, ButtonSize(savePresetLabel, 200.0f, 28.0f))) {
                    if (s_presetName[0] == '\0') {
                        state.SetStatus(false, L("请先输入预设名称", "enter a preset name first"), NowSeconds());
                    } else {
                        ops::CapturePreset(data::TargetState::Get().Handle(), s_presetName);
                    }
                }
                ImGui::SameLine();
                const char* openPresetFolderLabel = L("打开预设目录##open_preset_dir", "Open Folder##open_preset_dir");
                if (ImGui::Button(openPresetFolderLabel, ButtonSize(openPresetFolderLabel, 0.0f, 28.0f))) {
                    const auto dir = data::PresetStore::Get().Directory();
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
            FontScaleScope fontScope(Config::Get().fontScale);

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
            const float availY = ImGui::GetContentRegionAvail().y;
            const float panelHeight = std::max(380.0f, availY - 35.0f);

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
            const char* nameZh;
            const char* nameEn;
            std::uint32_t dik;
        };

        const KeyChoice kPopularKeys[] = {
            { "F10 (默认)", "F10 (Default)", 0x44 },
            { "F9", "F9", 0x43 },
            { "F8", "F8", 0x42 },
            { "F7", "F7", 0x41 },
            { "F6", "F6", 0x40 },
            { "F5", "F5", 0x3F },
            { "F4", "F4", 0x3E },
            { "F3", "F3", 0x3D },
            { "F2", "F2", 0x3C },
            { "F1", "F1", 0x3B },
            { "F11", "F11", 0x57 },
            { "F12", "F12", 0x58 },
            { "Insert", "Insert", 0xD2 },
            { "Delete", "Delete", 0xD3 },
            { "Home", "Home", 0xC7 },
            { "End", "End", 0xCF },
            { "Page Up", "Page Up", 0xC9 },
            { "Page Down", "Page Down", 0xD1 },
            { "` (波浪号 / Tilde)", "` (Tilde)", 0x29 },
            { "小键盘 0", "Num 0", 0x52 },
            { "小键盘 1", "Num 1", 0x4F },
            { "小键盘 2", "Num 2", 0x50 },
            { "小键盘 3", "Num 3", 0x51 },
            { "小键盘 4", "Num 4", 0x4B },
            { "小键盘 5", "Num 5", 0x4C },
            { "小键盘 6", "Num 6", 0x4D },
            { "小键盘 7", "Num 7", 0x47 },
            { "小键盘 8", "Num 8", 0x48 },
            { "小键盘 9", "Num 9", 0x49 },
            { "小键盘 *", "Num *", 0x37 },
            { "小键盘 -", "Num -", 0x4A },
            { "小键盘 +", "Num +", 0x4E },
            { "小键盘 /", "Num /", 0xB5 },
            { "小键盘 回车", "Num Enter", 0x9C },
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
            FontScaleScope fontScope(Config::Get().fontScale);
            auto&          config = Config::Get();

            auto SetStatus = [&](const char* a_zh, const char* a_en) {
                s_settingsStatus = L(a_zh, a_en);
                s_settingsStatusTime = NowSeconds();
            };

            ImGui::TextDisabled("%s", L("配置 SlaveTats 菜单的常规选项。所有修改会自动保存至 SlaveTatsMenu.ini。",
                                       "Configure general options for SlaveTats Menu. Changes are automatically saved to SlaveTatsMenu.ini."));
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 1. 界面与显示 (Interface & Display)
            if (ImGui::CollapsingHeader(L("界面与显示", "Interface & Display"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();

                // 语言
                ImGui::Text("%s:", L("界面语言", "Interface Language"));
                ImGui::SameLine();
                int langChoice = config.IsChinese() ? 0 : 1;
                bool langChanged = false;
                if (ImGui::RadioButton(L("简体中文", "Simplified Chinese"), &langChoice, 0)) {
                    langChanged = true;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("English", &langChoice, 1)) {
                    langChanged = true;
                }
                if (langChanged) {
                    config.language = (langChoice == 0) ? Language::kChinese : Language::kEnglish;
                    config.Save();
                    SetStatus("语言设置已更新并保存！", "Language updated and saved!");
                }

                ImGui::Spacing();

                // 文字大小
                float fScale = config.fontScale;
                ImGui::PushItemWidth(260.0f);
                if (ImGui::SliderFloat(L("文字大小缩放##font_scale", "Font Size Scale##font_scale"), &fScale, 0.75f, 1.50f, "%.2f x")) {
                    config.fontScale = fScale;
                    config.Save();
                }
                ImGui::PopItemWidth();
                ImGui::SameLine();
                if (ImGui::Button(L("重置为 1.0x##reset_font", "Reset to 1.0x##reset_font"), ButtonSize(L("重置为 1.0x", "Reset to 1.0x")))) {
                    config.fontScale = 1.0f;
                    config.Save();
                    SetStatus("文字大小已重置为 1.0x！", "Font size reset to 1.0x!");
                }

                ImGui::Spacing();

                // 窗口背景不透明度
                float winAlpha = config.windowAlpha;
                ImGui::PushItemWidth(260.0f);
                if (ImGui::SliderFloat(L("窗口背景不透明度##win_alpha", "Window Background Opacity##win_alpha"), &winAlpha, 0.20f, 1.0f, "%.2f")) {
                    config.windowAlpha = winAlpha;
                    config.Save();
                }
                ImGui::PopItemWidth();
                ImGui::TextDisabled("%s", L("说明: 调低不透明度可使浮动窗口半透明，方便透过窗口观察角色身上的着色效果。",
                                           "Note: Lower opacity makes the window translucent to see character tattoos directly through it."));
            }

            ImGui::Spacing();

            // 2. 默认窗口大小设置 (Default Menu Size)
            if (ImGui::CollapsingHeader(L("默认窗口大小设置", "Window Size Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();

                if (s_w <= 0.0f) s_w = config.windowWidth;
                if (s_h <= 0.0f) s_h = config.windowHeight;

                ImGui::Text("%s:", L("快速分辨率预设", "Quick Presets"));
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
                        SetStatus("已应用预设窗口尺寸并保存！", "Preset window size applied and saved!");
                    }
                }

                ImGui::Spacing();

                ImGui::PushItemWidth(260.0f);
                if (ImGui::SliderFloat(L("默认宽度##win_w", "Default Width##win_w"), &s_w, 800.0f, 2560.0f, "%.0f px")) {
                    config.windowWidth = std::round(s_w);
                    config.Save();
                }
                if (ImGui::SliderFloat(L("默认高度##win_h", "Default Height##win_h"), &s_h, 500.0f, 1440.0f, "%.0f px")) {
                    config.windowHeight = std::round(s_h);
                    config.Save();
                }
                ImGui::PopItemWidth();

                ImGui::Spacing();

                if (ImGui::Button(L("应用并调整窗口尺寸##apply_sz", "Apply & Resize Window##apply_sz"), ButtonSize(L("应用并调整窗口尺寸", "Apply & Resize Window")))) {
                    config.windowWidth = std::round(s_w);
                    config.windowHeight = std::round(s_h);
                    config.Save();
                    s_forceResizeWindow = true;
                    SetStatus("窗口尺寸已应用并保存！", "Window size applied and saved!");
                }

                if (s_lastFloatingWindowSize.x >= 600.0f && s_lastFloatingWindowSize.y >= 400.0f) {
                    ImGui::SameLine();
                    char grabLabel[64];
                    std::snprintf(grabLabel, sizeof(grabLabel), "%s (%.0fx%.0f)##grab_sz",
                                  L("抓取当前窗口尺寸", "Grab Current Window Size"),
                                  s_lastFloatingWindowSize.x, s_lastFloatingWindowSize.y);
                    if (ImGui::Button(grabLabel, ButtonSize(grabLabel))) {
                        s_w = std::round(s_lastFloatingWindowSize.x);
                        s_h = std::round(s_lastFloatingWindowSize.y);
                        config.windowWidth = s_w;
                        config.windowHeight = s_h;
                        config.Save();
                        SetStatus("已抓取当前窗口尺寸并保存为默认！", "Current window size saved as default!");
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", L("将浮动窗口当前被鼠标拉伸的实际尺寸保存为默认大小",
                                                 "Save the current floating window size as default"));
                    }
                }

                ImGui::SameLine();
                if (ImGui::Button(L("重置默认尺寸 (1280x720)##reset_sz", "Reset to 1280x720##reset_sz"), ButtonSize(L("重置默认尺寸 (1280x720)", "Reset to 1280x720")))) {
                    s_w = 1280.0f;
                    s_h = 720.0f;
                    config.windowWidth = 1280.0f;
                    config.windowHeight = 720.0f;
                    config.Save();
                    s_forceResizeWindow = true;
                    SetStatus("已重置为默认尺寸 (1280x720)！", "Reset to default size (1280x720)!");
                }

                ImGui::TextDisabled("%s", L("说明: 首次打开或点击「应用」时生效；在游戏中拖动窗口边框调整大小后，亦可点击「抓取」直接保存为默认。",
                                           "Note: Takes effect on first open or when clicking 'Apply'; after resizing the window border, click 'Grab' to save it directly."));
                ImGui::Spacing();
            }

            ImGui::Spacing();

            // 3. 纹身预览与槽位策略 (Preview & Slot Policy)
            if (ImGui::CollapsingHeader(L("纹身预览与槽位策略", "Preview & Slot Policy"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();

                // 槽位冲突策略
                ImGui::Text("%s:", L("槽位占用冲突策略", "Slot Conflict Policy"));
                ImGui::SameLine();
                int policyChoice = (config.slotPolicy == SlotPolicy::kOverwrite) ? 1 : 0;
                bool policyChanged = false;
                if (ImGui::RadioButton(L("自动顺延至空槽位##slot_autofree", "Auto Allocate to First Free Slot##slot_autofree"), &policyChoice, 0)) {
                    policyChanged = true;
                }
                ImGui::SameLine();
                if (ImGui::RadioButton(L("直接覆盖所选槽位##slot_overwrite", "Overwrite Selected Slot##slot_overwrite"), &policyChoice, 1)) {
                    policyChanged = true;
                }
                if (policyChanged) {
                    config.slotPolicy = (policyChoice == 1) ? SlotPolicy::kOverwrite : SlotPolicy::kAutoFreeSlot;
                    config.Save();
                    SetStatus("槽位冲突策略已更新！", "Slot policy updated!");
                }
                ImGui::TextDisabled("%s", L("说明: 当手动指定的目标槽位已被占用时，选择自动顺延寻找空槽位，还是直接替换该槽位的现有纹身。",
                                           "Note: Choose whether to auto-route to an open slot or replace existing ink when the target slot is occupied."));

                ImGui::Spacing();

                // 图案预览
                bool showPrev = config.showPreview;
                if (ImGui::Checkbox(L("显示纹身贴图预览##show_prev", "Show Tattoo Texture Preview##show_prev"), &showPrev)) {
                    config.showPreview = showPrev;
                    config.Save();
                    SetStatus("贴图预览开关已更新！", "Preview toggle updated!");
                }

                if (config.showPreview) {
                    ImGui::SameLine();
                    ImGui::Text("%s:", L("预览尺寸预设", "Preview Sizes"));
                    const float sizes[] = { 64.0f, 96.0f, 128.0f, 192.0f };
                    for (int i = 0; i < 4; ++i) {
                        ImGui::SameLine();
                        char sbuf[32];
                        std::snprintf(sbuf, sizeof(sbuf), "%.0fpx##prev_sz_%d", sizes[i], i);
                        if (ImGui::Button(sbuf)) {
                            config.previewSize = sizes[i];
                            config.Save();
                            SetStatus("预览尺寸已更新！", "Preview size updated!");
                        }
                    }

                    ImGui::PushItemWidth(260.0f);
                    float prevSz = config.previewSize;
                    if (ImGui::SliderFloat(L("预览图边长##prev_slider", "Preview Dimension##prev_slider"), &prevSz, 64.0f, 256.0f, "%.0f px")) {
                        config.previewSize = prevSz;
                        config.Save();
                    }
                    ImGui::PopItemWidth();
                }

                ImGui::Spacing();
            }

            ImGui::Spacing();

            // 4. 预设管理与实用工具 (Presets & Utilities)
            if (ImGui::CollapsingHeader(L("预设与文件管理", "Presets & File Management"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();

                const char* openExplorerLabel = L("在资源管理器中打开预设文件夹##open_presets_dir", "Open Presets Folder in Explorer##open_presets_dir");
                if (ImGui::Button(openExplorerLabel, ButtonSize(openExplorerLabel, 260.0f, 28.0f))) {
                    const auto dir = data::PresetStore::Get().Directory();
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    SetStatus("已在文件资源管理器中打开预设目录！", "Presets folder opened in Explorer!");
                }
                ImGui::TextDisabled("%s", L("说明: 直接打开预设 JSON 文件存放目录，方便导入外部预设、复制备份或分享给他人。",
                                           "Note: Opens the presets directory in Windows Explorer for easy import, backup, and sharing of .json preset sets."));

                ImGui::Spacing();
            }

            ImGui::Spacing();

            // 5. 快捷键设置 (Hotkey)
            if (ImGui::CollapsingHeader(L("快捷键设置", "Hotkey Settings"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Spacing();
                ImGui::Text("%s:", L("独立窗口呼出快捷键", "Floating Window Hotkey"));
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
                                    SetStatus("快捷键已更新并保存！", "Hotkey updated and saved!");
                                    break;
                                }
                            }
                        }
                    }

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.1f, 1.0f));
                    ImGui::Button(L("等待按键输入... (按 ESC 取消)##bind_btn", "Press any key... (ESC to cancel)##bind_btn"), ButtonSize("", 240.0f));
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (ImGui::Button(L("取消##bind_cancel", "Cancel##bind_cancel"), ButtonSize(L("取消", "Cancel")))) {
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
                        ImGui::SetTooltip("%s", L("点击以按下键盘录入新按键", "Click to record a new key"));
                    }

                    ImGui::SameLine();
                    if (ImGui::Button(L("快速选择##key_quick", "Quick Select##key_quick"), ButtonSize(L("快速选择", "Quick Select")))) {
                        ImGui::OpenPopup("QuickKeyPopup");
                    }

                    if (ImGui::BeginPopup("QuickKeyPopup")) {
                        for (const auto& choice : kPopularKeys) {
                            const bool isSelected = (config.toggleKey == choice.dik);
                            if (ImGui::Selectable(L(choice.nameZh, choice.nameEn), isSelected)) {
                                config.toggleKey = choice.dik;
                                config.Save();
                                SetStatus("快捷键已更改并保存！", "Hotkey updated and saved!");
                            }
                        }
                        ImGui::EndPopup();
                    }

                    ImGui::SameLine();
                    if (ImGui::Button(L("默认 (F10)##reset_hotkey", "Default (F10)##reset_hotkey"), ButtonSize(L("默认 (F10)", "Default (F10)")))) {
                        config.toggleKey = 0x44;
                        config.Save();
                        SetStatus("快捷键已重置为 F10！", "Hotkey reset to F10!");
                    }
                }

                ImGui::TextDisabled("%s", L("说明: 在游戏内随时按下此快捷键可切换呼出/隐藏独立的 SlaveTats 纹身管理器窗口。",
                                           "Note: Press this hotkey anytime in-game to toggle the standalone SlaveTats Manager window."));
                ImGui::Spacing();
            }

            ImGui::Spacing();

            // 6. 其他常规设置 (Other General Settings)
            if (ImGui::CollapsingHeader(L("其他设置", "Other Settings"))) {
                ImGui::Spacing();

                float inkColor[3] = {
                    static_cast<float>((config.defaultColor >> 16) & 0xFF) / 255.0f,
                    static_cast<float>((config.defaultColor >> 8) & 0xFF) / 255.0f,
                    static_cast<float>(config.defaultColor & 0xFF) / 255.0f
                };
                if (ImGui::ColorEdit3(L("默认纹身墨水颜色##def_col", "Default Tattoo Ink Color##def_col"), inkColor)) {
                    config.defaultColor = PackColor(inkColor);
                    config.Save();
                }

                if (ImGui::SliderFloat(L("默认纹身透明度##def_alpha", "Default Tattoo Opacity##def_alpha"), &config.defaultAlpha, 0.05f, 1.0f, "%.2f")) {
                    config.Save();
                }

                if (ImGui::SliderInt(L("已生效槽位刷新间隔 (毫秒)##slot_refresh", "Applied Slots Refresh Interval (ms)##slot_refresh"), &config.slotRefreshMs, 50, 1000, "%d ms")) {
                    config.Save();
                }

                ImGui::Spacing();
                if (ImGui::Button(L("恢复全部默认设置##reset_all", "Restore All Defaults##reset_all"), ButtonSize(L("恢复全部默认设置", "Restore All Defaults")))) {
                    config.toggleKey = 0x44;
                    config.language = Language::kChinese;
                    config.windowWidth = 1280.0f;
                    config.windowHeight = 720.0f;
                    config.windowAlpha = 0.95f;
                    config.fontScale = 1.0f;
                    config.showPreview = true;
                    config.previewSize = 96.0f;
                    config.slotPolicy = SlotPolicy::kAutoFreeSlot;
                    config.defaultAlpha = 1.0f;
                    config.defaultColor = 0x000000;
                    config.slotRefreshMs = 250;
                    s_w = 1280.0f;
                    s_h = 720.0f;
                    config.Save();
                    s_forceResizeWindow = true;
                    SetStatus("已恢复全部默认设置并保存！", "All default settings restored and saved!");
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
        return s_bindingHotkey;
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
        SKSEMenuFramework::AddSectionItem(L("设置", "Settings"), RenderSettings);
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

    void __stdcall RenderSettings()
    {
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

        const bool justOpened = !s_wasOpen;
        s_wasOpen = true;

        const auto toggleKey = Config::Get().toggleKey;
        const auto toggleImKey = DirectInputToImGuiKey(toggleKey);

        if (justOpened) {
            s_canCloseWithToggleKey = false;
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
                return;
            }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (s_bindingHotkey) {
                s_bindingHotkey = false;
                return;
            }
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
                return;
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
        if (ImGui::Begin(L("SlaveTats 纹身管理器##stm_window", "SlaveTats Manager##stm_window"), &open, ImGuiWindowFlags_NoCollapse)) {
            FontScaleScope fontScope(Config::Get().fontScale);
            s_lastFloatingWindowSize = ImGui::GetWindowSize();
            if (ImGui::BeginTabBar("##stm_tabs")) {
                if (ImGui::BeginTabItem(L("纹身管理", "Tattoo Manager"))) {
                    DrawMain();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(L("设置", "Settings"))) {
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
        }
    }
}
