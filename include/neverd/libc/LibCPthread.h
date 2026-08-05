#ifndef NEVERD_LIBC_LIBCPTHREAD_H
#define NEVERD_LIBC_LIBCPTHREAD_H

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX pthread.h — threads, mutexes, conditions, rwlocks, barriers, TLS
inline constexpr std::string_view kPthreadHeader = "pthread.h";

inline constexpr std::array kPthreadFunctions = {
    // lifecycle
    "pthread_create",
    "pthread_detach",
    "pthread_equal",
    "pthread_exit",
    "pthread_join",
    "pthread_self",
    "pthread_atfork",
    // attributes
    "pthread_attr_destroy",
    "pthread_attr_getdetachstate",
    "pthread_attr_getguardsize",
    "pthread_attr_getschedparam",
    "pthread_attr_getstack",
    "pthread_attr_getstacksize",
    "pthread_attr_init",
    "pthread_attr_setdetachstate",
    "pthread_attr_setguardsize",
    "pthread_attr_setschedparam",
    "pthread_attr_setstack",
    "pthread_attr_setstacksize",
    // mutex
    "pthread_mutex_destroy",
    "pthread_mutex_init",
    "pthread_mutex_lock",
    "pthread_mutex_trylock",
    "pthread_mutex_unlock",
    "pthread_mutexattr_destroy",
    "pthread_mutexattr_getpshared",
    "pthread_mutexattr_getrobust",
    "pthread_mutexattr_gettype",
    "pthread_mutexattr_init",
    "pthread_mutexattr_setpshared",
    "pthread_mutexattr_setrobust",
    "pthread_mutexattr_settype",
    // condition variable
    "pthread_cond_broadcast",
    "pthread_cond_destroy",
    "pthread_cond_init",
    "pthread_cond_signal",
    "pthread_cond_timedwait",
    "pthread_cond_wait",
    "pthread_condattr_destroy",
    "pthread_condattr_getclock",
    "pthread_condattr_getpshared",
    "pthread_condattr_init",
    "pthread_condattr_setclock",
    "pthread_condattr_setpshared",
    // read-write lock
    "pthread_rwlock_destroy",
    "pthread_rwlock_init",
    "pthread_rwlock_rdlock",
    "pthread_rwlock_timedrdlock",
    "pthread_rwlock_timedwrlock",
    "pthread_rwlock_tryrdlock",
    "pthread_rwlock_trywrlock",
    "pthread_rwlock_unlock",
    "pthread_rwlock_wrlock",
    "pthread_rwlockattr_destroy",
    "pthread_rwlockattr_getpshared",
    "pthread_rwlockattr_init",
    "pthread_rwlockattr_setpshared",
    // barrier
    "pthread_barrier_destroy",
    "pthread_barrier_init",
    "pthread_barrier_wait",
    // once
    "pthread_once",
    // thread-specific data
    "pthread_getspecific",
    "pthread_key_create",
    "pthread_key_delete",
    "pthread_setspecific",
    // spinlock
    "pthread_spin_destroy",
    "pthread_spin_init",
    "pthread_spin_lock",
    "pthread_spin_trylock",
    "pthread_spin_unlock",
    // GNU extensions commonly seen in binaries
    "pthread_getattr_np",
    "pthread_getname_np",
    "pthread_setname_np",
};

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCPTHREAD_H
