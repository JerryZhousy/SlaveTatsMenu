#include "PCH.h"

#include "bridge/JcApi.h"
#include "bridge/SlaveTatsNg.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Tasks.h"
#include "data/TattooDatabase.h"
#include "diag/CallTrace.h"
#include "diag/Probe.h"
#include "render/MannequinRenderer.h"
#include "ui/InputHandler.h"
#include "ui/Ui.h"

#ifndef DLLEXPORT
#    define DLLEXPORT __declspec(dllexport)
#endif

namespace
{
    void OnMessage(SKSE::MessagingInterface::Message* a_message)
    {
        switch (a_message->type) {
        case SKSE::MessagingInterface::kPostLoad:
            {
                // SlaveTatsNG publishes its interface during kPostPostLoad, and
                // JContainers during the same stage - listening from kPostLoad
                // guarantees we are registered before either broadcast.
                stm::ng::InstallListener();
                stm::jc::InstallListener();

                // Same reasoning: the call tracer has to be listening before the
                // interface is broadcast so it can wrap the table in place.
                stm::diag::InstallCallTrace();
            }
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            {
                // SlaveTatsNG only compiles its tattoo cache once a game is
                // actually loaded, so at kDataLoaded the API still returns an
                // empty list and the catalogue has to fall back to the JSON dump.
                // Retry now that the cache exists, so the authoritative source
                // wins for the rest of the session.
                SKSE::log::info("Boot: refreshing the tattoo catalogue now that a game is loaded");
                stm::data::TattooDatabase::Get().Rebuild(/*a_allowCompile=*/true);
            }
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            {
                stm::jc::BindReflection();
                stm::Config::Get().Load();

                // The bridge installs the tracer the moment it receives the
                // interface; the INI decides whether it stays on.
                if (stm::Config::Get().callTraceEnabled) {
                    stm::diag::InstallCallTrace();
                } else {
                    stm::diag::UninstallCallTrace();
                }

                // Warm up the slot limits on this single thread: they are loaded
                // lazily from skee64.ini and would otherwise be raced for by the
                // task queue and the render callback.
                for (const char* area : { "Body", "Face", "Hands", "Feet" }) {
                    (void)stm::ng::MaxSlots(area);
                }

                stm::data::TattooDatabase::Get().Rebuild();

                stm::ui::InstallInputHandler();
                stm::ui::Initialize();

                if (stm::Config::Get().probeEnabled) {
                    stm::diag::Start();
                }

                SKSE::log::info("Boot: ready (SlaveTatsNG={}, JContainers={}, library={} from {}, callTrace={})",
                    stm::ng::Ready() ? "ok" : "missing",
                    stm::jc::Ready() ? "ok" : "missing",
                    stm::data::TattooDatabase::Get().Size(),
                    stm::data::TattooDatabase::Get().SourceName(),
                    stm::diag::CallTraceInstalled() ? "on" : "off");
            }
            break;

        default:
            break;
        }
    }
}

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() {
    SKSE::PluginVersionData v;
    v.PluginVersion(REL::Version{ 1, 0, 0, 0 });
    v.PluginName("SlaveTatsMenu");
    v.AuthorName("4x9M133");
    v.UsesAddressLibrary();
    v.UsesNoStructs();
    return v;
}();

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = "SlaveTatsMenu";
    a_info->version = 1;

    if (a_skse->IsEditor()) {
        return false;
    }

    if (a_skse->RuntimeVersion() < SKSE::RUNTIME_SSE_1_5_39) {
        return false;
    }

    return true;
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    stm::log::Init();

    stm::LoadThreadId() = stm::ThisThreadId();

    SKSE::log::info("SlaveTatsMenu {} loaded (game {}, load thread {})", "1.0.0", a_skse->RuntimeVersion().string(), stm::LoadThreadId());

    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        SKSE::log::critical("Boot: messaging interface unavailable");
        return false;
    }

    std::atexit([] {
        stm::diag::Stop();
        stm::render::MannequinRenderer::Get().Shutdown();
    });

    messaging->RegisterListener(OnMessage);
    return true;
}
