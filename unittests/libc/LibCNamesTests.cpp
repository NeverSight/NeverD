//===- LibCNamesTests.cpp - Unit tests for LibCNames ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/libc/LibCComplex.h"
#include "neverd/libc/LibCExceptionRuntime.h"
#include "neverd/libc/LibCFortify.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/LibCResult.h"
#include "neverd/libc/LibCStartup.h"
#include "neverd/libc/MinGWHeaderFunctions.h"
#include "neverd/libc/WindowsCRT.h"
#include "neverd/loader/BinaryImage.h"

#include <gtest/gtest.h>
#include <utility>

using namespace neverd;
using namespace neverd::libc;

// =====================================================================
// varArgFixedCount — pattern matching for *printf / *scanf
// =====================================================================

TEST(VarArgFixedCount, PrintfFamily1Fixed) {
  EXPECT_EQ(varArgFixedCount("printf", LibCVariant::GNU), 1u);
  EXPECT_EQ(varArgFixedCount("scanf", LibCVariant::GNU), 1u);
  EXPECT_EQ(varArgFixedCount("wprintf", LibCVariant::GNU), 1u);
  EXPECT_EQ(varArgFixedCount("wscanf", LibCVariant::GNU), 1u);
}

TEST(VarArgFixedCount, PrintfFamily2Fixed) {
  EXPECT_EQ(varArgFixedCount("fprintf", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("fscanf", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("sprintf", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("sscanf", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("dprintf", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("asprintf", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("fwprintf", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("fwscanf", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("swscanf", LibCVariant::GNU), 2u);
}

TEST(VarArgFixedCount, PrintfFamily3Fixed) {
  EXPECT_EQ(varArgFixedCount("snprintf", LibCVariant::GNU), 3u);
  EXPECT_EQ(varArgFixedCount("snwprintf", LibCVariant::GNU), 3u);
  // C99 swprintf(buf, size, fmt, ...) has same shape as snprintf
  EXPECT_EQ(varArgFixedCount("swprintf", LibCVariant::GNU), 3u);
}

TEST(VarArgFixedCount, UCRTCommonConsumersKeepTheirVaListArgument) {
  for (const auto *Routine :
       {"__stdio_common_vfprintf", "__stdio_common_vfscanf",
        "__stdio_common_vswscanf", "__stdio_common_vfprintf_p",
        "__stdio_common_vsnprintf_s", "__conio_common_vcprintf",
        "__conio_common_vcwprintf", "__conio_common_vcprintf_s",
        "__conio_common_vcprintf_p", "__conio_common_vcscanf_s"}) {
    for (const auto *Prefix : {"", "_o_"}) {
      const std::string Name = std::string(Prefix) + Routine;
      EXPECT_EQ(varArgFixedCount(Name, LibCVariant::Windows), 0u) << Name;
      EXPECT_TRUE(isVaListConsumer(Name)) << Name;
    }
  }
  EXPECT_EQ(varArgFixedCount("printf", LibCVariant::Windows), 1u);
}

TEST(VarArgFixedCount, MicrosoftPrefixesKeepCapacityAndCountOnlyContracts) {
  for (const auto Name : {"_snprintf", "_snwprintf", "_snscanf", "_snwscanf"}) {
    EXPECT_EQ(varArgFixedCount(Name, LibCVariant::Windows), 3u) << Name;
    EXPECT_EQ(varArgFixedParamKind(Name, 0, LibCVariant::Windows),
              VarArgFixedParamKind::Pointer);
    EXPECT_EQ(varArgFixedParamKind(Name, 1, LibCVariant::Windows),
              VarArgFixedParamKind::Integer);
    EXPECT_EQ(varArgFixedParamKind(Name, 2, LibCVariant::Windows),
              VarArgFixedParamKind::Pointer);
    EXPECT_EQ(varArgFixedParamKind(Name, 3, LibCVariant::Windows),
              VarArgFixedParamKind::Unknown);
  }
  for (const auto Name : {"_scprintf", "_scwprintf"}) {
    EXPECT_EQ(varArgFixedCount(Name, LibCVariant::Windows), 1u) << Name;
    EXPECT_EQ(varArgFixedParamKind(Name, 0, LibCVariant::Windows),
              VarArgFixedParamKind::Pointer);
    EXPECT_EQ(varArgFixedParamKind(Name, 1, LibCVariant::Windows),
              VarArgFixedParamKind::Unknown);
  }
}

TEST(VarArgFixedCount, WindowsWideFormattersNeedTheirDeclarationContext) {
  // The WDK has (buffer, format, ...), the ISO CRT (buffer, count, format,
  // ...). PE alone cannot select either or forward to a guessed vswprintf.
  EXPECT_EQ(varArgFixedCount("swprintf", LibCVariant::Windows), 0u);
  EXPECT_EQ(varArgFixedParamKind("swprintf", 1, LibCVariant::Windows),
            VarArgFixedParamKind::Unknown);
  EXPECT_FALSE(vaListParameter("vswprintf", LibCVariant::Windows));
  const LibCVariadicForward *Forward = libcVariadicForward("swprintf");
  ASSERT_NE(Forward, nullptr);
  EXPECT_FALSE(variadicIn(*Forward, LibCVariant::Windows));
  EXPECT_TRUE(variadicIn(*Forward, LibCVariant::GNU));
  EXPECT_EQ(varArgFixedCount("swprintf", LibCVariant::GNU), 3u);
  EXPECT_EQ(vaListParameter("vswprintf", LibCVariant::GNU), 3u);
}

TEST(LibCArity, WindowsComplexRecordsNeedTheirFullABIType) {
  for (const auto &Entry : kComplexArity) {
    EXPECT_FALSE(libcArity(Entry.Name, LibCVariant::Windows)) << Entry.Name;
    ASSERT_TRUE(libcArity(Entry.Name, LibCVariant::GNU)) << Entry.Name;
    EXPECT_EQ(libcArity(Entry.Name, LibCVariant::GNU)->FpArgs,
              Entry.Arity.FpArgs)
        << Entry.Name;
  }
  ASSERT_TRUE(libcArity("hypot", LibCVariant::Windows));
  EXPECT_EQ(libcArity("hypot", LibCVariant::Windows)->FpArgs, 2u);
}

// Fortified _FORTIFY_SOURCE variants: __<name>_chk prepend guard arguments
// (flag, and for the buffer forms the dest/object size) before the format.
// The caller strips at most one leading '_' (a Mach-O prefix), so the matcher
// must accept both "__snprintf_chk" (Mach-O) and "_snprintf_chk" (ELF
// over-strip) by normalizing every leading underscore.
TEST(VarArgFixedCount, FortifiedChkVariants) {
  // Mach-O form (one '_' already stripped from "___snprintf_chk").
  EXPECT_EQ(varArgFixedCount("__printf_chk", LibCVariant::GNU),
            2u); // flag, fmt
  EXPECT_EQ(varArgFixedCount("__fprintf_chk", LibCVariant::GNU),
            3u); // stream, flag, fmt
  EXPECT_EQ(varArgFixedCount("__dprintf_chk", LibCVariant::GNU),
            3u); // fd, flag, fmt
  EXPECT_EQ(varArgFixedCount("__asprintf_chk", LibCVariant::GNU),
            3u); // &buf, flag, fmt
  EXPECT_EQ(varArgFixedCount("__sprintf_chk", LibCVariant::GNU),
            4u); // buf, flag, slen, fmt
  EXPECT_EQ(varArgFixedCount("__snprintf_chk", LibCVariant::GNU),
            5u); // buf, maxlen, flag, slen, fmt
  // ELF form (over-stripped to a single leading '_').
  EXPECT_EQ(varArgFixedCount("_snprintf_chk", LibCVariant::GNU), 5u);
  EXPECT_EQ(varArgFixedCount("_printf_chk", LibCVariant::GNU), 2u);
  // va_list _chk variants are not variadic.
  EXPECT_EQ(varArgFixedCount("__vsnprintf_chk", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("__vsprintf_chk", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("__vprintf_chk", LibCVariant::GNU), 0u);
  // Unknown _chk callees stay non-variadic (e.g. memcpy/strcpy fortified
  // forms).
  EXPECT_EQ(varArgFixedCount("__memcpy_chk", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("__strcpy_chk", LibCVariant::GNU), 0u);
}

// =====================================================================
// varArgFixedCount — POSIX whitelist
// =====================================================================

TEST(VarArgFixedCount, PosixWhitelist) {
  // Every parameter a declaration names before its `...` is fixed: Apple's
  // AArch64 convention passes those in registers and the rest on the stack.
  EXPECT_EQ(varArgFixedCount("open", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("openat", LibCVariant::GNU), 3u);
  EXPECT_EQ(varArgFixedCount("fcntl", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("ioctl", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("execl", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("execlp", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("execle", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("syslog", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("err", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("errx", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("warn", LibCVariant::GNU), 1u);
  EXPECT_EQ(varArgFixedCount("warnx", LibCVariant::GNU), 1u);
  EXPECT_EQ(varArgFixedCount("mq_open", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("sem_open", LibCVariant::GNU), 2u);
}

TEST(VarArgFixedCount, LinuxProcessControlNamesOneInteger) {
  // syscall(number, ...) and prctl(option, ...) read their further
  // arguments as the integer they name requires.
  for (const char *Name : {"syscall", "prctl"}) {
    SCOPED_TRACE(Name);
    EXPECT_TRUE(isKnownFunction(Name));
    EXPECT_EQ(varArgFixedCount(Name, LibCVariant::GNU), 1u);
    EXPECT_EQ(varArgFixedParamKind(Name, 0, LibCVariant::GNU),
              VarArgFixedParamKind::Integer);
    EXPECT_EQ(varArgFixedParamKind(Name, 1, LibCVariant::GNU),
              VarArgFixedParamKind::Unknown);
  }
  EXPECT_STREQ(headerFor("syscall"), "unistd.h");
  // ptrace(request, ...) is variadic only as Linux and Android declare it;
  // Darwin's takes its pid, address and datum fixed, so no call reads its
  // arguments as variadic ones.
  EXPECT_TRUE(isKnownFunction("ptrace"));
  EXPECT_STREQ(headerFor("ptrace"), "sys/ptrace.h");
  EXPECT_EQ(varArgFixedCount("ptrace", LibCVariant::GNU), 0u);
  const LibCVariadicForward *Forward = libcVariadicForward("ptrace");
  ASSERT_NE(Forward, nullptr);
  EXPECT_EQ(Forward->Format, BinaryFormat::ELF);
  EXPECT_EQ(Forward->Count, 3u);
  EXPECT_EQ(libcVariadicForward("syscall")->Format, BinaryFormat::Unknown);
}

TEST(VarArgFixedParamKind, OpenPreservesPathAndFlags) {
  EXPECT_EQ(varArgFixedParamKind("open", 0, LibCVariant::GNU),
            VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("open", 1, LibCVariant::GNU),
            VarArgFixedParamKind::Integer);
  EXPECT_EQ(varArgFixedParamKind("open", 2, LibCVariant::GNU),
            VarArgFixedParamKind::Unknown);
  EXPECT_EQ(varArgFixedParamKind("execl", 0, LibCVariant::GNU),
            VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("ioctl", 0, LibCVariant::GNU),
            VarArgFixedParamKind::Integer);
}

TEST(VarArgFixedCount, DarwinObjectiveCMessageStubs) {
  // The linker-specialized stub supplies _cmd in x1.  The external call still
  // models that register as part of the fixed prefix so method arguments start
  // at x2 and any true varargs follow the selector's colon-counted arguments.
  EXPECT_EQ(varArgFixedCount("objc_msgSend$length", LibCVariant::GNU), 2u);
  EXPECT_EQ(
      varArgFixedCount("objc_msgSend$stringWithFormat:", LibCVariant::GNU), 3u);
  EXPECT_EQ(varArgFixedCount("objc_msgSend$exceptionWithName:reason:userInfo:",
                             LibCVariant::GNU),
            5u);
  EXPECT_EQ(varArgFixedCount("objc_msgSend$", LibCVariant::GNU), 0u);
}

// =====================================================================
// varArgFixedCount — non-variadic functions should return 0
// =====================================================================

TEST(VarArgFixedCount, NonVariadicReturnsZero) {
  EXPECT_EQ(varArgFixedCount("strlen", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("memcpy", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("malloc", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("free", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("exit", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("read", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("write", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("close", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("puts", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("fopen", LibCVariant::GNU), 0u);
}

TEST(VarArgFixedCount, AllVaListVariantsReturnZero) {
  EXPECT_EQ(varArgFixedCount("vprintf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vscanf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vfprintf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vfscanf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vsprintf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vsscanf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vsnprintf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vasprintf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vdprintf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vwprintf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vwscanf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vswprintf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vswscanf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vfwprintf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vfwscanf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("vsnwprintf", LibCVariant::GNU), 0u);
}

TEST(VaListParameter, FollowsTheVariadicRoutinesFixedParameters) {
  EXPECT_EQ(vaListParameter("vfprintf", LibCVariant::GNU), 2u);
  EXPECT_EQ(vaListParameter("vwarn", LibCVariant::GNU), 1u);
  EXPECT_EQ(vaListParameter("__vsnprintf_chk", LibCVariant::GNU), 5u);
  EXPECT_EQ(vaListParameter("__isoc23_vfscanf", LibCVariant::GNU), 2u);
  // strerror_r's buffer, and a variadic routine's own parameters, are no
  // va_list.
  EXPECT_FALSE(vaListParameter("strerror_r", LibCVariant::GNU).has_value());
  EXPECT_FALSE(vaListParameter("fprintf", LibCVariant::GNU).has_value());
  EXPECT_FALSE(vaListParameter("", LibCVariant::GNU).has_value());
}

TEST(VarArgFixedCount, EmptyAndGarbage) {
  EXPECT_EQ(varArgFixedCount("", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("x", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("__some_internal_func", LibCVariant::GNU), 0u);
}

TEST(VarArgFixedParamKind, PrintfFamilyPreservesScalarParameters) {
  EXPECT_EQ(varArgFixedParamKind("printf", 0, LibCVariant::GNU),
            VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("dprintf", 0, LibCVariant::GNU),
            VarArgFixedParamKind::Integer);
  EXPECT_EQ(varArgFixedParamKind("dprintf", 1, LibCVariant::GNU),
            VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("snprintf", 0, LibCVariant::GNU),
            VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("snprintf", 1, LibCVariant::GNU),
            VarArgFixedParamKind::Integer);
  EXPECT_EQ(varArgFixedParamKind("snprintf", 2, LibCVariant::GNU),
            VarArgFixedParamKind::Pointer);
}

TEST(VarArgFixedParamKind, FortifiedSnprintfShape) {
  constexpr VarArgFixedParamKind Expected[] = {
      VarArgFixedParamKind::Pointer, VarArgFixedParamKind::Integer,
      VarArgFixedParamKind::Integer, VarArgFixedParamKind::Integer,
      VarArgFixedParamKind::Pointer};
  for (unsigned I = 0; I < 5; ++I) {
    EXPECT_EQ(varArgFixedParamKind("___snprintf_chk", I, LibCVariant::GNU),
              Expected[I]);
    EXPECT_EQ(varArgFixedParamKind("snprintf_chk", I, LibCVariant::GNU),
              Expected[I]);
  }
  EXPECT_EQ(varArgFixedParamKind("snprintf_chk", 5, LibCVariant::GNU),
            VarArgFixedParamKind::Unknown);
}

TEST(VarArgFixedParamKind, UnknownSelectorArgumentsStayRecovered) {
  EXPECT_EQ(varArgFixedParamKind("objc_msgSend$stringWithFormat:", 0,
                                 LibCVariant::GNU),
            VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("objc_msgSend$stringWithFormat:", 1,
                                 LibCVariant::GNU),
            VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("objc_msgSend$stringWithFormat:", 2,
                                 LibCVariant::GNU),
            VarArgFixedParamKind::Unknown);
  EXPECT_EQ(varArgFixedParamKind("not_variadic", 0, LibCVariant::GNU),
            VarArgFixedParamKind::Unknown);
}

// =====================================================================
// isVaListConsumer — the v*printf/v*scanf family and __v*_chk variants
// =====================================================================

TEST(IsVaListConsumer, VPrintfScanfFamily) {
  EXPECT_TRUE(isVaListConsumer("vprintf"));
  EXPECT_TRUE(isVaListConsumer("vfprintf"));
  EXPECT_TRUE(isVaListConsumer("vsprintf"));
  EXPECT_TRUE(isVaListConsumer("vsnprintf"));
  EXPECT_TRUE(isVaListConsumer("vdprintf"));
  EXPECT_TRUE(isVaListConsumer("vasprintf"));
  EXPECT_TRUE(isVaListConsumer("vscanf"));
  EXPECT_TRUE(isVaListConsumer("vfscanf"));
  EXPECT_TRUE(isVaListConsumer("vsscanf"));
  EXPECT_TRUE(isVaListConsumer("vwprintf"));
  EXPECT_TRUE(isVaListConsumer("vfwprintf"));
  EXPECT_TRUE(isVaListConsumer("vswprintf"));
}

TEST(IsVaListConsumer, FortifiedChkVariants) {
  EXPECT_TRUE(isVaListConsumer("vsnprintf_chk"));
  EXPECT_TRUE(isVaListConsumer("vsprintf_chk"));
  EXPECT_TRUE(isVaListConsumer("vprintf_chk"));
  EXPECT_TRUE(isVaListConsumer("vfprintf_chk"));
  EXPECT_TRUE(isVaListConsumer("vdprintf_chk"));
  EXPECT_TRUE(isVaListConsumer("vasprintf_chk"));
}

TEST(IsVaListConsumer, IrregularConsumers) {
  EXPECT_TRUE(isVaListConsumer("vsyslog"));
  EXPECT_TRUE(isVaListConsumer("verr"));
  EXPECT_TRUE(isVaListConsumer("verrx"));
  EXPECT_TRUE(isVaListConsumer("vwarn"));
  EXPECT_TRUE(isVaListConsumer("vwarnx"));
}

TEST(IsVaListConsumer, NotConsumers) {
  // The "..." variadic producers take a format + ellipsis, not a va_list.
  EXPECT_FALSE(isVaListConsumer("printf"));
  EXPECT_FALSE(isVaListConsumer("fprintf"));
  EXPECT_FALSE(isVaListConsumer("snprintf"));
  EXPECT_FALSE(isVaListConsumer("scanf"));
  EXPECT_FALSE(isVaListConsumer("syslog"));
  EXPECT_FALSE(isVaListConsumer("err"));
  EXPECT_FALSE(isVaListConsumer("warn"));
  // Ordinary non-variadic functions.
  EXPECT_FALSE(isVaListConsumer("strlen"));
  EXPECT_FALSE(isVaListConsumer("memcpy"));
  EXPECT_FALSE(isVaListConsumer("vsnprint")); // not a real printf suffix
  EXPECT_FALSE(isVaListConsumer(""));
  EXPECT_FALSE(isVaListConsumer("v"));
}

TEST(LibCArity, ExceptionRuntimeFunctions) {
  auto BeginCatch = libcArity("cxa_begin_catch", LibCVariant::GNU);
  ASSERT_TRUE(BeginCatch.has_value());
  EXPECT_EQ(BeginCatch->IntArgs, 1);
  EXPECT_EQ(BeginCatch->FpArgs, 0);

  auto Throw = libcArity("cxa_throw", LibCVariant::GNU);
  ASSERT_TRUE(Throw.has_value());
  EXPECT_EQ(Throw->IntArgs, 3);
  EXPECT_EQ(Throw->FpArgs, 0);

  auto ObjCThrow = libcArity("objc_exception_throw", LibCVariant::GNU);
  ASSERT_TRUE(ObjCThrow.has_value());
  EXPECT_EQ(ObjCThrow->IntArgs, 1);
  EXPECT_EQ(ObjCThrow->FpArgs, 0);

  auto Cookie = libcArity("security_check_cookie", LibCVariant::GNU);
  ASSERT_TRUE(Cookie.has_value());
  EXPECT_EQ(Cookie->IntArgs, 1);
  EXPECT_EQ(Cookie->FpArgs, 0);
  EXPECT_TRUE(libcArityForSymbol("__security_check_cookie", LibCVariant::GNU)
                  .has_value());
  EXPECT_EQ(
      libcArityForSymbol("__security_check_cookie", LibCVariant::GNU)->IntArgs,
      1);

  auto Raise = libcArity("raise_securityfailure", LibCVariant::GNU);
  ASSERT_TRUE(Raise.has_value());
  EXPECT_EQ(Raise->IntArgs, 1);

  auto GetCurrentProcess = libcArity("GetCurrentProcess", LibCVariant::GNU);
  ASSERT_TRUE(GetCurrentProcess.has_value());
  EXPECT_EQ(GetCurrentProcess->IntArgs, 0);
  EXPECT_EQ(GetCurrentProcess->FpArgs, 0);
  auto Terminate = libcArity("TerminateProcess", LibCVariant::GNU);
  ASSERT_TRUE(Terminate.has_value());
  EXPECT_EQ(Terminate->IntArgs, 2);
  EXPECT_TRUE(libcArityForSymbol("IsProcessorFeaturePresent", LibCVariant::GNU)
                  .has_value());
  EXPECT_EQ(libcArityForSymbol("IsProcessorFeaturePresent", LibCVariant::GNU)
                ->IntArgs,
            1);
}

TEST(LibCArity, VaListConsumersHaveFixedPrototypes) {
  auto VPrintf = libcArity("vprintf", LibCVariant::GNU);
  ASSERT_TRUE(VPrintf.has_value());
  EXPECT_EQ(VPrintf->IntArgs, 2);
  EXPECT_EQ(VPrintf->FpArgs, 0);

  // The va_list is one more pointer argument, after every fixed one.
  const std::pair<const char *, int> Consumers[] = {
      {"vfprintf", 3}, {"vsnprintf", 4}, {"vsprintf", 3}, {"vdprintf", 3},
      {"vscanf", 2},   {"vfscanf", 3},   {"vsscanf", 3},  {"vasprintf", 3}};
  for (const auto &[Name, IntArgs] : Consumers) {
    const auto Arity = libcArity(Name, LibCVariant::GNU);
    ASSERT_TRUE(Arity.has_value()) << Name;
    EXPECT_EQ(Arity->IntArgs, IntArgs) << Name;
  }
}

TEST(LibCArity, PosixRoutinesTakeTheirPrototypesArguments) {
  // Without a table entry the call-argument scan decides the count: gzip
  // printed fstat(fd, &st) with five arguments, and each PLT stub of these
  // routines printed its call with none.
  const std::pair<const char *, int> Expected[] = {
      {"fstat", 2},         {"stat", 2},        {"mmap", 6},
      {"sigaction", 3},     {"sigprocmask", 3}, {"setlocale", 2},
      {"readdir", 1},       {"strftime", 4},    {"getopt_long", 5},
      {"nl_langinfo", 1},   {"fnmatch", 3},     {"mbrtowc", 4},
      {"regexec", 5},       {"re_search", 6},   {"pthread_create", 4},
      {"socket", 3},        {"waitpid", 3},     {"reallocarray", 3},
      {"__assert_fail", 4}, {"__overflow", 2},  {"__uflow", 1},
      {"__fpending", 1},    {"mbrtoc32", 4},    {"fwrite_unlocked", 4}};
  for (const auto &[Name, IntArgs] : Expected) {
    const auto Arity = libcArityForSymbol(Name, LibCVariant::GNU);
    ASSERT_TRUE(Arity.has_value()) << Name;
    EXPECT_EQ(Arity->IntArgs, IntArgs) << Name;
    EXPECT_EQ(Arity->FpArgs, 0) << Name;
  }
}

TEST(LibCArity, NoArityWhereACountCannotSayIt) {
  // A variadic routine, a floating-point result, an aggregate result (a
  // hidden pointer argument on i386) or an argument two 32-bit slots wide
  // keeps the call-argument scan's count.
  for (const char *Name : {"printf", "open", "fcntl", "strtod_l", "div", "ldiv",
                           "lldiv", "imaxdiv", "ffsll", "imaxabs"})
    EXPECT_FALSE(libcArityForSymbol(Name, LibCVariant::GNU).has_value())
        << Name;
  // So does a routine the Windows C runtimes declare otherwise, since the
  // tables apply to every format by name: _mkdir(dir), Win64's
  // _setjmp(env, frame), msvcrt's wcstok(str, delim).
  for (const char *Name : {"mkdir", "setjmp", "_setjmp", "wcstok"})
    EXPECT_FALSE(libcArityForSymbol(Name, LibCVariant::GNU).has_value())
        << Name;
}

TEST(LibCArity, PreservesDarwinErrorSymbolSpelling) {
  auto DarwinError = libcArityForSymbol("___error", LibCVariant::GNU);
  ASSERT_TRUE(DarwinError.has_value());
  EXPECT_EQ(DarwinError->IntArgs, 0);
  EXPECT_EQ(DarwinError->FpArgs, 0);

  EXPECT_FALSE(libcArityForSymbol("error", LibCVariant::GNU).has_value());
  EXPECT_FALSE(libcArityForSymbol("__error", LibCVariant::GNU).has_value());
}

TEST(LibCArity, TranslationRoutines) {
  // sed's `_("unmatched `{'")` is dcgettext(NULL, msgid, LC_MESSAGES).  An
  // unbounded scan took two more live registers as arguments.
  const std::pair<const char *, int> Expected[] = {
      {"gettext", 1},     {"dgettext", 2},       {"dcgettext", 3},
      {"__dcgettext", 3}, {"ngettext", 3},       {"dngettext", 4},
      {"dcngettext", 5},  {"bindtextdomain", 2}, {"textdomain", 1}};
  for (const auto &[Name, IntArgs] : Expected) {
    const auto Arity = libcArityForSymbol(Name, LibCVariant::GNU);
    ASSERT_TRUE(Arity.has_value()) << Name;
    EXPECT_EQ(Arity->IntArgs, IntArgs) << Name;
    EXPECT_EQ(Arity->FpArgs, 0) << Name;
  }
  EXPECT_STREQ(headerFor("dcgettext"), "libintl.h");
}

// =====================================================================
// isKnownFunction
// =====================================================================

TEST(IsKnownFunction, BasicLibCFunctions) {
  EXPECT_TRUE(isKnownFunction("printf"));
  EXPECT_TRUE(isKnownFunction("strlen"));
  EXPECT_TRUE(isKnownFunction("malloc"));
  EXPECT_TRUE(isKnownFunction("free"));
  EXPECT_TRUE(isKnownFunction("memcpy"));
}

TEST(IsKnownFunction, NotLibC) {
  EXPECT_FALSE(isKnownFunction("my_custom_func"));
  EXPECT_FALSE(isKnownFunction(""));
  EXPECT_FALSE(isKnownFunction("printk"));
  EXPECT_FALSE(isKnownFunction("NSLog"));
}

// =====================================================================
// headerFor
// =====================================================================

TEST(HeaderFor, GnuAndPosixHeadersOfTheArityTables) {
  EXPECT_STREQ(headerFor("getopt_long"), "getopt.h");
  EXPECT_STREQ(headerFor("nl_langinfo"), "langinfo.h");
  EXPECT_STREQ(headerFor("fnmatch"), "fnmatch.h");
  EXPECT_STREQ(headerFor("__fpending"), "stdio_ext.h");
  EXPECT_STREQ(headerFor("mbrtoc32"), "uchar.h");
  EXPECT_STREQ(headerFor("renameat"), "stdio.h");
  EXPECT_STREQ(headerFor("futimens"), "sys/stat.h");
  EXPECT_STREQ(headerFor("re_search"), "regex.h");
}

TEST(HeaderFor, SemaphoreRoutines) {
  // A unit that takes bionic's typedef-named sem_t from <semaphore.h>
  // includes the header, which declares these routines: one the unit
  // declared `extern int sem_open();` beside it did not compile.
  for (const char *Name :
       {"sem_close", "sem_destroy", "sem_getvalue", "sem_init", "sem_open",
        "sem_post", "sem_timedwait", "sem_trywait", "sem_unlink", "sem_wait"})
    EXPECT_STREQ(headerFor(Name), "semaphore.h") << Name;
  EXPECT_EQ(varArgFixedCount("sem_open", LibCVariant::Bionic), 2u);
  // Its name is a pointer C does not convert an integer to, and its result
  // a pointer, which an integer local takes converted.
  EXPECT_TRUE(isObjectPointerParameter("sem_open", 0, LibCVariant::Bionic));
  EXPECT_TRUE(isCStringParameter("sem_open", 0));
  EXPECT_EQ(libcReturnsValue("sem_open", LibCVariant::Bionic), true);
  const auto Init = libcArityForSymbol("sem_init", LibCVariant::Bionic);
  ASSERT_TRUE(Init.has_value());
  EXPECT_EQ(Init->IntArgs, 3);
}

TEST(HeaderFor, DataObjectsTheirHeadersDeclare) {
  // The image's copy of a header's object is the header's, of the type the
  // header declares it with.
  EXPECT_STREQ(headerOfDataObject("stdin"), "stdio.h");
  EXPECT_STREQ(typeOfDataObject("stdin"), "FILE *");
  EXPECT_STREQ(typeOfDataObject("optind"), "int");
  EXPECT_STREQ(typeOfDataObject("timezone"), "long");
  EXPECT_EQ(typeOfDataObject("my_stream"), nullptr);
  // The GNU objects 398 Debian programs copy (program_invocation_short_name)
  // and 187 reach (__environ), which <errno.h> and <unistd.h> declare under
  // _GNU_SOURCE, as the units define it.
  EXPECT_STREQ(headerOfDataObject("environ"), "unistd.h");
  EXPECT_STREQ(typeOfDataObject("__environ"), "char **");
  EXPECT_STREQ(headerOfDataObject("program_invocation_short_name"), "errno.h");
  EXPECT_STREQ(typeOfDataObject("program_invocation_name"), "char *");
}

TEST(HeaderFor, RoutinesTheGnuLibrarysMacrosCall) {
  // The GNU C Library's headers declare the routines their macros and
  // inline functions call, which programs import by these names: getline's
  // __getdelim, SIGRTMIN's __libc_current_sigrtmin, sigsetjmp's
  // __sigsetjmp, CPU_COUNT's __sched_cpucount, gettext's __dcgettext and
  // mbrlen's __mbrlen.  A unit that includes the header takes their
  // declarations and declares none of its own beside them.
  EXPECT_STREQ(headerFor("__getdelim"), "stdio.h");
  EXPECT_STREQ(headerFor("__libc_current_sigrtmin"), "signal.h");
  EXPECT_STREQ(headerFor("__libc_current_sigrtmax"), "signal.h");
  EXPECT_STREQ(headerFor("__sigsetjmp"), "setjmp.h");
  EXPECT_STREQ(headerFor("__sched_cpucount"), "sched.h");
  EXPECT_STREQ(headerFor("__dcgettext"), "libintl.h");
  EXPECT_STREQ(headerFor("__mbrlen"), "wchar.h");
  const auto Getdelim = libcArity("__getdelim", LibCVariant::GNU);
  ASSERT_TRUE(Getdelim.has_value());
  EXPECT_EQ(Getdelim->IntArgs, 4);
  EXPECT_TRUE(isObjectPointerParameter("__getdelim", 3, LibCVariant::GNU));
  EXPECT_FALSE(isObjectPointerParameter("__getdelim", 2, LibCVariant::GNU));
  EXPECT_EQ(libcResult("__sched_cpualloc", LibCVariant::GNU),
            LibCResult::Pointer);
}

TEST(HeaderFor, UnderscoredRoutinesTakeTheirObjectPointers) {
  // A row names a routine as its header declares it, and the lookups ignore
  // leading underscores on both sides: __fwriting's and __fpending's
  // streams and __assert_fail's strings are pointers C converts no integer
  // to, as __memcpy's are memcpy's.  The rows never matched before.
  EXPECT_TRUE(isObjectPointerParameter("__fwriting", 0, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("__fpending", 0, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("__overflow", 0, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("__assert_fail", 1, LibCVariant::GNU));
  EXPECT_FALSE(isObjectPointerParameter("__assert_fail", 2, LibCVariant::GNU));
}

TEST(HeaderFor, LargeFileRoutinesAreTheirRoutines) {
  // The GNU C Library's large-file names, which 197 Debian programs import
  // as open64 and 185 as fopen64, are declared by their routines' headers
  // and take and return what the routines do.  Their arity is their own: an
  // off64_t takes two slots of a 32-bit target's stack.
  EXPECT_STREQ(headerFor("open64"), "fcntl.h");
  EXPECT_STREQ(headerFor("fopen64"), "stdio.h");
  EXPECT_TRUE(isKnownFunction("stat64"));
  EXPECT_EQ(largeFileRoutine("readdir64"), "readdir");
  EXPECT_EQ(largeFileRoutine("read"), "");
  EXPECT_TRUE(isObjectPointerParameter("fopen64", 0, LibCVariant::GNU));
  EXPECT_TRUE(isCStringParameter("open64", 0));
  EXPECT_EQ(varArgFixedCount("open64", LibCVariant::GNU), 2u);
  EXPECT_EQ(libcResult("fopen64", LibCVariant::GNU), LibCResult::Pointer);
  EXPECT_FALSE(libcArity("pread64", LibCVariant::GNU).has_value());
  // A function it takes names the large-file record, not the routine's.
  EXPECT_EQ(
      functionPointerParameter("scandir64", 2, LibCVariant::GNU).value_or(""),
      "int (*)(const struct dirent64 *)");
  EXPECT_EQ(
      functionPointerParameter("scandir", 2, LibCVariant::GNU).value_or(""),
      "int (*)(const struct dirent *)");
}

TEST(HeaderFor, PasswordAndGroupDatabases) {
  // A program that reads a user's entry calls getpwnam, which <pwd.h>
  // declares with a const parameter: the unit takes that declaration, where
  // one of its own from the type library, which keeps no const, contradicts
  // it once a record definition includes the header.
  EXPECT_STREQ(headerFor("getpwnam"), "pwd.h");
  EXPECT_STREQ(headerFor("getgrgid_r"), "grp.h");
  EXPECT_STREQ(headerFor("getspnam"), "shadow.h");
  EXPECT_EQ(libcArity("getpwnam_r", LibCVariant::GNU)->IntArgs, 5);
  EXPECT_EQ(libcArity("endgrent", LibCVariant::GNU)->IntArgs, 0);
  EXPECT_TRUE(isObjectPointerParameter("getpwnam", 0, LibCVariant::GNU));
  EXPECT_EQ(libcResult("getpwuid", LibCVariant::GNU), LibCResult::Pointer);
  // Darwin's <unistd.h> declares initgroups, which glibc's <grp.h> does.
  EXPECT_EQ(headerFor("initgroups"), nullptr);
}

TEST(HeaderFor, StdioFunctions) {
  EXPECT_STREQ(headerFor("printf"), "stdio.h");
  EXPECT_STREQ(headerFor("fprintf"), "stdio.h");
  EXPECT_STREQ(headerFor("snprintf"), "stdio.h");
}

TEST(HeaderFor, PosixAndBsdRoutinesTheArityTablesName) {
  // Absent from the header lists, each was declared `extern int` and lost
  // its result: popen's FILE *, random's long, drand48's double.
  for (const char *Name :
       {"ctermid", "fmemopen", "getdelim", "getline", "getw", "open_memstream",
        "pclose", "popen", "putw", "tempnam"})
    EXPECT_STREQ(headerFor(Name), "stdio.h") << Name;
  for (const char *Name :
       {"arc4random", "drand48", "erand48", "getprogname", "initstate",
        "jrand48", "lrand48", "mkdtemp", "mrand48", "nrand48", "random",
        "reallocf", "seed48", "setprogname", "setstate", "srand48", "srandom"})
    EXPECT_STREQ(headerFor(Name), "stdlib.h") << Name;
}

TEST(HeaderFor, UnknownReturnsNull) {
  EXPECT_EQ(headerFor("not_a_real_function"), nullptr);
  EXPECT_EQ(headerFor(""), nullptr);
}

// =====================================================================
// LibCPrototype — routines no standard header declares
// =====================================================================

namespace {
template <size_t N>
void expectWellFormed(const std::array<LibCPrototype, N> &Prototypes) {
  for (const LibCPrototype &Prototype : Prototypes) {
    SCOPED_TRACE(std::string(Prototype.Name));
    EXPECT_FALSE(Prototype.Name.empty());
    EXPECT_FALSE(Prototype.Return.empty());
    for (size_t I = 0; I < Prototype.Params.size(); ++I)
      EXPECT_EQ(Prototype.Params[I].empty(), I >= Prototype.ParamCount);
    // A header declaring the routine would conflict with the prototype.
    EXPECT_FALSE(isKnownFunction(Prototype.Name));
    EXPECT_FALSE(isKnownFunction(stripLeadingUnderscores(Prototype.Name)));
    EXPECT_EQ(libcPrototype(Prototype.Name, BinaryFormat::COFF), &Prototype);
    // The vararg traits read the same fixed parameters.
    EXPECT_EQ(varArgFixedCount(Prototype.Name, LibCVariant::GNU),
              Prototype.Variadic ? Prototype.ParamCount : 0u);
  }
}

/// A table whose arities the registry derives.
template <size_t N>
void expectDerivedArity(const std::array<LibCPrototype, N> &Prototypes) {
  for (const LibCPrototype &Prototype : Prototypes) {
    SCOPED_TRACE(std::string(Prototype.Name));
    const auto Arity = libcArityForSymbol(Prototype.Name, LibCVariant::GNU);
    if (Prototype.Variadic) {
      EXPECT_FALSE(Arity.has_value());
      continue;
    }
    ASSERT_TRUE(Arity.has_value());
    EXPECT_EQ(Arity->IntArgs, Prototype.ParamCount);
    EXPECT_EQ(Arity->FpArgs, 0);
    EXPECT_EQ(libcPrototype(Prototype.Name, BinaryFormat::ELF), &Prototype);
  }
}
} // namespace

TEST(LibCPrototype, TablesAreWellFormed) {
  expectWellFormed(kStartupPrototypes);
  expectWellFormed(kItaniumRuntimePrototypes);
  expectWellFormed(kFortifyPrototypes);
  expectWellFormed(kWindowsCRTPrototypes);
}

TEST(LibCPrototype, ArityDerivesFromThePortablePrototypes) {
  expectDerivedArity(kStartupPrototypes);
  expectDerivedArity(kItaniumRuntimePrototypes);
  expectDerivedArity(kFortifyPrototypes);
  // The arities the exception runtime table gave before.
  EXPECT_EQ(libcArity("cxa_throw", LibCVariant::GNU)->IntArgs, 3);
  EXPECT_EQ(libcArity("cxa_begin_catch", LibCVariant::GNU)->IntArgs, 1);
  EXPECT_EQ(libcArity("Unwind_ForcedUnwind", LibCVariant::GNU)->IntArgs, 3);
  EXPECT_EQ(libcArity("libc_start_main", LibCVariant::GNU)->IntArgs, 7);
}

TEST(LibCPrototype, WindowsRuntimeNamesStayInPE) {
  for (const LibCPrototype &Prototype : kWindowsCRTPrototypes) {
    SCOPED_TRACE(std::string(Prototype.Name));
    EXPECT_EQ(libcPrototype(Prototype.Name, BinaryFormat::ELF), nullptr);
    EXPECT_EQ(libcPrototype(Prototype.Name, BinaryFormat::MachO), nullptr);
    // Without underscores _lock is any other runtime's lock: only the
    // Windows API routines the exception tables already name have a
    // portable arity, and it is theirs.
    if (const auto Arity = libcArityForSymbol(Prototype.Name, LibCVariant::GNU))
      EXPECT_EQ(Arity->IntArgs, Prototype.ParamCount);
  }
  EXPECT_FALSE(libcArityForSymbol("_lock", LibCVariant::GNU).has_value());
  EXPECT_FALSE(libcArityForSymbol("terminate", LibCVariant::GNU).has_value());
}

TEST(LibCPrototype, WindowsDriverCRTUsesExactFixedParameters) {
  for (const auto &[Name, Count] : {std::pair{"wcscat_s", 3u},
                                    {"wcscpy_s", 3u},
                                    {"wcsncat_s", 4u},
                                    {"wcsncpy_s", 4u},
                                    {"__C_specific_handler", 4u}}) {
    const auto *Prototype = libcPrototype(Name, BinaryFormat::COFF);
    ASSERT_NE(Prototype, nullptr) << Name;
    EXPECT_EQ(Prototype->ParamCount, Count) << Name;
    EXPECT_EQ(Prototype->Return, "int") << Name;
    EXPECT_FALSE(Prototype->Variadic) << Name;
    EXPECT_FALSE(Prototype->Winapi) << Name;
  }
  const auto *Wide = libcPrototype("wcscat_s", BinaryFormat::COFF);
  EXPECT_EQ(Wide->Params[1], "size_t");
}

TEST(LibCPrototype, PointerTypes) {
  EXPECT_TRUE(isPointerType("void *"));
  EXPECT_TRUE(isPointerType("const unsigned short **"));
  EXPECT_TRUE(isPointerType("int (*)(int, char **, char **)"));
  EXPECT_TRUE(isPointerType("void (**)(void)"));
  EXPECT_TRUE(isPointerType("int32_t (WINAPI *)(void *)"));
  EXPECT_FALSE(isPointerType("int"));
  EXPECT_FALSE(isPointerType("uintptr_t"));
  EXPECT_FALSE(isPointerType("size_t"));
}

TEST(LibCReturnsValue, ReadsTheDeclaredResult) {
  // A header's declaration.
  EXPECT_EQ(libcReturnsValue("calloc", LibCVariant::GNU), true);
  EXPECT_EQ(libcReturnsValue("atexit", LibCVariant::Windows), true);
  EXPECT_EQ(libcReturnsValue("free", LibCVariant::Windows), false);
  EXPECT_EQ(libcReturnsValue("qsort", LibCVariant::Darwin), false);
  // A function returning a pointer to a function returns a value.
  EXPECT_EQ(libcReturnsValue("signal", LibCVariant::GNU), true);
  // A prototype's result, in the image format it applies to.
  EXPECT_EQ(libcReturnsValue("_strtoi64", LibCVariant::Windows), true);
  EXPECT_EQ(libcReturnsValue("_initterm", LibCVariant::Windows), false);
  EXPECT_FALSE(libcReturnsValue("_strtoi64", LibCVariant::GNU).has_value());
  EXPECT_EQ(libcReturnsValue("__libc_start_main", LibCVariant::GNU), true);
  // Neither declares it.
  EXPECT_FALSE(
      libcReturnsValue("neverd_unknown_routine", LibCVariant::GNU).has_value());
}

TEST(LibCResult, ReadsWhetherTheDeclaredResultIsAPointer) {
  // A header's declaration, a typedef's pointer too.
  EXPECT_EQ(libcResult("getenv", LibCVariant::GNU), LibCResult::Pointer);
  EXPECT_EQ(libcResult("malloc", LibCVariant::Windows), LibCResult::Pointer);
  EXPECT_EQ(libcResult("signal", LibCVariant::GNU), LibCResult::Pointer);
  EXPECT_EQ(libcResult("newlocale", LibCVariant::GNU), LibCResult::Pointer);
  EXPECT_EQ(libcResult("strlen", LibCVariant::GNU), LibCResult::Value);
  EXPECT_EQ(libcResult("pthread_self", LibCVariant::GNU), LibCResult::Value);
  EXPECT_EQ(libcResult("free", LibCVariant::Darwin), LibCResult::Void);
  // A prototype's result, in the image format it applies to.
  EXPECT_EQ(libcResult("__acrt_iob_func", LibCVariant::Windows),
            LibCResult::Pointer);
  EXPECT_EQ(libcResult("__getmainargs", LibCVariant::Windows),
            LibCResult::Value);
  EXPECT_EQ(libcResult("_initterm", LibCVariant::Windows), LibCResult::Void);
  EXPECT_EQ(libcResult("_onexit", LibCVariant::Windows), LibCResult::Pointer);
  EXPECT_FALSE(libcResult("__acrt_iob_func", LibCVariant::GNU).has_value());
  EXPECT_FALSE(
      libcResult("neverd_unknown_routine", LibCVariant::GNU).has_value());
}

TEST(LibCReturnsValue, TheGeneratedTableNamesOnlyKnownRoutines) {
#define NEVERD_LIBC_RETURN_KIND(NAME, KIND) EXPECT_TRUE(isKnownFunction(NAME));
#include "neverd/libc/LibCReturnKinds.inc"
#undef NEVERD_LIBC_RETURN_KIND
}

TEST(MinGWHeaderFunctions, NamesWhatTheHeadersDeclare) {
  // mingw-w64's own routines, the runtime's and the C library's.
  EXPECT_TRUE(mingwHeadersDeclare("__mingw_printf"));
  EXPECT_TRUE(mingwHeadersDeclare("_lock_file"));
  EXPECT_TRUE(mingwHeadersDeclare("_onexit"));
  EXPECT_TRUE(mingwHeadersDeclare("printf"));
  // The start-up code's internals and a program's own names are not.
  EXPECT_FALSE(mingwHeadersDeclare("_pei386_runtime_relocator"));
  EXPECT_FALSE(mingwHeadersDeclare("main"));
  EXPECT_FALSE(mingwHeadersDeclare("sub_140001000"));
}

TEST(VarArgFixedCount, IsoAliasesTakeTheStandardRoutinesArguments) {
  EXPECT_EQ(varArgFixedCount("__isoc99_scanf", LibCVariant::GNU), 1u);
  EXPECT_EQ(varArgFixedCount("__isoc99_sscanf", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("__isoc23_fscanf", LibCVariant::GNU), 2u);
  EXPECT_EQ(varArgFixedCount("__isoc99_vscanf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("__syslog_chk", LibCVariant::GNU), 3u);
  EXPECT_TRUE(isVaListConsumer("isoc99_vsscanf"));
  EXPECT_TRUE(isVaListConsumer("__isoc23_vfscanf"));
  EXPECT_FALSE(isVaListConsumer("isoc99_sscanf"));
  EXPECT_EQ(varArgFixedParamKind("__isoc99_fscanf", 0, LibCVariant::GNU),
            VarArgFixedParamKind::Pointer);
  // The Universal CRT's printf implementation takes a va_list.
  EXPECT_EQ(varArgFixedCount("__stdio_common_vfprintf", LibCVariant::GNU), 0u);
  EXPECT_EQ(varArgFixedCount("__stdio_common_vsscanf", LibCVariant::GNU), 0u);
  EXPECT_TRUE(isVaListConsumer("stdio_common_vfprintf"));
}

TEST(ObjectPointerParameter, HeadersSayWhichParametersTakePointers) {
  EXPECT_TRUE(isObjectPointerParameter("memcpy", 0, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("memcpy", 1, LibCVariant::GNU));
  EXPECT_FALSE(isObjectPointerParameter("memcpy", 2, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("__memcpy", 1, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("fputs", 1, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("strtol", 1, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("pthread_create", 3, LibCVariant::GNU));
  // A function pointer and a va_list have their own rules.
  EXPECT_FALSE(isObjectPointerParameter("qsort", 3, LibCVariant::GNU));
  EXPECT_FALSE(isObjectPointerParameter("pthread_create", 2, LibCVariant::GNU));
  EXPECT_FALSE(isObjectPointerParameter("signal", 1, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("vprintf", 0, LibCVariant::GNU));
  EXPECT_FALSE(isObjectPointerParameter("vprintf", 1, LibCVariant::GNU));
  EXPECT_FALSE(
      isObjectPointerParameter("not_a_libc_function", 0, LibCVariant::GNU));
}

TEST(ObjectPointerParameter, SocketAddressesTakeTheirPointers) {
  // glibc declares a socket function's address as a transparent union of
  // the sockaddr pointers under _GNU_SOURCE, which the units define: the
  // union takes a machine integer converted to a pointer, as each member.
  EXPECT_TRUE(isObjectPointerParameter("bind", 1, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("connect", 1, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("accept", 1, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("sendto", 4, LibCVariant::GNU));
  EXPECT_TRUE(isObjectPointerParameter("recvfrom", 4, LibCVariant::GNU));
  EXPECT_FALSE(isObjectPointerParameter("bind", 2, LibCVariant::GNU));
}

TEST(FunctionPointerParameter, StandardCallbacks) {
  EXPECT_EQ(functionPointerParameter("qsort", 3, LibCVariant::GNU),
            "int (*)(const void *, const void *)");
  EXPECT_EQ(functionPointerParameter("_qsort", 3, LibCVariant::GNU),
            "int (*)(const void *, const void *)");
  EXPECT_EQ(functionPointerParameter("signal", 1, LibCVariant::GNU),
            "void (*)(int)");
  EXPECT_EQ(functionPointerParameter("pthread_create", 2, LibCVariant::GNU),
            "void *(*)(void *)");
  EXPECT_FALSE(
      functionPointerParameter("qsort", 0, LibCVariant::GNU).has_value());
  EXPECT_FALSE(
      functionPointerParameter("printf", 0, LibCVariant::GNU).has_value());
  // Each names a function its header declares.
  for (const char *Name :
       {"atexit", "on_exit", "qsort", "bsearch", "signal", "pthread_create",
        "pthread_once", "thrd_create", "tsearch", "scandir", "dl_iterate_phdr",
        "makecontext"})
    EXPECT_TRUE(isKnownFunction(Name)) << Name;
}

// =====================================================================
// isMemCopyName / isMemSetName
// =====================================================================

TEST(IsMemCopyName, RecognizesCopyFamily) {
  EXPECT_TRUE(isMemCopyName("memcpy"));
  EXPECT_TRUE(isMemCopyName("memmove"));
  EXPECT_TRUE(isMemCopyName("memcpy_chk"));
  EXPECT_TRUE(isMemCopyName("memmove_chk"));
  EXPECT_TRUE(isMemCopyName("_memcpy"));
  EXPECT_TRUE(isMemCopyName("__memcpy"));
}

TEST(IsMemCopyName, RejectsUnrelated) {
  EXPECT_FALSE(isMemCopyName("memset"));
  EXPECT_FALSE(isMemCopyName("memcmp"));
  EXPECT_FALSE(isMemCopyName("strlen"));
  EXPECT_FALSE(isMemCopyName(""));
}

TEST(IsMemSetName, RecognizesSetFamily) {
  EXPECT_TRUE(isMemSetName("memset"));
  EXPECT_TRUE(isMemSetName("memset_chk"));
  EXPECT_TRUE(isMemSetName("_memset"));
  EXPECT_TRUE(isMemSetName("__memset"));
}

TEST(IsMemSetName, RejectsUnrelated) {
  EXPECT_FALSE(isMemSetName("memcpy"));
  EXPECT_FALSE(isMemSetName("memmove"));
  EXPECT_FALSE(isMemSetName(""));
}

// =====================================================================
// isNoReturnFunction / isReturnsTwiceFunction
// =====================================================================

TEST(IsNoReturnFunction, Terminators) {
  EXPECT_TRUE(isNoReturnFunction("abort"));
  EXPECT_TRUE(isNoReturnFunction("exit"));
  EXPECT_TRUE(isNoReturnFunction("_exit"));
  EXPECT_TRUE(isNoReturnFunction("longjmp"));
  EXPECT_TRUE(isNoReturnFunction("_longjmp"));
}

TEST(IsNoReturnFunction, WindowsNonlocalJumpFamily) {
  for (const char *Name : {"_longjmpex", "longjmpex", "__mingw_longjmp"}) {
    SCOPED_TRACE(Name);
    EXPECT_TRUE(isNoReturnFunction(Name));
    EXPECT_FALSE(isReturnsTwiceFunction(Name));
  }
  for (const char *Name : {"_setjmp3", "_setjmpex", "__intrinsic_setjmp",
                           "__intrinsic_setjmpex", "__mingw_setjmp"}) {
    SCOPED_TRACE(Name);
    EXPECT_TRUE(isReturnsTwiceFunction(Name));
    EXPECT_FALSE(isNoReturnFunction(Name));
  }
  for (const char *Name :
       {"my_setjmp3", "longjmpex_cleanup", "mingw_setjmp_init"}) {
    SCOPED_TRACE(Name);
    EXPECT_FALSE(isReturnsTwiceFunction(Name));
    EXPECT_FALSE(isNoReturnFunction(Name));
  }
}

TEST(IsNoReturnFunction, ItaniumCxxTerminators) {
  EXPECT_TRUE(isNoReturnFunction("_ZSt9terminatev"));
  EXPECT_TRUE(isNoReturnFunction("__cxa_call_terminate"));
  EXPECT_TRUE(isNoReturnFunction("__clang_call_terminate"));
}

TEST(IsNoReturnFunction, BionicStartupDoesNotMakeEveryInitializerTerminal) {
  EXPECT_TRUE(isNoReturnFunction("__libc_init"));
  EXPECT_FALSE(isNoReturnFunction("__libc_init_common"));
  EXPECT_FALSE(isNoReturnFunction("__libc_init_globals"));
  EXPECT_FALSE(isNoReturnFunction("__libc_init_scudo"));
}

TEST(IsNoReturnFunction, RustRoutinesDeclaredToReturnNever) {
  // v0 names in ELF and Mach-O spelling, a legacy name, a generic instance,
  // the panic_const family, and the panic handler std exports under its own
  // name or as a member of the synthetic `__rustc` crate.
  for (const char *Name :
       {"_RNvNtCsbAqs9W1eE8G_4core6option13unwrap_failed",
        "__RNvNtCsbAqs9W1eE8G_4core6option13unwrap_failed",
        "_ZN4core9panicking5panic17h0123456789abcdefE",
        "__RINvNtCsbAqs9W1eE8G_4core9panicking13assert_failedyyEB4_",
        "__RNvNtNtCsbAqs9W1eE8G_4core9panicking11panic_const"
        "24panic_const_add_overflow",
        "__RNvNtNtCsbAqs9W1eE8G_4core5slice5index16slice_index_fail",
        "__RNvNtCsl2jJwwz0T1e_5alloc7raw_vec12handle_error",
        "__RNvCs9wFQrvczXsK_7___rustc17rust_begin_unwind",
        "_rust_begin_unwind"}) {
    SCOPED_TRACE(Name);
    EXPECT_TRUE(isNoReturnFunction(Name));
  }
  // std::thread::panicking, std::panicking::panic_count::increase and
  // std::process::id return; __rust_start_panic returns when no unwinder
  // takes the panic; a crate's own `panicking::panic` is not core's.
  for (const char *Name :
       {"_RNvNtCsgejaSCmAoRz_3std6thread9panicking",
        "__RNvNtNtCsgejaSCmAoRz_3std9panicking11panic_count8increase",
        "_RNvNtCsgejaSCmAoRz_3std7process2id",
        "__RNvCs9wFQrvczXsK_7___rustc18___rust_start_panic",
        "_RNvNtCs1a2b3c4d5e6_7mycrate9panicking5panic"}) {
    SCOPED_TRACE(Name);
    EXPECT_FALSE(isNoReturnFunction(Name));
  }
}

TEST(IsNoReturnFunction, MsvcGsHelpers) {
  EXPECT_TRUE(isNoReturnFunction("report_gsfailure"));
  EXPECT_TRUE(isNoReturnFunction("_report_gsfailure"));
  EXPECT_TRUE(isNoReturnFunction("raise_securityfailure"));
  EXPECT_TRUE(isNoReturnFunction("_raise_securityfailure"));
  // It returns when it ends another process; only the helpers' own call,
  // with the current process's pseudo-handle, does not.
  EXPECT_FALSE(isNoReturnFunction("TerminateProcess"));
}

TEST(IsNoReturnFunction, MicrosoftRuntimeTerminators) {
  EXPECT_TRUE(isNoReturnFunction("_amsg_exit"));
  EXPECT_TRUE(isNoReturnFunction("_invoke_watson"));
  EXPECT_TRUE(isNoReturnFunction("_invalid_parameter_noinfo_noreturn"));
  EXPECT_TRUE(isNoReturnFunction("__std_terminate"));
  // A program's own function may take the name terminate.
  EXPECT_FALSE(isNoReturnFunction("terminate"));
  EXPECT_FALSE(isNoReturnFunction("unexpected"));
  for (const auto *Name : {"_o__exit", "_o__invalid_parameter_noinfo_noreturn",
                           "_o_abort", "_o_exit", "_o_terminate"})
    EXPECT_TRUE(isNoReturnFunction(Name)) << Name;
  EXPECT_FALSE(isNoReturnFunction("_o_TerminateProcess"));
  EXPECT_FALSE(isNoReturnFunction("_o_unreviewed_abort"));
}

TEST(IsNoReturnFunction, SometimesReturningExcluded) {
  EXPECT_FALSE(isNoReturnFunction("warn"));
  EXPECT_FALSE(isNoReturnFunction("warnx"));
  EXPECT_FALSE(isNoReturnFunction("printf"));
}

TEST(IsNoReturnTarget, ResolvesImportVeneerAndStaticSymbol) {
  constexpr va_t ImportStubVA = 0x1010;
  constexpr va_t StaticExitVA = 0x1020;
  constexpr va_t ReturningVA = 0x1030;

  BinaryImage Img;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Img.Segments.push_back(std::move(Text));

  Img.Imports.push_back({"libSystem.B.dylib", "_abort", 0, 0});
  ASSERT_TRUE(Img.recordImportStub(ImportStubVA, 0));

  Symbol StaticExit = Symbol::makeFunc(StaticExitVA);
  StaticExit.Name = "_exit";
  Img.Symbols.push_back(std::move(StaticExit));
  Symbol Returning = Symbol::makeFunc(ReturningVA);
  Returning.Name = "warn";
  Img.Symbols.push_back(std::move(Returning));

  EXPECT_TRUE(isNoReturnTarget(Img, ImportStubVA));
  EXPECT_TRUE(isNoReturnTarget(Img, StaticExitVA));
  EXPECT_FALSE(isNoReturnTarget(Img, ReturningVA));
  EXPECT_FALSE(isNoReturnTarget(Img, InvalidVA));
}

TEST(IsNoReturnTarget, IndexPreservesFirstNamesAndImportPrecedence) {
  BinaryImage Img;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Img.Segments.push_back(std::move(Text));
  auto ImportAt = [&](va_t Address, const char *Name) {
    Import Imp;
    Imp.Name = Name;
    Imp.IATAddr = Address;
    Img.Imports.push_back(std::move(Imp));
  };
  ImportAt(0x1000, "warn");
  ImportAt(0x1000, "abort");
  ImportAt(0x1100, "_abort");
  ImportAt(0x1100, "warn");
  ImportAt(0x1200, "");
  ImportAt(0x1200, "abort");
  ImportAt(0x1700, "warn");
  ImportAt(0x1800, "abort");
  ImportAt(0, "abort");
  ImportAt(InvalidVA, "abort");
  Img.ImportStubIndices[0x1000] = 1;
  Img.ImportStubIndices[0x1200] = 1;
  Img.ImportStubIndices[0x1300] = 1;
  Img.ImportStubIndices[0x1400] = Img.Imports.size();
  Img.ImportStubIndices[0x1900] = Img.Imports.size();
  auto SymbolAt = [&](va_t Address, const char *Name) {
    Symbol Sym;
    Sym.Addr = Address;
    Sym.Name = Name;
    Img.Symbols.push_back(std::move(Sym));
  };
  SymbolAt(0x1500, "warn");
  SymbolAt(0x1500, "abort");
  SymbolAt(0x1600, "_exit");
  SymbolAt(0x1600, "warn");
  SymbolAt(0x1700, "abort");
  SymbolAt(0x1800, "warn");
  SymbolAt(0x1900, "__cxa_call_terminate");
  SymbolAt(0x2000, "");
  SymbolAt(0x2000, "abort");
  SymbolAt(InvalidVA, "abort");

  const NoReturnTargetIndex Index(Img);
  const std::pair<va_t, bool> Cases[] = {
      {0, true},       {0x1000, false},   {0x1100, true},  {0x1200, false},
      {0x1300, true},  {0x1400, false},   {0x1500, false}, {0x1600, true},
      {0x1700, true},  {0x1800, true},    {0x1900, true},  {0x2000, false},
      {0x9999, false}, {InvalidVA, false}};
  for (const auto &[Address, Expected] : Cases) {
    SCOPED_TRACE(Address);
    EXPECT_EQ(isNoReturnTarget(Img, Address), Expected);
    EXPECT_EQ(Index.contains(Img, Address), Expected);
  }
}

TEST(IsNoReturnTarget, VeneerIndexUsesTheSameEntryAdmissionAsLiveLookup) {
  for (Arch A : {Arch::X64, Arch::AArch64, Arch::ARM}) {
    for (unsigned Case = 0; Case < 5; ++Case) {
      SCOPED_TRACE(::testing::Message() << unsigned(A) << ':' << Case);
      BinaryImage Img;
      Img.Arch = A;
      if (A == Arch::ARM)
        Img.Mode = InstructionMode::Thumb;
      Segment Text;
      Text.VA = 0x1000;
      Text.Size = 0x100;
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Text.Data.resize(Text.Size);
      if (Case != 1)
        Img.Segments.push_back(std::move(Text));
      Import Imp;
      Imp.Name = "abort";
      Imp.IATAddr = 0x3000;
      Img.Imports.push_back(std::move(Imp));
      const va_t Address = Case == 4 ? 0x1011 : 0x1010;
      Img.ImportStubIndices[Address] = 0;
      if (Case == 2)
        Img.DataInCode.insert(Address);
      if (Case == 3)
        Img.ContinuationCodeStarts.insert(Address);
      const NoReturnTargetIndex Index(Img);
      for (va_t Target : {Address, Address | va_t(1)}) {
        const bool Expected = Case == 0 || (Case == 4 && A == Arch::X64);
        const bool TaggedThumb = A == Arch::ARM && Case == 0;
        EXPECT_EQ(isNoReturnTarget(Img, Target),
                  Expected && (Target == Address || TaggedThumb));
        EXPECT_EQ(Index.contains(Img, Target), isNoReturnTarget(Img, Target));
      }
    }
  }
}

TEST(IsNoReturnTarget, UnregisteredX86VeneersAndSectionAliasesMatchLiveLookup) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = 6;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0xff, 0x25, 0xfa, 0x1f, 0, 0}; // jmp [rip + 0x1ffa]
  Img.Segments.push_back(std::move(Text));
  Import Imp;
  Imp.Name = "abort";
  Imp.IATAddr = 0x3000;
  Img.Imports.push_back(Imp);
  Symbol Section;
  Section.Addr = 0x4000;
  Section.Name = "abort";
  Section.IsSection = true;
  Img.Symbols.push_back(Section);
  Section.Addr = 0x5000;
  Section.Name = "warn";
  Img.Symbols.push_back(Section);
  Section.IsSection = false;
  Section.Name = "abort";
  Img.Symbols.push_back(Section);
  for (bool Restricted : {false, true}) {
    if (Restricted)
      Img.LoadOnlyFunctionEntries.insert(0x1000);
    const NoReturnTargetIndex Index(Img);
    for (const auto &[Address, Expected] : {std::pair<va_t, bool>{0x1000, true},
                                            {0x4000, false},
                                            {0x5000, true}}) {
      EXPECT_EQ(isNoReturnTarget(Img, Address), Expected);
      EXPECT_EQ(Index.contains(Img, Address), Expected);
      EXPECT_EQ(Index.contains(Img, Address), Expected);
    }
  }
  // An exact IAT entry takes precedence over the decoded veneer at its VA.
  Imp.Name = "warn";
  Imp.IATAddr = 0x1000;
  Img.Imports.push_back(Imp);
  const NoReturnTargetIndex Index(Img);
  EXPECT_FALSE(isNoReturnTarget(Img, 0x1000));
  EXPECT_FALSE(Index.contains(Img, 0x1000));
}

TEST(IsNoReturnTarget, ExactIATNamesPrecedeOtherStorageBindings) {
  for (bool NoReturn : {false, true}) {
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = 0x1000;
    Data.Size = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(Data.Size);
    Img.Segments.push_back(std::move(Data));
    Img.Imports.push_back({"runtime", NoReturn ? "abort" : "warn", 0, 0x1000});
    Img.ImportPtrSlots[0x1000] = NoReturn ? "warn" : "abort";
    ASSERT_TRUE(Img.isValidImportStorageSlot(0x1000, "abort"));
    for (bool Restricted : {false, true}) {
      if (Restricted)
        Img.LoadOnlyFunctionEntries.insert(0x2000);
      const NoReturnTargetIndex Index(Img);
      EXPECT_EQ(isNoReturnTarget(Img, 0x1000), NoReturn);
      EXPECT_EQ(Index.contains(Img, 0x1000), NoReturn);
    }
  }
}

TEST(IsNoReturnTarget, NewIndexObservesChangedImageAndMismatchUsesLiveLookup) {
  BinaryImage Img, Other;
  constexpr va_t Target = 0x1234;
  Import Imp;
  Imp.Name = "abort";
  Imp.IATAddr = Target;
  Img.Imports.push_back(Imp);
  Imp.Name = "warn";
  Other.Imports.push_back(std::move(Imp));
  {
    const NoReturnTargetIndex Index(Img);
    EXPECT_TRUE(Index.contains(Img, Target));
    EXPECT_FALSE(Index.contains(Other, Target));
    Other.Imports[0].Name = "abort";
    EXPECT_TRUE(Index.contains(Other, Target));
  }
  Img.Imports[0].Name = "warn";
  const NoReturnTargetIndex Changed(Img);
  EXPECT_FALSE(Changed.contains(Img, Target));
  EXPECT_TRUE(Changed.contains(Other, Target));
  EXPECT_FALSE(Changed.contains(Other, InvalidVA));
}

TEST(IsNoReturnTarget, RestrictedLoadAnswersEachTargetOnDemand) {
  // `--func` skips the whole-image symbol walk and loads PDB names on demand.
  BinaryImage Img;
  Img.LoadOnlyFunctionEntries.insert(0x1000);
  Img.Symbols.push_back(Symbol::makeFunc(0x2000));
  Img.Symbols.back().Name = "abort";
  unsigned Lookups = 0;
  auto Resolve = [&](va_t Addr) -> std::optional<std::string> {
    ++Lookups;
    if (Addr == 0x3000)
      return std::string("__report_gsfailure");
    if (Addr == 0x4000)
      return std::string("helper");
    return std::nullopt;
  };
  const NoReturnTargetIndex Index(Img, Resolve);
  EXPECT_TRUE(Index.contains(Img, 0x2000));
  EXPECT_TRUE(Index.contains(Img, 0x3000));
  EXPECT_FALSE(Index.contains(Img, 0x4000));
  EXPECT_FALSE(Index.contains(Img, 0x5000));
  EXPECT_FALSE(Index.contains(Img, InvalidVA));
  const unsigned FirstLookups = Lookups;
  EXPECT_TRUE(Index.contains(Img, 0x3000));
  EXPECT_EQ(Lookups, FirstLookups) << "a target is resolved once";

  // Without a resolver only names already in the image count.
  const NoReturnTargetIndex Unnamed(Img);
  EXPECT_TRUE(Unnamed.contains(Img, 0x2000));
  EXPECT_FALSE(Unnamed.contains(Img, 0x3000));

  // A whole-image load has walked every symbol; the resolver is not asked.
  Img.LoadOnlyFunctionEntries.clear();
  Lookups = 0;
  const NoReturnTargetIndex Eager(Img, Resolve);
  EXPECT_TRUE(Eager.contains(Img, 0x2000));
  EXPECT_FALSE(Eager.contains(Img, 0x3000));
  EXPECT_EQ(Lookups, 0u);
}

TEST(IsNoReturnTarget, ExportsAndDeclaredPrototypesNameNoReturnRoutines) {
  // The kernel calls the ExRaiseStatus it exports, which no symbol names
  // without its PDB; the user declares AbandonRequest and PluginPanic
  // _Noreturn, which no table names.
  BinaryImage Img;
  Export Raise;
  Raise.Name = "ExRaiseStatus";
  Raise.Addr = 0x1000;
  Img.Exports.push_back(Raise);
  Export Exit;
  Exit.Name = "AbandonRequest";
  Exit.Addr = 0x2000;
  Img.Exports.push_back(Exit);
  Import Imported;
  Imported.Name = "PluginPanic";
  Imported.IATAddr = 0x3000;
  Img.Imports.push_back(Imported);
  const auto Declared = [](llvm::StringRef Name) {
    return Name == "AbandonRequest" || Name == "PluginPanic";
  };
  const NoReturnTargetIndex Tables(Img);
  EXPECT_TRUE(Tables.contains(Img, 0x1000));
  EXPECT_FALSE(Tables.contains(Img, 0x2000));
  EXPECT_FALSE(Tables.contains(Img, 0x3000));
  const NoReturnTargetIndex Prototypes(Img, {}, Declared);
  EXPECT_TRUE(Prototypes.contains(Img, 0x1000));
  EXPECT_TRUE(Prototypes.contains(Img, 0x2000));
  EXPECT_TRUE(Prototypes.contains(Img, 0x3000));
  EXPECT_TRUE(isNoReturnTarget(Img, 0x1000));
  // A restricted load answers each target the same way.
  Img.LoadOnlyFunctionEntries.insert(0x4000);
  const NoReturnTargetIndex Restricted(Img, {}, Declared);
  EXPECT_TRUE(Restricted.contains(Img, 0x1000));
  EXPECT_TRUE(Restricted.contains(Img, 0x2000));
  EXPECT_FALSE(Restricted.contains(Img, 0x4000));
}

TEST(IsReturnsTwiceFunction, SetjmpFamily) {
  EXPECT_TRUE(isReturnsTwiceFunction("vfork"));
  EXPECT_TRUE(isReturnsTwiceFunction("_vfork"));
  EXPECT_TRUE(isReturnsTwiceFunction("setjmp"));
  EXPECT_TRUE(isReturnsTwiceFunction("sigsetjmp"));
  EXPECT_TRUE(isReturnsTwiceFunction("_setjmp"));
  EXPECT_TRUE(isReturnsTwiceFunction("__sigsetjmp"));
}

TEST(IsReturnsTwiceFunction, RejectsUnrelated) {
  EXPECT_FALSE(isReturnsTwiceFunction("longjmp"));
  EXPECT_FALSE(isReturnsTwiceFunction("malloc"));
  EXPECT_FALSE(isReturnsTwiceFunction(""));
}

// A DI mode routine returns a double word on a 32-bit target, a TI mode one
// on a 64-bit target; one returning an int returns one register on either.
TEST(ReturnsDoubleWord, TakesTheTargetWord) {
  EXPECT_TRUE(returnsDoubleWord("__udivdi3", 4));
  EXPECT_FALSE(returnsDoubleWord("__udivdi3", 8));
  EXPECT_TRUE(returnsDoubleWord("__udivti3", 8));
  EXPECT_FALSE(returnsDoubleWord("__udivti3", 4));
  EXPECT_TRUE(returnsDoubleWord("__fixunsxfti", 8));
  EXPECT_TRUE(returnsDoubleWord("_aulldiv", 4));
  EXPECT_FALSE(returnsDoubleWord("__clzti2", 8));
  EXPECT_FALSE(returnsDoubleWord("__popcountti2", 8));
  EXPECT_FALSE(returnsDoubleWord("strcmp", 8));
}
