#pragma once

#include "PCH.h"

#include "JContainers/jc_interface.h"

namespace stm::jc
{
    // A JContainers object id. Non-zero means "some object"; ids are not stable
    // across synchronisations and must never be cached long term.
    struct ObjId
    {
        std::int32_t id = 0;

        explicit operator bool() const { return id > 0; }
        bool operator==(const ObjId&) const = default;
    };

    // kPostLoad: remember the root interface published by JContainers.
    void InstallListener();

    // kDataLoaded: resolve every function pointer we use. Never aborts - on
    // failure the whole integration degrades to "unavailable".
    bool BindReflection();

    bool Ready();

    // --- lifetime ---------------------------------------------------------
    ObjId CreateMap();
    ObjId CreateArray();

    // Keeps a temporary object alive beyond JContainers' ~10s grace period.
    // Always pair with CleanPool.
    ObjId AddToPool(ObjId a_obj, std::string_view a_pool);
    void  CleanPool(std::string_view a_pool);

    // --- maps -------------------------------------------------------------
    std::string  MapGetStr(ObjId a_obj, std::string_view a_key, std::string_view a_default = "");
    std::int32_t MapGetInt(ObjId a_obj, std::string_view a_key, std::int32_t a_default = 0);
    float        MapGetFlt(ObjId a_obj, std::string_view a_key, float a_default = 0.0f);
    ObjId        MapGetObj(ObjId a_obj, std::string_view a_key);
    bool         MapHasKey(ObjId a_obj, std::string_view a_key);
    void         MapSetStr(ObjId a_obj, std::string_view a_key, std::string_view a_value);
    void         MapSetInt(ObjId a_obj, std::string_view a_key, std::int32_t a_value);
    void         MapSetFlt(ObjId a_obj, std::string_view a_key, float a_value);
    void         MapSetObj(ObjId a_obj, std::string_view a_key, ObjId a_value);

    // --- arrays -----------------------------------------------------------
    std::int32_t ArrayCount(ObjId a_arr);
    ObjId        ArrayGetObj(ObjId a_arr, std::int32_t a_index);
    void         ArrayAddForm(ObjId a_arr, RE::TESForm* a_form);
    std::int32_t ArrayFindForm(ObjId a_arr, RE::TESForm* a_form);
    void         ArrayAddObj(ObjId a_arr, ObjId a_obj);

    // --- databases --------------------------------------------------------
    ObjId JdbSolveObj(std::string_view a_path);

    // Returns true on success ONLY. This is NOT an object id - callers that
    // need the stored object must call JdbSolveObj again afterwards.
    bool JdbSolveObjSetter(std::string_view a_path, ObjId a_obj, bool a_createMissing);

    void         FormDbSetInt(RE::TESForm* a_form, std::string_view a_key, std::int32_t a_value);
    std::int32_t FormDbGetInt(RE::TESForm* a_form, std::string_view a_key, std::int32_t a_default = 0);
    std::string  FormDbGetStr(RE::TESForm* a_form, std::string_view a_key, std::string_view a_default = "");
    void         FormDbSetStr(RE::TESForm* a_form, std::string_view a_key, std::string_view a_value);
}
