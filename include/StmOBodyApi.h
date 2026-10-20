#pragma once

// The C ABI between SlaveTatsMenu.dll and the optional SlaveTatsMenuOBody.dll.
//
// The OBody support lives in its own plugin so that the main plugin keeps
// working - and keeps building - on a machine that has never heard of OBody.
// Nothing here includes CommonLibSSE: actors cross the boundary as `void*` and
// both sides are built by the same compiler, so a plain function table is
// enough and neither DLL has to link against the other.
//
// Versioning: `StmOBodyGetApi` always returns the newest table it has, and the
// caller checks `version` before touching anything past the fields it knows.

#include <cstdint>

#define STM_OBODY_API_VERSION 2u

// What the shape was resolved from.
enum StmOBodySource : std::uint32_t
{
    kStmOBodySourceNone = 0,
    kStmOBodySourcePreset = 1,      // OBody's plugin API named a BodySlide preset
    kStmOBodySourceLiveMorphs = 2,  // snapshot of the morphs RaceMenu has on the actor
};

// Opaque, owned by the add-on. Created by `resolve`, consumed by `apply`, and
// must be handed back through `release` exactly once.
struct StmOBodyShape;

typedef struct StmOBodyApi
{
    std::uint32_t version;

    // OBody.dll and its preset folder are on disk. Cheap file probe.
    int (*detected)(void);

    // OBody's plugin API answered and is currently safe to use (OBody sends a
    // readiness event around save/load). Live-morph fallback does not need it.
    int (*ready)(void);

    // Game thread only. `actor` is a RE::Actor*. Returns a shape to be applied
    // on a worker thread, or null when the actor has no preset and no live
    // morphs. `label` receives a short human-readable name for the UI.
    StmOBodyShape* (*resolve)(void* actor, int sex,
                              char* label, std::uint32_t labelSize,
                              char* detail, std::uint32_t detailSize);

    // Worker thread. `positions` is `vertexCount` tightly packed xyz floats and
    // is modified in place. `nifPath` is the body NIF the vertices came from -
    // the matching .tri file sits next to it - and `shapeName` is the shape
    // inside that NIF.
    //
    // `normalizeScale` is the NIF-units-to-viewport factor the mesh parser
    // applied, and the parser also swaps Y and Z. Morph offsets are authored in
    // NIF units, so the add-on applies both: without this, a 2-unit offset on a
    // 1.9-unit-tall preview mesh tears the body apart.
    //
    // Returns 1 when something was applied, 0 otherwise (with a reason in `info`).
    int (*apply)(StmOBodyShape* shape, float* positions, std::uint32_t vertexCount,
                 const char* nifPath, const char* shapeName, float normalizeScale,
                 char* info, std::uint32_t infoSize);

    void (*release)(StmOBodyShape* shape);

    const char* (*versionString)(void);
} StmOBodyApi;

typedef const StmOBodyApi* (*StmOBodyGetApiFn)(void);

// Only the add-on defines and exports this. The main plugin includes this header
// too, purely for the table layout, and an unguarded __declspec(dllexport) would
// leave it with a declaration for a symbol it never provides - harmless while
// nothing calls it, a link error the moment something does.
#ifdef STM_OBODY_EXPORTS
extern "C" __declspec(dllexport) const StmOBodyApi* StmOBodyGetApi(void);
#endif
