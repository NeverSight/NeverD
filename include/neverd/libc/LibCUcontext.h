#ifndef NEVERD_LIBC_LIBCUCONTEXT_H
#define NEVERD_LIBC_LIBCUCONTEXT_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX ucontext.h — user-level context switching
inline constexpr std::string_view kUcontextHeader = "ucontext.h";

inline constexpr std::array kUcontextFunctions = {
    "getcontext",
    "makecontext",
    "setcontext",
    "swapcontext",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCUCONTEXT_H
