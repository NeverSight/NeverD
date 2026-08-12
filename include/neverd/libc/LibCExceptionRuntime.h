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
inline constexpr auto kExceptionRuntimeArity =
    std::to_array<LibCArityEntry>({
        // Itanium C++ ABI and LLVM unwinder.
        {"cxa_allocate_exception", {1, 0}},
        {"cxa_free_exception", {1, 0}},
        {"cxa_throw", {3, 0}},
        {"cxa_rethrow", {0, 0}},
        {"cxa_begin_catch", {1, 0}},
        {"cxa_end_catch", {0, 0}},
        {"cxa_get_exception_ptr", {1, 0}},
        {"cxa_current_exception_type", {0, 0}},
        {"cxa_call_terminate", {1, 0}},
        {"clang_call_terminate", {1, 0}},
        {"ZSt9terminatev", {0, 0}},
        {"Unwind_Resume", {1, 0}},
        {"Unwind_DeleteException", {1, 0}},
        {"Unwind_RaiseException", {1, 0}},
        {"Unwind_ForcedUnwind", {3, 0}},
        {"Unwind_Backtrace", {2, 0}},

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
    });

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCEXCEPTIONRUNTIME_H
