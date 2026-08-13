#ifndef NEVERD_LIBC_LIBCWCHAR_H
#define NEVERD_LIBC_LIBCWCHAR_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C wchar.h — wide-character string and I/O
inline constexpr std::string_view kWcharHeader = "wchar.h";

inline constexpr std::array kWcharFunctions = {
    // conversion
    "btowc",
    "mbrlen",
    "mbrtowc",
    "mbsinit",
    "mbsnrtowcs",
    "mbsrtowcs",
    "wcrtomb",
    "wcsnrtombs",
    "wcsrtombs",
    "wctob",
    // string manipulation
    "wcscat",
    "wcschr",
    "wcscmp",
    "wcscoll",
    "wcscpy",
    "wcscspn",
    "wcsdup",
    "wcslcat",
    "wcslcpy",
    "wcslen",
    "wcsncat",
    "wcsncmp",
    "wcsncpy",
    "wcsnlen",
    "wcspbrk",
    "wcsrchr",
    "wcsspn",
    "wcsstr",
    "wcstok",
    "wcsxfrm",
    "wcpcpy",
    "wcpncpy",
    // memory
    "wmemchr",
    "wmemcmp",
    "wmemcpy",
    "wmemmove",
    "wmempcpy",
    "wmemset",
    // numeric conversion
    "wcstod",
    "wcstof",
    "wcstol",
    "wcstold",
    "wcstoll",
    "wcstoul",
    "wcstoull",
    // wide I/O
    "fgetwc",
    "fgetws",
    "fputwc",
    "fputws",
    "fwide",
    "getwc",
    "getwchar",
    "putwc",
    "putwchar",
    "ungetwc",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCWCHAR_H
