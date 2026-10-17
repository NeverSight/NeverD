#ifndef NEVERD_LIBC_LIBCEXCEPTIONRUNTIME_H
#define NEVERD_LIBC_LIBCEXCEPTIONRUNTIME_H

#include "neverd/libc/LibCNames.h"

#include <array>

namespace neverd::libc {

/// Fixed integer/pointer arities for external exception-runtime entry points.
///
/// Names use the canonical form produced by stripLeadingUnderscores: Mach-O's
/// extra symbol prefix and ABI-reserved leading underscores are both absent.
/// These signatures bound call-argument recovery just like libc signatures do;
/// in particular they preserve live-in exception objects passed through tiny
/// compiler-generated catch/terminate helpers.
inline constexpr auto kExceptionRuntimeArity = std::to_array<LibCArityEntry>({
    // Itanium C++ ABI (kItaniumRuntimePrototypes gives the C-linkage entry
    // points).
    {"ZSt9terminatev", {0, 0}},

    // Objective-C table and fragile runtimes.
    {"objc_exception_throw", {1, 0}},
    {"objc_exception_rethrow", {0, 0}},
    {"objc_begin_catch", {1, 0}},
    {"objc_end_catch", {0, 0}},
    {"objc_terminate", {0, 0}},
    {"objc_sync_enter", {1, 0}},
    {"objc_sync_exit", {1, 0}},
    {"objc_exception_try_enter", {1, 0}},
    {"objc_exception_try_exit", {1, 0}},
    {"objc_exception_extract", {1, 0}},
    {"objc_exception_match", {2, 0}},

    // Microsoft C++/SEH throw and unwind entry points.
    {"CxxThrowException", {2, 0}},
    {"RaiseException", {4, 0}},
    {"RtlRaiseException", {1, 0}},
    {"RtlUnwindEx", {6, 0}},

    // MSVC's x64 and ARM compilers lower setjmp to these, passing the frame
    // that longjmp unwinds to after the buffer.
    {"intrinsic_setjmp", {2, 0}},
    {"intrinsic_setjmpex", {2, 0}},

    // MSVC /GS cookie helpers.  Intra-image CRT copies are not imports, so
    // call-argument recovery would otherwise treat leftover rdx/r8/r9 as
    // extra parameters of __security_check_cookie.
    {"security_check_cookie", {1, 0}},
    {"report_gsfailure", {1, 0}},
    {"raise_securityfailure", {1, 0}},
    {"GSHandlerCheckCommon", {3, 0}},

    // kernel32 helpers used by MSVC /GS report paths.  Intra-image
    // wrappers call these as imports; leftover rcx/rdx must not become
    // extra arguments of GetCurrentProcess.
    {"GetCurrentProcess", {0, 0}},
    {"GetCurrentProcessId", {0, 0}},
    {"TerminateProcess", {2, 0}},
    {"SetUnhandledExceptionFilter", {1, 0}},
    {"UnhandledExceptionFilter", {1, 0}},
    {"IsProcessorFeaturePresent", {1, 0}},
});

/// The C declarations of the Itanium C++ ABI's runtime entry points and of the
/// unwinder it runs on, as the C++ ABI and the unwind.h of libgcc and LLVM's
/// libunwind declare them.  A `std::type_info *` or `__class_type_info *` is
/// a `void *` in C, a `struct _Unwind_Exception *` or `struct _Unwind_Context
/// *` too, and a guard object the 64-bit integer the generic ABI makes it.
/// Their arities are derived from them.
inline constexpr auto kItaniumRuntimePrototypes = std::to_array<LibCPrototype>({
    makeLibCPrototype("__cxa_allocate_exception", "void *", {"size_t"},
                      "stddef.h"),
    makeLibCPrototype("__cxa_free_exception", "void", {"void *"}),
    makeLibCPrototype("__cxa_throw", "void",
                      {"void *", "void *", "void (*)(void *)"}),
    makeLibCPrototype("__cxa_rethrow", "void", {}),
    makeLibCPrototype("__cxa_begin_catch", "void *", {"void *"}),
    makeLibCPrototype("__cxa_end_catch", "void", {}),
    makeLibCPrototype("__cxa_end_cleanup", "void", {}),
    makeLibCPrototype("__cxa_get_exception_ptr", "void *", {"void *"}),
    makeLibCPrototype("__cxa_current_exception_type", "void *", {}),
    makeLibCPrototype("__cxa_call_terminate", "void", {"void *"}),
    makeLibCPrototype("__cxa_call_unexpected", "void", {"void *"}),
    makeLibCPrototype("__clang_call_terminate", "void", {"void *"}),
    makeLibCPrototype("__cxa_guard_acquire", "int", {"int64_t *"}),
    makeLibCPrototype("__cxa_guard_release", "void", {"int64_t *"}),
    makeLibCPrototype("__cxa_guard_abort", "void", {"int64_t *"}),
    makeLibCPrototype("__cxa_pure_virtual", "void", {}),
    makeLibCPrototype("__cxa_deleted_virtual", "void", {}),
    makeLibCPrototype("__cxa_bad_cast", "void", {}),
    makeLibCPrototype("__cxa_bad_typeid", "void", {}),
    makeLibCPrototype("__cxa_throw_bad_array_new_length", "void", {}),
    makeLibCPrototype("__cxa_get_globals", "void *", {}),
    makeLibCPrototype("__cxa_get_globals_fast", "void *", {}),
    makeLibCPrototype(
        "__dynamic_cast", "void *",
        {"const void *", "const void *", "const void *", "ptrdiff_t"},
        "stddef.h"),
    makeLibCPrototype("__cxa_demangle", "char *",
                      {"const char *", "char *", "size_t *", "int *"},
                      "stddef.h"),
    makeLibCPrototype("_Unwind_Resume", "void", {"void *"}),
    makeLibCPrototype("_Unwind_Resume_or_Rethrow", "int", {"void *"}),
    makeLibCPrototype("_Unwind_RaiseException", "int", {"void *"}),
    makeLibCPrototype("_Unwind_DeleteException", "void", {"void *"}),
    makeLibCPrototype("_Unwind_ForcedUnwind", "int",
                      {"void *",
                       "int (*)(int, int, uint64_t, void *, void *, void *)",
                       "void *"}),
    makeLibCPrototype("_Unwind_Backtrace", "int",
                      {"int (*)(void *, void *)", "void *"}),
    makeLibCPrototype("_Unwind_GetIP", "uintptr_t", {"void *"}),
    makeLibCPrototype("_Unwind_GetIPInfo", "uintptr_t", {"void *", "int *"}),
    makeLibCPrototype("_Unwind_GetCFA", "uintptr_t", {"void *"}),
    makeLibCPrototype("_Unwind_GetGR", "uintptr_t", {"void *", "int"}),
    makeLibCPrototype("_Unwind_SetGR", "void", {"void *", "int", "uintptr_t"}),
    makeLibCPrototype("_Unwind_SetIP", "void", {"void *", "uintptr_t"}),
    makeLibCPrototype("_Unwind_GetLanguageSpecificData", "void *", {"void *"}),
    makeLibCPrototype("_Unwind_GetRegionStart", "uintptr_t", {"void *"}),
    makeLibCPrototype("_Unwind_FindEnclosingFunction", "void *", {"void *"}),
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCEXCEPTIONRUNTIME_H
