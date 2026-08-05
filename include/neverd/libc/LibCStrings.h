#ifndef NEVERD_LIBC_LIBCSTRINGS_H
#define NEVERD_LIBC_LIBCSTRINGS_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// BSD/POSIX strings.h — legacy byte/string operations
inline constexpr std::string_view kStringsHeader = "strings.h";

inline constexpr std::array kStringsFunctions = {
    "bcmp",       "bcopy",        "bzero",       "ffs",
    "ffsl",       "ffsll",        "index",       "rindex",
    "strcasecmp", "strcasecmp_l", "strncasecmp", "strncasecmp_l",
};

/// Fixed arity of the legacy strings.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kStringsArity = std::to_array<LibCArityEntry>({
    {"strcasecmp", {2, 0}},
    {"strncasecmp", {3, 0}},
    {"bcopy", {3, 0}},
    {"bzero", {2, 0}},
    {"bcmp", {3, 0}},
    {"index", {2, 0}},
    {"rindex", {2, 0}},
    {"ffs", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTRINGS_H
