#include "diag/CallTrace.h"

#include "SlaveTatsNG_Interface.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <mutex>

namespace stm::diag
{
    namespace
    {
        std::mutex g_traceMutex;

        struct Shim
        {
            slavetats::Addresses  original{};
            slavetats::Addresses* table = nullptr;
            bool                  installed = false;
        };

        Shim g_shim;

        // Resolves the wrapper's caller into "Module.dll+0x1234". _ReturnAddress()
        // is the address the wrapper will return to, i.e. the instruction right
        // after the indirect call inside the caller's module.
        const char* CallerTag(char (&a_buf)[260])
        {
            const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());

            HMODULE module = nullptr;
            if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(ret), &module) &&
                module) {
                char path[MAX_PATH]{};
                if (::GetModuleFileNameA(module, path, MAX_PATH)) {
                    const char* base = std::strrchr(path, '\\');
                    base = base ? base + 1 : path;
                    std::snprintf(a_buf, sizeof(a_buf), "%s+0x%llX", base,
                        static_cast<unsigned long long>(ret - reinterpret_cast<std::uintptr_t>(module)));
                    return a_buf;
                }
            }

            std::snprintf(a_buf, sizeof(a_buf), "0x%llX", static_cast<unsigned long long>(ret));
            return a_buf;
        }

        // The shims are stored in function pointers, so they cannot be inlined
        // away; noinline keeps the frame - and therefore _ReturnAddress() - exact.
        __declspec(noinline) slavetats::fail_t ShimSimpleAdd(RE::Actor* a_actor, RE::BSFixedString a_section, RE::BSFixedString a_name, int a_color, bool a_last, bool a_silent, float a_alpha)
        {
            char        buf[260];
            const char* who = CallerTag(buf);

            const auto result = g_shim.original.simple_add_tattoo(a_actor, a_section, a_name, a_color, a_last, a_silent, a_alpha);

            SKSE::log::info("[CALLTRACE] {} -> simple_add_tattoo(actor=0x{:X}, '{}'/'{}', color=0x{:X}, last={}, silent={}, alpha={:.2f}) = {}",
                who, reinterpret_cast<std::uintptr_t>(a_actor), a_section.c_str(), a_name.c_str(), a_color, a_last, a_silent, a_alpha, result);
            return result;
        }

        __declspec(noinline) slavetats::fail_t ShimSimpleRemove(RE::Actor* a_actor, RE::BSFixedString a_section, RE::BSFixedString a_name, bool a_last, bool a_silent)
        {
            char        buf[260];
            const char* who = CallerTag(buf);

            const auto result = g_shim.original.simple_remove_tattoo(a_actor, a_section, a_name, a_last, a_silent);

            SKSE::log::info("[CALLTRACE] {} -> simple_remove_tattoo(actor=0x{:X}, '{}'/'{}', last={}, silent={}) = {}",
                who, reinterpret_cast<std::uintptr_t>(a_actor), a_section.c_str(), a_name.c_str(), a_last, a_silent, result);
            return result;
        }

        __declspec(noinline) slavetats::fail_t ShimSynchronize(RE::Actor* a_actor, bool a_silent)
        {
            char        buf[260];
            const char* who = CallerTag(buf);

            const auto result = g_shim.original.synchronize_tattoos(a_actor, a_silent);

            SKSE::log::info("[CALLTRACE] {} -> synchronize_tattoos(actor=0x{:X}, silent={}) = {}",
                who, reinterpret_cast<std::uintptr_t>(a_actor), a_silent, result);
            return result;
        }

        __declspec(noinline) slavetats::fail_t ShimAddTattoo(RE::Actor* a_actor, int a_tattoo, int a_slot, bool a_ignoreLock, bool a_silent)
        {
            char        buf[260];
            const char* who = CallerTag(buf);

            const auto result = g_shim.original.add_tattoo(a_actor, a_tattoo, a_slot, a_ignoreLock, a_silent);

            SKSE::log::info("[CALLTRACE] {} -> add_tattoo(actor=0x{:X}, tattoo={}, slot={}, ignoreLock={}, silent={}) = {}",
                who, reinterpret_cast<std::uintptr_t>(a_actor), a_tattoo, a_slot, a_ignoreLock, a_silent, result);
            return result;
        }

        __declspec(noinline) int ShimAddAndGetTattoo(RE::Actor* a_actor, int a_tattoo, int a_slot, bool a_ignoreLock, bool a_silent, bool a_tryUpgrade)
        {
            char        buf[260];
            const char* who = CallerTag(buf);

            const int result = g_shim.original.add_and_get_tattoo(a_actor, a_tattoo, a_slot, a_ignoreLock, a_silent, a_tryUpgrade);

            SKSE::log::info("[CALLTRACE] {} -> add_and_get_tattoo(actor=0x{:X}, tattoo={}, slot={}, ignoreLock={}, silent={}, tryUpgrade={}) = {}",
                who, reinterpret_cast<std::uintptr_t>(a_actor), a_tattoo, a_slot, a_ignoreLock, a_silent, a_tryUpgrade, result);
            return result;
        }

        __declspec(noinline) slavetats::fail_t ShimRemoveTattoos(RE::Actor* a_actor, int a_template, bool a_ignoreLock, bool a_silent)
        {
            char        buf[260];
            const char* who = CallerTag(buf);

            const auto result = g_shim.original.remove_tattoos(a_actor, a_template, a_ignoreLock, a_silent);

            SKSE::log::info("[CALLTRACE] {} -> remove_tattoos(actor=0x{:X}, template={}, ignoreLock={}, silent={}) = {}",
                who, reinterpret_cast<std::uintptr_t>(a_actor), a_template, a_ignoreLock, a_silent, result);
            return result;
        }

        __declspec(noinline) slavetats::fail_t ShimRemoveFromSlot(RE::Actor* a_actor, RE::BSFixedString a_area, int a_slot, bool a_ignoreLock, bool a_silent)
        {
            char        buf[260];
            const char* who = CallerTag(buf);

            const auto result = g_shim.original.remove_tattoo_from_slot(a_actor, a_area, a_slot, a_ignoreLock, a_silent);

            SKSE::log::info("[CALLTRACE] {} -> remove_tattoo_from_slot(actor=0x{:X}, '{}', slot={}, ignoreLock={}, silent={}) = {}",
                who, reinterpret_cast<std::uintptr_t>(a_actor), a_area.c_str(), a_slot, a_ignoreLock, a_silent, result);
            return result;
        }

        // The table lives in SlaveTatsNG's read-only data, so it has to be made
        // writable for the duration of the swap.
        void PatchTable(slavetats::Addresses* a_table)
        {
            std::lock_guard<std::mutex> lock(g_traceMutex);
            if (!a_table) {
                return;
            }
            if (g_shim.installed) {
                return;
            }
            if (a_table->current_version != slavetats::Addresses::version) {
                SKSE::log::error("[CALLTRACE] interface version {} is not {}, refusing to patch", a_table->current_version, static_cast<int>(slavetats::Addresses::version));
                return;
            }

            g_shim.original = *a_table;

            DWORD previous = 0;
            if (!::VirtualProtect(a_table, sizeof(slavetats::Addresses), PAGE_READWRITE, &previous)) {
                SKSE::log::error("[CALLTRACE] VirtualProtect failed (error {}), tracer not installed", ::GetLastError());
                return;
            }

            // Only wrap entries upstream actually published. A null slot would be
            // reached through our shim and forwarded to a null pointer.
            int  wrapped = 0;
            auto wrap = [&wrapped](auto& a_slot, auto a_shim) {
                if (a_slot) {
                    a_slot = a_shim;
                    ++wrapped;
                }
            };

            wrap(a_table->simple_add_tattoo, &ShimSimpleAdd);
            wrap(a_table->simple_remove_tattoo, &ShimSimpleRemove);
            wrap(a_table->synchronize_tattoos, &ShimSynchronize);
            wrap(a_table->add_tattoo, &ShimAddTattoo);
            wrap(a_table->add_and_get_tattoo, &ShimAddAndGetTattoo);
            wrap(a_table->remove_tattoos, &ShimRemoveTattoos);
            wrap(a_table->remove_tattoo_from_slot, &ShimRemoveFromSlot);

            DWORD ignored = 0;
            ::VirtualProtect(a_table, sizeof(slavetats::Addresses), previous, &ignored);

            if (wrapped == 0) {
                SKSE::log::error("[CALLTRACE] the interface table has no callable entries, nothing to trace");
                return;
            }

            g_shim.table = a_table;
            g_shim.installed = true;

            SKSE::log::info("[CALLTRACE] installed on SlaveTatsNG interface table 0x{:X} ({} mutating entries wrapped)",
                reinterpret_cast<std::uintptr_t>(a_table), wrapped);
        }

        void RestoreTable()
        {
            std::lock_guard<std::mutex> lock(g_traceMutex);
            if (!g_shim.installed || !g_shim.table) {
                return;
            }

            DWORD previous = 0;
            if (!::VirtualProtect(g_shim.table, sizeof(slavetats::Addresses), PAGE_READWRITE, &previous)) {
                // Keep installed = true so a later attempt can still succeed;
                // dropping the flag here used to leave the table patched forever
                // with UninstallCallTrace() permanently disabled.
                SKSE::log::error("[CALLTRACE] VirtualProtect failed (error {}), cannot restore original entries", ::GetLastError());
                return;
            }

            // Put back only the entries that still point at our own shims. Another
            // plugin may have wrapped the same table after us, and assigning the
            // whole struct back would silently unwrap it.
            int  restored = 0;
            auto put = [&restored](auto& a_slot, auto a_original, auto a_shim) {
                if (a_slot == a_shim) {
                    a_slot = a_original;
                    ++restored;
                }
            };

            auto* table = g_shim.table;
            put(table->simple_add_tattoo, g_shim.original.simple_add_tattoo, &ShimSimpleAdd);
            put(table->simple_remove_tattoo, g_shim.original.simple_remove_tattoo, &ShimSimpleRemove);
            put(table->synchronize_tattoos, g_shim.original.synchronize_tattoos, &ShimSynchronize);
            put(table->add_tattoo, g_shim.original.add_tattoo, &ShimAddTattoo);
            put(table->add_and_get_tattoo, g_shim.original.add_and_get_tattoo, &ShimAddAndGetTattoo);
            put(table->remove_tattoos, g_shim.original.remove_tattoos, &ShimRemoveTattoos);
            put(table->remove_tattoo_from_slot, g_shim.original.remove_tattoo_from_slot, &ShimRemoveFromSlot);

            DWORD ignored = 0;
            ::VirtualProtect(table, sizeof(slavetats::Addresses), previous, &ignored);

            g_shim.table = nullptr;
            g_shim.installed = false;

            if (restored != 7) {
                SKSE::log::warn("[CALLTRACE] restored {} of 7 entries - another plugin wrapped the same table, leaving its shims in place", restored);
            } else {
                SKSE::log::info("[CALLTRACE] restored the original interface entries");
            }
        }
    }

    void InstallCallTrace()
    {
        // No listener here on purpose: see the note in the header. The bridge
        // calls this right after storing the published pointer, and main.cpp
        // calls it once more at kDataLoaded as a safety net.
        const auto* published = slavetats::singleton::get()->iface;
        if (!published) {
            SKSE::log::warn("[CALLTRACE] SlaveTatsNG interface is not published yet (the bridge will call again)");
            return;
        }

        PatchTable(const_cast<slavetats::Addresses*>(published));
    }

    void UninstallCallTrace()
    {
        RestoreTable();
    }

    bool CallTraceInstalled()
    {
        std::lock_guard<std::mutex> lock(g_traceMutex);
        return g_shim.installed;
    }
}
