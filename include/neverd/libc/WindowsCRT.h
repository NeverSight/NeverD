#ifndef NEVERD_LIBC_WINDOWSCRT_H
#define NEVERD_LIBC_WINDOWSCRT_H

#include "neverd/libc/LibCNames.h"

#include <array>
#include <initializer_list>
#include <string_view>

namespace neverd::libc {

/// The prefix of the Universal CRT's stdio implementation routines, which the
/// inline printf and scanf of its headers call: `__stdio_common_vfprintf`
/// implements vfprintf and, like it, takes a va_list rather than `...`.
inline constexpr std::string_view kUCRTStdioPrefix = "stdio_common_";

/// The prototype of a routine a Windows C runtime DLL exports.
constexpr LibCPrototype
windowsCRTPrototype(std::string_view Name, std::string_view Return,
                    std::initializer_list<std::string_view> Params,
                    std::string_view Header = {}) {
  LibCPrototype Prototype = makeLibCPrototype(Name, Return, Params, Header);
  Prototype.Format = BinaryFormat::COFF;
  return Prototype;
}

/// The prototype of a Windows API routine, which is stdcall on 32-bit x86.
constexpr LibCPrototype
windowsAPIPrototype(std::string_view Name, std::string_view Return,
                    std::initializer_list<std::string_view> Params) {
  LibCPrototype Prototype = windowsCRTPrototype(Name, Return, Params);
  Prototype.Winapi = true;
  return Prototype;
}

/// The C declarations of the Windows C runtime routines a program's start-up
/// code and compiler-generated code call: the startup and exit, argument,
/// environment and stdio-stream routines of msvcrt.dll (which MinGW links)
/// and of the Universal CRT (api-ms-win-crt-*.dll), the exception helpers of
/// vcruntime, and the kernel32 routines the exception start-up and /GS
/// failure paths call.  They are those of corecrt_startup.h, corecrt.h,
/// stdio.h, vcruntime.h and the Windows SDK.
///
/// A `_PVFV`, `_PIFV` or `_onexit_t` is the function pointer it is; a
/// `_startupinfo *`, `_onexit_table_t *`, `struct _exception *`,
/// `struct _EXCEPTION_POINTERS *` or a pointer to another structure the
/// routine fills or reads (`STARTUPINFOW`, `FILETIME`, `SLIST_HEADER`,
/// `CONTEXT`, `RUNTIME_FUNCTION`) a `void *`; a `va_list` the `char *` it is
/// on Windows; an `unsigned long` or `DWORD` a `uint32_t`, a `DWORD64` a
/// `uint64_t`, a `long` an `int32_t`, a `LARGE_INTEGER` an `int64_t`, a `BOOL`
/// an `int`, a `HANDLE` or `HMODULE` a `void *` and a `WCHAR` the
/// `uint16_t` it is on Windows.  They apply to a PE image alone, and no arity
/// is derived from them: `_lock` or `terminate` is a different routine of
/// another runtime.
inline constexpr auto kWindowsCRTPrototypes = std::to_array<LibCPrototype>({
    // msvcrt.dll start-up and exit.
    windowsCRTPrototype("__getmainargs", "int",
                        {"int *", "char ***", "char ***", "int", "void *"}),
    windowsCRTPrototype(
        "__wgetmainargs", "int",
        {"int *", "wchar_t ***", "wchar_t ***", "int", "void *"}, "stddef.h"),
    windowsCRTPrototype("__set_app_type", "void", {"int"}),
    windowsCRTPrototype("__setusermatherr", "void", {"int (*)(void *)"}),
    windowsCRTPrototype("_initterm", "void",
                        {"void (**)(void)", "void (**)(void)"}),
    windowsCRTPrototype("_initterm_e", "int",
                        {"int (**)(void)", "int (**)(void)"}),
    windowsCRTPrototype("_amsg_exit", "void", {"int"}),
    windowsCRTPrototype("_cexit", "void", {}),
    windowsCRTPrototype("_c_exit", "void", {}),
    windowsCRTPrototype("_lock", "void", {"int"}),
    windowsCRTPrototype("_unlock", "void", {"int"}),
    windowsCRTPrototype("___lc_codepage_func", "unsigned int", {}),
    windowsCRTPrototype("___mb_cur_max_func", "int", {}),
    windowsCRTPrototype("__iob_func", "FILE *", {}, "stdio.h"),
    windowsCRTPrototype("_errno", "int *", {}),
    windowsCRTPrototype("__p___argc", "int *", {}),
    windowsCRTPrototype("__p___argv", "char ***", {}),
    windowsCRTPrototype("__p___wargv", "wchar_t ***", {}, "stddef.h"),
    windowsCRTPrototype("__p__environ", "char ***", {}),
    windowsCRTPrototype("__p__wenviron", "wchar_t ***", {}, "stddef.h"),
    windowsCRTPrototype("__p___initenv", "char ***", {}),
    windowsCRTPrototype("__p__fmode", "int *", {}),
    windowsCRTPrototype("__p__commode", "int *", {}),
    // msvcrt.dll 64-bit conversions, which MinGW's strtoll, strtoull, wcstoll
    // and wcstoull forward to.
    windowsCRTPrototype("_strtoi64", "int64_t",
                        {"const char *", "char **", "int"}),
    windowsCRTPrototype("_strtoui64", "uint64_t",
                        {"const char *", "char **", "int"}),
    windowsCRTPrototype("_wcstoi64", "int64_t",
                        {"const wchar_t *", "wchar_t **", "int"}, "stddef.h"),
    windowsCRTPrototype("_wcstoui64", "uint64_t",
                        {"const wchar_t *", "wchar_t **", "int"}, "stddef.h"),
    // Universal CRT start-up and exit.
    windowsCRTPrototype("_set_app_type", "void", {"int"}),
    windowsCRTPrototype("_configure_narrow_argv", "int", {"int"}),
    windowsCRTPrototype("_configure_wide_argv", "int", {"int"}),
    windowsCRTPrototype("_initialize_narrow_environment", "int", {}),
    windowsCRTPrototype("_initialize_wide_environment", "int", {}),
    windowsCRTPrototype("_get_initial_narrow_environment", "char **", {}),
    windowsCRTPrototype("_get_initial_wide_environment", "wchar_t **", {},
                        "stddef.h"),
    windowsCRTPrototype("_initialize_onexit_table", "int", {"void *"}),
    windowsCRTPrototype("_register_onexit_function", "int",
                        {"void *", "int (*)(void)"}),
    windowsCRTPrototype("_execute_onexit_table", "int", {"void *"}),
    windowsCRTPrototype("_crt_atexit", "int", {"void (*)(void)"}),
    windowsCRTPrototype("_crt_at_quick_exit", "int", {"void (*)(void)"}),
    windowsCRTPrototype("_register_thread_local_exe_atexit_callback", "void",
                        {"void (WINAPI *)(void *, uint32_t, void *)"}),
    windowsCRTPrototype("_seh_filter_exe", "int", {"uint32_t", "void *"}),
    windowsCRTPrototype("_seh_filter_dll", "int", {"uint32_t", "void *"}),
    windowsCRTPrototype("_set_fmode", "int", {"int"}),
    windowsCRTPrototype("_set_new_mode", "int", {"int"}),
    windowsCRTPrototype("_configthreadlocale", "int", {"int"}),
    windowsCRTPrototype("_controlfp_s", "int",
                        {"unsigned int *", "unsigned int", "unsigned int"}),
    windowsCRTPrototype("terminate", "void", {}),
    // Universal CRT stdio, which the inline printf and scanf call.
    windowsCRTPrototype("__acrt_iob_func", "FILE *", {"unsigned int"},
                        "stdio.h"),
    windowsCRTPrototype(
        "__stdio_common_vfprintf", "int",
        {"uint64_t", "FILE *", "const char *", "void *", "char *"}, "stdio.h"),
    windowsCRTPrototype(
        "__stdio_common_vsprintf", "int",
        {"uint64_t", "char *", "size_t", "const char *", "void *", "char *"},
        "stddef.h"),
    windowsCRTPrototype(
        "__stdio_common_vsprintf_s", "int",
        {"uint64_t", "char *", "size_t", "const char *", "void *", "char *"},
        "stddef.h"),
    windowsCRTPrototype("__stdio_common_vsnprintf_s", "int",
                        {"uint64_t", "char *", "size_t", "size_t",
                         "const char *", "void *", "char *"},
                        "stddef.h"),
    windowsCRTPrototype(
        "__stdio_common_vfscanf", "int",
        {"uint64_t", "FILE *", "const char *", "void *", "char *"}, "stdio.h"),
    windowsCRTPrototype("__stdio_common_vsscanf", "int",
                        {"uint64_t", "const char *", "size_t", "const char *",
                         "void *", "char *"},
                        "stddef.h"),
    // vcruntime exception helpers.
    windowsCRTPrototype("__current_exception", "void **", {}),
    windowsCRTPrototype("__current_exception_context", "void **", {}),
    windowsCRTPrototype("__std_type_info_destroy_list", "void", {"void *"}),
    windowsCRTPrototype("__std_terminate", "void", {}),
    windowsCRTPrototype("__std_exception_copy", "void",
                        {"const void *", "void *"}),
    windowsCRTPrototype("__std_exception_destroy", "void", {"void *"}),
    windowsCRTPrototype("_purecall", "int", {}),
    // kernel32 and ntdll.
    // synchapi.h: these calls have no floating-point return value.
    windowsAPIPrototype("Sleep", "void", {"uint32_t"}),
    windowsAPIPrototype("InitializeCriticalSection", "void", {"void *"}),
    windowsAPIPrototype("EnterCriticalSection", "void", {"void *"}),
    windowsAPIPrototype("LeaveCriticalSection", "void", {"void *"}),
    windowsAPIPrototype("GetCurrentProcess", "void *", {}),
    windowsAPIPrototype("GetCurrentProcessId", "uint32_t", {}),
    windowsAPIPrototype("GetCurrentThreadId", "uint32_t", {}),
    windowsAPIPrototype("GetModuleHandleA", "void *", {"const char *"}),
    windowsAPIPrototype("GetModuleHandleW", "void *", {"const uint16_t *"}),
    windowsAPIPrototype("GetStartupInfoW", "void", {"void *"}),
    windowsAPIPrototype("GetSystemTimeAsFileTime", "void", {"void *"}),
    windowsAPIPrototype("InitializeSListHead", "void", {"void *"}),
    windowsAPIPrototype("IsDebuggerPresent", "int", {}),
    windowsAPIPrototype("QueryPerformanceCounter", "int", {"int64_t *"}),
    windowsAPIPrototype("RtlCaptureContext", "void", {"void *"}),
    windowsAPIPrototype("RtlLookupFunctionEntry", "void *",
                        {"uint64_t", "uint64_t *", "void *"}),
    windowsAPIPrototype("RtlVirtualUnwind", "void *",
                        {"uint32_t", "uint64_t", "uint64_t", "void *", "void *",
                         "void **", "uint64_t *", "void *"}),
    windowsAPIPrototype("TerminateProcess", "int", {"void *", "unsigned int"}),
    windowsAPIPrototype("IsProcessorFeaturePresent", "int", {"uint32_t"}),
    windowsAPIPrototype("SetUnhandledExceptionFilter", "void *",
                        {"int32_t (WINAPI *)(void *)"}),
    windowsAPIPrototype("UnhandledExceptionFilter", "int32_t", {"void *"}),
    windowsAPIPrototype(
        "RaiseException", "void",
        {"uint32_t", "uint32_t", "uint32_t", "const uintptr_t *"}),
    windowsAPIPrototype("RtlRaiseException", "void", {"void *"}),
    windowsAPIPrototype(
        "RtlUnwindEx", "void",
        {"void *", "void *", "void *", "void *", "void *", "void *"}),
});

} // namespace neverd::libc

#endif // NEVERD_LIBC_WINDOWSCRT_H
