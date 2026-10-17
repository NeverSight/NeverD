// C++ runtime functions that never return to their caller.
// Included by LibCCallTraits.cpp with LIBC_NO_RETURN_SYMBOL defined; names
// are written without their leading underscores.

// The Itanium C++ ABI runtime and the unwinder.
LIBC_NO_RETURN_SYMBOL("cxa_throw")
LIBC_NO_RETURN_SYMBOL("cxa_rethrow")
LIBC_NO_RETURN_SYMBOL("cxa_call_terminate")
LIBC_NO_RETURN_SYMBOL("clang_call_terminate")
LIBC_NO_RETURN_SYMBOL("ZSt9terminatev")
LIBC_NO_RETURN_SYMBOL("cxa_bad_cast")
LIBC_NO_RETURN_SYMBOL("cxa_bad_typeid")
LIBC_NO_RETURN_SYMBOL("cxa_pure_virtual")
LIBC_NO_RETURN_SYMBOL("cxa_deleted_virtual")
LIBC_NO_RETURN_SYMBOL("cxa_throw_bad_array_new_length")
LIBC_NO_RETURN_SYMBOL("Unwind_Resume")
// ARM EHABI's end of a cleanup, which resumes the propagating exception.
LIBC_NO_RETURN_SYMBOL("cxa_end_cleanup")
// libstdc++'s [[noreturn]] exception helpers (bits/functexcept.h).
LIBC_NO_RETURN_SYMBOL("ZSt22__throw_bad_exceptionv")
LIBC_NO_RETURN_SYMBOL("ZSt17__throw_bad_allocv")
LIBC_NO_RETURN_SYMBOL("ZSt28__throw_bad_array_new_lengthv")
LIBC_NO_RETURN_SYMBOL("ZSt16__throw_bad_castv")
LIBC_NO_RETURN_SYMBOL("ZSt18__throw_bad_typeidv")
LIBC_NO_RETURN_SYMBOL("ZSt19__throw_logic_errorPKc")
LIBC_NO_RETURN_SYMBOL("ZSt20__throw_domain_errorPKc")
LIBC_NO_RETURN_SYMBOL("ZSt24__throw_invalid_argumentPKc")
LIBC_NO_RETURN_SYMBOL("ZSt20__throw_length_errorPKc")
LIBC_NO_RETURN_SYMBOL("ZSt20__throw_out_of_rangePKc")
LIBC_NO_RETURN_SYMBOL("ZSt24__throw_out_of_range_fmtPKcz")
LIBC_NO_RETURN_SYMBOL("ZSt21__throw_runtime_errorPKc")
LIBC_NO_RETURN_SYMBOL("ZSt19__throw_range_errorPKc")
LIBC_NO_RETURN_SYMBOL("ZSt22__throw_overflow_errorPKc")
LIBC_NO_RETURN_SYMBOL("ZSt23__throw_underflow_errorPKc")
LIBC_NO_RETURN_SYMBOL("ZSt19__throw_ios_failurePKc")
LIBC_NO_RETURN_SYMBOL("ZSt19__throw_ios_failurePKci")
LIBC_NO_RETURN_SYMBOL("ZSt20__throw_system_errori")
LIBC_NO_RETURN_SYMBOL("ZSt20__throw_future_errori")
LIBC_NO_RETURN_SYMBOL("ZSt25__throw_bad_function_callv")
