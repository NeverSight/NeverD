#ifndef NEVERD_LIBC_LIBCTIME_H
#define NEVERD_LIBC_LIBCTIME_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C time.h — calendar time and clocks
inline constexpr std::string_view kTimeHeader = "time.h";

inline constexpr std::array kTimeFunctions = {
    "asctime",       "asctime_r",    "clock",     "clock_gettime",
    "clock_settime", "ctime",        "ctime_r",   "difftime",
    "gmtime",        "gmtime_r",     "localtime", "localtime_r",
    "mktime",        "nanosleep",    "strftime",  "strftime_l",
    "time",          "timespec_get",
};

/// difftime(time_t, time_t) -> double: two integer args, FP return (FpRet).
inline constexpr auto kTimeArity = std::to_array<LibCArityEntry>({
    {"difftime", {2, 0, false, false, true}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCTIME_H
