#pragma once

#ifdef STACKSORT_PROFILE
#include <intrin.h>
#pragma intrinsic(__rdtsc)

// Inner-loop phase timing. PROF_DECL snapshots the TSC at the top of an
// iter; PROF_TICK attributes elapsed cycles to the named counter and
// advances the snapshot for the next phase. Zero overhead in non-profile
// builds.
#define PROF_DECL() unsigned long long _profT = __rdtsc()
#define PROF_TICK(acc)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        unsigned long long _now = __rdtsc();                                                                           \
        acc += _now - _profT;                                                                                          \
        _profT = _now;                                                                                                 \
    } while (0)

// One-shot rdtsc timers for per-run phases (pre-reservation scan, greedy
// seed, unconstrained fallback, OptimizeGrouping, BordersRaw). Each phase
// gets a uniquely-named snapshot variable so the BEGIN/END pairs can be
// nested or sequenced without collision.
#define PROF_PHASE_BEGIN(tag) unsigned long long _profPhase_##tag = __rdtsc()
#define PROF_PHASE_END(tag, acc)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        acc += __rdtsc() - _profPhase_##tag;                                                                           \
    } while (0)
#else
#define PROF_DECL() ((void)0)
#define PROF_TICK(acc) ((void)0)
#define PROF_PHASE_BEGIN(tag) ((void)0)
#define PROF_PHASE_END(tag, acc) ((void)0)
#endif

// Sub-phase counters. Gated on STACKSORT_PROFILE_SUBPHASE (not PROFILE) so
// the per-candidate rdtsc pairs don't inflate the coarse SkylinePack/LER
// totals used for apples-to-apples optimization comparisons.
#ifdef STACKSORT_PROFILE_SUBPHASE
#ifndef STACKSORT_PROFILE
#include <intrin.h>
#pragma intrinsic(__rdtsc)
#endif
#define SUBPHASE_BEGIN(tag) unsigned long long _sp_##tag = __rdtsc()
#define SUBPHASE_END(tag, acc)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        acc += (long long)(__rdtsc() - _sp_##tag);                                                                     \
    } while (0)
#else
#define SUBPHASE_BEGIN(tag) ((void)0)
#define SUBPHASE_END(tag, acc) ((void)0)
#endif
