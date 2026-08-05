#ifndef NEVERD_LIBC_SYS_LIBCUIO_H
#define NEVERD_LIBC_SYS_LIBCUIO_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/uio.h — scatter/gather I/O
inline constexpr std::string_view kSysUioHeader = "sys/uio.h";

inline constexpr std::array kSysUioFunctions = {
    "readv",
    "writev",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCUIO_H
