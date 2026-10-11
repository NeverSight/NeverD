// Functions whose call sites require returns-twice register semantics.
// Included by LibCCallTraits.cpp with LIBC_RETURNS_TWICE_SYMBOL defined.

LIBC_RETURNS_TWICE_SYMBOL("setjmp")
// Windows CRT/compiler entry points preserve the same nonlocal return rule.
LIBC_RETURNS_TWICE_SYMBOL("setjmp3")
LIBC_RETURNS_TWICE_SYMBOL("setjmpex")
LIBC_RETURNS_TWICE_SYMBOL("intrinsic_setjmp")
LIBC_RETURNS_TWICE_SYMBOL("intrinsic_setjmpex")
LIBC_RETURNS_TWICE_SYMBOL("mingw_setjmp")
LIBC_RETURNS_TWICE_SYMBOL("sigsetjmp")
LIBC_RETURNS_TWICE_SYMBOL("vfork")
