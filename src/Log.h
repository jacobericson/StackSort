#pragma once

#include <Debug.h>

#include <string>
#include <sstream>

// InfoLog: always active, for lifecycle and user-visible events.
// Uses the same output channel as DebugLog (RE_Kenshi log).
inline void InfoLog(const std::string& message) { DebugLog(message); }
inline void InfoLog(const char* message) { DebugLog(message); }

inline std::string IntToStr(int v)
{
    std::stringstream ss;
    ss << v;
    return ss.str();
}

// In prod builds, DebugLog compiles out entirely (including string
// construction at the call site). InfoLog and ErrorLog remain active.
//
// INCLUDE ORDER: Log.h must be the final Debug.h consumer in any TU.
// A later `#include <Debug.h>` rebinds its macro and silently undoes
// the prod-build compile-out below.
#ifndef STACKSORT_VERBOSE
#undef DebugLog
#define DebugLog(...) ((void)0)
#endif
