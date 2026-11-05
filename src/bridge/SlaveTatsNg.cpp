#include "bridge/SlaveTatsNg.h"

#include "SlaveTatsNG_Interface.h"
#include "SimpleIni.h"

#include "core/Config.h"
#include "core/Log.h"
#include "diag/CallTrace.h"

#include "bridge/JcApi.h"
#include "bridge/NgGuardedCall.h"

#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstring>

namespace stm::ng
{
    namespace
    {
        // Conservative fallbacks; overwritten from skee64.ini when readable.
        struct SlotLimits
        {
            int body = 12;
            int face = 6;
            int hands = 6;
            int feet = 6;
        };

        // Upstream's upgrade_tattoos() compares .SlaveTats.version against a
        // string literal compiled into SlaveTatsNG itself. We cannot read that
        // literal, and hard-coding a guess means that the day upstream bumps it,
        // the stamp we just wrote becomes a mismatch and triggers the very
        // ledger rebuild the stamp exists to prevent.
        //
        // So the constant below is only a bootstrap. As soon as we see an actor
        // whose .SlaveTats.version is already set, that value was written by
        // upstream and is therefore authoritative - adopt it. If our bootstrap
        // guess was wrong, upstream rebuilds once, writes its own value, and the
        // next synchronisation adopts it: the stamp self-corrects instead of
        // staying wrong forever.
        constexpr const char* kBootstrapLedgerVersion = "2.0.0";
        std::string           g_ledgerVersion = kBootstrapLedgerVersion;

        // Holds the temporary JContainers array ExternalSlots() reads its answer
        // out of. Namespace scope because a local struct cannot capture it.
        constexpr std::string_view kExternalPool = "SlaveTatsMenu-Ng";

        // BSFixedString interns a copy, but the source must stay alive for the
        // duration of the call - hence the local std::string at each call site.
        RE::BSFixedString Bs(const std::string& a_str)
        {
            return RE::BSFixedString(a_str.c_str());
        }

        // ------------------------------------------------- interface table
        //
        // SlaveTatsNG publishes a struct of function pointers and has appended
        // to it twice (external_slots, then compile_cache) WITHOUT bumping the
        // version it reports - the header's own comment says as much: "strictly
        // this isn't version one anymore". So "version 1" does not tell us how
        // long the table is, and reading a field that a given build never wrote
        // reads whatever sits behind the struct in its .rdata. Calling that is a
        // jump to a garbage address: instant CTD, nothing in the log, and the
        // crash looks like ours because we were the caller.
        //
        // Every entry this plugin calls therefore goes through Entry(), which
        // checks that the slot is mapped and that the value in it points at
        // executable memory before anything is called.

        bool IsExecutableAddress(const void* a_ptr)
        {
            if (!a_ptr) {
                return false;
            }

            MEMORY_BASIC_INFORMATION info{};
            if (::VirtualQuery(a_ptr, &info, sizeof(info)) != sizeof(info)) {
                return false;
            }
            if (info.State != MEM_COMMIT) {
                return false;
            }

            const DWORD protection = info.Protect & 0xFF;
            return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
                   protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        }

        template <class Fn>
        Fn Entry(const slavetats::Addresses* a_table, std::size_t a_offset)
        {
            const auto* slot = reinterpret_cast<const std::byte*>(a_table) + a_offset;

            MEMORY_BASIC_INFORMATION info{};
            if (::VirtualQuery(slot, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT) {
                return nullptr;
            }

            Fn value{};
            std::memcpy(&value, slot, sizeof(value));

            const auto raw = reinterpret_cast<std::uintptr_t>(value);
            return IsExecutableAddress(reinterpret_cast<const void*>(raw)) ? value : nullptr;
        }

        // One line at interface hand-over that says which of the entries this
        // plugin uses actually exist in the build that is installed. An older
        // SlaveTatsNG shows up here as "missing: external_slots, compile_cache"
        // instead of as a crash an hour later.
        void AuditInterface(const slavetats::Addresses* a_table)
        {
            struct Check
            {
                const char* name;
                std::size_t offset;
            };

            const Check checks[] = {
                { "simple_add_tattoo", offsetof(slavetats::Addresses, simple_add_tattoo) },
                { "simple_remove_tattoo", offsetof(slavetats::Addresses, simple_remove_tattoo) },
                { "synchronize_tattoos", offsetof(slavetats::Addresses, synchronize_tattoos) },
                { "query_available_tattoos", offsetof(slavetats::Addresses, query_available_tattoos) },
                { "remove_tattoos", offsetof(slavetats::Addresses, remove_tattoos) },
                { "remove_tattoo_from_slot", offsetof(slavetats::Addresses, remove_tattoo_from_slot) },
                { "get_applied_tattoo_in_slot", offsetof(slavetats::Addresses, get_applied_tattoo_in_slot) },
                { "external_slots", offsetof(slavetats::Addresses, external_slots) },
                { "add_tattoo", offsetof(slavetats::Addresses, add_tattoo) },
                { "add_and_get_tattoo", offsetof(slavetats::Addresses, add_and_get_tattoo) },
                { "get_form", offsetof(slavetats::Addresses, get_form) },
                { "compile_cache", offsetof(slavetats::Addresses, compile_cache) },
            };

            std::string missing;
            int         present = 0;
            for (const auto& check : checks) {
                if (Entry<void*>(a_table, check.offset)) {
                    ++present;
                } else {
                    if (!missing.empty()) {
                        missing += ", ";
                    }
                    missing += check.name;
                }
            }

            if (missing.empty()) {
                SKSE::log::info("SlaveTatsNG: interface audit - {}/{} entries usable", present, static_cast<int>(std::size(checks)));
            } else {
                SKSE::log::warn("SlaveTatsNG: interface audit - {}/{} entries usable, missing: {} (the installed SlaveTatsNG is older than the header this build was compiled against; those features will be off)",
                    present, static_cast<int>(std::size(checks)), missing);
            }
        }

        SlotLimits ParseSlotLimits()
        {
            SlotLimits limits;

            CSimpleIniA ini;
            ini.SetUnicode();

            for (const char* path : {
                     "Data/SKSE/Plugins/skee64.ini",
                     "Data/SKSE/Plugins/skeevr.ini",
                     "Data/SKSE/Plugins/skee.ini",
                     "Data/SKSE/Plugins/nioverride.ini" }) {
                if (ini.LoadFile(path) < 0) {
                    continue;
                }

                auto parseSlot = [&](const char* section, int fallback) {
                    const char* val = ini.GetValue(section, "iNumOverlays", nullptr);
                    if (!val || !*val) {
                        return fallback;
                    }
                    while (*val && std::isspace(static_cast<unsigned char>(*val))) {
                        ++val;
                    }
                    char* end = nullptr;
                    long parsed = std::strtol(val, &end, 0);
                    return (end == val || parsed <= 0) ? fallback : static_cast<int>(parsed);
                };

                limits.body = parseSlot("Overlays/Body", limits.body);
                limits.face = parseSlot("Overlays/Face", limits.face);
                limits.hands = parseSlot("Overlays/Hands", limits.hands);
                limits.feet = parseSlot("Overlays/Feet", limits.feet);

                SKSE::log::info("SlaveTatsNG: slot limits from {} -> Body={} Face={} Hands={} Feet={}", path, limits.body, limits.face, limits.hands, limits.feet);
                return limits;
            }

            SKSE::log::warn("SlaveTatsNG: skee64.ini not readable, using default slot limits");
            return limits;
        }

        // Parsed once, on first use. A function-local static provides the
        // synchronisation the old hand-rolled `loaded` flag did not: MaxSlots()
        // is called from the render thread, the task queue and the probe thread,
        // and the flag was a plain bool.
        const SlotLimits& Slots()
        {
            static const SlotLimits s_limits = ParseSlotLimits();
            return s_limits;
        }

        const slavetats::Addresses* Iface()
        {
            return slavetats::singleton::get()->iface;
        }

        Status FromFailT(bool a_fail)
        {
            return a_fail ? Status::kFailed : Status::kOk;
        }

        // The two high level entry points do not follow fail_t. Both end with
        // "return true" after doing the work and return false on every path
        // that did not: unknown tattoo, already applied, add failed, and the
        // internal synchronise being aborted. Reading them through FromFailT
        // inverted the answer, so a successful add looked like a failure (which
        // is why the preset fallback never counted one) and a dropped sync
        // looked like success (which is how a tattoo ends up in the ledger and
        // not on the body while every log line says "ok").
        Status FromHighLevel(bool a_ok)
        {
            return a_ok ? Status::kOk : Status::kFailed;
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
                AuditInterface(iface);

                // SKSE hands a sender's message to one listener per plugin, so the
                // diagnostic tracer cannot listen for this itself - it hooks in
                // here, as early as the table pointer exists.
                if (stm::Config::Get().callTraceEnabled) {
                    stm::diag::InstallCallTrace();
                }
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

        // NOTE: upstream returns false both when the tattoo does not exist and
        // when it is already applied (and in the latter case it does NOT update
        // colour/alpha), and it also returns false when the synchronise it was
        // asked to run was dropped. Callers must verify the ledger instead of
        // trusting this return value; when the ledger did change, false means
        // the visual sync did not happen.
        return FromHighLevel(iface->simple_add_tattoo(a_actor, Bs(section), Bs(name), static_cast<int>(a_color), a_sync, false, a_alpha));
    }

    Status SimpleRemoveTattoo(RE::Actor* a_actor, std::string_view a_section, std::string_view a_name, bool a_sync)
    {
        const auto* iface = Iface();
        if (!iface || !a_actor) {
            return Status::kUnavailable;
        }

        const std::string section(a_section);
        const std::string name(a_name);

        return FromHighLevel(iface->simple_remove_tattoo(a_actor, Bs(section), Bs(name), a_sync, false));
    }

    Status Synchronize(RE::Actor* a_actor, bool a_forceUpdated)
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
        //
        // See g_ledgerVersion above for why the stamped value is learned rather
        // than hard-coded.
        if (jc::Ready()) {
            const auto current = jc::FormDbGetStr(a_actor, ".SlaveTats.version");

            if (current.empty()) {
                jc::FormDbSetStr(a_actor, ".SlaveTats.version", g_ledgerVersion);

                const auto stamped = jc::FormDbGetStr(a_actor, ".SlaveTats.version");
                SKSE::log::info("SlaveTatsNG: stamped .SlaveTats.version = '{}' (was empty) so synchronisation stops discarding the ledger", stamped);
            } else if (current != g_ledgerVersion) {
                // A non-empty value came from upstream, so it beats our guess.
                SKSE::log::info("SlaveTatsNG: upstream uses .SlaveTats.version = '{}' (we were stamping '{}'), switching to it",
                    current, g_ledgerVersion);
                g_ledgerVersion = current;
            }

            // synchronize_tattoos_internal cancels without touching a single
            // overlay while ".SlaveTats.updated" is 0, and returns "no error"
            // when it does. Every path inside this plugin raises the flag as
            // part of its write, so the default keeps upstream's own check
            // meaningful; a user-requested force sync raises it first, because
            // a ledger written by a mod that never raised the flag could not be
            // pushed onto the body any other way.
            if (a_forceUpdated) {
                jc::FormDbSetInt(a_actor, ".SlaveTats.updated", 1);
            }
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
        if (!iface) {
            return -1;
        }

        // Appended to the table after external_slots, so on an older build this
        // slot is past the end of the struct - see the note above Entry().
        const auto compile_cache = Entry<int (*)()>(iface, offsetof(slavetats::Addresses, compile_cache));
        if (!compile_cache) {
            return -1;
        }
        return compile_cache();
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

    bool ExternalSlots(RE::Actor* a_actor, std::string_view a_area, std::vector<int>& a_out)
    {
        a_out.clear();

        const auto* iface = Iface();
        if (!iface || !a_actor || !jc::Ready()) {
            return false;
        }

        // Validated, not merely non-null: on a SlaveTatsNG older than the header
        // this build was compiled against the slot does not exist at all and the
        // bytes behind the struct are not a function pointer.
        const auto external_slots = Entry<ExternalSlotsFn>(iface, offsetof(slavetats::Addresses, external_slots));
        if (!external_slots) {
            log::Once("ng-no-external-slots", spdlog::level::warn,
                "SlaveTatsNG: this build does not publish external_slots() - slots held by RaceMenu or another overlay mod will look empty, and nothing here will write into them");
            return false;
        }

        // A fault inside upstream is not retried: whatever it was doing, doing it
        // again every quarter second would turn one bad call into a crash loop.
        static std::atomic_bool s_probeDisabled{ false };
        if (s_probeDisabled.load()) {
            return false;
        }

        // Upstream only ever calls this once the actor's 3D exists - it reads
        // the NiOverride node the overlay was written into - and the answer is
        // meaningless before that anyway. Same precondition here.
        if (!a_actor->Is3DLoaded()) {
            return false;
        }

        // The result is handed back through a JContainers array, so this needs
        // the integration to be bound even though the interface call itself is
        // native. Without it there is nothing to read the answer out of.
        struct PoolGuard
        {
            ~PoolGuard() { jc::CleanPool(kExternalPool); }
        } guard;

        const auto matches = jc::AddToPool(jc::CreateArray(), kExternalPool);
        if (!matches) {
            return false;
        }

        const std::string area(a_area);

        unsigned long     exceptionCode = 0;
        const GuardedResult result = CallExternalSlotsGuarded(iface, external_slots, a_actor, area.c_str(), matches.id, exceptionCode);

        if (result == GuardedResult::kFaulted) {
            s_probeDisabled.store(true);
            SKSE::log::error("SlaveTatsNG: external_slots() raised 0x{:X} - external slot detection is OFF for the rest of this session. "
                             "Nothing in this plugin writes into those slots any more, but they will be listed as empty. "
                             "Please report this with the log: it means the installed SlaveTatsNG and the interface this build was compiled against disagree.",
                exceptionCode);
            return false;
        }

        if (result == GuardedResult::kUpstream) {
            // fail_t: true means error, not success.
            return false;
        }

        const auto count = jc::ArrayCount(matches);
        if (count > 0) {
            a_out.reserve(static_cast<std::size_t>(count));
        }
        for (std::int32_t i = 0; i < count; ++i) {
            const auto slot = jc::ArrayGetInt(matches, i, -1);
            if (slot >= 0) {
                a_out.push_back(static_cast<int>(slot));
            }
        }

        // Said once per area: this is the line that proves the probe is live on
        // a machine where the slot panel looks wrong, and how many slots the
        // other overlay mods actually hold there.
        const std::string key = "ng-external-first-" + std::string(a_area);
        log::Once(key, spdlog::level::info, "SlaveTatsNG: external_slots({}) answered - {} of {} slot(s) belong to another overlay mod",
            a_area, a_out.size(), static_cast<std::size_t>(MaxSlots(a_area)));

        return true;
    }

    int MaxSlots(std::string_view a_area)
    {
        const auto& limits = Slots();

        if (a_area == "Body") {
            return limits.body;
        }
        if (a_area == "Face") {
            return limits.face;
        }
        if (a_area == "Hands") {
            return limits.hands;
        }
        if (a_area == "Feet") {
            return limits.feet;
        }
        return 0;
    }
}
