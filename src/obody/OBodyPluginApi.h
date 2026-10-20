#pragma once

// Minimal declarations of OBody NG's published SKSE plugin API, matching the
// ABI of OBody's `include/API/API.h` (v1). Only what this add-on calls is
// declared; the vtable order of `IPluginInterface` must stay identical to the
// published header, so every method that precedes the one we call is listed.
//
// The upstream interface is GPL-3.0; this add-on is distributed under the same
// terms (see LICENSES/OBody-API.txt and THIRD_PARTY.md).

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace OBody::API
{
    enum class PluginAPIVersion : int
    {
        Invalid = 0,
        v1 = 1,
        Latest = v1
    };

    class IActorChangeEventListener;
    struct PresetCounts;
    enum PresetCategory : std::uint64_t;
    struct PresetAssignmentInformation;
    struct AssignPresetPayload;

    class IPluginInterfaceVersionIndependent
    {
    public:
        const char* owner = nullptr;
        void*       context = nullptr;

        virtual PluginAPIVersion PluginAPIVersion() = 0;
        virtual const char*      SetOwner(const char* a_owner) = 0;
    };

    class IPluginInterface : public IPluginInterfaceVersionIndependent
    {
    public:
        virtual bool ActorIsNaked(RE::Actor* a_actor) = 0;
        virtual bool ActorIsNaked(RE::Actor* a_actor, bool a_isEquipping, const RE::TESForm* a_armor) = 0;
        virtual bool ActorHasORefitApplied(RE::Actor* a_actor) = 0;
        virtual bool ActorIsProcessed(RE::Actor* a_actor) = 0;
        virtual bool ActorIsBlacklisted(RE::Actor* a_actor) = 0;
        virtual bool IsORefitEnabled() = 0;

        virtual bool RegisterEventListener(IActorChangeEventListener& a_listener) = 0;
        virtual bool DeregisterEventListener(IActorChangeEventListener& a_listener) = 0;
        virtual bool HasRegisteredEventListener(IActorChangeEventListener& a_listener) = 0;

        virtual void   GetPresetCounts(PresetCounts& a_payload) = 0;
        virtual size_t GetPresetNames(PresetCategory a_category, std::string_view* a_buffer, size_t a_bufferLength,
                                      size_t a_offset = 0, size_t a_limit = SIZE_MAX) = 0;

        virtual void EnsureActorIsProcessed(RE::Actor* a_actor) = 0;
        virtual void ApplyOBodyMorphsToActor(RE::Actor* a_actor) = 0;
        virtual void RemoveOBodyMorphsFromActor(RE::Actor* a_actor) = 0;
        virtual void ForcefullyChangeORefitForActor(RE::Actor* a_actor, bool a_shouldBeApplied) = 0;

        // The one call this add-on makes.
        virtual void GetPresetAssignedToActor(RE::Actor* a_actor, PresetAssignmentInformation& a_payload) = 0;
    };

    struct PresetAssignmentInformation
    {
        enum Flags : std::uint64_t
        {
            None = 0,
            IsFemale = 1 << 0
        };

        Flags           flags = Flags::None;
        std::string_view presetName;
    };

    class IOBodyReadinessEventListener
    {
    public:
        virtual void OBodyIsReady() = 0;
        virtual void OBodyIsNoLongerReady() = 0;
        virtual void OBodyIsBecomingReady() {}
        virtual void OBodyIsBecomingUnready() {}
    };

    namespace SKSEMessages
    {
        struct RequestPluginInterface
        {
            static constexpr std::uint32_t type = 0xc0B0D9cc;

            PluginAPIVersion           version;
            IPluginInterface**         pluginInterface;
            IOBodyReadinessEventListener* readinessEventListener;
        };
    }
}
