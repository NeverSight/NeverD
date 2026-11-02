// C and POSIX functions that never return to their caller.
// Included by LibCCallTraits.cpp with LIBC_NO_RETURN_SYMBOL defined, next to
// the C++ runtime (CxxRuntimeNoReturn.inc) and Windows (WindowsNoReturn.inc)
// tables.

LIBC_NO_RETURN_SYMBOL("abort")
// FreeBSD's abort with a reason, and C11 Annex K's constraint handler.
LIBC_NO_RETURN_SYMBOL("abort2")
LIBC_NO_RETURN_SYMBOL("abort_handler_s")
LIBC_NO_RETURN_SYMBOL("exit")
// glibc and musl call exit() with main's result; `_start` follows the call
// with a `hlt` that only marks unreachable code.
LIBC_NO_RETURN_SYMBOL("libc_start_main")
// Android bionic's startup entry also exits with the application's result.
LIBC_NO_RETURN_SYMBOL("libc_init")
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
// The BSDs' <err.h>, Darwin's among them, also exits through errc and
// verrc, which take the error code to report.
LIBC_NO_RETURN_SYMBOL("errc")
LIBC_NO_RETURN_SYMBOL("verrc")
LIBC_NO_RETURN_SYMBOL("assert_fail")
LIBC_NO_RETURN_SYMBOL("assert_rtn")
LIBC_NO_RETURN_SYMBOL("assert_perror_fail")
// glibc's __assert and mingw-w64's _assert, which their headers declare
// noreturn, and bionic's __assert2, which its assert calls.
LIBC_NO_RETURN_SYMBOL("assert")
LIBC_NO_RETURN_SYMBOL("assert2")
// The NDK's <android/log.h>: __android_log_assert logs and aborts, as the
// default aborter it calls does.
LIBC_NO_RETURN_SYMBOL("android_log_assert")
LIBC_NO_RETURN_SYMBOL("android_log_default_aborter")
LIBC_NO_RETURN_SYMBOL("stack_chk_fail")
