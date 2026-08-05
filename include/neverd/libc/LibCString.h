#ifndef NEVERD_LIBC_LIBCSTRING_H
#define NEVERD_LIBC_LIBCSTRING_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C string.h — memory and string operations
inline constexpr std::string_view kStringHeader = "string.h";

inline constexpr std::array kStringFunctions = {
    "memccpy",   "memchr",    "memcmp",     "memcpy",     "memmem",
    "memmove",   "mempcpy",   "memrchr",    "memset",     "memset_explicit",
    "stpcpy",    "stpncpy",   "strcasestr", "strcat",     "strchr",
    "strchrnul", "strcmp",    "strcoll",    "strcoll_l",  "strcpy",
    "strcspn",   "strdup",    "strerror",   "strerror_r", "strlcat",
    "strlcpy",   "strlen",    "strncat",    "strncmp",    "strncpy",
    "strndup",   "strnlen",   "strpbrk",    "strrchr",    "strsep",
    "strsignal", "strspn",    "strstr",     "strtok",     "strtok_r",
    "strxfrm",   "strxfrm_l",
};

/// Fixed arity of the string.h functions.  {IntArgs, FpArgs}.
inline constexpr auto kStringArity = std::to_array<LibCArityEntry>({
    {"strlen", {1, 0}},     {"strnlen", {2, 0}}, {"strcmp", {2, 0}},
    {"strncmp", {3, 0}},    {"strcpy", {2, 0}},  {"strncpy", {3, 0}},
    {"strcat", {2, 0}},     {"strncat", {3, 0}}, {"strchr", {2, 0}},
    {"strrchr", {2, 0}},    {"strstr", {2, 0}},  {"strdup", {1, 0}},
    {"strndup", {2, 0}},    {"strtok", {2, 0}},  {"strtok_r", {3, 0}},
    {"strspn", {2, 0}},     {"strcspn", {2, 0}}, {"strpbrk", {2, 0}},
    {"strcoll", {2, 0}},    {"strxfrm", {3, 0}}, {"strerror", {1, 0}},
    {"strerror_r", {3, 0}}, {"strsep", {2, 0}},  {"strlcpy", {3, 0}},
    {"strlcat", {3, 0}},    {"memcpy", {3, 0}},  {"memmove", {3, 0}},
    {"memset", {3, 0}},     {"memcmp", {3, 0}},  {"memchr", {3, 0}},
    {"memrchr", {3, 0}},    {"memccpy", {4, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTRING_H
