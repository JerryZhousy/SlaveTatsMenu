#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "StmOBodyApi.h"
#include "obody/MorphApply.h"
#include "obody/OBodyPluginApi.h"
#include "obody/PresetXml.h"
#include "obody/TriFile.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// SlaveTatsMenuOBody - the optional half of the 3D preview's OBody support.
//
// The main plugin renders the body NIF straight off disk, which in an OBody
// install is the *zeroed* mesh: every slider at rest. This add-on turns a
// resolved OBody preset back into vertex offsets and hands the main plugin a
// deformed copy, so the mannequin shows the shape the actor actually has in
// game.
//
// Two data sources, in order:
//   A. OBody NG's plugin API (GetPresetAssignedToActor) -> BodySlide preset XML
//   C. a snapshot of the "OBody" morph key RaceMenu already holds for the actor
//
// A is authoritative but depends on OBody's own bookkeeping; C is what the game
// is displaying right now and works even when the preset file is gone.

namespace
{
    using namespace OBody::API;

    std::atomic<IPluginInterface*> g_obodyInterface{ nullptr };
    std::atomic<bool>              g_obodyReady{ false };
    bool                           g_loggedSkeeMiss = false;
    bool                           g_loggedPresetCount = false;

    // ---------------------------------------------------------------- SKEE
    //
    // RaceMenu's BodyMorph interface. Declared here rather than linked, same as
    // OBody does, because skee64 publishes it through a message, not an import
    // library.
    namespace SKEE
    {
        class IPluginInterface
        {
        public:
            virtual ~IPluginInterface() = default;
            virtual std::uint32_t GetVersion() = 0;
            virtual void          Revert() = 0;
        };

        class IInterfaceMap
        {
        public:
            virtual IPluginInterface* QueryInterface(const char* a_name) = 0;
            virtual bool              AddInterface(const char* a_name, IPluginInterface* a_interface) = 0;
            virtual IPluginInterface* RemoveInterface(const char* a_name) = 0;
        };

        struct InterfaceExchangeMessage
        {
            enum : std::uint32_t
            {
                kExchangeInterface = 0x9E3779B9
            };

            IInterfaceMap* interfaceMap = nullptr;
        };

        class IBodyMorphInterface : public IPluginInterface
        {
        public:
            class MorphKeyVisitor
            {
            public:
                virtual void Visit(const char*, float) = 0;
            };

            class StringVisitor
            {
            public:
                virtual void Visit(const char*) = 0;
            };

            class ActorVisitor
            {
            public:
                virtual void Visit(RE::TESObjectREFR*) = 0;
            };

            class MorphValueVisitor
            {
            public:
                virtual void Visit(RE::TESObjectREFR*, const char*, const char*, float) = 0;
            };

            class MorphVisitor
            {
            public:
                virtual void Visit(RE::TESObjectREFR*, const char*) = 0;
            };

            virtual void  SetMorph(RE::TESObjectREFR* a_actor, const char* a_name, const char* a_key, float a_value) = 0;
            virtual float GetMorph(RE::TESObjectREFR* a_actor, const char* a_name, const char* a_key) = 0;
            virtual void  ClearMorph(RE::TESObjectREFR* a_actor, const char* a_name, const char* a_key) = 0;

            // Everything below is never called; it is here so the vtable above
            // keeps the offsets skee64 expects.
            virtual float    GetBodyMorphs(RE::TESObjectREFR*, const char*) = 0;
            virtual void     ClearBodyMorphNames(RE::TESObjectREFR*, const char*) = 0;
            virtual void     VisitMorphs(RE::TESObjectREFR*, MorphVisitor&) = 0;
            virtual void     VisitKeys(RE::TESObjectREFR*, const char*, MorphKeyVisitor&) = 0;
            virtual void     VisitMorphValues(RE::TESObjectREFR*, MorphValueVisitor&) = 0;
            virtual void     ClearMorphs(RE::TESObjectREFR*) = 0;
            virtual void     ApplyVertexDiff(RE::TESObjectREFR*, RE::NiAVObject*, bool) = 0;
            virtual void     ApplyBodyMorphs(RE::TESObjectREFR*, bool) = 0;
            virtual void     UpdateModelWeight(RE::TESObjectREFR*, bool) = 0;
            virtual void     SetCacheLimit(std::size_t) = 0;
            virtual bool     HasMorphs(RE::TESObjectREFR*) = 0;
            virtual uint32_t EvaluateBodyMorphs(RE::TESObjectREFR*) = 0;
            virtual bool     HasBodyMorph(RE::TESObjectREFR*, const char*, const char*) = 0;
            virtual bool     HasBodyMorphName(RE::TESObjectREFR*, const char*) = 0;
            virtual bool     HasBodyMorphKey(RE::TESObjectREFR*, const char*) = 0;
            virtual void     ClearBodyMorphKeys(RE::TESObjectREFR*, const char*) = 0;
            virtual void     VisitStrings(StringVisitor&) = 0;
            virtual void     VisitActors(ActorVisitor&) = 0;
            virtual size_t   ClearMorphCache() = 0;
        };

        IBodyMorphInterface* g_morph = nullptr;
    }

    class ReadinessListener : public IOBodyReadinessEventListener
    {
    public:
        void OBodyIsReady() override
        {
            g_obodyReady.store(true);
            SKSE::log::info("[OBODY] plugin API is ready");
        }

        void OBodyIsNoLongerReady() override
        {
            g_obodyReady.store(false);
            SKSE::log::info("[OBODY] plugin API is no longer ready");
        }
    };

    ReadinessListener g_readinessListener;
}

// ---------------------------------------------------------------------------
// shape
// ---------------------------------------------------------------------------

struct StmOBodyShape
{
    std::uint32_t                             source = kStmOBodySourceNone;
    std::string                               label;
    std::string                               detail;
    int                                       sex = 0;
    // Set when source == preset. The preset is looked up on the worker (see
    // ApplyImpl): scanning SliderPresets is tens of megabytes of XML and must
    // never happen on the game thread, which is where resolve() runs.
    std::string                               presetName;
    std::shared_ptr<const stm::obody::BodyPreset> preset;  // may be null until applied
    std::vector<std::pair<std::string, float>> liveMorphs;  // source == live
};

namespace
{
    namespace fs = std::filesystem;

    void CopyTo(char* a_buffer, std::uint32_t a_size, const std::string& a_text)
    {
        if (!a_buffer || a_size == 0) {
            return;
        }
        std::size_t length = (std::min)(static_cast<std::size_t>(a_text.size()),
            static_cast<std::size_t>(a_size) - 1u);
        // Never cut a UTF-8 sequence in half: preset names are user text and a
        // dangling lead byte turns into a replacement character in the UI.
        while (length > 0 && (static_cast<unsigned char>(a_text[length]) & 0xC0) == 0x80) {
            --length;
        }
        std::memcpy(a_buffer, a_text.data(), length);
        a_buffer[length] = '\0';
    }

    // Case-insensitive substring test, used for the sex the NIF path spells out.
    // "femalebody" also contains "male", so callers must test female first.
    bool ContainsNoCase(std::string_view a_haystack, std::string_view a_needle)
    {
        if (a_needle.empty() || a_needle.size() > a_haystack.size()) {
            return false;
        }
        const auto it = std::search(a_haystack.begin(), a_haystack.end(), a_needle.begin(), a_needle.end(),
            [](char a_lhs, char a_rhs) {
                return std::tolower(static_cast<unsigned char>(a_lhs)) == std::tolower(static_cast<unsigned char>(a_rhs));
            });
        return it != a_haystack.end();
    }

    bool FileExists(const fs::path& a_path)
    {
        std::error_code ec;
        return fs::is_regular_file(a_path, ec);
    }

    int DetectedImpl()
    {
        const auto dll = fs::current_path() / "Data" / "SKSE" / "Plugins" / "OBody.dll";
        const auto presets = fs::path(stm::obody::PresetDirectory());
        std::error_code ec;
        const bool hasDll = FileExists(dll);
        const bool hasPresets = fs::is_directory(presets, ec);
        return (hasDll && hasPresets) ? 1 : 0;
    }

    int ReadyImpl()
    {
        return (g_obodyInterface.load() != nullptr && g_obodyReady.load()) ? 1 : 0;
    }

    // The tri that belongs to a built body sits next to it: femalebody_1.nif
    // pairs with femalebody.tri, not femalebody_1.tri.
    std::string SiblingTriPath(const std::string& a_nifPath)
    {
        if (a_nifPath.empty()) {
            return {};
        }
        fs::path path(a_nifPath);
        auto       stem = path.stem().string();
        if (stem.size() > 2 && stem[stem.size() - 2] == '_' &&
            (stem.back() == '0' || stem.back() == '1')) {
            stem.resize(stem.size() - 2);
        }
        return (path.parent_path() / (stem + ".tri")).string();
    }

    // Collects the morphs OBody itself applied. Other keys ("RaceMenu",
    // "BodyGen", "OClothe") are deliberately ignored: the preview is meant to
    // show the preset, not whatever the player tweaked by hand.
    class OBodMorphCollector : public SKEE::IBodyMorphInterface::MorphValueVisitor
    {
    public:
        void Visit(RE::TESObjectREFR*, const char* a_name, const char* a_key, float a_value) override
        {
            if (a_name && a_key && std::strcmp(a_key, "OBody") == 0) {
                values.emplace_back(a_name, a_value);
            }
        }

        std::vector<std::pair<std::string, float>> values;
    };

    StmOBodyShape* ResolveImpl(void* a_actorPtr, int a_sex, char* a_label, std::uint32_t a_labelSize,
                               char* a_detail, std::uint32_t a_detailSize)
    {
        auto* actor = static_cast<RE::Actor*>(a_actorPtr);

        auto shape = std::make_unique<StmOBodyShape>();
        shape->sex = a_sex;

        std::string detail;
        // A preset that belongs to the other sex is refused outright rather than
        // quietly replaced by the live morphs: those belong to the actor, not to
        // the mesh on screen, so they would be just as wrong.
        bool sexMismatch = false;

        // A: OBody's own answer.
        if (actor) {
            if (auto* api = g_obodyInterface.load(); api && g_obodyReady.load()) {
                PresetAssignmentInformation info{};
                info.flags = PresetAssignmentInformation::Flags::None;
                api->GetPresetAssignedToActor(actor, info);

                if (!info.presetName.empty()) {
                    shape->label = std::string(info.presetName);
                    shape->presetName = shape->label;
                    shape->source = kStmOBodySourcePreset;

                    // Only answer "does this preset exist" when the database is
                    // already in memory and nobody is rescanning it. On the first
                    // resolve of the session it is not, and loading it here would
                    // stall the game thread for the whole scan; ApplyImpl does the
                    // lookup on the worker instead and reports the truth from there.
                    //
                    // TryFindPreset, not FindPreset: a re-scan holds the database
                    // mutex for tens of megabytes of XML, and this is the game
                    // thread. A miss here is "not known yet", never "does not
                    // exist" - the worker re-resolves it either way.
                    if (stm::obody::PresetsLoaded()) {
                        if (const auto preset = stm::obody::TryFindPreset(shape->presetName)) {
                            if (preset->sexKnown && preset->female != (a_sex != 1)) {
                                // A female preset on a male body (or the reverse)
                                // deforms the wrong vertices: the slider names are
                                // shared, so it would not fail loudly, it would
                                // just look wrong. Refuse it and leave the body
                                // neutral - the live morphs are no better here,
                                // they belong to the actor, not to this mesh.
                                detail = "preset '" + shape->label + "' is a " +
                                         (preset->female ? "female" : "male") + " preset, preview is " +
                                         ((a_sex != 1) ? "female" : "male");
                                shape->source = kStmOBodySourceNone;
                                shape->presetName.clear();
                                sexMismatch = true;
                            } else {
                                shape->preset = preset;
                                detail = "OBody preset, " + std::to_string(preset->sliders.size()) + " sliders";
                                if (std::exchange(g_loggedPresetCount, true) == false) {
                                    SKSE::log::info("[OBODY] preset database: {} presets from {}",
                                        stm::obody::TryPresetCount(), stm::obody::PresetDirectory());
                                }
                            }
                        } else if (!stm::obody::PresetsLoaded()) {
                            // A re-scan started between the two checks. Keep the
                            // preset source so ApplyImpl resolves it on the worker.
                            detail = "OBody preset '" + shape->label + "' (preset database busy)";
                        } else {
                            detail = "preset '" + shape->label + "' not found in SliderPresets";
                            shape->source = kStmOBodySourceNone;
                            shape->presetName.clear();
                        }
                    } else {
                        detail = "OBody preset '" + shape->label + "' (preset database loading)";
                    }
                } else {
                    detail = "actor has no OBody preset";
                }
            } else {
                detail = "OBody plugin API unavailable";
            }
        } else {
            detail = "no actor";
        }

        // C: what the game is actually showing.
        if (shape->source == kStmOBodySourceNone && !sexMismatch && actor && SKEE::g_morph) {
            OBodMorphCollector collector;
            SKEE::g_morph->VisitMorphValues(actor, collector);
            if (!collector.values.empty()) {
                shape->source = kStmOBodySourceLiveMorphs;
                shape->liveMorphs = std::move(collector.values);
                shape->label = "live morphs";
                detail += " - using " + std::to_string(shape->liveMorphs.size()) + " live morph values";
            }
        }

        if (shape->source == kStmOBodySourceNone) {
            CopyTo(a_label, a_labelSize, "");
            CopyTo(a_detail, a_detailSize, detail.empty() ? "nothing to apply" : detail);
            return nullptr;
        }

        shape->detail = detail;
        CopyTo(a_label, a_labelSize, shape->label);
        CopyTo(a_detail, a_detailSize, detail);

        SKSE::log::info("[OBODY] resolved '{}' for actor {:08X} ({}), {}", shape->label,
            actor ? actor->GetFormID() : 0, detail, shape->source == kStmOBodySourcePreset ? "preset" : "live");
        return shape.release();
    }

    bool ShapeFitsMesh(const stm::obody::TriShape& a_shape, std::uint32_t a_vertexCount)
    {
        return stm::obody::ShapeFits(a_shape, a_vertexCount);
    }

    int ApplyImpl(StmOBodyShape* a_shape, float* a_positions, std::uint32_t a_vertexCount, const char* a_nifPath,
                  const char* a_shapeName, float a_normalizeScale, char* a_info, std::uint32_t a_infoSize)
    {
        if (!a_shape || !a_positions || a_vertexCount == 0) {
            CopyTo(a_info, a_infoSize, "empty request");
            return 0;
        }

        const std::string triPath = SiblingTriPath(a_nifPath ? a_nifPath : "");
        const auto        tri = stm::obody::LoadTriFile(triPath);
        if (!tri) {
            CopyTo(a_info, a_infoSize, "no readable tri at " + triPath);
            SKSE::log::warn("[OBODY] {}", std::string("no readable tri at ") + triPath);
            return 0;
        }

        const stm::obody::TriShape* shape = a_shapeName ? tri->FindShape(a_shapeName) : nullptr;
        if (shape && !ShapeFitsMesh(*shape, a_vertexCount)) {
            SKSE::log::warn("[OBODY] tri shape '{}' does not fit {} vertices, falling back", shape->name, a_vertexCount);
            shape = nullptr;
        }
        if (!shape) {
            shape = tri->BestShape();
            if (shape && !ShapeFitsMesh(*shape, a_vertexCount)) {
                shape = nullptr;
            }
        }
        if (!shape) {
            CopyTo(a_info, a_infoSize, "no tri shape matches the mesh");
            return 0;
        }

        std::vector<std::pair<std::string, float>> values;
        if (a_shape->source == kStmOBodySourcePreset) {
            // Resolved here, on the worker: the first lookup scans the whole
            // SliderPresets folder and must never run on the game thread.
            auto preset = a_shape->preset;
            if (!preset) {
                preset = stm::obody::FindPreset(a_shape->presetName);
            }
            if (!preset && stm::obody::RefreshPresets()) {
                // The folder may have gained this preset since the last scan.
                preset = stm::obody::FindPreset(a_shape->presetName);
            }
            if (!preset) {
                const std::string message = "preset '" + a_shape->presetName + "' not found in SliderPresets";
                CopyTo(a_info, a_infoSize, message);
                SKSE::log::warn("[OBODY] {}", message);
                return 0;
            }

            // The resolve-time check cannot run before the database is loaded,
            // so the same rule is enforced again here where the preset is in
            // hand and the NIF path says which body we are deforming.
            const std::string_view nifPath = a_nifPath ? std::string_view(a_nifPath) : std::string_view{};
            const bool pathFemale = ContainsNoCase(nifPath, "female");
            const bool pathMale = !pathFemale && ContainsNoCase(nifPath, "male");
            if (preset->sexKnown && (pathFemale || pathMale) && preset->female != pathFemale) {
                const std::string message = "preset '" + a_shape->presetName + "' is a " +
                                            (preset->female ? "female" : "male") + " preset but the mesh is " +
                                            (pathFemale ? "female" : "male");
                CopyTo(a_info, a_infoSize, message);
                SKSE::log::warn("[OBODY] {}", message);
                return 0;
            }

            values.reserve(preset->sliders.size());
            for (const auto& slider : preset->sliders) {
                values.emplace_back(slider.name, slider.value);
            }
        } else {
            values = a_shape->liveMorphs;
        }

        stm::obody::AppliedMorphInfo applied;
        const stm::obody::VertexSpace space{ a_normalizeScale > 0.0f ? a_normalizeScale : 1.0f, true };
        if (!stm::obody::ApplyMorphs(*shape, values, a_positions, a_vertexCount, space, applied)) {
            CopyTo(a_info, a_infoSize, "preset shares no morph names with the tri");
            SKSE::log::warn("[OBODY] '{}' shares no morph names with tri shape '{}'", a_shape->label, shape->name);
            return 0;
        }

        const std::string info = "shape '" + applied.shapeName + "', " + std::to_string(applied.morphs) +
                                 " morphs / " + std::to_string(applied.offsets) + " offsets";
        CopyTo(a_info, a_infoSize, info);
        SKSE::log::info("[OBODY] applied {} to '{}'", info, a_shape->label);
        return 1;
    }

    void ReleaseImpl(StmOBodyShape* a_shape)
    {
        delete a_shape;
    }

    const char* VersionStringImpl()
    {
        return "SlaveTatsMenuOBody " STM_VERSION_STRING;
    }

    // ------------------------------------------------------------ acquisition

    void RequestOBodyInterface()
    {
        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            return;
        }

        IPluginInterface* interfacePtr = nullptr;

        SKSEMessages::RequestPluginInterface request{};
        request.version = PluginAPIVersion::Latest;
        request.pluginInterface = &interfacePtr;
        request.readinessEventListener = &g_readinessListener;

        messaging->Dispatch(SKSEMessages::RequestPluginInterface::type, &request, sizeof(request), "OBody");

        if (interfacePtr) {
            g_obodyInterface.store(interfacePtr);
            SKSE::log::info("[OBODY] plugin API acquired (version {})",
                static_cast<int>(interfacePtr->PluginAPIVersion()));
        } else {
            SKSE::log::info("[OBODY] no plugin API - OBody NG not installed (or older than 4.3.7)");
        }
    }

    void AcquireSkee()
    {
        if (SKEE::g_morph) {
            return;
        }

        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            return;
        }

        SKEE::InterfaceExchangeMessage message;
        messaging->Dispatch(SKEE::InterfaceExchangeMessage::kExchangeInterface, &message,
            sizeof(SKEE::InterfaceExchangeMessage*), "skee");

        if (!message.interfaceMap) {
            // skee64 registers its handler during its own kPostPostLoad, and
            // which of us gets there first is a load-order coin toss. The caller
            // retries at kDataLoaded, so this is not final - say it once.
            if (std::exchange(g_loggedSkeeMiss, true) == false) {
                SKSE::log::info("[OBODY] skee64 not answering yet - will retry after data load");
            }
            return;
        }

        auto* morph = static_cast<SKEE::IBodyMorphInterface*>(message.interfaceMap->QueryInterface("BodyMorph"));
        if (!morph) {
            SKSE::log::info("[OBODY] skee64 BodyMorph interface unavailable");
            return;
        }

        SKEE::g_morph = morph;
        SKSE::log::info("[OBODY] skee64 BodyMorph interface acquired (version {})", morph->GetVersion());
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_message)
    {
        switch (a_message->type) {
        case SKSE::MessagingInterface::kPostPostLoad:
            // OBody registers its API handler during its own kPostPostLoad, so
            // the request has to go out here. skee64 publishes at the same
            // stage and the winner depends on load order, hence the retry below.
            RequestOBodyInterface();
            AcquireSkee();
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            // The preset database is deliberately NOT scanned here: it is tens
            // of megabytes of XML, and the first preset lookup pays for it once
            // on the mesh worker (see ApplyImpl) rather than stalling the load
            // screen for everyone who never opens the preview.
            SKSE::log::info("[OBODY] detected={}", DetectedImpl());
            // Second chance for skee64, which may not have been listening yet.
            AcquireSkee();
            break;

        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// exports / plugin entry
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) const StmOBodyApi* StmOBodyGetApi(void)
{
    static const StmOBodyApi s_api{
        STM_OBODY_API_VERSION,
        &DetectedImpl,
        &ReadyImpl,
        &ResolveImpl,
        &ApplyImpl,
        &ReleaseImpl,
        &VersionStringImpl,
    };
    return &s_api;
}

extern "C" __declspec(dllexport) constinit auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData v;
    // Same source as the main plugin: CMakeLists.txt's project VERSION. Hard
    // coding it here is how the plugin ends up reporting one version to SKSE and
    // another in the log and in fomod/info.xml.
    v.PluginVersion(REL::Version{ STM_VERSION_MAJOR, STM_VERSION_MINOR, STM_VERSION_PATCH, 0 });
    v.PluginName("SlaveTatsMenuOBody");
    v.AuthorName("4x9M133");
    v.UsesAddressLibrary();
    v.UsesNoStructs();
    return v;
}();

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
    // SKSE packs a plugin version as major/minor/patch in the top three bytes.
    constexpr std::uint32_t kPluginVersion =
        (static_cast<std::uint32_t>(STM_VERSION_MAJOR) << 24) |
        (static_cast<std::uint32_t>(STM_VERSION_MINOR) << 16) |
        (static_cast<std::uint32_t>(STM_VERSION_PATCH) << 8);

    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = "SlaveTatsMenuOBody";
    a_info->version = kPluginVersion;

    if (a_skse->IsEditor()) {
        return false;
    }
    return a_skse->RuntimeVersion() >= SKSE::RUNTIME_SSE_1_5_39;
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);

    SKSE::log::info("[OBODY] SlaveTatsMenuOBody {} loaded (game {})", STM_VERSION_STRING,
        a_skse->RuntimeVersion().string());

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        SKSE::log::critical("[OBODY] messaging interface unavailable");
        return false;
    }

    messaging->RegisterListener(OnMessage);
    return true;
}
