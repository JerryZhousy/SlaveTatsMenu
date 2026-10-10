# Third-party components

What this source tree contains, and what ends up inside `SKSE/Plugins/SlaveTatsMenu.dll`.
Full license texts are in `LICENSES/`; the license for this project's own code is in `LICENSE`.

The release package is the DLL plus `SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini`. No file from
SlaveTats, SlaveTatsNG, JContainers, SKSE Menu Framework or RaceMenu is redistributed here.

## Integration headers kept in this tree

- **SlaveTatsNG_Interface.h** — nopse0, [SlaveTatsNG](https://github.com/nopse0/SlaveTatsNG),
  Apache-2.0 (`LICENSES/Apache-2.0.txt`). Declares SlaveTatsNG's published C++ interface table,
  which this plugin looks up at runtime.
- **JContainers/jc_interface.h** — SilverIce, [JContainers](https://github.com/SilverIce/JContainers),
  maintained by ryobg, MIT (`LICENSES/JContainers.txt`). Matches upstream `master` apart from the
  trailing newline.
- **JContainers/jcontainers_constants.h** — same project and license. This copy is the v4.2.9
  revision (JC API 4.2.9), not current `master` (4.3.4); it is older, not modified.
- **SKSEMenuFramework.h** — QTR-Modding,
  [SKSE-Menu-Framework-3-API](https://github.com/QTR-Modding/SKSE-Menu-Framework-3-API), LGPL-2.1
  (`LICENSES/SKSEMenuFramework-API.txt`). It declares the framework's exported renderer functions
  and the ImGui structures they take. The framework itself is a separate, user-installed DLL; this
  plugin resolves its exports with `GetProcAddress` and does not link or redistribute it. The
  framework's own source is at
  [SKSE-Menu-Framework-3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3) under GPL-3.0.
- **nlohmann/json.hpp 3.11.3** — Niels Lohmann, MIT (`LICENSES/nlohmann-json.txt`). Compiled into
  the plugin for cache and preset parsing. The single-header file also carries SPDX notices for the
  Hedley preprocessor macros and for the Abseil-derived `from_chars` / `to_chars` code; those
  notices are left as upstream ships them.
- **SimpleIni.h** — Brodie Thiesfield, MIT (`LICENSES/SimpleIni.txt`). Compiled into the plugin as
  the INI reader for `SlaveTatsMenu.ini` and `skee64.ini`.
- **Dear ImGui 1.89.6** — Omar Cornut and contributors, MIT (`LICENSES/imgui.txt`): `imgui.h`,
  `imgui_internal.h`, `imgui_stdlib.h`, `imstb_rectpack.h`, `imstb_textedit.h`, `imstb_truetype.h`,
  `imgui_impl_dx11.h`. Kept for reference only; no translation unit in `src/` includes them, because
  the renderer lives in the framework's DLL.

## Local modifications to bundled headers

Marked here because Apache-2.0 §4(b) and LGPL-2.1 §2(b) require changed files to carry a prominent
notice of the change and its date.

- `include/SlaveTatsNG_Interface.h`, changed 2026-10-09: added a four-line guard at the top
  (`#ifdef interface` / `#undef interface` / `#endif` plus a blank line), because `interface` is a
  macro from `combaseapi.h` in MSVC builds. Verified as the only difference from upstream `master`.
- `include/SKSEMenuFramework.h`, changed 2026-10-09: a modified copy of the published API header
  (11411 lines against upstream's 11214). The module lookup was renamed to
  `GetMenuFrameworkHandle` behind a `menuFramework` macro, `<filesystem>` was added for the
  install check, an extra `namespace ImGui = ImGuiMCP` alias plus a using-directive were appended,
  and a few bindings were added. Upstream header:
  `SKSEMenuFramework.h` in the API repository above.
- `include/JContainers/*.h`, `include/SimpleIni.h`, `include/nlohmann/json.hpp`,
  `include/imgui*.h`, `include/imstb_*.h`: unmodified copies at the revisions described above.

## Statically linked into SlaveTatsMenu.dll

- **CommonLibSSE-NG 6.7.1** — GPL-3.0-or-later (`LICENSES/CommonLibSSE-NG.txt`) with the upstream
  Modding and GPL-3.0 section 7 linking exceptions
  (`LICENSES/CommonLibSSE-NG-EXCEPTIONS.md`).
  <https://github.com/CharmedBaryon/CommonLibSSE>. Built from an unmodified checkout: tag v6.7.1
  plus two commits, commit `fd60ebdfe`, git working tree clean. Corresponding source for the parts
  used here is that upstream checkout.
- **spdlog** — Gabi Melman, MIT (`LICENSES/spdlog.txt`). Reached through CommonLibSSE-NG's logging.
- **{fmt}** — Victor Zverovich and contributors, MIT (`LICENSES/fmt.txt`). Same route as spdlog.
- **DirectXTK** — Microsoft, MIT (`LICENSES/directxtk.txt`). `DirectXTK.lib` is in the plugin's
  link line.
- **DirectXMath** — Microsoft, MIT (`LICENSES/directxmath.txt`). Header-only; arrives with
  DirectXTK.
- **hde64** — the instruction-length decoder vendored from MinHook, Tsuda Kageyu, BSD-2-Clause
  (`LICENSES/MinHook-hde64.txt`). CommonLibSSE-NG fetches MinHook's sources with `FetchContent` and
  compiles `hde64.c` into the static library the plugin links.
- **OpenVR** — Valve Corporation, BSD-3-Clause (`LICENSES/OpenVR.txt`). CommonLibSSE-NG links
  `openvr_api.lib` because `CMakeLists.txt` enables the Skyrim VR targets alongside SE and AE.

## Runtime dependencies, not redistributed

Required in the game, but not built against and not shipped:

- **SlaveTatsNG** (nopse0, Apache-2.0): the plugin drives the interface it publishes.
- **SlaveTats** (murfk): the original mod SlaveTatsNG installs over. No Papyrus script, ESP or asset
  from it is copied into this project; the plugin only reads and writes the JContainers ledger keys
  that SlaveTats defines.
- **JContainers SE** (SilverIce / ryobg, MIT): the storage layer behind that ledger.
- **SKSE Menu Framework** (QTR-Modding): hosts the window and the ImGui renderer.
- **SKSE64** (the SKSE team): its plugin API headers are what CommonLibSSE-NG compiles against.
- **RaceMenu**: read for `skee64.ini` only, to get the per-area slot counts.

## Not part of this build

`xbyak` (the build prints `Enable Xbyak: OFF`), `rapidcsv`, `toml11` and `boost` appear in the shared
vcpkg tree used by other projects, but none of them shows up in this build's include or link lines.

## Packaging notes

- Ship `SKSE/Plugins/SlaveTatsMenu.dll` and `SKSE/Plugins/SlaveTatsMenu/SlaveTatsMenu.ini`. Nothing
  else from `SKSE/`.
- Ship the notices alongside them, or the binary goes out without the licenses it is required to
  carry: `LICENSE`, `THIRD_PARTY.md` and every file in `LICENSES/`. `tools/package-release.ps1`
  builds that archive into `dist/` and prints the contents; it refuses to run if a notice is
  missing and never picks up `build/` or the old `.bak` DLL.
- Do not ship `build/`. It holds CommonLibSSE-NG object files and `CommonLibSSE.lib`, the fetched
  MinHook/hde64 sources and a precompiled header of several hundred megabytes, plus the old
  `SlaveTatsMenu.dll.bak-before-calltrace`. Those are development artifacts, not redistribution
  targets.
- GPL obligations for the binary are satisfied by the notices in `LICENSES/` plus the published
  source of the plugin and the upstream CommonLibSSE-NG revision named above.
- Two vendored headers carry local changes; the change notice is inside each file as well as in
  the section above, per Apache-2.0 section 4(b) and LGPL-2.1 section 2(b).
