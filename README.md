# SlaveTats Menu

An in-game tattoo editor for [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG), running as an SKSE plugin with a Dear ImGui window drawn over the running game. Browse a few thousand tattoos by body area, mod or name, apply and recolour them, see which overlay slots an actor is using, clear individual slots, and save an actor's whole set as a preset you can apply to someone else.

It talks to SlaveTatsNG's published C++ interface directly through SKSE rather than going through Papyrus. The catalogue lives in memory with virtual scrolling, every operation is verified by reading the ledger back, and the actor you touched is registered in `.SlaveTatsMenu.known_targets` so the tattoos survive a reload. There is no ESP and no plugin slot involved.

<!-- Nexus page URL goes here once the page is live. -->

## Requirements

| | |
|---|---|
| SKSE64 | 1.5.39 or newer, matching your game version |
| [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG) | required |
| SlaveTats (the original by murfk) | required, SlaveTatsNG installs over it |
| [JContainers SE](https://www.nexusmods.com/skyrimspecialedition/mods/16495) | required |
| [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352) | required, it hosts the window |
| RaceMenu | needed by SlaveTats itself; its `skee64.ini` supplies the per-area slot limits this plugin displays |
| [OBody NG](https://www.nexusmods.com/skyrimspecialedition/mods/77016) 4.3.7+ | optional: lets the 3D preview show the selected actor's preset instead of the zeroed body |

Built with CommonLibSSE-NG, so the DLL covers SE 1.5.39+ and AE. SE 1.5.97 is the runtime it has actually been played through; AE loads but is unverified. VR is not supported.

## Install

Install the release archive directly via your mod manager (Mod Organizer 2 or Vortex), which places `SKSE/Plugins/SlaveTatsMenu.dll` and the configuration `SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini`. The archive carries a FOMOD installer whose single step asks for the interface language and writes it into that INI. Installing by hand works too: copy the `SKSE` folder into your game `Data` directory.

The interface language (Simplified Chinese or English) can be toggled on the fly in the in-game Settings tab or configured via `Language = zh` / `en` in `SlaveTatsMenu.ini`.

Nothing to enable in the MCM, no ESP to activate.

## Usage

`F10` toggles the window, and `ToggleKey` in the INI rebinds it (a DirectInput scan code, `0x44` being F10). The window is also reachable from SKSE Menu Framework's own menu as `SlaveTats -> Tattoo Manager`.

The window is two panels. The left one is a two-step browser: it lists every mod that ships tattoos, filtered by the body area you select at the top, and picking a mod (or `[All mods]`) replaces that same column with the tattoos that mod ships. A `< Back` button on the first row of the tattoo list returns to the mod list. The column keeps the width it has in both steps, so stepping in and out never moves the inspector. The right one shows the selected tattoo's texture, colour, opacity and target slot, with the apply and remove actions underneath, followed by the slot status of the current body area, presets, and the diagnostics header. Going back leaves the selected tattoo and everything you dialled into it alone, so you can look at another mod without losing your work. Clicking the tattoo that is already selected clears it again, which empties the inspector without leaving the list.

The catalogue is rebuilt when a game loads, and once more the first time a window is opened after that load. The second pass exists because SlaveTatsNG binds its tattoo cache per save and the two are not ordered: a rebuild that wins the race would leave the whole session reading the `slavetats_cache.json` fallback until you pressed `Rescan`. It is skipped when the live API source already won, so it never costs a full pack scan for nothing.

The 3D preview in that right-hand panel follows the area picked at the top: `All` shows the whole body, and picking a tattoo switches the preview to that tattoo's own part, so there is no second part selector to keep in sync. Its ink is deliberately optimistic. A texture that carries its own colours is drawn as authored while the ink colour is still the default black, which keeps the pattern legible; a black ink colour makes the game render that same texture black. Line art is tinted the way the game tints it.

Results appear in the status line at the bottom of the window:

- `OK` means the ledger changed as requested and the overlay was synchronised.
- `DEFERRED` means the data is written but the sync did not run: the target's 3D was not loaded, or SlaveTatsNG gave up for another reason (its NiOverride lock was taken, or adding the overlay nodes failed). With `AutoFinishSync` on - the default - the menu finishes that sync itself the moment the body is there, and says so when it does.
- `FAILED` comes with a reason.

### Overlay slots are shared with RaceMenu

SlaveTats and RaceMenu's own tattoo page write the same NiOverride nodes: each area has a fixed number of slots (`iNumOverlays` in `skee64.ini`, 12/6/6/6 by default), named `Body [Ovl0]` and so on. A tattoo from either side takes one for good. The slot status panel therefore counts both: a slot that belongs to another overlay mod is listed in orange as `[external]` and is never written to or cleared, and the header shows how many slots are still free. Once an area reaches zero, RaceMenu's tattoo page has nowhere left to preview or apply into either - that is what "the tattoo menu stopped working after I installed this" usually is. Clear a slot, or raise `iNumOverlays`, and both work again.

### OBody preview

An OBody install ships a *zeroed* body: the shape is applied at runtime, so the NIF on disk has none of it. The bundled `SlaveTatsMenuOBody.dll` makes the preview draw the preset the selected actor is actually wearing. It is installed by default and does nothing when OBody is absent:

- the preset name comes from OBody NG's plugin API (`GetPresetAssignedToActor`, OBody 4.3.7 or newer);
- if that fails - older OBody, or the preset XML is gone - it falls back to the `OBody` morph values RaceMenu already holds for the actor, which is what the game is showing anyway;
- the preset's BodySlide sliders are turned into vertex offsets with the body's `.tri` file (the one sitting next to the NIF), so 3BA, UBE and HIMBO work as long as BodySlide was run with **Build Morphs**;
- bone-scale sliders are not applied, and the preview always renders the weight-100 (`_1`) mesh;
- an actor with no assigned preset keeps the zeroed body, and the mesh line says `OBody: <preset>` when a shape is in use.

Resolving runs once per menu opening, and again when the target, the sex or the preset changes. The `SliderPresets` folder is scanned on the loading worker the first time a preset is needed, and the body is deformed there too, so neither the game thread nor the render thread ever waits for it. `PreviewUseOBody = 0` in the INI turns it off.

## Configuration
`Data/SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini`

```ini
[General]
ToggleKey = 0x44        ; DirectInput scan code, 0x44 = F10
DefaultColor = 0x000000 ; starting colour for a new tattoo
DefaultAlpha = 1.0
Language = en           ; en / zh
AutoFinishSync = 1      ; finish the syncs SlaveTatsNG dropped while the window is open

[UI]
WindowWidth = 1280.0
WindowHeight = 720.0
SlotRefreshMs = 250     ; how often the slot list is rebuilt
PreviewUseOBody = 1     ; draw the target's OBody preset (needs the add-on DLL)

[Debug]
Probe = 0               ; diagnostic probe, writes only changes to the log
ProbeIntervalMs = 1000
ProbeArea = Body
ProbeSlots = 12
CallTrace = 0           ; log which module called each mutating SlaveTatsNG function
                        ; (opt-in: it patches another plugin's read-only table)
```

Presets are plain JSON, one file per preset, in `Data/SKSE/Plugins/SlaveTatsMenu/Presets/`. They can be edited by hand or handed to someone else.

The log is at `Documents/My Games/Skyrim Special Edition/SKSE/SlaveTatsMenu.log`. It records the boot handshake, which catalogue source won, and the outcome of every operation. When a tattoo shows up in the ledger but not on the body, turn on the probe, press `Snapshot now`, and the log will name the likely cause: 3D not loaded, a stuck `.updated` flag, an empty `.SlaveTats.version`, or another overlay mod holding the slot.

That stuck `.updated` flag is worth knowing about: it is SlaveTatsNG's own "this actor still needs a sync" marker, raised by every ledger write and cleared only by a sync that runs to the end. Nothing in SlaveTatsNG ever reads it again, so a sync that is skipped - the body was not loaded yet, the NiOverride lock was held by something else - strands the actor until something calls sync again. That is what "the tattoo is in the menu but not on the body, and force sync fixes it" looks like, and it is not specific to this menu: a mod that applies tattoos on an actor it has not looked at hits the same wall. `AutoFinishSync` closes the loop while the window is open, retrying a flagged actor up to five times, two seconds apart.

## Build from source

Visual Studio 2022, CMake 3.22 or newer, vcpkg with the `x64-windows-static-md` triplet, and a [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) checkout:

```cmd
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
    -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
    -DCOMMONLIBSSE_DIR=<CommonLibSSE-NG checkout>
cmake --build build --config Release
```

The DLL lands in `SKSE/Plugins/SlaveTatsMenu.dll`. `build.cmd` is the same thing with this machine's absolute paths baked in.

The tree also produces an offline self-test that exercises the data layer (cache parsing, area normalisation, filtering, preset round-trip, name sanitising) against a real `slavetats_cache.json`, with no game and no CommonLibSSE involved:

```cmd
build\selftest\stm_selftest.exe "<path>\slavetats_cache.json"
```

`tools/check-acceptance.ps1` reads a SlaveTatsMenu.log and reports whether a session actually exercised the interface handshake, the catalogue, the operations and the log contract.

The Chinese developer notes are in [`docs/README.zh.md`](docs/README.zh.md).

## Layout

```
src/main.cpp          plugin lifecycle, SKSE messages, subsystem wiring
src/core/             log, config, main-thread tasks and result board, UTF-8
src/bridge/           SlaveTatsNG / JContainers wrappers, known_targets contract
src/data/             catalogue, slot snapshots, target state, preset IO
src/ops/              apply / recolour / remove / clear / sync / presets
src/ui/               ImGui window and the hotkey
src/diag/             diagnostic probe, call tracer, diffing
tools/selftest/       offline self-test
include/              vendored interface headers (see below)
```

## Third-party code

Nothing from SlaveTatsNG, JContainers, the original SlaveTats or SKSE Menu Framework is redistributed in the release archive; the release contains only this plugin's DLL and an INI. The source tree vendors the first group of headers below, and the DLL statically links the second group. License texts are in `LICENSES/`; `THIRD_PARTY.md` records versions, revisions and the local changes made to two of the headers.

| Path / component | Project | License |
|---|---|---|
| `include/SimpleIni.h` | SimpleIni | MIT |
| `include/nlohmann/json.hpp` | JSON for Modern C++ 3.11.3 | MIT |
| `include/imgui*.h`, `include/imstb_*.h` | Dear ImGui 1.89.6, kept for reference only | MIT |
| `include/JContainers/*.h` | JContainers, constants at v4.2.9 | MIT |
| `include/SlaveTatsNG_Interface.h` | SlaveTatsNG, plus a local `#undef interface` guard | Apache-2.0 |
| `include/SKSEMenuFramework.h` | SKSE Menu Framework consumer header, locally modified | LGPL-2.1 |
| linked into the DLL | CommonLibSSE-NG 6.7.1 | GPL-3.0-or-later with linking exceptions |
| linked into the DLL | spdlog, {fmt} | MIT |
| linked into the DLL | DirectXTK, DirectXMath | MIT |
| linked into the DLL | hde64, vendored from MinHook | BSD-2-Clause |
| linked into the DLL | OpenVR | BSD-3-Clause |

## Credits

SlaveTatsNG and its C++ interface by nopse0. The original SlaveTats by murfk. JContainers by ryobg and SilverIce. SKSE Menu Framework by Thiago. SKSE by the SKSE team, and everyone who kept the tattoo packs coming. On the code side: CommonLibSSE-NG and the CommonLibSSE authors, Dear ImGui by Omar Cornut and contributors, JSON for Modern C++ by Niels Lohmann, SimpleIni by Brodie Thiesfield, MinHook and hde64 by Tsuda Kageyu, OpenVR by Valve.
