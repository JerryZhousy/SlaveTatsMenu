cmake_minimum_required(VERSION 3.22)

project(SlaveTatsMenu VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

# CommonLibSSE-NG checkout. Override with -DCOMMONLIBSSE_DIR=<path> when the
# dependency lives somewhere else (see build.cmd).
if(NOT DEFINED COMMONLIBSSE_DIR)
    set(COMMONLIBSSE_DIR "D:/Projects/OpenAnimationReplacer/extern/CommonLibSSE" CACHE PATH "CommonLibSSE-NG source directory")
endif()
add_subdirectory("${COMMONLIBSSE_DIR}" CommonLibSSE)

set(STM_SOURCES
    src/PCH.h
    src/main.cpp
    src/core/Config.cpp
    src/core/Config.h
    src/core/Log.h
    src/core/Tasks.h
    src/bridge/JcApi.cpp
    src/bridge/JcApi.h
    src/bridge/KnownTargets.cpp
    src/bridge/KnownTargets.h
    src/bridge/SlaveTatsNg.cpp
    src/bridge/SlaveTatsNg.h
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
    src/data/TargetState.cpp
    src/data/TargetState.h
    src/ui/InputHandler.h
    src/ui/Ui.h
    src/ui/UiMain.cpp
    src/ui/UiState.h
    src/diag/Probe.cpp
    src/diag/Probe.h
    src/diag/CallTrace.cpp
    src/diag/CallTrace.h
    src/render/MannequinRenderer.cpp
    src/render/MannequinRenderer.h
    src/render/NifMeshExtractor.cpp
    src/render/NifMeshExtractor.h
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

set_target_properties(${PROJECT_NAME} PROPERTIES
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
