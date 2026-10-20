#include "obody/OBodyBridge.h"

#include "core/Config.h"
#include "core/Tasks.h"
#include "data/TargetState.h"
#include "render/MannequinRenderer.h"

#include <atomic>

namespace stm::obody
{
    namespace
    {
        const StmOBodyApi* g_api = nullptr;
        HMODULE            g_module = nullptr;

        std::mutex g_mutex;
        ShapePtr   g_resolved;

        std::atomic<bool> g_resolving{ false };
        bool              g_loggedMissing = false;

        struct ShapeDeleter
        {
            void operator()(Shape* a_shape) const noexcept
            {
                if (!a_shape) {
                    return;
                }
                if (a_shape->handle && g_api && g_api->release) {
                    g_api->release(a_shape->handle);
                }
                delete a_shape;
            }
        };
    }

    void Initialize()
    {
        g_module = ::GetModuleHandleW(L"SlaveTatsMenuOBody.dll");
        if (!g_module) {
            if (!g_loggedMissing) {
                g_loggedMissing = true;
                SKSE::log::info("OBody: add-on not installed - the preview keeps the zeroed body");
            }
            return;
        }

        const auto getApi = reinterpret_cast<StmOBodyGetApiFn>(::GetProcAddress(g_module, "StmOBodyGetApi"));
        if (!getApi) {
            SKSE::log::warn("OBody: add-on loaded but StmOBodyGetApi is missing");
            return;
        }

        const auto* api = getApi();
        if (!api || api->version < STM_OBODY_API_VERSION) {
            SKSE::log::warn("OBody: add-on API version mismatch (have {}, need {})",
                api ? api->version : 0u, STM_OBODY_API_VERSION);
            return;
        }

        g_api = api;

        SKSE::log::info("OBody: add-on {} (detected={}, ready={})", AddonVersion(), Detected(), Available());
    }

    bool Available()
    {
        return g_api && g_api->resolve && g_api->apply;
    }

    bool Detected()
    {
        return g_api && g_api->detected && g_api->detected() != 0;
    }

    const char* AddonVersion()
    {
        return (g_api && g_api->versionString) ? g_api->versionString() : "unknown";
    }

    void RequestResolve()
    {
        if (!Available() || !Config::Get().previewUseOBody) {
            return;
        }
        if (g_resolving.exchange(true)) {
            return;
        }

        const int sex = render::MannequinRenderer::Get().GetCurrentSex();

        const bool posted = PostToGameThread([sex] {
            // Clears the in-flight flag on every exit path, including a throw
            // from the preset lookup: a wedged flag would silently disable the
            // preview for the rest of the session.
            struct InFlightGuard
            {
                ~InFlightGuard() { g_resolving.store(false); }
            } guard;

            if (!g_api) {
                return;
            }

            auto* actor = data::TargetState::Get().Resolve();

            char label[256]{};
            char detail[512]{};
            auto* handle = g_api->resolve(actor, sex, label, sizeof(label), detail, sizeof(detail));

            auto shape = ShapePtr(new Shape{ handle, label, detail, actor ? actor->GetFormID() : 0u, sex },
                ShapeDeleter{});

            std::scoped_lock lock(g_mutex);
            g_resolved = std::move(shape);
        });

        if (!posted) {
            g_resolving.store(false);
        }
    }

    ShapePtr TakeResolved()
    {
        std::scoped_lock lock(g_mutex);
        return std::move(g_resolved);
    }

    bool Apply(const ShapePtr& a_shape, float* a_positions, std::uint32_t a_count,
               const std::string& a_nifPath, const std::string& a_shapeName, float a_normalizeScale,
               std::string& a_info)
    {
        if (!g_api || !g_api->apply || !a_shape || !a_shape->handle) {
            return false;
        }

        char info[256]{};
        const int applied = g_api->apply(a_shape->handle, a_positions, a_count, a_nifPath.c_str(),
            a_shapeName.c_str(), a_normalizeScale, info, sizeof(info));
        a_info = info;
        return applied != 0;
    }
}
