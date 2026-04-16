#pragma once

#include <Debug.h> // KenshiLib: DebugLog, ErrorLog

#include <string>
#include <sstream>

inline std::string IntToStr(int v)
{
    std::stringstream ss;
    ss << v;
    return ss.str();
}

// Info/Error: always active. Route through KenshiLib's loggers.
inline void LogInfo(const std::string& m)
{
    DebugLog(m);
}
inline void LogInfo(const char* m)
{
    DebugLog(m);
}
inline void LogError(const std::string& m)
{
    ErrorLog(m);
}
inline void LogError(const char* m)
{
    ErrorLog(m);
}

// Debug: compiled out in prod. Macro so call-site string construction
// (e.g. "foo" + IntToStr(bar)) is elided entirely.
#ifdef STACKSORT_VERBOSE
#define LogDebug(msg) DebugLog(msg)
#else
#define LogDebug(msg) ((void)0)
#endif
