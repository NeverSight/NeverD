#ifndef NEVERD_LIBC_SYS_LIBCSOCKET_H
#define NEVERD_LIBC_SYS_LIBCSOCKET_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/socket.h — sockets
inline constexpr std::string_view kSysSocketHeader = "sys/socket.h";

inline constexpr std::array kSysSocketFunctions = {
    "accept",      "accept4",    "bind",     "connect",    "getpeername",
    "getsockname", "getsockopt", "listen",   "recv",       "recvfrom",
    "recvmsg",     "send",       "sendmmsg", "sendmsg",    "sendto",
    "setsockopt",  "shutdown",   "socket",   "socketpair",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSOCKET_H
