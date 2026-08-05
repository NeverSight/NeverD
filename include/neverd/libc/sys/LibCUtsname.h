#ifndef NEVERD_LIBC_SYS_LIBCUTSNAME_H
#define NEVERD_LIBC_SYS_LIBCUTSNAME_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX sys/utsname.h — system identification
inline constexpr std::string_view kSysUtsnameHeader = "sys/utsname.h";

inline constexpr std::array kSysUtsnameFunctions = {
    "uname",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCUTSNAME_H
