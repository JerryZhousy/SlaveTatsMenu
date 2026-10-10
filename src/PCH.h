#pragma once

// CommonLibSSE-NG
#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

// logging
#include <spdlog/sinks/basic_file_sink.h>

// SKSE Menu Framework bridge (also brings the ImGui C-API declarations)
#include "SKSEMenuFramework.h"

// standard library actually used across the project
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std::literals;
