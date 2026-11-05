cmake_minimum_required(VERSION 3.22)

project(SlaveTatsMenu VERSION 1.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
# Left ON so the binary keeps loading on the VR runtime, but VR is NOT supported:
# the mannequin is an offscreen flat D3D11 target and the UI assumes a screen.
# Note this is not FORCE'd, so `-DENABLE_SKYRIM_VR=OFF` actually sticks and gives
# a build that refuses to load in VR instead of misbehaving there - at the cost of
# rebuilding all of CommonLibSSE-NG.
set(ENABLE_SKYRIM_VR ON CACHE BOOL "")
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

# CommonLibSSE-NG checkout. Override with -DCOMMONLIBSSE_DIR=<path> when the
# dependency lives somewhere else (see build.cmd).
if(NOT DEFINED COMMONLIBSSE_DIR)
    set(COMMONLIBSSE_DIR "D:/Projects/OpenAnimationReplacer/extern/CommonLibSSE" CACHE PATH "CommonLibSSE-NG source directory")
endif()
# add_subdirectory() on a missing path produces a wall of confusing errors about
# a missing CMakeLists.txt; say what is actually wrong.
if(NOT EXISTS "${COMMONLIBSSE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "CommonLibSSE-NG not found at '${COMMONLIBSSE_DIR}'.\n"
        "Pass -DCOMMONLIBSSE_DIR=<path to your CommonLibSSE-NG checkout>, "
        "or edit the default in CMakeLists.txt.")
endif()
add_subdirectory("${COMMONLIBSSE_DIR}" CommonLibSSE)

set(STM_SOURCES
    src/PCH.h
    src/main.cpp
    src/core/Config.cpp
    src/core/Config.h
    src/core/Translation.cpp
    src/core/Translation.h
    src/core/Log.h
    src/core/Tasks.h
    src/bridge/JcApi.cpp
    src/bridge/JcApi.h
    src/bridge/KnownTargets.cpp
    src/bridge/KnownTargets.h
    src/bridge/SlaveTatsNg.cpp
    src/bridge/SlaveTatsNg.h
    src/bridge/NgGuardedCall.cpp
    src/bridge/NgGuardedCall.h
    src/data/SlotSnapshot.cpp
    src/data/SlotSnapshot.h
    src/data/CacheParser.cpp
    src/data/CacheParser.h
    src/data/PresetIO.cpp
    src/data/PresetIO.h
    src/data/PresetStore.cpp
    src/data/PresetStore.h
    src/data/TattooDatabase.cpp
    src/data/TattooDatabase.h
    src/core/Text.h
    src/ops/PresetOps.cpp
    src/ops/PresetOps.h
    src/ops/TattooOps.cpp
    src/ops/TattooOps.h
    src/ops/PendingSync.cpp
    src/ops/PendingSync.h
    src/ops/LoadRepair.cpp
    src/ops/LoadRepair.h
    src/data/TargetState.cpp
    src/data/TargetState.h
    src/obody/OBodyBridge.cpp
    src/obody/OBodyBridge.h
    include/StmOBodyApi.h
    src/ui/InputHandler.h
    src/ui/Ui.h
    src/ui/UiMain.cpp
    src/ui/UiState.h
    src/diag/Probe.cpp
    src/diag/Probe.h
    src/diag/OverlayState.cpp
    src/diag/OverlayState.h
    src/diag/CallTrace.cpp
    src/diag/CallTrace.h
    src/render/MannequinRenderer.cpp
    src/render/MannequinRenderer.h
    src/render/NifMeshExtractor.cpp
    src/render/NifMeshExtractor.h
    src/render/DDSTextureLoader11.cpp
    src/render/DDSTextureLoader11.h
    src/render/TextureLoader.cpp
    src/render/TextureLoader.h
)

add_library(${PROJECT_NAME} SHARED ${STM_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    d3dcompiler
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
    SI_NO_CONVERSION
)

# Single source of truth for the version. fomod/info.xml is stamped from
# ${PROJECT_VERSION} at packaging time; the plugin reported "1" to SKSE while
# the log line said "1.1.0" because the two were written by hand in different
# places. Anything that needs the version reads these instead.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    STM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    STM_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    STM_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    STM_VERSION_STRING="${PROJECT_VERSION}"
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/SKSE/Plugins"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/SKSE/Plugins"
)

# ---------------------------------------------------------------------------
# Optional add-on: the OBody-aware 3D preview.
#
# Separate DLL on purpose. The main plugin must keep loading on a machine with
# no OBody and no RaceMenu-specific API, and this half talks to OBody NG's
# plugin API and to skee64's morph interface - neither of which exists there.
# The two halves only share include/StmOBodyApi.h, a plain C function table.
# ---------------------------------------------------------------------------
set(STM_OBODY_SOURCES
    src/obody/AddonMain.cpp
    src/obody/MorphApply.cpp
    src/obody/MorphApply.h
    src/obody/OBodyPluginApi.h
    src/obody/PresetXml.cpp
    src/obody/PresetXml.h
    src/obody/TriFile.cpp
    src/obody/TriFile.h
    include/StmOBodyApi.h
)

add_library(SlaveTatsMenuOBody SHARED ${STM_OBODY_SOURCES})

target_include_directories(SlaveTatsMenuOBody PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(SlaveTatsMenuOBody PRIVATE
    CommonLibSSE::CommonLibSSE
)

target_compile_definitions(SlaveTatsMenuOBody PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
    # Selects the one place StmOBodyApi.h is allowed to declare the export.
    STM_OBODY_EXPORTS
)

# Same single source of truth as the main target: fomod/info.xml is stamped from
# ${PROJECT_VERSION}, so a hand-written 1.0.0.0 here would be the version drift
# this block exists to prevent.
target_compile_definitions(SlaveTatsMenuOBody PRIVATE
    STM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    STM_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    STM_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    STM_VERSION_STRING="${PROJECT_VERSION}"
)

set_target_properties(SlaveTatsMenuOBody PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/SKSE/Plugins"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/SKSE/Plugins"
)

# Offline self-test for the pure data layer. Deliberately does NOT link
# CommonLibSSE and does not use the plugin PCH: CacheParser only needs the
# standard library and nlohmann/json, which is exactly what makes it testable
# without a game.
add_executable(stm_selftest
    tools/selftest/main.cpp
    src/data/CacheParser.cpp
    src/data/CacheParser.h
    src/data/PresetIO.cpp
    src/data/PresetIO.h
)

target_include_directories(stm_selftest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(stm_selftest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build/selftest"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/build/selftest"
)

# Offline check for the OBody data layer (tri parsing, preset XML, deformation).
# Same rule as the data selftest: standard library only, so it runs anywhere.
add_executable(stm_obodytest
    tools/obodytest/main.cpp
    src/obody/MorphApply.cpp
    src/obody/MorphApply.h
    src/obody/PresetXml.cpp
    src/obody/PresetXml.h
    src/obody/TriFile.cpp
    src/obody/TriFile.h
)

target_include_directories(stm_obodytest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(stm_obodytest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build/obodytest"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/build/obodytest"
)
