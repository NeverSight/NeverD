// Irregular variadic functions whose fixed parameters no name suffix gives:
// the ABI category (VarArgFixedParamKind) of each parameter their
// declarations name before the `...`, one per parameter.  The count is an
// ABI fact: Apple's AArch64 convention passes the named parameters in
// registers and every further argument on the stack.
// Included with LIBC_IRREGULAR_VARARG(Name, Kinds...).

LIBC_IRREGULAR_VARARG("sqlite3_mprintf", Pointer)
LIBC_IRREGULAR_VARARG("sqlite3_snprintf", Integer, Pointer, Pointer)
LIBC_IRREGULAR_VARARG("syslog", Integer, Pointer)
LIBC_IRREGULAR_VARARG("err", Integer, Pointer)
LIBC_IRREGULAR_VARARG("errx", Integer, Pointer)
LIBC_IRREGULAR_VARARG("warn", Pointer)
LIBC_IRREGULAR_VARARG("warnx", Pointer)
LIBC_IRREGULAR_VARARG("open", Pointer, Integer)
LIBC_IRREGULAR_VARARG("openat", Integer, Pointer, Integer)
LIBC_IRREGULAR_VARARG("fcntl", Integer, Integer)
LIBC_IRREGULAR_VARARG("ioctl", Integer, Integer)
LIBC_IRREGULAR_VARARG("execl", Pointer, Pointer)
LIBC_IRREGULAR_VARARG("execlp", Pointer, Pointer)
LIBC_IRREGULAR_VARARG("execle", Pointer, Pointer)
LIBC_IRREGULAR_VARARG("mq_open", Pointer, Integer)
LIBC_IRREGULAR_VARARG("sem_open", Pointer, Integer)
LIBC_IRREGULAR_VARARG("prctl", Integer)
LIBC_IRREGULAR_VARARG("syscall", Integer)
