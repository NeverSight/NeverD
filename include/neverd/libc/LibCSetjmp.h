#ifndef NEVERD_LIBC_LIBCSETJMP_H
#define NEVERD_LIBC_LIBCSETJMP_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C setjmp.h — non-local jumps
inline constexpr std::string_view kSetjmpHeader = "setjmp.h";

inline constexpr std::array kSetjmpFunctions = {
    "longjmp",
    "setjmp",
    "siglongjmp",
    "sigsetjmp",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSETJMP_H
