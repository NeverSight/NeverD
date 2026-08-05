#ifndef NEVERD_LIBC_LIBCFENV_H
#define NEVERD_LIBC_LIBCFENV_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C fenv.h — floating-point environment
inline constexpr std::string_view kFenvHeader = "fenv.h";

inline constexpr std::array kFenvFunctions = {
    "feclearexcept", "fedisableexcept", "feenableexcept",   "fegetenv",
    "fegetexcept",   "fegetexceptflag", "fegetround",       "feholdexcept",
    "feraiseexcept", "fesetenv",        "fesetexcept",      "fesetexceptflag",
    "fesetround",    "fetestexcept",    "fetestexceptflag", "feupdateenv",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCFENV_H
