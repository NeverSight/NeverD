#ifndef NEVERD_LIBC_SYS_LIBCPRCTL_H
#define NEVERD_LIBC_SYS_LIBCPRCTL_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// Linux sys/prctl.h — process control
inline constexpr std::string_view kSysPrctlHeader = "sys/prctl.h";

inline constexpr std::array kSysPrctlFunctions = {
    "prctl",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCPRCTL_H
