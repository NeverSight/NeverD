#ifndef NEVERD_LIBC_SYS_LIBCRESOURCE_H
#define NEVERD_LIBC_SYS_LIBCRESOURCE_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/resource.h — resource limits and usage
inline constexpr std::string_view kSysResourceHeader = "sys/resource.h";

inline constexpr std::array kSysResourceFunctions = {
    "getrlimit",
    "getrusage",
    "setrlimit",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCRESOURCE_H
