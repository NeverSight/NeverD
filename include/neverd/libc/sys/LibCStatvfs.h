#ifndef NEVERD_LIBC_SYS_LIBCSTATVFS_H
#define NEVERD_LIBC_SYS_LIBCSTATVFS_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/statvfs.h — filesystem statistics
inline constexpr std::string_view kSysStatvfsHeader = "sys/statvfs.h";

inline constexpr std::array kSysStatvfsFunctions = {
    "fstatvfs",
    "statvfs",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSTATVFS_H
