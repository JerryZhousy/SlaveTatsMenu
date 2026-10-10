#include "bridge/SlaveTatsNg.h"

#include "SlaveTatsNG_Interface.h"
#include "SimpleIni.h"

#include "core/Log.h"
#include "diag/CallTrace.h"

#include "bridge/JcApi.h"

namespace stm::ng
{
    namespace
    {
        // Conservative fallbacks; overwritten from skee64.ini when readable.
        struct SlotLimits
        {
            int  body = 12;
            int  face = 6;
            int  hands = 6;
            int  feet = 6;
            bool loaded = false;
        };

        SlotLimits g_slots;

        // BSFixedString interns a copy, but the source must stay alive for the
        // duration of the call - hence the local std::string at each call site.
        RE::BSFixedString Bs(const std::string& a_str)
        {
            return RE::BSFixedString(a_str.c_str());
        }

        void LoadSlotLimits()
        {
            if (g_slots.loaded) {
                return;
            }

            CSimpleIniA ini;
            ini.SetUnicode();

            for (const char* path : { "Data/SKSE/Plugins/skee64.ini", "Data/SKSE/Plugins/nioverride.ini" }) {
                if (ini.LoadFile(path) < 0) {
                    continue;
                }

                g_slots.body = static_cast<int>(ini.GetLongValue("Overlays/Body", "iNumOverlays", g_slots.body));
                g_slots.face = static_cast<int>(ini.GetLongValue("Overlays/Face", "iNumOverlays", g_slots.face));
                g_slots.hands = static_cast<int>(ini.GetLongValue("Overlays/Hands", "iNumOverlays", g_slots.hands));
                g_slots.feet = static_cast<int>(ini.GetLongValue("Overlays/Feet", "iNumOverlays", g_slots.feet));
                g_slots.loaded = true;

                SKSE::log::info("SlaveTatsNG: slot limits from {} -> Body={} Face={} Hands={} Feet={}", path, g_slots.body, g_slots.face, g_slots.hands, g_slots.feet);
                return;
            }

            SKSE::log::warn("SlaveTatsNG: skee64.ini not readable, using default slot limits");
            g_slots.loaded = true;
        }

        const slavetats::Addresses* Iface()
        {
            return slavetats::singleton::get()->iface;
        }

        Status FromFailT(bool a_fail)
        {
            return a_fail ? Status::kFailed : Status::kOk;
        }
    }

    const char* ToString(Status a_status)
    {
        switch (a_status) {
        case Status::kOk:
            return "ok";
        case Status::kFailed:
            return "failed";
        default:
            return "unavailable";
        }
    }

    void InstallListener()
    {
        const auto messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            SKSE::log::error("SlaveTatsNG: no messaging interface, cannot listen for the C++ API");
            return;
        }

        messaging->RegisterListener("SlaveTatsNG", [](SKSE::MessagingInterface::Message* a_msg) {
            if (!a_msg || a_msg->type != slavetats::MessageType::Interface) {
                return;
            }

            const auto* iface = slavetats::Addresses::from_void(a_msg->data);
            if (iface) {
                slavetats::singleton::get()->iface = iface;
                SKSE::log::info("SlaveTatsNG: C++ interface acquired (version {})", iface->current_version);

                // SKSE hands a sender's message to one listener per plugin, so the
                // diagnostic tracer cannot listen for this itself - it hooks in
                // here, as early as the table pointer exists.
                stm::diag::InstallCallTrace();
            } else {
                SKSE::log::error("SlaveTatsNG: interface version mismatch, refusing to use it");
            }
        });
    }

    bool Ready()
    {
        return Iface() != nullptr;
    }

    bool EnsureReady(std::string_view a_reason)
    {
        if (Ready()) {
            return true;
        }

        log::Once(std::string("ng-missing:") + std::string(a_reason), spdlog::level::warn, "SlaveTatsNG: C++ interface unavailable ({}) - is SlaveTatsNG installed and loaded?", a_reason);
        return false;
    }

    Status SimpleAddTattoo(RE::Actor* a_actor, std::string_view a_section, std::string_view a_name, std::uint32_t a_color, float a_alpha, bool a_sync)
    {
        const auto* iface = Iface();
        if (!iface || !a_actor) {
            return Status::kUnavailable;
        }

        const std::string section(a_section);
        const std::string name(a_name);

        // NOTE: upstream returns "no error" (false) both when the tattoo does
        // not exist and when it is already applied, and in the latter case it
        // does NOT update colour/alpha. Callers must verify the ledger instead
        // of trusting this return value.
        return FromFailT(iface->simple_add_tattoo(a_actor, Bs(section), Bs(name), static_cast<int>(a_color), a_sync, false, a_alpha));
    }

    Status SimpleRemoveTattoo(RE::Actor* a_actor, std::string_view a_section, std::string_view a_name, bool a_sync)
    {
        const auto* iface = Iface();
        if (!iface || !a_actor) {
            return Status::kUnavailable;
        }

        const std::string section(a_section);
        const std::string name(a_name);

        return FromFailT(iface->simple_remove_tattoo(a_actor, Bs(section), Bs(name), a_sync, false));
    }

    Status Synchronize(RE::Actor* a_actor)
    {
        const auto* iface = Iface();
        if (!iface || !a_actor) {
            return Status::kUnavailable;
        }

        // Upstream's upgrade_tattoos() wipes the entire ledger and rebuilds it from
        // the 3D nodes whenever .SlaveTats.version does not equal its own version,
        // keeping only the texture path - colour, glow and gloss are dropped (the
        // author's own TODO). An *empty* version therefore means every single
        // synchronisation throws the ledger away, which also silently undoes the
        // colour changes this menu just wrote. Stamping the version makes
        // upgrade_tattoos() return early instead.
        if (jc::Ready() && jc::FormDbGetStr(a_actor, ".SlaveTats.version").empty()) {
            jc::FormDbSetStr(a_actor, ".SlaveTats.version", "2.0.0");

            const auto stamped = jc::FormDbGetStr(a_actor, ".SlaveTats.version");
            SKSE::log::info("SlaveTatsNG: stamped .SlaveTats.version = '{}' (was empty) so synchronisation stops discarding the ledger", stamped);
        }

        return FromFailT(iface->synchronize_tattoos(a_actor, false));
    }

    Status RemoveFromSlot(RE::Actor* a_actor, std::string_view a_area, int a_slot, bool a_ignoreLock)
    {
        const auto* iface = Iface();
        if (!iface || !a_actor || a_slot < 0) {
            return Status::kUnavailable;
        }

        if (a_slot >= MaxSlots(a_area)) {
            return Status::kFailed;
        }

        const std::string area(a_area);

        return FromFailT(iface->remove_tattoo_from_slot(a_actor, Bs(area), a_slot, a_ignoreLock, false));
    }

    Status AddAndGetTattoo(RE::Actor* a_actor, std::int32_t a_template, int a_slot, std::int32_t& a_outHandle)
    {
        const auto* iface = Iface();
        if (!iface || !a_actor || a_template <= 0) {
            return Status::kUnavailable;
        }

        const int handle = iface->add_and_get_tattoo(a_actor, a_template, a_slot, true, false, true);
        if (handle <= 0) {
            return Status::kFailed;
        }

        a_outHandle = handle;
        return Status::kOk;
    }

    Status QueryAvailable(std::int32_t a_templateHandle, std::int32_t a_matchesHandle)
    {
        const auto* iface = Iface();
        if (!iface) {
            return Status::kUnavailable;
        }

        const RE::BSFixedString domain("default");
        return FromFailT(iface->query_available_tattoos(a_templateHandle, a_matchesHandle, 0, domain));
    }

    int CompileCache()
    {
        const auto* iface = Iface();
        if (!iface || !iface->compile_cache) {
            return -1;
        }
        return iface->compile_cache();
    }

    std::int32_t GetAppliedInSlot(RE::Actor* a_actor, std::string_view a_area, int a_slot)
    {
        const auto* iface = Iface();
        if (!iface || !a_actor || a_slot < 0) {
            return 0;
        }

        const std::string area(a_area);

        return iface->get_applied_tattoo_in_slot(a_actor, Bs(area), a_slot);
    }

    int MaxSlots(std::string_view a_area)
    {
        LoadSlotLimits();

        if (a_area == "Body") {
            return g_slots.body;
        }
        if (a_area == "Face") {
            return g_slots.face;
        }
        if (a_area == "Hands") {
            return g_slots.hands;
        }
        if (a_area == "Feet") {
            return g_slots.feet;
        }
        return 0;
    }
}
