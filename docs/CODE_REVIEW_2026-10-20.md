# SlaveTatsMenu — Performance & Defect Review (2026-10-20)

Scope: `src/` (all 44 translation units), with emphasis on the code that is new since the
2026-10-18 review (`src/obody/`, `src/ops/PendingSync.cpp`, the enlarged 3D preview, the settings
tab, `TextureLoader`).

Baseline: `de6fad0` + uncommitted work (`src/obody/**`, `PendingSync.*`, `include/StmOBodyApi.h`,
modified `UiMain.cpp` / `MannequinRenderer.cpp` / `TattooDatabase.cpp`).

Method: full read of every source file, cross-referenced call graphs, manual threading analysis of
the render / task-queue / probe / mesh-worker split. Findings already fixed by the 10-18 review are
**not** repeated here.

Severity: **H** = crash / UB / freeze, **M** = wrong behaviour or visible hitch, **L** = cleanup.

---

## Summary

| # | Sev | Area | Finding |
|---|---|---|---|
| B-1 | H | `UiMain` + `MannequinRenderer` | Enlarged preview frees the SRV it just submitted to the draw list |
| B-2 | H | `obody/PresetXml` | Game thread can block behind a full SliderPresets rescan (tens of MB) |
| B-3 | M | `data/TargetState` | `ActorHandle` written by the UI thread, read by the task thread, no lock |
| B-4 | M | `data/SlotSnapshot` | `Build()` result for a stale area can overwrite a newer one |
| B-5 | M | `bridge/SlaveTatsNg` | `LoadSlotLimits()` lazy init still unsynchronised (open since 10-18) |
| B-6 | M | `Config` / `PresetStore` | `current_path()` inside a function-local static can throw → `std::terminate` |
| B-7 | M | `render/TextureLoader` | Unbounded allocation from a BSA-reported size (NIF path got a cap, this did not) |
| B-8 | L | `ops/PendingSync` | Raw `Watch*` into a `std::vector` that reallocates |
| B-9 | L | `ui/UiMain` | `IsBackTattoo()`'s `area == "back"` branch is unreachable |
| B-10 | L | `ops/TattooOps` | Requested slot silently ignored when JContainers is unavailable |
| P-1 | H | `data/TattooDatabase` | Catalogue rebuild runs on the render thread and costs thousands of JContainers calls |
| P-2 | M | `ui/UiMain` | `KnownTargetCount()` → JContainers round trip **every frame** |
| P-3 | M | `ui/UiMain` | `Config::Save()` (INI write + log line) on every colour-picker frame |
| P-4 | M | `data/CacheParser` | Filtering allocates two lowercase copies per entry per keystroke |
| P-5 | M | `render/TextureLoader` | DDS decode on the render thread, with the global cache mutex held |
| P-6 | M | `render/MannequinRenderer` | Full scene re-render + ~20 COM get/release per frame even when idle |
| P-7 | M | `obody/AddonMain` | `.tri` file re-read and re-parsed on every preset switch |
| P-8 | L | `ui/UiMain` | Per-frame `SlotSnapshot::Copy()`, label strings, `CurrentArea()` twice, `Tr()` temporaries |

---

## Bugs

### B-1 (H) The enlarged preview frees the texture it just handed to ImGui

`src/ui/UiMain.cpp:937-960`, `:1054-1059`, `:784-787`; `src/render/MannequinRenderer.cpp:1251-1264`

Frame N, single pass through `DrawInspector`:

1. `isModalOpen` is evaluated at line 937 — **false**, because the popup is opened later in the
   same frame by the "Enlarge" button (line 1054-1056).
2. `RenderFrame()` at 946 renders into the 220 px target and returns `_shaderResourceView`.
3. `ImGui::Image(srv, …)` at 960 appends that raw `ID3D11ShaderResourceView*` to the window's
   draw list. ImGui stores the pointer as an `ImTextureID` and does **not** AddRef it.
4. The button opens the popup; `DrawEnlargedPreviewModal()` at 1059 calls `BeginPopupModal`,
   which returns true on the same frame, and at 784-787 sees a different size → `Resize()`.
5. `Resize()` (MannequinRenderer.cpp:1258-1263) releases `_shaderResourceView`,
   `_renderTargetView` and `_renderTargetTexture`.

The object referenced by the draw list is now destroyed. The framework's DX11 backend binds it
later in the frame → freed COM pointer (debug-layer error, black panel, driver reset, or a crash
inside `PSSetShaderResources`). The mirror case — closing the modal — happens to be safe because
`Resize()` runs before the `Image()` call there.

Fix: defer the release. Either keep the previous SRV/RTV/texture alive in a one-frame "retire"
list drained at the start of the next `RenderFrame`, or skip the resize on the frame the popup
opens (`if (!justOpened) Resize(...)`, and this frame draw the old SRV).

Same class of hazard: after a device-removed `Shutdown()` (MannequinRenderer.cpp:1467-1473)
`TextureLoader::ClearCache()` frees every SRV, while `UiState::previewTexture` still holds one.
If the user then presses "Click to retry", `Initialize()` succeeds and the next `RenderFrame` binds
that dangling pointer. `state.previewTexture` needs clearing in the same place.

### B-2 (H) Game thread can block behind a full SliderPresets rescan

`src/obody/PresetXml.cpp:364-374` (`FindPreset`), `:383-408` (`RefreshPresets`), `:376-381`
(`PresetsLoaded`); called from `src/obody/AddonMain.cpp:296-316` (game thread) and `:409-411`
(mesh worker)

`PresetsLoaded()` is documented as "the cheap question the game thread is allowed to ask", and
`ResolveImpl` uses it as a guard before calling `FindPreset()`. But `RefreshPresets()` does **not**
clear `g_loadedFlag` when it starts a rescan — it holds `g_mutex` for the entire
`ComputeStamp()` + `LoadDatabaseLocked()` (every `*.xml` under `Data/CalienteTools/BodySlide/SliderPresets`,
routinely tens of megabytes).

Sequence: worker calls `RefreshPresets()` → takes `g_mutex`; game thread calls `PresetsLoaded()` →
still `true` (stale) → calls `FindPreset()` → blocks on `g_mutex` for the whole scan. That is a
main-thread stall of hundreds of ms, i.e. a visible freeze, in the exact code path that exists to
avoid one. `PresetCount()` (`:410`) has the same shape and is called from the game thread at
`AddonMain.cpp:315`.

Fix: a "rescan in progress" atomic that `PresetsLoaded()` also consults, and make the game-thread
lookup non-blocking (`try_lock` → return `nullptr` and let `ApplyImpl` resolve it on the worker,
which is already the documented fallback).

### B-3 (M) `TargetState::_handle` is shared across threads without synchronisation

`src/data/TargetState.h:33`, `src/data/TargetState.cpp:15-40`, readers:
`src/ops/PendingSync.cpp:121` (`data::TargetState::Get().Resolve()` on the task thread)

`Set()` / `ResetToPlayer()` run on the render thread (target row buttons); `Resolve()` runs on the
task thread from `pending::Sample()`. `RE::ActorHandle` is not trivially copyable and is not
atomic, so this is a genuine data race, not a theoretical one — a torn read hands `Resolve()` a
half-updated handle and it dereferences it.

Fix: store `RE::FormID` (a `uint32_t`) as an `std::atomic`, or guard the handle with a small
`std::mutex` and return a copy. `FormID` + `TESForm::LookupByID` is cheaper and matches how
`PendingSync` already identifies actors.

### B-4 (M) A stale slot-snapshot build can overwrite a newer one

`src/data/SlotSnapshot.cpp:26-58` (`Request`), `:60-114` (`Build`)

`Request()` is called every frame from `DrawInspector` (UiMain.cpp:1193). When the area changes,
`_reqArea` is updated and a build is posted; `Build()` writes `_area = a_area` unconditionally when
it finishes. If the user switches area twice quickly, the *first* (now stale) build can land last
and set `_area` to the old value, while `_reqArea` already says the new one — so `Request()` sees
`changed == false` and `stale == false` and posts nothing for up to `slotRefreshMs`. The panel then
sits on "Reading slots" for a quarter of a second.

Fix: carry a monotonically increasing request id and drop results whose id is not the latest, or
compare `a_area` against `_reqArea` inside `Build()` before publishing.

### B-5 (M) `LoadSlotLimits()` still has an unsynchronised lazy init

`src/bridge/SlaveTatsNg.cpp:50-93`

Still open from the 10-18 review (listed there as a P3). `g_slots.loaded` is a plain `bool` written
under no lock, and `MaxSlots()` is called from the render thread (`UiMain.cpp:1111`), the task
thread (`SlotSnapshot.cpp:67`, `TattooOps.cpp:101/123/238`) and the probe thread. The kDataLoaded
warm-up (`main.cpp:83-85`) only removes the race when that warm-up actually runs first — it does
not cover a `MaxSlots()` call from `SlotSnapshot::Build` racing the warm-up loop itself.

Fix: `std::call_once` (or a function-local static `SlotLimits` initialised by an immediately-invoked
lambda), which also deletes the `loaded` flag.

### B-6 (M) `current_path()` in a function-local static can throw → `std::terminate`

`src/core/Config.cpp:14-19`, `src/data/PresetStore.cpp:111-116`, `src/obody/PresetXml.cpp:234-240`

```cpp
static const std::filesystem::path s_path = std::filesystem::current_path() / "Data" / ...;
```

`current_path()` throws `filesystem_error` (no `error_code` overload used). An exception escaping a
static initialiser during a render callback — where nothing catches — calls `std::terminate`. It is
also evaluated on the *first* call, which may be from the probe thread or the mesh worker, so the
throw can land on a thread with no handler at all. Rare, but the failure mode is a hard crash on
startup with no log line.

Fix: `std::filesystem::current_path(ec)`, fall back to `REL::Module::get().path().parent_path()`,
and never let the initialiser throw.

### B-7 (M) Unbounded allocation from a BSA-reported texture size

`src/render/TextureLoader.cpp:40-59`

```cpp
std::uint32_t totalSize = a_stream.stream->totalSize;      // or Info::fileSize
if (totalSize < 128) return nullptr;
std::vector<std::uint8_t> buffer(totalSize);
```

Only a lower bound is checked. `NifMeshExtractor` got a 64 MB cap and a short-read check in the
10-18 pass (P2-7); this path — which reads from third-party BSAs, i.e. equally untrusted input —
still trusts the header. A corrupt `totalSize` of 0xFFFFFFFF attempts a 4 GB allocation on the
render thread.

Fix: cap at the same 64 MB, and use `DoGetInfo` to cross-check before allocating.

### B-8 (L) `PendingSync` hands out pointers into a growing vector

`src/ops/PendingSync.cpp:52-93`

`FindOrAdd()` returns `&g_watches.back()`; `Watch` is stored by value in a `std::vector`. Every
`push_back` can reallocate and invalidate every outstanding pointer. Today each pointer is used
immediately and the code happens to be correct, but the pattern is one refactor away from a
use-after-free (e.g. hoisting `auto* watch = FindOrAdd(...)` above the `Is3DLoaded()` branch).

Fix: `std::deque<Watch>`, or return an index / `std::unique_ptr`.

### B-9 (L) `IsBackTattoo()`'s first branch is dead

`src/ui/UiMain.cpp:224-237`

`if (a_entry.area == "back") return true;` — `area` is canonicalised by `NormalizeArea()` and
filtered by `IsKnownArea()` before it ever reaches the UI, so it is always one of
`Body/Face/Hands/Feet`. Harmless, but it reads as if lowercase area names were possible.

### B-10 (L) A requested slot is silently ignored when JContainers is missing

`src/ops/TattooOps.cpp:244`

`if (slotInRange && jc::Ready()) { … }` — with JContainers unavailable the whole explicit-slot
branch is skipped and execution falls through to `SimpleAddTattoo()`, which always auto-allocates.
The user picked slot 7, gets slot 0, and the status line says "OK". Only the query-failure path
logs a fallback.

Fix: emit the same "falling back to auto allocation" detail into the `OpResult` when
`a_request.targetSlot >= 0 && !jc::Ready()`.

---

## Performance

### P-1 (H) The catalogue rebuild is a main-thread operation with tens of thousands of JContainers calls

`src/data/TattooDatabase.cpp:53-174`; entry points `main.cpp:49,87`, `UiMain.cpp:462-468`
("Rescan"), `UiMain.cpp:1362-1377` (first open after load)

`BuildFromApi()` walks every tattoo and performs 7 JContainers calls per entry:

```cpp
entry.name    = jc::MapGetStr(handle, "name");
entry.section = jc::MapGetStr(handle, "section");
entry.area    = NormalizeArea(jc::MapGetStr(handle, "area"));
entry.texture = jc::MapGetStr(handle, "texture");
... MapGetInt("color"), MapGetFlt("invertedAlpha"), MapGetInt("glow")
```

Each of those goes through `Bs()` (`JcApi.cpp:70-74`), which builds a `std::string` and a fresh
`RE::BSFixedString` — i.e. an interner lock + hash lookup — per key, per entry, per call. On a
10 000-tattoo install that is ~70 000 interned string constructions plus ~40 000 `std::string`
allocations, all on the game thread, three times per session (kDataLoaded, kPostLoadGame, first
open). The "Rescan" button runs it on the render thread, so the window visibly freezes.

Fix, in order of payoff:
1. Hoist the keys into `static const RE::BSFixedString` (or `static const char*` + one `Bs()` each)
   so interning happens 7 times per process instead of 70 000 times.
2. Run `Rebuild()` on the mesh worker (or a dedicated thread) and swap the vector under a mutex;
   the UI can keep drawing the old list until the new one lands.
3. `BuildFromCacheFile()` (the JSON fallback) has the same problem with nlohmann parsing on the
   main thread — same treatment.

### P-2 (M) `KnownTargetCount()` is a JContainers round trip, once per frame

`src/ui/UiMain.cpp:1418-1425` → `src/bridge/KnownTargets.cpp:66-78`

```cpp
ImGui::TextDisabled(..., ng::KnownTargetCount());   // every frame
```

That is `JdbSolveObj(".SlaveTatsMenu.known_targets")` (a string-keyed DB lookup returning a fresh
object id) plus `ArrayCount`, on the render thread, to display a number that changes only when the
plugin registers an actor. Cache it, refresh every few seconds or on `RegisterKnownTarget()`.

### P-3 (M) `Config::Save()` runs on every frame of a colour drag

`src/ui/UiMain.cpp:2173-2176`

```cpp
if (ImGui::ColorEdit3(Tr("$STM_Settings_DefaultInkColor"), inkColor)) {
    config.defaultColor = PackColor(inkColor);
    config.Save();                       // ← every frame while dragging
}
```

`Config::Save()` (Config.cpp:77-122) does `create_directories` + `LoadFile` (to preserve comments) +
~19 `Set*Value` + `SaveFile` + an `info` log line. Dragging the picker therefore writes the INI to
disk ~60 times a second and spams the log. Every other setting in that tab correctly waits for
`IsItemDeactivatedAfterEdit()`.

Fix: set `config.defaultColor` in the `if`, call `Save()` on `IsItemDeactivatedAfterEdit()`.

### P-4 (M) Filtering allocates two lowercase copies per entry, per keystroke

`src/data/CacheParser.cpp:165-189`; `src/data/TattooDatabase.cpp:293-335`

```cpp
if (!keyword.empty()) {
    if (ToLower(entry.name).find(keyword) == std::string::npos &&
        ToLower(entry.section).find(keyword) == std::string::npos) continue;
}
```

`ToLower` constructs a `std::string` (allocation for names over 15 chars, which most are). One
keystroke in the search box = 2 × N allocations and 2 × N scans, N ≈ catalogue size, on the render
thread. `QuerySections()` adds a second copy of the problem: it rebuilds `std::string wanted(keyword)`
**inside** the per-section lambda, so the keyword is lowercased once per section rather than once.

Fix: store a pre-lowered `name`/`section` alongside each `TattooEntry` at build time (the entry is
immutable after `Rebuild`), and hoist `wanted` out of the lambda. Also consider debouncing the
search string by one frame.

### P-5 (M) DDS decoding happens on the render thread with the cache mutex held

`src/render/TextureLoader.cpp:161-202`, `:40-85`

`UpdatePreview()` (UiMain.cpp:198-220) calls `LoadTattooTexture()` synchronously from the render
callback; that takes `s_cacheMutex` for the whole VFS read + `CreateDDSTextureFromMemory` of a
possibly 4K texture. Selecting a tattoo with a large DDS stalls the frame, and any other caller of
the cache blocks behind the decode as well.

Fix: decode on the mesh worker and hand the SRV over through the existing `g_meshResult`-style
mailbox (D3D resource creation stays on the render thread — `CreateDDSTextureFromMemory` takes a
device, so the parse/read half is what should move), and narrow the lock to the map/list
manipulation only.

### P-6 (M) The preview re-renders the whole scene every frame, idle or not

`src/render/MannequinRenderer.cpp:1430-1731`

Per frame, regardless of whether anything changed: `ApplyPendingMeshes()` takes `g_meshMutex`;
`GetDeviceRemovedReason()`; then ~20 `*Get*` calls that each AddRef a COM object, followed by ~20
`*Set*` restores and a matching `Release` sweep; a constant-buffer `Map`/`Unmap`; two full
`DrawIndexed` passes (depth prepass + colour). At 60 fps that is ~1200 COM refcount operations and
120 draw calls per second for a 220 px thumbnail that usually shows a still image.

Fix: keep a dirty flag (camera, colour, alpha, texture, mesh generation) and, when clean, return the
existing `_shaderResourceView` without touching the pipeline. Also skip `ApplyPendingMeshes()` when
a cheap atomic "result ready" flag is false, instead of taking the mutex every frame.

### P-7 (M) The OBody `.tri` file is re-parsed on every preset switch

`src/obody/AddonMain.cpp:377-383`

`ApplyImpl()` calls `LoadTriFile(SiblingTriPath(nifPath))` on every apply. A preset switch triggers
a body-only rebuild, so browsing presets re-reads and re-parses the same `.tri` (hundreds of KB to
several MB of offsets) each time, on the mesh worker.

Fix: cache the parsed `TriFile` keyed by path + `last_write_time`, guarded by a mutex. The data is
immutable and there are at most four bodies in play.

### P-8 (L) Per-frame allocation in the UI hot path

- `SlotSnapshot::Copy()` twice per frame (`UiMain.cpp:630` and `:1134` when the slot combo is open):
  copies up to 12 `SlotInfo`, each with three `std::string`s. Gate it on `revision` — the snapshot
  already carries one.
- List labels are rebuilt every frame: `UiMain.cpp:553` (`label + "##sec_" + section.name`) and
  `:606-614` (3-4 concatenations per row). With the clipper this is ~20 rows, but `PushID` +
  `Selectable(label, …)` removes the `##` suffix allocation entirely.
- `CurrentArea()` returns `std::string` by value and is called twice on one line
  (`UiMain.cpp:1189`), plus once per panel per frame.
- `Translation::Tr()` (`Translation.cpp:299-316`) constructs a temporary `std::string` for each of
  its two `unordered_map::find` calls (the hasher is not transparent), i.e. ~2 temporaries per
  label, dozens of labels per frame. A `std::string_view` key with a transparent hash, or a small
  last-hit cache, removes them.

---

## Suggested order

1. **B-1** — one-frame deferred release; fixes a real crash/glitch on a first-click path.
2. **B-2** — non-blocking `FindPreset`; turns a freeze into a one-frame "still loading" state.
3. **P-1** — hoist the `BSFixedString` keys first (a ~20-line change with the largest single win),
   then move `Rebuild()` off the render thread.
4. **P-2 / P-3** — two one-line-per-site fixes (cache the count, save on deactivate).
5. **B-3 / B-5** — atomic `FormID` for the target, `call_once` for slot limits.
6. **P-4 / P-5 / P-7** — precompute lowercase fields, move texture decode, cache the `.tri`.
7. **P-6** — dirty-flag the preview render.
8. **B-4, B-6, B-7, B-8, B-9, B-10, P-8** — cheap correctness/cleanup, any time.

---

## Notes on what not to change

The ledger-authority contract (`LedgerVerify.h`, `OpResult::Deferred`, `PendingSync`) and the
`PoolGuard` / `CleanPool` pairing are the two invariants this codebase gets right and are worth
protecting from "simplification". The NIF parse offload (`MeshWorkerMain`) and the coalescing logic
in its publish loop are also correct and non-obvious — leave them alone.
