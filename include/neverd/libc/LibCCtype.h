#ifndef NEVERD_LIBC_LIBCCTYPE_H
#define NEVERD_LIBC_LIBCCTYPE_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C ctype.h — character classification and conversion
inline constexpr std::string_view kCtypeHeader = "ctype.h";

inline constexpr std::array kCtypeFunctions = {
    "isalnum",
    "isalpha",
    "isascii",
    "isblank",
    "iscntrl",
    "isdigit",
    "isgraph",
    "islower",
    "isprint",
    "ispunct",
    "isspace",
    "isupper",
    "isxdigit",
    "toascii",
    "tolower",
    "toupper",
    // POSIX locale variants
    "isalnum_l",
    "isalpha_l",
    "isblank_l",
    "iscntrl_l",
    "isdigit_l",
    "isgraph_l",
    "islower_l",
    "isprint_l",
    "ispunct_l",
    "isspace_l",
    "isupper_l",
    "isxdigit_l",
    "tolower_l",
    "toupper_l",
};

/// Fixed arity of the ctype.h classification/conversion functions (int -> int).
inline constexpr auto kCtypeArity = std::to_array<LibCArityEntry>({
    {"isalpha", {1, 0}},
    {"isdigit", {1, 0}},
    {"isalnum", {1, 0}},
    {"isspace", {1, 0}},
    {"isupper", {1, 0}},
    {"islower", {1, 0}},
    {"isprint", {1, 0}},
    {"ispunct", {1, 0}},
    {"iscntrl", {1, 0}},
    {"isxdigit", {1, 0}},
    {"isgraph", {1, 0}},
    {"isblank", {1, 0}},
    {"toupper", {1, 0}},
    {"tolower", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCCTYPE_H
