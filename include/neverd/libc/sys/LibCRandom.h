#ifndef NEVERD_LIBC_SYS_LIBCRANDOM_H
#define NEVERD_LIBC_SYS_LIBCRANDOM_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// Linux sys/random.h — random number generation
inline constexpr std::string_view kSysRandomHeader = "sys/random.h";

inline constexpr std::array kSysRandomFunctions = {
    "getrandom",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_SYS_LIBCRANDOM_H
