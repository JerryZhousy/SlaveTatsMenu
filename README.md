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

Built with CommonLibSSE-NG, so the DLL covers SE 1.5.39+ and AE. SE 1.5.97 is the runtime it has actually been played through; AE loads but is unverified. VR is not supported.

## Install

Install the release archive directly via your mod manager (Mod Organizer 2 or Vortex), which places `SKSE/Plugins/SlaveTatsMenu.dll` and the configuration `SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini`. Installing by hand works too: copy the `SKSE` folder into your game `Data` directory.

The interface language (Simplified Chinese or English) can be toggled on the fly in the in-game Settings tab or configured via `Language = zh` / `en` in `SlaveTatsMenu.ini`.

Nothing to enable in the MCM, no ESP to activate.

## Usage

`F10` toggles the window, and `ToggleKey` in the INI rebinds it (a DirectInput scan code, `0x44` being F10). The window is also reachable from SKSE Menu Framework's own menu as `SlaveTats -> Tattoo Manager`.

The window is three panels. The left one lists every mod that ships tattoos, filtered by the body area you select at the top. The middle one is the tattoo list for whatever is selected on the left, and it stays collapsed until you pick a mod; clicking the same entry again closes it and hands its width back to the mod list. The right one shows the selected tattoo's texture, colour, opacity and target slot, with the apply and remove actions underneath, followed by the slot status of the current body area, presets, and the diagnostics header.

The 3D preview in that right-hand panel follows the area picked at the top: `All` shows the whole body, and picking a tattoo switches the preview to that tattoo's own part, so there is no second part selector to keep in sync. Its ink is deliberately optimistic. A texture that carries its own colours is drawn as authored while the ink colour is still the default black, which keeps the pattern legible; a black ink colour makes the game render that same texture black. Line art is tinted the way the game tints it.

Results appear in the status line at the bottom of the window:

- `OK` means the ledger changed as requested and the overlay was synchronised.
- `DEFERRED` means the data is written but the target's 3D was not loaded, so re-apply once the actor is in view.
- `FAILED` comes with a reason.

## Configuration

`Data/SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini`

```ini
[General]
ToggleKey = 0x44        ; DirectInput scan code, 0x44 = F10
DefaultColor = 0x000000 ; starting colour for a new tattoo
DefaultAlpha = 1.0
Language = en           ; en / zh

[UI]
WindowWidth = 1280.0
WindowHeight = 720.0
SlotRefreshMs = 250     ; how often the slot list is rebuilt

[Debug]
Probe = 0               ; diagnostic probe, writes only changes to the log
ProbeIntervalMs = 1000
ProbeArea = Body
ProbeSlots = 12
CallTrace = 1           ; log which module called each mutating SlaveTatsNG function
```

Presets are plain JSON, one file per preset, in `Data/SKSE/Plugins/SlaveTatsMenu/Presets/`. They can be edited by hand or handed to someone else.

The log is at `Documents/My Games/Skyrim Special Edition/SKSE/SlaveTatsMenu.log`. It records the boot handshake, which catalogue source won, and the outcome of every operation. When a tattoo shows up in the ledger but not on the body, turn on the probe, press `Snapshot now`, and the log will name the likely cause: 3D not loaded, a stuck `.updated` flag, an empty `.SlaveTats.version`, or another overlay mod holding the slot.

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
