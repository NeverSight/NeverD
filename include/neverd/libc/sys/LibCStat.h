#ifndef NEVERD_LIBC_SYS_LIBCSTAT_H
#define NEVERD_LIBC_SYS_LIBCSTAT_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/stat.h — file status
inline constexpr std::string_view kSysStatHeader = "sys/stat.h";

inline constexpr std::array kSysStatFunctions = {
    "chmod", "fchmod",  "fchmodat", "fstat", "fstatat", "lstat",
    "mkdir", "mkdirat", "mkfifo",   "stat",  "umask",   "utimensat",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSTAT_H
