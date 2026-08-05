#ifndef NEVERD_LIBC_SYS_LIBCWAIT_H
#define NEVERD_LIBC_SYS_LIBCWAIT_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/wait.h — process wait
inline constexpr std::string_view kSysWaitHeader = "sys/wait.h";

inline constexpr std::array kSysWaitFunctions = {
    "wait",
    "wait4",
    "waitid",
    "waitpid",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCWAIT_H
