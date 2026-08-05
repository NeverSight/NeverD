#ifndef NEVERD_LIBC_SYS_LIBCSELECT_H
#define NEVERD_LIBC_SYS_LIBCSELECT_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/select.h — synchronous I/O multiplexing
inline constexpr std::string_view kSysSelectHeader = "sys/select.h";

inline constexpr std::array kSysSelectFunctions = {
    "pselect",
    "select",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCSELECT_H
