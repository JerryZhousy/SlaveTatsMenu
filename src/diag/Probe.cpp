#include "diag/Probe.h"

#include "bridge/JcApi.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Tasks.h"
#include "diag/SegmentDiff.h"

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

namespace stm::diag
{
    namespace
    {
        std::atomic_bool g_running{ false };
        std::thread      g_thread;

        // game-thread-only state
        bool        g_loggedBaselineData = false;
        bool        g_loggedOverlayNames = false;
        std::string g_lastData;
        std::string g_lastRender;

        std::string ToStr(int a_value)
        {
            return std::to_string(a_value);
        }

        const char* Safe(const char* a_str)
        {
            return a_str ? a_str : "";
        }

        int SlotCount()
        {
            return std::clamp(Config::Get().probeSlots, 1, 24);
        }

        // DiffSegments lives in diag/SegmentDiff.h so that the offline self-test
        // can cover it - it decides when this probe writes anything at all.

        RE::BSLightingShaderMaterialBase* AsLightingMaterial(RE::BSShaderMaterial* a_material)
        {
            if (!a_material || a_material->GetType() != RE::BSShaderMaterial::Type::kLighting) {
                return nullptr;
            }
            return static_cast<RE::BSLightingShaderMaterialBase*>(a_material);
        }

        // What the live scene graph looks like for one overlay node.
        //
        // The object registered under "<Area> [OvlN]" is NOT guaranteed to be a
        // NiNode - depending on the RaceMenu/skee build it can be the geometry
        // itself. An earlier version assumed NiNode and therefore reported every
        // slot as "not-a-node" even though all 24 nodes existed, which made the
        // render side of this probe useless.
        std::string ReadRenderState(RE::NiAVObject* a_root, const std::string& a_area, int a_slot)
        {
            const std::string nodeName = a_area + " [Ovl" + ToStr(a_slot) + "]";
            auto*             node = a_root->GetObjectByName(RE::BSFixedString(nodeName.c_str()));
            if (!node) {
                return "absent";
            }

            const auto* rtti = node->GetRTTI();
            // NOTE: separators inside a sample must not be '|' - the sample itself
            // is "0=v|1=v|...", and DiffSegments splits on '|'. Using '|' here made
            // every changed line print bare "tex=..." fragments with no slot index.
            std::string result = "type=" + std::string(rtti && rtti->name ? rtti->name : "?");

            // Whatever could hold the overlay geometry.
            std::vector<RE::NiAVObject*> candidates;

            if (auto* asNode = netimmerse_cast<RE::NiNode*>(node)) {
                auto& children = asNode->GetChildren();
                result += ",kids=" + ToStr(static_cast<int>(children.size()));
                candidates.reserve(children.size());
                for (std::uint32_t i = 0; i < children.size(); ++i) {
                    if (auto* child = children[i].get()) {
                        candidates.push_back(child);
                    }
                }
            } else {
                result += ",self";
                candidates.push_back(node);
            }

            int described = 0;
            for (auto* candidate : candidates) {
                auto* geo = netimmerse_cast<RE::BSGeometry*>(candidate);
                if (!geo) {
                    continue;
                }

                ++described;
                auto* shader = geo->lightingShaderProp_cast();
                if (!shader) {
                    result += ",geo(no-light-shader)";
                    continue;
                }

                result += ",alpha=" + std::to_string(shader->alpha);

                auto* material = shader->GetBaseMaterial();
                if (!material) {
                    result += ",mat=none";
                    continue;
                }

                auto* lighting = AsLightingMaterial(material);
                if (!lighting) {
                    result += ",mat=other";
                    continue;
                }

                auto textureSet = lighting->GetTextureSet();
                if (!textureSet) {
                    result += ",tex=none";
                    continue;
                }

                result += ",tex=" + std::string(Safe(textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)));
            }

            if (described == 0) {
                result += ",no-geometry";
            }

            return result;
        }

        void CollectOverlayNodeNames(RE::NiAVObject* a_obj, int a_depth, std::string& a_out, int& a_count, int a_maxCount, int a_maxDepth)
        {
            if (!a_obj || a_depth > a_maxDepth || a_count >= a_maxCount) {
                return;
            }

            if (const char* name = a_obj->name.c_str(); name && std::strstr(name, "[Ovl")) {
                a_out += "\n    ";
                a_out += name;
                ++a_count;
            }

            auto* asNode = netimmerse_cast<RE::NiNode*>(a_obj);
            if (!asNode) {
                return;
            }

            auto& children = asNode->GetChildren();
            for (std::uint32_t i = 0; i < children.size() && a_count < a_maxCount; ++i) {
                CollectOverlayNodeNames(children[i].get(), a_depth + 1, a_out, a_count, a_maxCount, a_maxDepth);
            }
        }

        std::string LedgerState(RE::Actor* a_actor, const std::string& a_area)
        {
            std::string result;
            const int   slots = SlotCount();
            const bool  canReadNames = jc::Ready();

            for (int i = 0; i < slots; ++i) {
                if (!result.empty()) {
                    result += "|";
                }

                const std::int32_t handle = ng::GetAppliedInSlot(a_actor, a_area, i);
                if (handle <= 0) {
                    result += ToStr(i) + "=0";
                    continue;
                }

                // Occupancy alone cannot see one entry replace another in the same
                // slot - which is exactly how another tattoo mod "takes a slot
                // back" while the ledger count never moves. Read the identity as
                // well, so the diff shows 6=Rush/腿部_肉奴隶 turning into
                // 6=LewdMarks/003 instead of staying silently "1".
                std::string label = "1";
                if (canReadNames) {
                    const std::string section = jc::MapGetStr(jc::ObjId{ handle }, "section");
                    const std::string name = jc::MapGetStr(jc::ObjId{ handle }, "name");
                    if (!section.empty() || !name.empty()) {
                        label = section + "/" + name;
                    }
                }

                result += ToStr(i) + "=" + label;
            }
            return result;
        }

        int LedgerCount(RE::Actor* a_actor, const std::string& a_area)
        {
            int count = 0;
            const int slots = SlotCount();
            for (int i = 0; i < slots; ++i) {
                if (ng::GetAppliedInSlot(a_actor, a_area, i) > 0) {
                    ++count;
                }
            }
            return count;
        }

        // RaceMenu parks its own blank overlay texture in unused slots, so "a
        // node exists" is not the same as "a tattoo is showing". Counting the
        // blank ones made the verdict read "consistent (ledger=9, nodes=12)".
        bool LooksLikeBlankSlot(const std::string& a_state)
        {
            if (a_state == "absent") {
                return true;
            }

            std::string lower(a_state);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            return lower.find("overlays\\default.dds") != std::string::npos
                || lower.find("overlays/default.dds") != std::string::npos
                || lower.find("alpha=0.000000") != std::string::npos;
        }

        int RenderCount(RE::NiAVObject* a_root, const std::string& a_area)
        {
            int count = 0;
            const int slots = SlotCount();
            for (int i = 0; i < slots; ++i) {
                if (!LooksLikeBlankSlot(ReadRenderState(a_root, a_area, i))) {
                    ++count;
                }
            }
            return count;
        }

        // The one-line answer to "why did nothing happen".
        std::string Diagnose(RE::Actor* a_actor, RE::NiAVObject* a_root, const std::string& a_area, int a_ledgerCount, int a_renderCount)
        {
            if (!a_actor) {
                return "no actor";
            }
            if (!ng::Ready()) {
                return "SlaveTatsNG interface missing - every call is a no-op";
            }
            if (!a_actor->Is3DLoaded()) {
                return "3D not loaded: synchronize_tattoos aborts and leaves .SlaveTats.updated = 1";
            }
            if (!jc::Ready()) {
                return "JContainers unavailable: cannot read .updated / .version, ledger state unknown";
            }

            const auto updated = jc::FormDbGetInt(a_actor, ".SlaveTats.updated", -1);
            const auto version = jc::FormDbGetStr(a_actor, ".SlaveTats.version", "");

            if (version.empty()) {
                return "'.SlaveTats.version' is empty: the next sync rebuilds the ledger from the 3D nodes and drops colour/glow";
            }
            if (a_ledgerCount == 0 && a_renderCount > 0) {
                return "overlay nodes exist without ledger entries - another mod wrote them, or the ledger was rebuilt";
            }
            if (a_ledgerCount != a_renderCount) {
                // Equal counts are the healthy case. A gap means something is in the
                // ledger but not on the body (or the reverse); reporting
                // "consistent (ledger=9, nodes=8)" in that state was a false
                // negative, and a missing tattoo is exactly what this probe exists
                // to catch.
                const int missing = a_ledgerCount - a_renderCount;
                return "count mismatch: ledger=" + std::to_string(a_ledgerCount) + " render=" + std::to_string(a_renderCount)
                    + (missing > 0 ? " (" + std::to_string(missing) + " recorded but not on the body)" : " (more overlay nodes than ledger entries)")
                    + " updated=" + std::to_string(updated);
            }
            if (updated != 0) {
                return "'.SlaveTats.updated' is stuck at " + std::to_string(updated) + " - pending sync";
            }
            return "consistent (ledger=" + std::to_string(a_ledgerCount) + ", nodes=" + std::to_string(a_renderCount) + ")";
        }
    }

    void LogSnapshot()
    {
        PostToGameThread([] {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !ng::Ready()) {
                SKSE::log::info("[PROBE] snapshot skipped (player={}, interface={})", player ? "ok" : "null", ng::Ready() ? "ok" : "missing");
                return;
            }

            auto* root = player->Get3D();
            if (!root) {
                SKSE::log::info("[PROBE] snapshot skipped: 3D not loaded yet");
                return;
            }

            const std::string area = Config::Get().probeArea;
            SKSE::log::info("[PROBE] snapshot: area={} slots={} updated={} version='{}'",
                area,
                SlotCount(),
                jc::FormDbGetInt(player, ".SlaveTats.updated", -1),
                jc::FormDbGetStr(player, ".SlaveTats.version", "?"));

            const int slots = SlotCount();
            for (int i = 0; i < slots; ++i) {
                SKSE::log::info("[PROBE] snapshot slot {:>2} ledger={} render={}", i, ng::GetAppliedInSlot(player, area, i), ReadRenderState(root, area, i));
            }

            SKSE::log::info("[PROBE] snapshot verdict: {}", Diagnose(player, root, area, LedgerCount(player, area), RenderCount(root, area)));
        });
    }

    namespace
    {
        void RunTick()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                log::Once("probe-no-player", spdlog::level::warn, "[PROBE] no player yet");
                return;
            }
            if (!ng::Ready()) {
                log::Once("probe-no-iface", spdlog::level::warn, "[PROBE] SlaveTatsNG interface unavailable - sampling is meaningless");
                return;
            }

            auto* root = player->Get3D();
            if (!root) {
                log::Once("probe-no-3d", spdlog::level::warn, "[PROBE] 3D not loaded - sampling skipped");
                return;
            }

            const std::string area = Config::Get().probeArea;
            const int         slots = SlotCount();

            const std::string data = LedgerState(player, area);
            if (!g_loggedBaselineData) {
                g_loggedBaselineData = true;
                SKSE::log::info("[PROBE] ledger baseline ({}): {}", area, data);
            } else if (data != g_lastData) {
                SKSE::log::info("[PROBE] *** LEDGER CHANGED ({}): {}", area, DiffSegments(g_lastData, data));
            }
            g_lastData = data;

            std::string render;
            for (int i = 0; i < slots; ++i) {
                if (!render.empty()) {
                    render += "|";
                }
                render += ToStr(i) + "=" + ReadRenderState(root, area, i);
            }

            if (render != g_lastRender) {
                if (g_lastRender.empty()) {
                    SKSE::log::info("[PROBE] render baseline ({}): {}", area, render);
                } else {
                    SKSE::log::info("[PROBE] *** RENDER CHANGED ({}): {}", area, DiffSegments(g_lastRender, render));
                }
                g_lastRender = render;
            }

            if (!g_loggedOverlayNames) {
                g_loggedOverlayNames = true;
                std::string names;
                int         count = 0;
                CollectOverlayNodeNames(root, 0, names, count, 40, 4);
                SKSE::log::info("[PROBE] overlay nodes containing '[Ovl': {}{}", count, names.empty() ? " (none - overlays may live elsewhere)" : names);
            }

            // Only log when the conclusion changes: the SKSE task queue can back
            // up during a load and then drain dozens of samples in the same
            // millisecond, which used to flood the log with identical lines.
            static std::string s_lastVerdict;
            const auto         verdict = Diagnose(player, root, area, LedgerCount(player, area), RenderCount(root, area));
            if (verdict != s_lastVerdict) {
                s_lastVerdict = verdict;
                SKSE::log::info("[PROBE] verdict: {}", verdict);
            }
        }
    }

    void Start()
    {
        if (g_running.exchange(true)) {
            return;
        }

        if (g_thread.joinable()) {
            g_thread.join();
        }

        g_thread = std::thread([] {
            while (g_running.load()) {
                const int interval = std::clamp(Config::Get().probeIntervalMs, 100, 60000);

                // Sleep in slices so Stop() returns quickly.
                for (int slept = 0; slept < interval && g_running.load();) {
                    const int slice = std::min(50, interval - slept);
                    std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                    slept += slice;
                }

                if (!g_running.load()) {
                    break;
                }

                PostToGameThread([] { RunTick(); });
            }
        });

        SKSE::log::info("[PROBE] started (interval={}ms, area={}, slots={})", Config::Get().probeIntervalMs, Config::Get().probeArea, SlotCount());
    }

    void Stop()
    {
        g_running.store(false);

        if (g_thread.joinable()) {
            g_thread.join();
        }

        SKSE::log::info("[PROBE] stopped");
    }

    bool IsRunning()
    {
        return g_running.load();
    }
}
