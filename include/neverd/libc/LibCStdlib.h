#ifndef NEVERD_LIBC_LIBCSTDLIB_H
#define NEVERD_LIBC_LIBCSTDLIB_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C stdlib.h — memory, process, string conversion, sorting, etc.
inline constexpr std::string_view kStdlibHeader = "stdlib.h";

inline constexpr std::array kStdlibFunctions = {
    "_Exit",
    "a64l",
    "abort",
    "abs",
    "aligned_alloc",
    "at_quick_exit",
    "atexit",
    "atof",
    "atoi",
    "atol",
    "atoll",
    "bsearch",
    "calloc",
    "div",
    "exit",
    "free",
    "getenv",
    "labs",
    "ldiv",
    "llabs",
    "lldiv",
    "malloc",
    "mblen",
    "mbstowcs",
    "mbtowc",
    "posix_memalign",
    "qsort",
    "qsort_r",
    "quick_exit",
    "rand",
    "realloc",
    "srand",
    "strfromd",
    "strfromf",
    "strfroml",
    "strtod",
    "strtof",
    "strtol",
    "strtold",
    "strtoll",
    "strtoul",
    "strtoull",
    // locale-aware variants
    "strtod_l",
    "strtof_l",
    "strtol_l",
    "strtold_l",
    "strtoll_l",
    "strtoul_l",
    "strtoull_l",
    "system",
    "wcstombs",
    "wctomb",
    // malloc.h / additional allocators
    "malloc_usable_size",
    "mallopt",
    "memalign",
    "pvalloc",
    "reallocarray",
    "valloc",
    // common extensions / platform aliases seen in binaries
    "clearenv",
    "itoa",
    "ltoa",
    "mkstemp",
    "mktemp",
    "putenv",
    "realpath",
    "setenv",
    "ultoa",
    "unsetenv",
};

/// Fixed arity of the non-variadic stdlib.h functions.
inline constexpr auto kStdlibArity = std::to_array<LibCArityEntry>({
    {"malloc", {1, 0}},
    {"free", {1, 0}},
    {"calloc", {2, 0}},
    {"realloc", {2, 0}},
    {"reallocf", {2, 0}},
    {"aligned_alloc", {2, 0}},
    {"posix_memalign", {3, 0}},
    {"valloc", {1, 0}},
    {"atoi", {1, 0}},
    {"atol", {1, 0}},
    {"atoll", {1, 0}},
    {"strtol", {3, 0}},
    {"strtoul", {3, 0}},
    {"strtoll", {3, 0}},
    {"strtoull", {3, 0}},
    {"abs", {1, 0}},
    {"labs", {1, 0}},
    // Integer/pointer arguments but a FLOATING-POINT return (FpRet): the result
    // travels in d0/xmm0 (double) or s0/xmm0 (float), not the integer return
    // register.  strtold returns `long double`, modelled as double only where
    // long double == double (Apple AArch64; FpRetLongDouble gates it).
    // {IntArgs, FpArgs, FpIsFloat, FpFirst, FpRet, FpRetLongDouble}.
    {"atof", {1, 0, false, false, true}},
    {"strtod", {2, 0, false, false, true}},
    {"strtof", {2, 0, true, false, true}},
    {"strtold", {2, 0, false, false, false, true}},
    {"drand48", {0, 0, false, false, true}},
    {"erand48", {1, 0, false, false, true}},
    {"llabs", {1, 0}},
    {"exit", {1, 0}},
    {"_Exit", {1, 0}},
    {"abort", {0, 0}},
    {"qsort", {4, 0}},
    {"bsearch", {5, 0}},
    {"rand", {0, 0}},
    {"srand", {1, 0}},
    {"random", {0, 0}},
    {"srandom", {1, 0}},
    {"getenv", {1, 0}},
    {"setenv", {3, 0}},
    {"unsetenv", {1, 0}},
    {"putenv", {1, 0}},
    {"system", {1, 0}},
    {"atexit", {1, 0}},
    {"getprogname", {0, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSTDLIB_H
