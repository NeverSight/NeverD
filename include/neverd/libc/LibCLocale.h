#ifndef NEVERD_LIBC_LIBCLOCALE_H
#define NEVERD_LIBC_LIBCLOCALE_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C locale.h — localization
inline constexpr std::string_view kLocaleHeader = "locale.h";

inline constexpr std::array kLocaleFunctions = {
    "duplocale", "freelocale", "localeconv",
    "newlocale", "setlocale",  "uselocale",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCLOCALE_H
