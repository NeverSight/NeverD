#ifndef NEVERD_LIBC_SYS_LIBCEPOLL_H
#define NEVERD_LIBC_SYS_LIBCEPOLL_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// Linux sys/epoll.h — I/O event notification
inline constexpr std::string_view kSysEpollHeader = "sys/epoll.h";

inline constexpr std::array kSysEpollFunctions = {
    "epoll_create", "epoll_create1", "epoll_ctl",
    "epoll_pwait",  "epoll_pwait2",  "epoll_wait",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCEPOLL_H
