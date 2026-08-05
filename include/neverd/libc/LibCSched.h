#ifndef NEVERD_LIBC_LIBCSCHED_H
#define NEVERD_LIBC_LIBCSCHED_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sched.h — process scheduling
inline constexpr std::string_view kSchedHeader = "sched.h";

inline constexpr std::array kSchedFunctions = {
    "sched_get_priority_max",
    "sched_get_priority_min",
    "sched_getaffinity",
    "sched_getcpu",
    "sched_getparam",
    "sched_getscheduler",
    "sched_rr_get_interval",
    "sched_setaffinity",
    "sched_setparam",
    "sched_setscheduler",
    "sched_yield",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSCHED_H
