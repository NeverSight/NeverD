#ifndef NEVERD_LIBC_SYS_LIBCTIME_H
#define NEVERD_LIBC_SYS_LIBCTIME_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/time.h — time operations
inline constexpr std::string_view kSysTimeHeader = "sys/time.h";

inline constexpr std::array kSysTimeFunctions = {
    "getitimer",
    "setitimer",
    "utimes",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCTIME_H
