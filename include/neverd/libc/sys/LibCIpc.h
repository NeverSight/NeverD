#ifndef NEVERD_LIBC_SYS_LIBCIPC_H
#define NEVERD_LIBC_SYS_LIBCIPC_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/ipc.h — IPC key generation
inline constexpr std::string_view kSysIpcHeader = "sys/ipc.h";

inline constexpr std::array kSysIpcFunctions = {
    "ftok",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCIPC_H
