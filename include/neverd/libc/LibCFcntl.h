#ifndef NEVERD_LIBC_LIBCFCNTL_H
#define NEVERD_LIBC_LIBCFCNTL_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX fcntl.h — file control
inline constexpr std::string_view kFcntlHeader = "fcntl.h";

inline constexpr std::array kFcntlFunctions = {
    "creat",
    "fcntl",
    "open",
    "openat",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCFCNTL_H
