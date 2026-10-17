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
// TerminateProcess is absent: it returns when it ends another process, or
// fails to, and only the current process's pseudo-handle makes it end the
// caller, as raise_securityfailure calls it.
// msvcrt's _amsg_exit ends the program with exit code 255 after its runtime
// error message; the Universal CRT and vcruntime declare _invoke_watson,
// _invalid_parameter_noinfo_noreturn and __std_terminate noreturn.  A plain
// `terminate` is left out: a program's own function may take that name.
LIBC_NO_RETURN_SYMBOL("amsg_exit")
LIBC_NO_RETURN_SYMBOL("invoke_watson")
LIBC_NO_RETURN_SYMBOL("invalid_parameter_noinfo_noreturn")
LIBC_NO_RETURN_SYMBOL("std_terminate")
// Windows API routines the SDK declares DECLSPEC_NORETURN, as the user mode
// header libraries do: function discovery reads no header.
// RtlRestoreContext resumes the context it is given.
LIBC_NO_RETURN_SYMBOL("ExitProcess")
LIBC_NO_RETURN_SYMBOL("ExitThread")
LIBC_NO_RETURN_SYMBOL("FreeLibraryAndExitThread")
LIBC_NO_RETURN_SYMBOL("RpcRaiseException")
LIBC_NO_RETURN_SYMBOL("RtlRestoreContext")
// The native API routines phnt declares DECLSPEC_NORETURN, as the user mode
// libraries do: those that end the process or the thread, and an exception
// no handler may continue.  NtRaiseHardError, LdrShutdownProcess,
// LdrShutdownThread and RtlAssert, which phnt declares so as well, return
// (RETURNING in scripts/generate_windows_types.py).
LIBC_NO_RETURN_SYMBOL("RtlExitUserProcess")
LIBC_NO_RETURN_SYMBOL("RtlExitUserThread")
LIBC_NO_RETURN_SYMBOL("RtlRaiseNoncontinuableException")
// RtlFailFast and RtlFatalListEntryError, which phnt defines inline over
// __fastfail, end the process as a copy a build leaves uninlined does.
LIBC_NO_RETURN_SYMBOL("RtlFailFast")
LIBC_NO_RETURN_SYMBOL("RtlFatalListEntryError")
// Windows kernel routines the WDK declares DECLSPEC_NORETURN.  Routines that
// can return (RtlRaiseException when the exception is continued,
// PsTerminateSystemThread on failure) are deliberately absent.
LIBC_NO_RETURN_SYMBOL("KeBugCheck")
LIBC_NO_RETURN_SYMBOL("KeBugCheckEx")
LIBC_NO_RETURN_SYMBOL("ExRaiseStatus")
LIBC_NO_RETURN_SYMBOL("ExRaiseAccessViolation")
LIBC_NO_RETURN_SYMBOL("ExRaiseDatatypeMisalignment")
LIBC_NO_RETURN_SYMBOL("RtlRaiseStatus")
