#pragma once

#include "PCH.h"

namespace stm::data
{
    struct SlotInfo
    {
        int           slot = 0;
        bool          occupied = false;
        bool          external = false;  // occupied, but not by a SlaveTats tattoo
        std::string   name;
        std::string   section;
        std::uint32_t color = 0x000000;
        float         alpha = 1.0f;
    };

    // A snapshot of what a given actor currently wears in one area.
    //
    // Request() is called from the render callback while Build() runs on the
    // task queue - two different threads in practice, so everything here is
    // behind a mutex and the UI takes a copy rather than a reference.
    class SlotSnapshot
    {
    public:
        // What the UI actually consumes. A plain value so it cannot outlive the
        // lock or observe a half-finished rebuild.
        struct View
        {
            std::vector<SlotInfo> slots;
            std::string           area;
            std::uint32_t         actorFormID = 0;
            std::uint64_t         revision = 0;
            bool                  valid = false;
        };

        static SlotSnapshot& Get();

        // Called from the UI. Cheap when nothing changed and the refresh
        // interval has not elapsed.
        void Request(RE::ActorHandle a_actor, std::string a_area);

        // Rebuilds the snapshot. Task queue only.
        void Build(RE::ActorHandle a_actor, const std::string& a_area);

        // Thread-safe copy of the current snapshot.
        View Copy() const;

        // Marks the snapshot stale so the next Request() rebuilds it. Safe to
        // call from either thread.
        void Invalidate();

    private:
        SlotSnapshot() = default;

        mutable std::mutex _mutex;

        std::vector<SlotInfo> _slots;
        std::string           _area;
        std::uint32_t         _actorFormID = 0;
        std::uint64_t         _revision = 0;
        bool                  _valid = false;

        // request state (UI thread only, but kept under the same lock)
        RE::ActorHandle                       _reqHandle;
        std::string                           _reqArea;
        std::chrono::steady_clock::time_point _lastRequest{};
    };
}
