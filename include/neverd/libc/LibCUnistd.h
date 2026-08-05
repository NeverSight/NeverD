#ifndef NEVERD_LIBC_LIBCUNISTD_H
#define NEVERD_LIBC_LIBCUNISTD_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <string_view>

namespace neverd::libc {

/// POSIX unistd.h — very common in lifted binaries
inline constexpr std::string_view kUnistdHeader = "unistd.h";

inline constexpr std::array kUnistdFunctions = {
    "_exit",      "access",    "alarm",       "chdir",     "chown",
    "close",      "confstr",   "dup",         "dup2",      "dup3",
    "execl",      "execle",    "execlp",      "execv",     "execve",
    "execvp",     "faccessat", "fchdir",      "fchown",    "fork",
    "fpathconf",  "fsync",     "ftruncate",   "getcwd",    "getegid",
    "getentropy", "geteuid",   "getgid",      "getgroups", "gethostname",
    "getlogin",   "getopt",    "getpagesize", "getpgid",   "getpgrp",
    "getpid",     "getppid",   "getsid",      "gettid",    "getuid",
    "isatty",     "link",      "linkat",      "lseek",     "lseek64",
    "pathconf",   "pipe",      "pipe2",       "pread",     "pwrite",
    "read",       "readlink",  "readlinkat",  "rmdir",     "setgid",
    "setpgid",    "setsid",    "setuid",      "sleep",     "swab",
    "symlink",    "symlinkat", "sysconf",     "truncate",  "unlink",
    "unlinkat",   "usleep",    "write",
};

/// Fixed arity of the non-variadic unistd.h functions (open/fcntl are variadic
/// and are handled by varArgFixedCount instead).  {IntArgs, FpArgs}.
inline constexpr auto kUnistdArity = std::to_array<LibCArityEntry>({
    {"read", {3, 0}},
    {"write", {3, 0}},
    {"close", {1, 0}},
    {"lseek", {3, 0}},
    {"unlink", {1, 0}},
    {"access", {2, 0}},
    {"dup", {1, 0}},
    {"dup2", {2, 0}},
    {"pipe", {1, 0}},
    {"getpid", {0, 0}},
    {"getppid", {0, 0}},
    {"sleep", {1, 0}},
    {"usleep", {1, 0}},
    {"isatty", {1, 0}},
    {"chdir", {1, 0}},
    {"rmdir", {1, 0}},
    {"fsync", {1, 0}},
    {"_exit", {1, 0}},
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCUNISTD_H
