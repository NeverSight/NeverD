// Windows runtime routines that never return when a program imports them from
// the named module, although a program may define a returning routine of the
// same name. Included by LibCCallTraits.cpp with LIBC_NO_RETURN_IMPORT
// defined; module names compare without regard to case.

// corecrt_terminate.h declares the Universal CRT's terminate
// __declspec(noreturn); a program's own `terminate` is another routine.
LIBC_NO_RETURN_IMPORT("api-ms-win-crt-runtime-l1-1-0.dll", "terminate")
LIBC_NO_RETURN_IMPORT("ucrtbase.dll", "terminate")
LIBC_NO_RETURN_IMPORT("ucrtbased.dll", "terminate")
