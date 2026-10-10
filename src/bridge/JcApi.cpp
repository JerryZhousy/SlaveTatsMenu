#include "bridge/JcApi.h"

#include "JContainers/jcontainers_constants.h"

namespace stm::jc
{
    namespace
    {
        const ::jc::root_interface* g_root = nullptr;
        void*                       g_domain = nullptr;
        bool                        g_bound = false;

        // JContainers' reflected functions were built expecting RE::BSFixedString
        // for every string parameter and return value. BSFixedString has a
        // non-trivial copy constructor and destructor, so the MSVC x64 ABI passes
        // it by hidden reference: the callee dereferences the argument as a
        // BSFixedString object before touching its characters.
        //
        // Declaring these as `const char*` therefore hands the callee a pointer to
        // the raw characters; it reads the first eight of them as if they were the
        // BSFixedString's data pointer and chases that as an address, which is
        // exactly how the "SlaveTatsMenu-DB" pool tag crashed the game:
        //   EXCEPTION_ACCESS_VIOLATION, RBX = 0x7461546576616C53 == "SlaveTat".
        //
        // The declarations below mirror SlaveTatsNG's own wrapper
        // (include/jcontainers_wrapper.h) one for one so the ABI matches.
        struct Functions
        {
            // JMap
            std::int32_t (*map_count)(void*, std::int32_t) = nullptr;
            float (*map_get_flt)(void*, std::int32_t, RE::BSFixedString, float) = nullptr;
            std::int32_t (*map_get_int)(void*, std::int32_t, RE::BSFixedString, std::int32_t) = nullptr;
            std::int32_t (*map_get_obj)(void*, std::int32_t, RE::BSFixedString, std::int32_t) = nullptr;
            RE::BSFixedString (*map_get_str)(void*, std::int32_t, RE::BSFixedString, RE::BSFixedString) = nullptr;
            bool (*map_has_key)(void*, std::int32_t, RE::BSFixedString) = nullptr;
            std::int32_t (*map_object)(void*) = nullptr;
            void (*map_set_flt)(void*, std::int32_t, RE::BSFixedString, float) = nullptr;
            void (*map_set_int)(void*, std::int32_t, RE::BSFixedString, std::int32_t) = nullptr;
            void (*map_set_obj)(void*, std::int32_t, RE::BSFixedString, std::int32_t) = nullptr;
            void (*map_set_str)(void*, std::int32_t, RE::BSFixedString, RE::BSFixedString) = nullptr;

            // JArray
            void (*array_add_form)(void*, std::int32_t, RE::TESForm*, std::int32_t) = nullptr;
            void (*array_add_obj)(void*, std::int32_t, std::int32_t, std::int32_t) = nullptr;
            std::int32_t (*array_count)(void*, std::int32_t) = nullptr;
            std::int32_t (*array_find_form)(void*, std::int32_t, RE::TESForm*, std::int32_t) = nullptr;
            std::int32_t (*array_get_obj)(void*, std::int32_t, std::int32_t, std::int32_t) = nullptr;
            std::int32_t (*array_object)(void*) = nullptr;

            // JValue
            std::int32_t (*value_add_to_pool)(void*, std::int32_t, RE::BSFixedString) = nullptr;
            std::int32_t (*value_clean_pool)(void*, RE::BSFixedString) = nullptr;

            // JDB
            std::int32_t (*db_solve_obj)(void*, RE::BSFixedString, std::int32_t) = nullptr;
            bool (*db_solve_obj_setter)(void*, RE::BSFixedString, std::int32_t, bool) = nullptr;

            // JFormDB
            void (*formdb_set_int)(void*, RE::TESForm*, RE::BSFixedString, std::int32_t) = nullptr;
            std::int32_t (*formdb_get_int)(void*, RE::TESForm*, RE::BSFixedString) = nullptr;
            RE::BSFixedString (*formdb_get_str)(void*, RE::TESForm*, RE::BSFixedString) = nullptr;
            void (*formdb_set_str)(void*, RE::TESForm*, RE::BSFixedString, RE::BSFixedString) = nullptr;
        };

        Functions g_fn;

        // Constructs the interned string JContainers expects. The source only has
        // to stay alive for the constructor call; BSFixedString interns its own
        // copy, and the returned value is alive for the whole call expression.
        RE::BSFixedString Bs(std::string_view a_str)
        {
            const std::string owned(a_str);
            return RE::BSFixedString(owned.c_str());
        }

        template <class F>
        bool Resolve(const ::jc::reflection_interface* a_refl, const char* a_func, const char* a_class, F& a_out)
        {
            a_out = reinterpret_cast<F>(a_refl->tes_function_of_class(a_func, a_class));
            if (!a_out) {
                SKSE::log::error("JContainers: missing function {}::{}", a_class, a_func);
                return false;
            }
            return true;
        }
    }

    void InstallListener()
    {
        const auto messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            SKSE::log::error("JContainers: no messaging interface");
            return;
        }

        // VR/GOG builds publish the same message under a different sender name.
        messaging->RegisterListener(JC_PLUGIN_NAME, [](SKSE::MessagingInterface::Message* a_msg) {
            if (!a_msg || a_msg->type != static_cast<std::uint32_t>(::jc::message_root_interface)) {
                return;
            }

            const auto* root = ::jc::root_interface::from_void(a_msg->data);
            if (root) {
                g_root = root;
                SKSE::log::info("JContainers: root interface acquired (version {})", root->current_version);
            } else {
                SKSE::log::error("JContainers: root interface version mismatch");
            }
        });
    }

    bool BindReflection()
    {
        if (g_bound) {
            return true;
        }

        if (!g_root) {
            SKSE::log::warn("JContainers: root interface was never received - integration disabled");
            return false;
        }

        const auto* refl = g_root->query_interface<::jc::reflection_interface>();
        const auto* dom = g_root->query_interface<::jc::domain_interface>();
        if (!refl || !dom) {
            SKSE::log::error("JContainers: reflection/domain interface unavailable (refl={}, dom={})", static_cast<const void*>(refl), static_cast<const void*>(dom));
            return false;
        }

        g_domain = dom->get_default_domain();
        if (!g_domain) {
            SKSE::log::error("JContainers: could not obtain the default domain");
            return false;
        }

        bool ok = true;

        ok &= Resolve(refl, "count", "JMap", g_fn.map_count);
        ok &= Resolve(refl, "getFlt", "JMap", g_fn.map_get_flt);
        ok &= Resolve(refl, "getInt", "JMap", g_fn.map_get_int);
        ok &= Resolve(refl, "getObj", "JMap", g_fn.map_get_obj);
        ok &= Resolve(refl, "getStr", "JMap", g_fn.map_get_str);
        ok &= Resolve(refl, "hasKey", "JMap", g_fn.map_has_key);
        ok &= Resolve(refl, "object", "JMap", g_fn.map_object);
        ok &= Resolve(refl, "setFlt", "JMap", g_fn.map_set_flt);
        ok &= Resolve(refl, "setInt", "JMap", g_fn.map_set_int);
        ok &= Resolve(refl, "setObj", "JMap", g_fn.map_set_obj);
        ok &= Resolve(refl, "setStr", "JMap", g_fn.map_set_str);

        ok &= Resolve(refl, "addForm", "JArray", g_fn.array_add_form);
        ok &= Resolve(refl, "addObj", "JArray", g_fn.array_add_obj);
        ok &= Resolve(refl, "count", "JArray", g_fn.array_count);
        ok &= Resolve(refl, "findForm", "JArray", g_fn.array_find_form);
        ok &= Resolve(refl, "getObj", "JArray", g_fn.array_get_obj);
        ok &= Resolve(refl, "object", "JArray", g_fn.array_object);

        ok &= Resolve(refl, "addToPool", "JValue", g_fn.value_add_to_pool);
        ok &= Resolve(refl, "cleanPool", "JValue", g_fn.value_clean_pool);

        ok &= Resolve(refl, "solveObj", "JDB", g_fn.db_solve_obj);
        ok &= Resolve(refl, "solveObjSetter", "JDB", g_fn.db_solve_obj_setter);

        ok &= Resolve(refl, "setInt", "JFormDB", g_fn.formdb_set_int);
        ok &= Resolve(refl, "getInt", "JFormDB", g_fn.formdb_get_int);
        ok &= Resolve(refl, "getStr", "JFormDB", g_fn.formdb_get_str);
        ok &= Resolve(refl, "setStr", "JFormDB", g_fn.formdb_set_str);

        g_bound = ok;
        if (ok) {
            SKSE::log::info("JContainers: reflection functions bound, integration enabled");
        } else {
            SKSE::log::error("JContainers: binding incomplete - integration disabled");
        }
        return ok;
    }

    bool Ready()
    {
        return g_bound && g_domain != nullptr;
    }

    ObjId CreateMap()
    {
        return { g_fn.map_object ? g_fn.map_object(g_domain) : 0 };
    }

    ObjId CreateArray()
    {
        return { g_fn.array_object ? g_fn.array_object(g_domain) : 0 };
    }

    ObjId AddToPool(ObjId a_obj, std::string_view a_pool)
    {
        if (!g_fn.value_add_to_pool || !a_obj) {
            return a_obj;
        }
        return { g_fn.value_add_to_pool(g_domain, a_obj.id, Bs(a_pool)) };
    }

    void CleanPool(std::string_view a_pool)
    {
        if (g_fn.value_clean_pool) {
            (void)g_fn.value_clean_pool(g_domain, Bs(a_pool));
        }
    }

    std::string MapGetStr(ObjId a_obj, std::string_view a_key, std::string_view a_default)
    {
        if (!g_fn.map_get_str || !a_obj) {
            return std::string(a_default);
        }

        const RE::BSFixedString value = g_fn.map_get_str(g_domain, a_obj.id, Bs(a_key), Bs(a_default));
        const char* const        chars = value.c_str();
        return chars ? std::string(chars) : std::string(a_default);
    }

    std::int32_t MapGetInt(ObjId a_obj, std::string_view a_key, std::int32_t a_default)
    {
        if (!g_fn.map_get_int || !a_obj) {
            return a_default;
        }
        return g_fn.map_get_int(g_domain, a_obj.id, Bs(a_key), a_default);
    }

    float MapGetFlt(ObjId a_obj, std::string_view a_key, float a_default)
    {
        if (!g_fn.map_get_flt || !a_obj) {
            return a_default;
        }
        return g_fn.map_get_flt(g_domain, a_obj.id, Bs(a_key), a_default);
    }

    ObjId MapGetObj(ObjId a_obj, std::string_view a_key)
    {
        if (!g_fn.map_get_obj || !a_obj) {
            return {};
        }
        return { g_fn.map_get_obj(g_domain, a_obj.id, Bs(a_key), 0) };
    }

    bool MapHasKey(ObjId a_obj, std::string_view a_key)
    {
        if (!g_fn.map_has_key || !a_obj) {
            return false;
        }
        return g_fn.map_has_key(g_domain, a_obj.id, Bs(a_key));
    }

    void MapSetStr(ObjId a_obj, std::string_view a_key, std::string_view a_value)
    {
        if (!g_fn.map_set_str || !a_obj) {
            return;
        }
        g_fn.map_set_str(g_domain, a_obj.id, Bs(a_key), Bs(a_value));
    }

    void MapSetInt(ObjId a_obj, std::string_view a_key, std::int32_t a_value)
    {
        if (!g_fn.map_set_int || !a_obj) {
            return;
        }
        g_fn.map_set_int(g_domain, a_obj.id, Bs(a_key), a_value);
    }

    void MapSetFlt(ObjId a_obj, std::string_view a_key, float a_value)
    {
        if (!g_fn.map_set_flt || !a_obj) {
            return;
        }
        g_fn.map_set_flt(g_domain, a_obj.id, Bs(a_key), a_value);
    }

    void MapSetObj(ObjId a_obj, std::string_view a_key, ObjId a_value)
    {
        if (!g_fn.map_set_obj || !a_obj) {
            return;
        }
        g_fn.map_set_obj(g_domain, a_obj.id, Bs(a_key), a_value.id);
    }

    std::int32_t ArrayCount(ObjId a_arr)
    {
        if (!g_fn.array_count || !a_arr) {
            return 0;
        }
        return g_fn.array_count(g_domain, a_arr.id);
    }

    ObjId ArrayGetObj(ObjId a_arr, std::int32_t a_index)
    {
        if (!g_fn.array_get_obj || !a_arr || a_index < 0) {
            return {};
        }
        return { g_fn.array_get_obj(g_domain, a_arr.id, a_index, 0) };
    }

    void ArrayAddForm(ObjId a_arr, RE::TESForm* a_form)
    {
        if (g_fn.array_add_form && a_arr && a_form) {
            g_fn.array_add_form(g_domain, a_arr.id, a_form, -1);
        }
    }

    std::int32_t ArrayFindForm(ObjId a_arr, RE::TESForm* a_form)
    {
        if (!g_fn.array_find_form || !a_arr || !a_form) {
            return -1;
        }
        return g_fn.array_find_form(g_domain, a_arr.id, a_form, 0);
    }

    void ArrayAddObj(ObjId a_arr, ObjId a_obj)
    {
        if (g_fn.array_add_obj && a_arr && a_obj) {
            g_fn.array_add_obj(g_domain, a_arr.id, a_obj.id, -1);
        }
    }

    ObjId JdbSolveObj(std::string_view a_path)
    {
        if (!g_fn.db_solve_obj) {
            return {};
        }
        return { g_fn.db_solve_obj(g_domain, Bs(a_path), 0) };
    }

    bool JdbSolveObjSetter(std::string_view a_path, ObjId a_obj, bool a_createMissing)
    {
        if (!g_fn.db_solve_obj_setter || !a_obj) {
            return false;
        }
        return g_fn.db_solve_obj_setter(g_domain, Bs(a_path), a_obj.id, a_createMissing);
    }

    void FormDbSetInt(RE::TESForm* a_form, std::string_view a_key, std::int32_t a_value)
    {
        if (!g_fn.formdb_set_int || !a_form) {
            return;
        }
        g_fn.formdb_set_int(g_domain, a_form, Bs(a_key), a_value);
    }

    std::int32_t FormDbGetInt(RE::TESForm* a_form, std::string_view a_key, std::int32_t a_default)
    {
        if (!g_fn.formdb_get_int || !a_form) {
            return a_default;
        }
        return g_fn.formdb_get_int(g_domain, a_form, Bs(a_key));
    }

    std::string FormDbGetStr(RE::TESForm* a_form, std::string_view a_key, std::string_view a_default)
    {
        if (!g_fn.formdb_get_str || !a_form) {
            return std::string(a_default);
        }

        const RE::BSFixedString value = g_fn.formdb_get_str(g_domain, a_form, Bs(a_key));
        const char* const        chars = value.c_str();
        return chars ? std::string(chars) : std::string(a_default);
    }

    void FormDbSetStr(RE::TESForm* a_form, std::string_view a_key, std::string_view a_value)
    {
        if (!g_fn.formdb_set_str || !a_form) {
            return;
        }
        g_fn.formdb_set_str(g_domain, a_form, Bs(a_key), Bs(a_value));
    }
}
