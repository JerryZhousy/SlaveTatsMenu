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

    // Set from [Debug] StageLog: trace every stage change for the whole
    // session, not only the seconds around a window opening.
    inline bool& StageVerbose()
    {
        static bool s_verbose = false;
        return s_verbose;
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
    // Tracing is on for the first two seconds after a window opens, and off
    // afterwards. A crash on open is the case this exists for, and a steady
    // menu changes stage twelve times per frame: measured on a real session,
    // an always-on trace was 221 of 252 log lines. StageLog=1 in the INI
    // switches the full trace back on for a session that has to be chased.
    inline void Stage(const char* a_stage)
    {
        static thread_local const char* s_last = nullptr;
        if (s_last == a_stage) {
            return;
        }
        s_last = a_stage;

        const auto now = std::chrono::steady_clock::now();

        // A gap between frames means the menu was just opened, or the game came
        // back from a load screen - the two moments a fresh trace is worth
        // having. Nothing else re-arms it.
        static thread_local std::chrono::steady_clock::time_point s_lastCall{};
        static thread_local std::chrono::steady_clock::time_point s_traceUntil{};
        if (s_lastCall.time_since_epoch().count() == 0 || (now - s_lastCall) > std::chrono::milliseconds(500)) {
            s_traceUntil = now + std::chrono::seconds(2);
        }
        s_lastCall = now;

        if (!StageVerbose() && now > s_traceUntil) {
            return;
        }

        constexpr int kLinesPerSecond = 20;

        static thread_local std::chrono::steady_clock::time_point s_windowStart{};
        static thread_local int                                   s_inWindow = 0;

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
