#ifndef NEVERD_LIBC_LIBCTHREADS_H
#define NEVERD_LIBC_LIBCTHREADS_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// C11 threads.h — threading primitives
inline constexpr std::string_view kThreadsHeader = "threads.h";

inline constexpr std::array kThreadsFunctions = {
    "call_once",   "cnd_broadcast", "cnd_destroy", "cnd_init",
    "cnd_signal",  "cnd_timedwait", "cnd_wait",    "mtx_destroy",
    "mtx_init",    "mtx_lock",      "mtx_trylock", "mtx_unlock",
    "thrd_create", "thrd_current",  "thrd_detach", "thrd_equal",
    "thrd_exit",   "thrd_join",     "tss_create",  "tss_delete",
    "tss_get",     "tss_set",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCTHREADS_H
