#pragma once

#include "PCH.h"
#include "StmOBodyApi.h"

namespace stm::obody
{
    // One resolved body shape, owned here and handed to the render worker.
    // The add-on owns the underlying object; this wrapper exists so the last
    // reference - wherever it dies - releases it through the add-on's API
    // instead of a mismatched allocator.
    struct Shape
    {
        StmOBodyShape* handle = nullptr;
        std::string    label;
        std::string    detail;   // the add-on's own explanation, for the log and the UI
        std::uint32_t  actorFormID = 0;
        int            sex = 0;
    };

    using ShapePtr = std::shared_ptr<Shape>;

    // Looks for the optional SlaveTatsMenuOBody.dll. Does nothing (and logs
    // once) when it is not installed, which is the normal case.
    void Initialize();

    bool        Available();
    bool        Detected();
    const char* AddonVersion();

    // Queues a resolve for the current target on the game thread. Safe to call
    // from the render thread and cheap to call repeatedly: a request already in
    // flight swallows the next one.
    void RequestResolve();

    // Render thread. Returns the newest resolved shape, or null. Ownership
    // moves to the caller.
    ShapePtr TakeResolved();

    // Worker thread. Deforms `a_positions` in place.
    bool Apply(const ShapePtr& a_shape, float* a_positions, std::uint32_t a_count,
               const std::string& a_nifPath, const std::string& a_shapeName, float a_normalizeScale,
               std::string& a_info);
}
