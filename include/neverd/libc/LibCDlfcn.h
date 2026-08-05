#ifndef NEVERD_LIBC_LIBCDLFCN_H
#define NEVERD_LIBC_LIBCDLFCN_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX dlfcn.h — dynamic linking
inline constexpr std::string_view kDlfcnHeader = "dlfcn.h";

inline constexpr std::array kDlfcnFunctions = {
    "dladdr", "dlclose", "dlerror", "dlinfo", "dlopen", "dlsym",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCDLFCN_H
