#pragma once

#include <sstream>
#include <string>

namespace stm::diag
{
    // Compares two probe samples of the form "0=v|1=v|..." and returns the
    // segments that differ, separated by two spaces. An empty result means
    // "nothing changed".
    //
    // This decides when the probe writes a log line at all, so it is worth
    // getting right - and it is deliberately pure so the offline self-test can
    // cover it (see tools/selftest).
    inline std::string DiffSegments(const std::string& a_old, const std::string& a_new)
    {
        std::string       result;
        std::stringstream oldStream(a_old);
        std::stringstream newStream(a_new);
        std::string       oldSegment;
        std::string       newSegment;

        while (std::getline(newStream, newSegment, '|')) {
            if (!std::getline(oldStream, oldSegment, '|') || oldSegment != newSegment) {
                if (!result.empty()) {
                    result += "  ";
                }
                result += newSegment;
            }
        }
        return result;
    }
}
