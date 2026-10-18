// C and POSIX functions that never return to their caller.
// Included by LibCCallTraits.cpp with LIBC_NO_RETURN_SYMBOL defined, next to
// the C++ runtime (CxxRuntimeNoReturn.inc) and Windows (WindowsNoReturn.inc)
// tables.

LIBC_NO_RETURN_SYMBOL("abort")
LIBC_NO_RETURN_SYMBOL("exit")
// glibc and musl call exit() with main's result; `_start` follows the call
// with a `hlt` that only marks unreachable code.
LIBC_NO_RETURN_SYMBOL("libc_start_main")
LIBC_NO_RETURN_SYMBOL("Exit")
LIBC_NO_RETURN_SYMBOL("quick_exit")
LIBC_NO_RETURN_SYMBOL("longjmp")
LIBC_NO_RETURN_SYMBOL("siglongjmp")
LIBC_NO_RETURN_SYMBOL("pthread_exit")
LIBC_NO_RETURN_SYMBOL("thrd_exit")
LIBC_NO_RETURN_SYMBOL("err")
LIBC_NO_RETURN_SYMBOL("errx")
LIBC_NO_RETURN_SYMBOL("verr")
LIBC_NO_RETURN_SYMBOL("verrx")
LIBC_NO_RETURN_SYMBOL("assert_fail")
LIBC_NO_RETURN_SYMBOL("assert_rtn")
LIBC_NO_RETURN_SYMBOL("assert_perror_fail")
// glibc's __assert and mingw-w64's _assert, which their headers declare
// noreturn.
LIBC_NO_RETURN_SYMBOL("assert")
LIBC_NO_RETURN_SYMBOL("stack_chk_fail")
