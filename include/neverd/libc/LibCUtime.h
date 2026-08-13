#ifndef NEVERD_LIBC_LIBCUTIME_H
#define NEVERD_LIBC_LIBCUTIME_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX utime.h — file access and modification times
inline constexpr std::string_view kUtimeHeader = "utime.h";

inline constexpr std::array kUtimeFunctions = {
    "utime",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCUTIME_H
