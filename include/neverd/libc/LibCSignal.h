#ifndef NEVERD_LIBC_LIBCSIGNAL_H
#define NEVERD_LIBC_LIBCSIGNAL_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// ISO C signal.h — signal handling
inline constexpr std::string_view kSignalHeader = "signal.h";

inline constexpr std::array kSignalFunctions = {
    "kill",        "raise",       "sigaction",       "sigaddset",
    "sigaltstack", "sigdelset",   "sigemptyset",     "sigfillset",
    "signal",      "sigprocmask", "pthread_sigmask",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCSIGNAL_H
