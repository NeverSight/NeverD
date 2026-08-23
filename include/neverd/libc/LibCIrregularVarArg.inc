// Irregular variadic functions whose fixed count is not suffix-derivable.
// Included with LIBC_IRREGULAR_VARARG(Name, FixedCount).

LIBC_IRREGULAR_VARARG("syslog", 1)
LIBC_IRREGULAR_VARARG("err", 1)
LIBC_IRREGULAR_VARARG("errx", 1)
LIBC_IRREGULAR_VARARG("warn", 1)
LIBC_IRREGULAR_VARARG("warnx", 1)
LIBC_IRREGULAR_VARARG("open", 2)
LIBC_IRREGULAR_VARARG("openat", 1)
LIBC_IRREGULAR_VARARG("fcntl", 1)
LIBC_IRREGULAR_VARARG("ioctl", 1)
LIBC_IRREGULAR_VARARG("execl", 1)
LIBC_IRREGULAR_VARARG("execlp", 1)
LIBC_IRREGULAR_VARARG("execle", 1)
LIBC_IRREGULAR_VARARG("mq_open", 1)
LIBC_IRREGULAR_VARARG("sem_open", 1)
