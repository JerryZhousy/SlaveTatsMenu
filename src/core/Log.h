#pragma once

#include "PCH.h"

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

    // Logs at most once per key for the lifetime of the process.
    template <class... Args>
    void Once(std::string_view a_key, spdlog::level::level_enum a_level, spdlog::format_string_t<Args...> a_fmt, Args&&... a_args)
    {
        if (detail::MarkOnce(a_key)) {
            spdlog::log(a_level, a_fmt, std::forward<Args>(a_args)...);
        }
    }
}
