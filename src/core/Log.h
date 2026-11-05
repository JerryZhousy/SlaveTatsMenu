#pragma once

#include "PCH.h"

#include <chrono>

namespace stm::log
{
    namespace detail
    {
        // Returns true the first time a given key is seen. Used to keep
        // "still not ready" style messages from flooding the log every frame.
        inline bool MarkOnce(std::string_view a_key)
        {
            static std::mutex                            s_mutex;
            static std::unordered_set<std::string>       s_seen;

            std::scoped_lock lock(s_mutex);
            return s_seen.emplace(a_key).second;
        }
    }

    // Called from SKSEPlugin_Load right after SKSE::Init(). CommonLibSSE's
    // SKSE::Init() installs its own logger and pattern, so anything we set in
    // SKSEPlugin_Query would be thrown away - this has to run afterwards.
    inline void Init()
    {
        spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v"s);
    }

    // Crash breadcrumb.
    //
    // The menu framework calls the window callback from inside the game's own
    // frame, so an access violation there takes the process down with no stack
    // of ours to read: the log simply stops, and the last line names the
    // function that had been entered - which for a menu that draws a hundred
    // widgets per frame is not nearly precise enough to find the culprit.
    //
    // Stage() writes one line per step, and only when the step changes, so a
    // steady-state frame costs one pointer compare. Every call site passes a
    // string literal, so the comparison is on the pointer, not on the text.
    //
    // A menu that redraws every frame still changes stage several times per
    // frame, so the output is capped per second: enough to record the steps that
    // lead into a crash, not enough to bury the rest of the log.
    inline void Stage(const char* a_stage)
    {
        static thread_local const char* s_last = nullptr;
        if (s_last == a_stage) {
            return;
        }
        s_last = a_stage;

        constexpr int kLinesPerSecond = 20;

        static thread_local std::chrono::steady_clock::time_point s_windowStart{};
        static thread_local int                                   s_inWindow = 0;

        const auto now = std::chrono::steady_clock::now();
        if (s_windowStart.time_since_epoch().count() == 0 || (now - s_windowStart) > std::chrono::seconds(1)) {
            s_windowStart = now;
            s_inWindow = 0;
        }
        if (++s_inWindow > kLinesPerSecond) {
            return;
        }

        spdlog::log(spdlog::level::info, "[STAGE] {}", a_stage);
    }

    // Logs at most once per key for the lifetime of the process.
    template <class... Args>
    void Once(std::string_view a_key, spdlog::level::level_enum a_level, spdlog::format_string_t<Args...> a_fmt, Args&&... a_args)
    {
        if (detail::MarkOnce(a_key)) {
            spdlog::log(a_level, a_fmt, std::forward<Args>(a_args)...);
        }
    }
}
