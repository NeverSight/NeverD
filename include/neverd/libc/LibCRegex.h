#ifndef NEVERD_LIBC_LIBCREGEX_H
#define NEVERD_LIBC_LIBCREGEX_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX regex.h — regular expressions
inline constexpr std::string_view kRegexHeader = "regex.h";

inline constexpr std::array kRegexFunctions = {
    "regcomp",
    "regerror",
    "regexec",
    "regfree",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCREGEX_H
