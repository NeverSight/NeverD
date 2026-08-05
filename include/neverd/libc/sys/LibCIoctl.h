#ifndef NEVERD_LIBC_SYS_LIBCIOCTL_H
#define NEVERD_LIBC_SYS_LIBCIOCTL_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX/Linux sys/ioctl.h — device control
inline constexpr std::string_view kSysIoctlHeader = "sys/ioctl.h";

inline constexpr std::array kSysIoctlFunctions = {
    "ioctl",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCIOCTL_H
