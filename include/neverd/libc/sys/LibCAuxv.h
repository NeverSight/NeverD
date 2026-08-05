#ifndef NEVERD_LIBC_SYS_LIBCAUXV_H
#define NEVERD_LIBC_SYS_LIBCAUXV_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// Linux sys/auxv.h — auxiliary vector access
inline constexpr std::string_view kSysAuxvHeader = "sys/auxv.h";

inline constexpr std::array kSysAuxvFunctions = {
    "getauxval",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCAUXV_H
