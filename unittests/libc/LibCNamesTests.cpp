//===- LibCNamesTests.cpp - Unit tests for LibCNames ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include <gtest/gtest.h>

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
  EXPECT_EQ(varArgFixedCount("__snprintf_chk"), 5u); // buf, maxlen, flag, slen, fmt
  // ELF form (over-stripped to a single leading '_').
  EXPECT_EQ(varArgFixedCount("_snprintf_chk"), 5u);
  EXPECT_EQ(varArgFixedCount("_printf_chk"), 2u);
  // va_list _chk variants are not variadic.
  EXPECT_EQ(varArgFixedCount("__vsnprintf_chk"), 0u);
  EXPECT_EQ(varArgFixedCount("__vsprintf_chk"), 0u);
  EXPECT_EQ(varArgFixedCount("__vprintf_chk"), 0u);
  // Unknown _chk callees stay non-variadic (e.g. memcpy/strcpy fortified forms).
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

TEST(VarArgFixedCount, DarwinObjectiveCMessageStubs) {
  // The linker-specialized stub supplies _cmd in x1.  The external call still
  // models that register as part of the fixed prefix so method arguments start
  // at x2 and any true varargs follow the selector's colon-counted arguments.
  EXPECT_EQ(varArgFixedCount("objc_msgSend$length"), 2u);
  EXPECT_EQ(varArgFixedCount("objc_msgSend$stringWithFormat:"), 3u);
  EXPECT_EQ(
      varArgFixedCount("objc_msgSend$exceptionWithName:reason:userInfo:"),
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
}

TEST(LibCArity, PreservesDarwinErrorSymbolSpelling) {
  auto DarwinError = libcArityForSymbol("___error");
  ASSERT_TRUE(DarwinError.has_value());
  EXPECT_EQ(DarwinError->IntArgs, 0);
  EXPECT_EQ(DarwinError->FpArgs, 0);

  EXPECT_FALSE(libcArityForSymbol("error").has_value());
  EXPECT_FALSE(libcArityForSymbol("__error").has_value());
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

TEST(HeaderFor, StdioFunctions) {
  EXPECT_STREQ(headerFor("printf"), "stdio.h");
  EXPECT_STREQ(headerFor("fprintf"), "stdio.h");
  EXPECT_STREQ(headerFor("snprintf"), "stdio.h");
}

TEST(HeaderFor, UnknownReturnsNull) {
  EXPECT_EQ(headerFor("not_a_real_function"), nullptr);
  EXPECT_EQ(headerFor(""), nullptr);
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

TEST(IsNoReturnFunction, ItaniumCxxTerminators) {
  EXPECT_TRUE(isNoReturnFunction("_ZSt9terminatev"));
  EXPECT_TRUE(isNoReturnFunction("__cxa_call_terminate"));
  EXPECT_TRUE(isNoReturnFunction("__clang_call_terminate"));
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

TEST(IsReturnsTwiceFunction, SetjmpFamily) {
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
