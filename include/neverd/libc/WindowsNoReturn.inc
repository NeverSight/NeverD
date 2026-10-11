// Microsoft runtime and Windows functions that never return to their caller.
// Included by LibCCallTraits.cpp with LIBC_NO_RETURN_SYMBOL defined; names
// are written without their leading underscores.

// setjmpex.h and the ARM/ARM64 MinGW wrappers resume a saved environment;
// they never execute the instruction following the longjmp call.
LIBC_NO_RETURN_SYMBOL("longjmpex")
LIBC_NO_RETURN_SYMBOL("mingw_longjmp")
LIBC_NO_RETURN_SYMBOL("CxxThrowException")
LIBC_NO_RETURN_SYMBOL("report_gsfailure")
LIBC_NO_RETURN_SYMBOL("raise_securityfailure")
LIBC_NO_RETURN_SYMBOL("TerminateProcess")
// msvcrt's _amsg_exit ends the program with exit code 255 after its runtime
// error message; the Universal CRT and vcruntime declare _invoke_watson,
// _invalid_parameter_noinfo_noreturn and __std_terminate noreturn.  A plain
// `terminate` is left out: a program's own function may take that name.
LIBC_NO_RETURN_SYMBOL("amsg_exit")
LIBC_NO_RETURN_SYMBOL("invoke_watson")
LIBC_NO_RETURN_SYMBOL("invalid_parameter_noinfo_noreturn")
LIBC_NO_RETURN_SYMBOL("std_terminate")
// Windows kernel routines the WDK declares DECLSPEC_NORETURN.  Routines that
// can return (RtlRaiseException when the exception is continued,
// PsTerminateSystemThread on failure) are deliberately absent.
LIBC_NO_RETURN_SYMBOL("KeBugCheck")
LIBC_NO_RETURN_SYMBOL("KeBugCheckEx")
LIBC_NO_RETURN_SYMBOL("ExRaiseStatus")
LIBC_NO_RETURN_SYMBOL("ExRaiseAccessViolation")
LIBC_NO_RETURN_SYMBOL("ExRaiseDatatypeMisalignment")
LIBC_NO_RETURN_SYMBOL("RtlRaiseStatus")
