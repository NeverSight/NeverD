//===- LibCNamesTests.cpp - Unit tests for LibCNames ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/libc/LibCExceptionRuntime.h"
#include "neverd/libc/LibCFortify.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/LibCStartup.h"
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
  EXPECT_EQ(varArgFixedCount("printf"), 1u);
  EXPECT_EQ(varArgFixedCount("scanf"), 1u);
  EXPECT_EQ(varArgFixedCount("wprintf"), 1u);
  EXPECT_EQ(varArgFixedCount("wscanf"), 1u);
}

TEST(VarArgFixedCount, PrintfFamily2Fixed) {
  EXPECT_EQ(varArgFixedCount("fprintf"), 2u);
  EXPECT_EQ(varArgFixedCount("fscanf"), 2u);
  EXPECT_EQ(varArgFixedCount("sprintf"), 2u);
  EXPECT_EQ(varArgFixedCount("sscanf"), 2u);
  EXPECT_EQ(varArgFixedCount("dprintf"), 2u);
  EXPECT_EQ(varArgFixedCount("asprintf"), 2u);
  EXPECT_EQ(varArgFixedCount("fwprintf"), 2u);
  EXPECT_EQ(varArgFixedCount("fwscanf"), 2u);
  EXPECT_EQ(varArgFixedCount("swscanf"), 2u);
}

TEST(VarArgFixedCount, PrintfFamily3Fixed) {
  EXPECT_EQ(varArgFixedCount("snprintf"), 3u);
  EXPECT_EQ(varArgFixedCount("snwprintf"), 3u);
  // C99 swprintf(buf, size, fmt, ...) has same shape as snprintf
  EXPECT_EQ(varArgFixedCount("swprintf"), 3u);
}

// Fortified _FORTIFY_SOURCE variants: __<name>_chk prepend guard arguments
// (flag, and for the buffer forms the dest/object size) before the format.
// The caller strips at most one leading '_' (a Mach-O prefix), so the matcher
// must accept both "__snprintf_chk" (Mach-O) and "_snprintf_chk" (ELF
// over-strip) by normalizing every leading underscore.
TEST(VarArgFixedCount, FortifiedChkVariants) {
  // Mach-O form (one '_' already stripped from "___snprintf_chk").
  EXPECT_EQ(varArgFixedCount("__printf_chk"), 2u);   // flag, fmt
  EXPECT_EQ(varArgFixedCount("__fprintf_chk"), 3u);  // stream, flag, fmt
  EXPECT_EQ(varArgFixedCount("__dprintf_chk"), 3u);  // fd, flag, fmt
  EXPECT_EQ(varArgFixedCount("__asprintf_chk"), 3u); // &buf, flag, fmt
  EXPECT_EQ(varArgFixedCount("__sprintf_chk"), 4u);  // buf, flag, slen, fmt
  EXPECT_EQ(varArgFixedCount("__snprintf_chk"),
            5u); // buf, maxlen, flag, slen, fmt
  // ELF form (over-stripped to a single leading '_').
  EXPECT_EQ(varArgFixedCount("_snprintf_chk"), 5u);
  EXPECT_EQ(varArgFixedCount("_printf_chk"), 2u);
  // va_list _chk variants are not variadic.
  EXPECT_EQ(varArgFixedCount("__vsnprintf_chk"), 0u);
  EXPECT_EQ(varArgFixedCount("__vsprintf_chk"), 0u);
  EXPECT_EQ(varArgFixedCount("__vprintf_chk"), 0u);
  // Unknown _chk callees stay non-variadic (e.g. memcpy/strcpy fortified
  // forms).
  EXPECT_EQ(varArgFixedCount("__memcpy_chk"), 0u);
  EXPECT_EQ(varArgFixedCount("__strcpy_chk"), 0u);
}

// =====================================================================
// varArgFixedCount — POSIX whitelist
// =====================================================================

TEST(VarArgFixedCount, PosixWhitelist) {
  EXPECT_EQ(varArgFixedCount("open"), 2u);
  EXPECT_EQ(varArgFixedCount("openat"), 1u);
  EXPECT_EQ(varArgFixedCount("fcntl"), 1u);
  EXPECT_EQ(varArgFixedCount("ioctl"), 1u);
  EXPECT_EQ(varArgFixedCount("execl"), 1u);
  EXPECT_EQ(varArgFixedCount("execlp"), 1u);
  EXPECT_EQ(varArgFixedCount("execle"), 1u);
  EXPECT_EQ(varArgFixedCount("syslog"), 1u);
  EXPECT_EQ(varArgFixedCount("err"), 1u);
  EXPECT_EQ(varArgFixedCount("errx"), 1u);
  EXPECT_EQ(varArgFixedCount("warn"), 1u);
  EXPECT_EQ(varArgFixedCount("warnx"), 1u);
  EXPECT_EQ(varArgFixedCount("mq_open"), 1u);
  EXPECT_EQ(varArgFixedCount("sem_open"), 1u);
}

TEST(VarArgFixedParamKind, OpenPreservesPathAndFlags) {
  EXPECT_EQ(varArgFixedParamKind("open", 0), VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("open", 1), VarArgFixedParamKind::Integer);
  EXPECT_EQ(varArgFixedParamKind("open", 2), VarArgFixedParamKind::Unknown);
}

TEST(VarArgFixedCount, DarwinObjectiveCMessageStubs) {
  // The linker-specialized stub supplies _cmd in x1.  The external call still
  // models that register as part of the fixed prefix so method arguments start
  // at x2 and any true varargs follow the selector's colon-counted arguments.
  EXPECT_EQ(varArgFixedCount("objc_msgSend$length"), 2u);
  EXPECT_EQ(varArgFixedCount("objc_msgSend$stringWithFormat:"), 3u);
  EXPECT_EQ(varArgFixedCount("objc_msgSend$exceptionWithName:reason:userInfo:"),
            5u);
  EXPECT_EQ(varArgFixedCount("objc_msgSend$"), 0u);
}

// =====================================================================
// varArgFixedCount — non-variadic functions should return 0
// =====================================================================

TEST(VarArgFixedCount, NonVariadicReturnsZero) {
  EXPECT_EQ(varArgFixedCount("strlen"), 0u);
  EXPECT_EQ(varArgFixedCount("memcpy"), 0u);
  EXPECT_EQ(varArgFixedCount("malloc"), 0u);
  EXPECT_EQ(varArgFixedCount("free"), 0u);
  EXPECT_EQ(varArgFixedCount("exit"), 0u);
  EXPECT_EQ(varArgFixedCount("read"), 0u);
  EXPECT_EQ(varArgFixedCount("write"), 0u);
  EXPECT_EQ(varArgFixedCount("close"), 0u);
  EXPECT_EQ(varArgFixedCount("puts"), 0u);
  EXPECT_EQ(varArgFixedCount("fopen"), 0u);
}

TEST(VarArgFixedCount, AllVaListVariantsReturnZero) {
  EXPECT_EQ(varArgFixedCount("vprintf"), 0u);
  EXPECT_EQ(varArgFixedCount("vscanf"), 0u);
  EXPECT_EQ(varArgFixedCount("vfprintf"), 0u);
  EXPECT_EQ(varArgFixedCount("vfscanf"), 0u);
  EXPECT_EQ(varArgFixedCount("vsprintf"), 0u);
  EXPECT_EQ(varArgFixedCount("vsscanf"), 0u);
  EXPECT_EQ(varArgFixedCount("vsnprintf"), 0u);
  EXPECT_EQ(varArgFixedCount("vasprintf"), 0u);
  EXPECT_EQ(varArgFixedCount("vdprintf"), 0u);
  EXPECT_EQ(varArgFixedCount("vwprintf"), 0u);
  EXPECT_EQ(varArgFixedCount("vwscanf"), 0u);
  EXPECT_EQ(varArgFixedCount("vswprintf"), 0u);
  EXPECT_EQ(varArgFixedCount("vswscanf"), 0u);
  EXPECT_EQ(varArgFixedCount("vfwprintf"), 0u);
  EXPECT_EQ(varArgFixedCount("vfwscanf"), 0u);
  EXPECT_EQ(varArgFixedCount("vsnwprintf"), 0u);
}

TEST(VarArgFixedCount, EmptyAndGarbage) {
  EXPECT_EQ(varArgFixedCount(""), 0u);
  EXPECT_EQ(varArgFixedCount("x"), 0u);
  EXPECT_EQ(varArgFixedCount("__some_internal_func"), 0u);
}

TEST(VarArgFixedParamKind, PrintfFamilyPreservesScalarParameters) {
  EXPECT_EQ(varArgFixedParamKind("printf", 0), VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("dprintf", 0), VarArgFixedParamKind::Integer);
  EXPECT_EQ(varArgFixedParamKind("dprintf", 1), VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("snprintf", 0), VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("snprintf", 1), VarArgFixedParamKind::Integer);
  EXPECT_EQ(varArgFixedParamKind("snprintf", 2), VarArgFixedParamKind::Pointer);
}

TEST(VarArgFixedParamKind, OpenPreservesNamedPathAndFlags) {
  EXPECT_EQ(varArgFixedParamKind("open", 0), VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("open", 1), VarArgFixedParamKind::Integer);
  EXPECT_EQ(varArgFixedParamKind("open", 2), VarArgFixedParamKind::Unknown);
}

TEST(VarArgFixedParamKind, FortifiedSnprintfShape) {
  constexpr VarArgFixedParamKind Expected[] = {
      VarArgFixedParamKind::Pointer, VarArgFixedParamKind::Integer,
      VarArgFixedParamKind::Integer, VarArgFixedParamKind::Integer,
      VarArgFixedParamKind::Pointer};
  for (unsigned I = 0; I < 5; ++I) {
    EXPECT_EQ(varArgFixedParamKind("___snprintf_chk", I), Expected[I]);
    EXPECT_EQ(varArgFixedParamKind("snprintf_chk", I), Expected[I]);
  }
  EXPECT_EQ(varArgFixedParamKind("snprintf_chk", 5),
            VarArgFixedParamKind::Unknown);
}

TEST(VarArgFixedParamKind, UnknownSelectorArgumentsStayRecovered) {
  EXPECT_EQ(varArgFixedParamKind("objc_msgSend$stringWithFormat:", 0),
            VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("objc_msgSend$stringWithFormat:", 1),
            VarArgFixedParamKind::Pointer);
  EXPECT_EQ(varArgFixedParamKind("objc_msgSend$stringWithFormat:", 2),
            VarArgFixedParamKind::Unknown);
  EXPECT_EQ(varArgFixedParamKind("not_variadic", 0),
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
  auto BeginCatch = libcArity("cxa_begin_catch");
  ASSERT_TRUE(BeginCatch.has_value());
  EXPECT_EQ(BeginCatch->IntArgs, 1);
  EXPECT_EQ(BeginCatch->FpArgs, 0);

  auto Throw = libcArity("cxa_throw");
  ASSERT_TRUE(Throw.has_value());
  EXPECT_EQ(Throw->IntArgs, 3);
  EXPECT_EQ(Throw->FpArgs, 0);

  auto ObjCThrow = libcArity("objc_exception_throw");
  ASSERT_TRUE(ObjCThrow.has_value());
  EXPECT_EQ(ObjCThrow->IntArgs, 1);
  EXPECT_EQ(ObjCThrow->FpArgs, 0);

  auto Cookie = libcArity("security_check_cookie");
  ASSERT_TRUE(Cookie.has_value());
  EXPECT_EQ(Cookie->IntArgs, 1);
  EXPECT_EQ(Cookie->FpArgs, 0);
  EXPECT_TRUE(libcArityForSymbol("__security_check_cookie").has_value());
  EXPECT_EQ(libcArityForSymbol("__security_check_cookie")->IntArgs, 1);

  auto Raise = libcArity("raise_securityfailure");
  ASSERT_TRUE(Raise.has_value());
  EXPECT_EQ(Raise->IntArgs, 1);

  auto GetCurrentProcess = libcArity("GetCurrentProcess");
  ASSERT_TRUE(GetCurrentProcess.has_value());
  EXPECT_EQ(GetCurrentProcess->IntArgs, 0);
  EXPECT_EQ(GetCurrentProcess->FpArgs, 0);
  auto Terminate = libcArity("TerminateProcess");
  ASSERT_TRUE(Terminate.has_value());
  EXPECT_EQ(Terminate->IntArgs, 2);
  EXPECT_TRUE(libcArityForSymbol("IsProcessorFeaturePresent").has_value());
  EXPECT_EQ(libcArityForSymbol("IsProcessorFeaturePresent")->IntArgs, 1);
}

TEST(LibCArity, VaListConsumersHaveFixedPrototypes) {
  auto VPrintf = libcArity("vprintf");
  ASSERT_TRUE(VPrintf.has_value());
  EXPECT_EQ(VPrintf->IntArgs, 2);
  EXPECT_EQ(VPrintf->FpArgs, 0);

  // The va_list is one more pointer argument, after every fixed one.
  const std::pair<const char *, int> Consumers[] = {
      {"vfprintf", 3}, {"vsnprintf", 4}, {"vsprintf", 3}, {"vdprintf", 3},
      {"vscanf", 2},   {"vfscanf", 3},   {"vsscanf", 3},  {"vasprintf", 3}};
  for (const auto &[Name, IntArgs] : Consumers) {
    const auto Arity = libcArity(Name);
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
    const auto Arity = libcArityForSymbol(Name);
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
    EXPECT_FALSE(libcArityForSymbol(Name).has_value()) << Name;
  // So does a routine the Windows C runtimes declare otherwise, since the
  // tables apply to every format by name: _mkdir(dir), Win64's
  // _setjmp(env, frame), msvcrt's wcstok(str, delim).
  for (const char *Name : {"mkdir", "setjmp", "_setjmp", "wcstok"})
    EXPECT_FALSE(libcArityForSymbol(Name).has_value()) << Name;
}

TEST(LibCArity, PreservesDarwinErrorSymbolSpelling) {
  auto DarwinError = libcArityForSymbol("___error");
  ASSERT_TRUE(DarwinError.has_value());
  EXPECT_EQ(DarwinError->IntArgs, 0);
  EXPECT_EQ(DarwinError->FpArgs, 0);

  EXPECT_FALSE(libcArityForSymbol("error").has_value());
  EXPECT_FALSE(libcArityForSymbol("__error").has_value());
}

TEST(LibCArity, TranslationRoutines) {
  // sed's `_("unmatched `{'")` is dcgettext(NULL, msgid, LC_MESSAGES).  An
  // unbounded scan took two more live registers as arguments.
  const std::pair<const char *, int> Expected[] = {
      {"gettext", 1},     {"dgettext", 2},       {"dcgettext", 3},
      {"__dcgettext", 3}, {"ngettext", 3},       {"dngettext", 4},
      {"dcngettext", 5},  {"bindtextdomain", 2}, {"textdomain", 1}};
  for (const auto &[Name, IntArgs] : Expected) {
    const auto Arity = libcArityForSymbol(Name);
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
    EXPECT_EQ(varArgFixedCount(Prototype.Name),
              Prototype.Variadic ? Prototype.ParamCount : 0u);
  }
}

/// A table whose arities the registry derives.
template <size_t N>
void expectDerivedArity(const std::array<LibCPrototype, N> &Prototypes) {
  for (const LibCPrototype &Prototype : Prototypes) {
    SCOPED_TRACE(std::string(Prototype.Name));
    const auto Arity = libcArityForSymbol(Prototype.Name);
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
  EXPECT_EQ(libcArity("cxa_throw")->IntArgs, 3);
  EXPECT_EQ(libcArity("cxa_begin_catch")->IntArgs, 1);
  EXPECT_EQ(libcArity("Unwind_ForcedUnwind")->IntArgs, 3);
  EXPECT_EQ(libcArity("libc_start_main")->IntArgs, 7);
}

TEST(LibCPrototype, WindowsRuntimeNamesStayInPE) {
  for (const LibCPrototype &Prototype : kWindowsCRTPrototypes) {
    SCOPED_TRACE(std::string(Prototype.Name));
    EXPECT_EQ(libcPrototype(Prototype.Name, BinaryFormat::ELF), nullptr);
    EXPECT_EQ(libcPrototype(Prototype.Name, BinaryFormat::MachO), nullptr);
    // Without underscores _lock is any other runtime's lock: only the
    // Windows API routines the exception tables already name have a
    // portable arity, and it is theirs.
    if (const auto Arity = libcArityForSymbol(Prototype.Name))
      EXPECT_EQ(Arity->IntArgs, Prototype.ParamCount);
  }
  EXPECT_FALSE(libcArityForSymbol("_lock").has_value());
  EXPECT_FALSE(libcArityForSymbol("terminate").has_value());
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
  EXPECT_EQ(libcReturnsValue("calloc", BinaryFormat::ELF), true);
  EXPECT_EQ(libcReturnsValue("atexit", BinaryFormat::COFF), true);
  EXPECT_EQ(libcReturnsValue("free", BinaryFormat::COFF), false);
  EXPECT_EQ(libcReturnsValue("qsort", BinaryFormat::MachO), false);
  // A function returning a pointer to a function returns a value.
  EXPECT_EQ(libcReturnsValue("signal", BinaryFormat::ELF), true);
  // A prototype's result, in the image format it applies to.
  EXPECT_EQ(libcReturnsValue("_strtoi64", BinaryFormat::COFF), true);
  EXPECT_EQ(libcReturnsValue("_initterm", BinaryFormat::COFF), false);
  EXPECT_FALSE(libcReturnsValue("_strtoi64", BinaryFormat::ELF).has_value());
  EXPECT_EQ(libcReturnsValue("__libc_start_main", BinaryFormat::ELF), true);
  // Neither declares it.
  EXPECT_FALSE(libcReturnsValue("neverd_unknown_routine", BinaryFormat::ELF)
                   .has_value());
}

TEST(LibCReturnsValue, TheGeneratedTableNamesOnlyKnownRoutines) {
#define NEVERD_LIBC_RETURN_KIND(NAME, KIND) EXPECT_TRUE(isKnownFunction(NAME));
#include "neverd/libc/LibCReturnKinds.inc"
#undef NEVERD_LIBC_RETURN_KIND
}

TEST(VarArgFixedCount, IsoAliasesTakeTheStandardRoutinesArguments) {
  EXPECT_EQ(varArgFixedCount("__isoc99_scanf"), 1u);
  EXPECT_EQ(varArgFixedCount("__isoc99_sscanf"), 2u);
  EXPECT_EQ(varArgFixedCount("__isoc23_fscanf"), 2u);
  EXPECT_EQ(varArgFixedCount("__isoc99_vscanf"), 0u);
  EXPECT_EQ(varArgFixedCount("__syslog_chk"), 3u);
  EXPECT_TRUE(isVaListConsumer("isoc99_vsscanf"));
  EXPECT_TRUE(isVaListConsumer("__isoc23_vfscanf"));
  EXPECT_FALSE(isVaListConsumer("isoc99_sscanf"));
  EXPECT_EQ(varArgFixedParamKind("__isoc99_fscanf", 0),
            VarArgFixedParamKind::Pointer);
  // The Universal CRT's printf implementation takes a va_list.
  EXPECT_EQ(varArgFixedCount("__stdio_common_vfprintf"), 0u);
  EXPECT_EQ(varArgFixedCount("__stdio_common_vsscanf"), 0u);
  EXPECT_TRUE(isVaListConsumer("stdio_common_vfprintf"));
}

TEST(ObjectPointerParameter, HeadersSayWhichParametersTakePointers) {
  EXPECT_TRUE(isObjectPointerParameter("memcpy", 0));
  EXPECT_TRUE(isObjectPointerParameter("memcpy", 1));
  EXPECT_FALSE(isObjectPointerParameter("memcpy", 2));
  EXPECT_TRUE(isObjectPointerParameter("__memcpy", 1));
  EXPECT_TRUE(isObjectPointerParameter("fputs", 1));
  EXPECT_TRUE(isObjectPointerParameter("strtol", 1));
  EXPECT_TRUE(isObjectPointerParameter("pthread_create", 3));
  // A function pointer and a va_list have their own rules.
  EXPECT_FALSE(isObjectPointerParameter("qsort", 3));
  EXPECT_FALSE(isObjectPointerParameter("pthread_create", 2));
  EXPECT_FALSE(isObjectPointerParameter("signal", 1));
  EXPECT_TRUE(isObjectPointerParameter("vprintf", 0));
  EXPECT_FALSE(isObjectPointerParameter("vprintf", 1));
  EXPECT_FALSE(isObjectPointerParameter("not_a_libc_function", 0));
}

TEST(FunctionPointerParameter, StandardCallbacks) {
  EXPECT_EQ(functionPointerParameter("qsort", 3),
            "int (*)(const void *, const void *)");
  EXPECT_EQ(functionPointerParameter("_qsort", 3),
            "int (*)(const void *, const void *)");
  EXPECT_EQ(functionPointerParameter("signal", 1), "void (*)(int)");
  EXPECT_EQ(functionPointerParameter("pthread_create", 2), "void *(*)(void *)");
  EXPECT_FALSE(functionPointerParameter("qsort", 0).has_value());
  EXPECT_FALSE(functionPointerParameter("printf", 0).has_value());
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

TEST(IsNoReturnFunction, MsvcGsHelpers) {
  EXPECT_TRUE(isNoReturnFunction("report_gsfailure"));
  EXPECT_TRUE(isNoReturnFunction("_report_gsfailure"));
  EXPECT_TRUE(isNoReturnFunction("raise_securityfailure"));
  EXPECT_TRUE(isNoReturnFunction("_raise_securityfailure"));
  EXPECT_TRUE(isNoReturnFunction("TerminateProcess"));
}

TEST(IsNoReturnFunction, MicrosoftRuntimeTerminators) {
  EXPECT_TRUE(isNoReturnFunction("_amsg_exit"));
  EXPECT_TRUE(isNoReturnFunction("_invoke_watson"));
  EXPECT_TRUE(isNoReturnFunction("_invalid_parameter_noinfo_noreturn"));
  EXPECT_TRUE(isNoReturnFunction("__std_terminate"));
  // A program's own function may take the name terminate.
  EXPECT_FALSE(isNoReturnFunction("terminate"));
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
