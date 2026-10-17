//===- CSymbolNameTests.cpp - The C names emitted for symbols -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/ADT/Twine.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <tuple>

#ifndef NEVERD_RUNTIME_FIXTURE_COMPILER
#define NEVERD_RUNTIME_FIXTURE_COMPILER ""
#endif
#ifndef NEVERD_TEST_CLANG
#define NEVERD_TEST_CLANG ""
#endif

using namespace neverd;

namespace {

constexpr auto NotFound = std::string::npos;

HighStmt callStatement(const std::string &Target, va_t Address,
                       std::vector<ExprPtr> Arguments = {}) {
  HighStmt Call;
  Call.Kind = StmtKind::Call;
  Call.CallExpr = HighExpr::makeCall(Target, Address, std::move(Arguments));
  return Call;
}

HighStmt returnZero() {
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(0, 8);
  return Return;
}

HighFunc function(const char *Name, va_t Entry, std::vector<HighStmt> Body) {
  HighFunc F;
  F.Name = Name;
  F.Entry = Entry;
  F.ReturnType = NdType::makeInt(8, false);
  F.Body = std::move(Body);
  F.Body.push_back(returnZero());
  return F;
}

std::string emitHighC(const std::vector<HighFunc> &Funcs, BinaryFormat Format,
                      Arch Target) {
  CEmitterOptions Options;
  Options.TheArch = Target;
  Options.Format = Format;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  EXPECT_TRUE(HighCEmitter().emit(Funcs, OS, Options));
  return Text;
}

/// Foo::run() calls two constructors of Bar, operator delete and a C
/// function whose own name starts with underscores.
std::vector<HighFunc> cxxCaller(const char *Prefix) {
  const std::string P = Prefix;
  return {function(
      (P + "_ZN3Foo3runEv").c_str(), 0x1000,
      {callStatement(P + "_ZN3BarC1Ev", 0x2000),
       callStatement(P + "_ZN3BarC2Ei", 0x2010, {HighExpr::makeConst(7, 4)}),
       callStatement(P + "_ZdlPv", 0x2020, {HighExpr::makeConst(0, 8)}),
       callStatement(P + "__nd_probe_hook", 0x2030)})};
}

TEST(CSymbolNames, ABIUnderscoreOnlyWhereTheFormatAddsOne) {
  EXPECT_TRUE(hasABIUnderscore(BinaryFormat::MachO, Arch::AArch64));
  EXPECT_TRUE(hasABIUnderscore(BinaryFormat::COFF, Arch::X86));
  EXPECT_FALSE(hasABIUnderscore(BinaryFormat::COFF, Arch::X64));
  EXPECT_FALSE(hasABIUnderscore(BinaryFormat::ELF, Arch::X64));
  EXPECT_EQ(cNameOfSymbol("_printf", BinaryFormat::MachO, Arch::X64), "printf");
  EXPECT_EQ(cNameOfSymbol("__libc_start_main", BinaryFormat::ELF, Arch::X64),
            "__libc_start_main");
  EXPECT_EQ(symbolOfCName("_ZdlPv", BinaryFormat::MachO, Arch::AArch64),
            "__ZdlPv");
  EXPECT_EQ(symbolOfCName("_ZdlPv", BinaryFormat::ELF, Arch::X64), "_ZdlPv");
}

TEST(CSymbolNames, StdcallNamesSpellTheBytesTheirRoutinesPop) {
  // An import library's thunk and its slot, as MinGW links
  // NlsValidateLocale(lcid, flags).
  const auto Bytes = [](llvm::StringRef Symbol) {
    return stdcallArgumentBytes(Symbol, BinaryFormat::COFF, Arch::X86);
  };
  EXPECT_EQ(Bytes("_NlsValidateLocale@8"), 8u);
  EXPECT_EQ(Bytes("__imp__NlsValidateLocale@8"), 8u);
  EXPECT_EQ(Bytes("_GetLastError@0"), 0u);
  // cdecl and fastcall names, a C++ one, a GCC clone and stray `@`s.
  EXPECT_EQ(Bytes("_printf"), std::nullopt);
  EXPECT_EQ(Bytes("@InterlockedIncrement@4"), std::nullopt);
  EXPECT_EQ(Bytes("?f@@YGXH@Z"), std::nullopt);
  EXPECT_EQ(Bytes("_RtlNtStatusToDosErrorNoTeb@4.part.0"), std::nullopt);
  EXPECT_EQ(Bytes("_Name@"), std::nullopt);
  EXPECT_EQ(Bytes("_@8"), std::nullopt);
  // Only 32-bit Windows decorates stdcall names.
  EXPECT_EQ(stdcallArgumentBytes("_Sleep@4", BinaryFormat::COFF, Arch::X64),
            std::nullopt);
  EXPECT_EQ(stdcallArgumentBytes("_Sleep@4", BinaryFormat::ELF, Arch::X86),
            std::nullopt);
}

TEST(CSymbolNames, ElfCxxCallsReadByScopeAndLinkByMangledName) {
  const std::string Source =
      emitHighC(cxxCaller(""), BinaryFormat::ELF, Arch::X64);
  // The definition reads by its scopes and names its signature above.
  EXPECT_NE(Source.find("/* Foo::run() */\n"), NotFound) << Source;
  EXPECT_NE(Source.find(" Foo_run("), NotFound) << Source;
  // Both constructors stay distinct, each linking by its own symbol.
  EXPECT_NE(
      Source.find("Bar_ctor() __asm__(\"_ZN3BarC1Ev\"); /* Bar::Bar() */"),
      NotFound)
      << Source;
  EXPECT_NE(
      Source.find("Bar_ctor_2() __asm__(\"_ZN3BarC2Ei\"); /* Bar::Bar(int) */"),
      NotFound)
      << Source;
  EXPECT_NE(Source.find("operator_delete() __asm__(\"_ZdlPv\"); /* operator "
                        "delete(void*) */"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("Bar_ctor();"), NotFound) << Source;
  EXPECT_NE(Source.find("Bar_ctor_2(7);"), NotFound) << Source;
  EXPECT_NE(Source.find("operator_delete(0);"), NotFound) << Source;
  // An ELF symbol's underscores are part of its name.
  EXPECT_NE(Source.find("extern int __nd_probe_hook();"), NotFound) << Source;
  EXPECT_NE(Source.find("__nd_probe_hook();"), NotFound) << Source;
  // MSVC's member rules do not describe Itanium constructors.
  EXPECT_EQ(Source.find("__fastcall"), NotFound) << Source;
}

TEST(CSymbolNames, RenamedNoreturnImportsStillNeverReturn) {
  // std::__throw_bad_alloc() reads by its scopes, but its call still ends the
  // path; the declaration must say so, or C falls through.
  const std::string Source =
      emitHighC({function("_ZN3Foo3runEv", 0x1000,
                          {callStatement("_ZSt17__throw_bad_allocv", 0x2000)})},
                BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find(" std___throw_bad_alloc() "
                        "__asm__(\"_ZSt17__throw_bad_allocv\") "
                        "__attribute__((noreturn));"),
            NotFound)
      << Source;
}

TEST(CSymbolNames, KnownAritiesFollowTheSymbol) {
  // The runtime tables know `_Unwind_Resume` and `_ZSt9terminatev` by their
  // symbols, which their C names keep or read by scope; declarations and
  // calls take the tables' prototype or arity either way.
  const std::string Source =
      emitHighC({function("resume", 0x1000,
                          {callStatement("_Unwind_Resume", 0x2000,
                                         {HighExpr::makeConst(1, 8),
                                          HighExpr::makeConst(2, 8)})}),
                 function("terminate", 0x1100,
                          {callStatement("_ZSt9terminatev", 0x2010,
                                         {HighExpr::makeConst(3, 8)})})},
                BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find("extern void _Unwind_Resume(void *)"), NotFound)
      << Source;
  EXPECT_NE(Source.find("_Unwind_Resume((void *)1);"), NotFound) << Source;
  EXPECT_NE(Source.find(
                "extern int std_terminate(void) __asm__(\"_ZSt9terminatev\")"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("std_terminate();"), NotFound) << Source;
}

TEST(CSymbolNames, KnownArgumentsFillOneRegisterEach) {
  // MSVC's ARM compiler calls __intrinsic_setjmpex with the buffer and the
  // frame, and longjmp takes the buffer and the value everywhere: the
  // registers after them are not arguments, and on a 32-bit target each
  // argument is one 32-bit register.
  const std::string Source = emitHighC(
      {function(
          "jumps", 0x1000,
          {callStatement("__intrinsic_setjmpex", 0x2000,
                         {HighExpr::makeConst(1, 4), HighExpr::makeConst(2, 4),
                          HighExpr::makeConst(3, 4)}),
           callStatement("longjmp", 0x2010,
                         {HighExpr::makeConst(1, 4), HighExpr::makeConst(4, 4),
                          HighExpr::makeConst(5, 4),
                          HighExpr::makeConst(6, 4)})})},
      BinaryFormat::COFF, Arch::ARM);
  EXPECT_NE(Source.find("extern int __intrinsic_setjmpex(int32_t, int32_t);"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("__intrinsic_setjmpex(1, 2);"), NotFound) << Source;
  EXPECT_NE(Source.find("longjmp((void *)(uintptr_t)1, 4);"), NotFound)
      << Source;
}

TEST(CSymbolNames, LibraryPointerParametersTakeConvertedIntegers) {
  // C converts an integer to none of fputs's pointer parameters, nor a
  // pointer to an integer parameter: those values pass through casts, and
  // the integer argument of strncmp as it is.
  auto Param = [](int Id) {
    MedVar Var;
    Var.Kind = MedVar::Param;
    Var.Id = Id;
    Var.Size = 8;
    return HighExpr::makeVar(Var, NdType::makeInt(8));
  };
  HighFunc Report = function(
      "report", 0x1000,
      {callStatement("fputs", 0x2000, {Param(0), Param(1)}),
       callStatement("strncmp", 0x2010, {Param(0), Param(1), Param(2)}),
       callStatement("sink", 0x3000, {Param(3)})});
  Report.Params = {{"arg0", NdType::makeInt(8)},
                   {"arg1", NdType::makeInt(8)},
                   {"arg2", NdType::makeInt(8)},
                   {"arg3", NdType::makePtr()}};
  HighFunc Sink = function("sink", 0x3000, {});
  Sink.Params = {{"arg0", NdType::makeInt(8)}};
  const std::string Source =
      emitHighC({Report, Sink}, BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find("fputs((void *)(uintptr_t)arg0, "
                        "(void *)(uintptr_t)arg1);"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("strncmp((void *)(uintptr_t)arg0, "
                        "(void *)(uintptr_t)arg1, arg2);"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("sink((uintptr_t)arg3);"), NotFound) << Source;
}

TEST(CSymbolNames, TheEntryStackPointerIsAValue) {
  // `_start` passes the stack pointer it was entered with to
  // __libc_start_main as stack_end.  No statement assigns frame_base, and it
  // is a known value all the same.
  MedVar EntrySP;
  EntrySP.Kind = MedVar::Reg;
  EntrySP.TheArch = Arch::X64;
  EntrySP.Size = 8;
  EntrySP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  // The value reaches the call through a copy, as `push rsp` leaves it.
  MedVar StackEnd;
  StackEnd.Kind = MedVar::Temp;
  StackEnd.TheArch = Arch::X64;
  StackEnd.Id = 7;
  StackEnd.SSAVer = 1;
  StackEnd.Size = 8;
  StackEnd.RenameTag = 0;
  const TypeRef U64 = NdType::makeInt(8, false);
  HighStmt Copy;
  Copy.Kind = StmtKind::Assign;
  Copy.Dst = HighExpr::makeVar(StackEnd, U64);
  Copy.Val = HighExpr::makeVar(EntrySP, U64);
  std::vector<ExprPtr> Args;
  for (uint64_t I = 0; I < 6; ++I)
    Args.push_back(HighExpr::makeConst(I, 8));
  Args.push_back(HighExpr::makeVar(StackEnd, U64));
  HighFunc Start =
      function("_start", 0x1000,
               {Copy, callStatement("__libc_start_main", 0x2000, Args)});
  Start.FrameSize = 16;
  const std::string Source = emitHighC({Start}, BinaryFormat::ELF, Arch::X64);
  // Its prototype's pointer parameters take the values through casts.
  EXPECT_NE(
      Source.find("__libc_start_main(0, 1, (char **)2, (void (*)(void))3, "
                  "(void (*)(void))4, (void (*)(void))5, (void *)"),
      NotFound)
      << Source;
  EXPECT_EQ(Source.find("unknown"), NotFound) << Source;
}

TEST(CSymbolNames, UndeterminedKnownArgumentsAreUnknown) {
  // A catch fragment whose incoming exception object the lift does not
  // track still passes it: the call takes the declared argument, unknown.
  const std::string Source =
      emitHighC({function("catch_fragment", 0x1000,
                          {callStatement("__cxa_begin_catch", 0x2000)})},
                BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find("extern void *__cxa_begin_catch(void *);"), NotFound)
      << Source;
  EXPECT_NE(
      Source.find("__cxa_begin_catch((void *)(uintptr_t)(__builtin_trap(), "
                  "0 /* unknown value */));"),
      NotFound)
      << Source;
}

TEST(CSymbolNames, MachOLabelsCarryTheDecoration) {
  const std::string Source =
      emitHighC(cxxCaller("_"), BinaryFormat::MachO, Arch::AArch64);
  EXPECT_NE(Source.find(" Foo_run("), NotFound) << Source;
  EXPECT_NE(Source.find("Bar_ctor() __asm__(\"__ZN3BarC1Ev\");"), NotFound)
      << Source;
  // The C name of `___nd_probe_hook` is `__nd_probe_hook`; the compiler adds
  // the decoration back, so it needs no label.
  EXPECT_NE(Source.find("extern int __nd_probe_hook();"), NotFound) << Source;
}

/// The line after the first line of \p Source that is \p Line.
std::string lineAfter(llvm::StringRef Source, llvm::StringRef Line) {
  const size_t At = Source.find((Line + "\n").str());
  if (At == llvm::StringRef::npos)
    return {};
  return Source.substr(At + Line.size() + 1).split('\n').first.str();
}

TEST(CSymbolNames, DefinitionsStepAsideFromTheCRuntime) {
  const std::string Source = emitHighC(
      {function("_start", 0x1000, {callStatement("_helper", 0x1100)}),
       function("_helper", 0x1100, {}), function("_init", 0x1200, {})},
      BinaryFormat::ELF, Arch::X64);
  // glibc's start files define _start and _init themselves.
  EXPECT_NE(Source.find(" start("), NotFound) << Source;
  EXPECT_NE(Source.find(" init("), NotFound) << Source;
  EXPECT_EQ(Source.find(" _start("), NotFound) << Source;
  // Each names its symbol above its definition, as the listing names it.
  EXPECT_NE(lineAfter(Source, "/* _start */").find(" start("), NotFound)
      << Source;
  EXPECT_NE(lineAfter(Source, "/* _init */").find(" init("), NotFound)
      << Source;
  // Other definitions keep their names, which need no comment.
  EXPECT_NE(Source.find(" _helper("), NotFound) << Source;
  EXPECT_NE(Source.find("_helper();"), NotFound) << Source;
  EXPECT_EQ(Source.find("/* _helper */"), NotFound) << Source;
}

TEST(CSymbolNames, LLVMCDefinitionsStepAsideFromTheCRuntime) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
define void @_fini() {
  ret void
}
define i64 @_helper() {
  ret i64 0
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
  EXPECT_NE(lineAfter(Source, "/* _fini */").find(" fini("), NotFound)
      << Source;
  EXPECT_NE(Source.find(" _helper("), NotFound) << Source;
  EXPECT_EQ(Source.find("/* _helper */"), NotFound) << Source;
}

TEST(CSymbolNames, ANumberedDefinitionNamesItsSymbolAndItsCallsReachIt) {
  // Two files of a program each define a static helper; main calls the
  // second.
  const std::string Source =
      emitHighC({function("helper", 0x1100, {}), function("helper", 0x1200, {}),
                 function("main", 0x1300, {callStatement("helper", 0x1200)})},
                BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(lineAfter(Source, "/* helper */").find(" helper_2("), NotFound)
      << Source;
  EXPECT_NE(Source.find("helper_2();"), NotFound) << Source;
  // No other function stands for the second.
  EXPECT_EQ(Source.find("helper_1200"), NotFound) << Source;
}

TEST(CSymbolNames, LLVMCDeclaresCxxImportsByMangledName) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
declare i64 @_ZN3BarC1Ev(i64)
define i64 @_ZN3Foo3runEv(i64 %this) {
  %r = call i64 @_ZN3BarC1Ev(i64 %this)
  ret i64 %r
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
  EXPECT_NE(
      Source.find(
          "Bar_ctor(uint64_t) __asm__(\"_ZN3BarC1Ev\"); /* Bar::Bar() */"),
      NotFound)
      << Source;
  EXPECT_NE(Source.find("/* Foo::run() */\n"), NotFound) << Source;
  EXPECT_NE(Source.find(" Foo_run("), NotFound) << Source;
}

TEST(CSymbolNames, LLVMCNamesExternalSymbolsAsTheyLink) {
  // An ELF image's GOT mirror references its imports as data, and the code
  // calls them through those declarations; their underscores are part of
  // their names.
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
@__cxa_atexit = external global i8
@slots = global [1 x i64] [i64 ptrtoint (ptr @__cxa_atexit to i64)]
define i64 @install(i64 %handler) {
  %r = call i64 @__cxa_atexit(i64 %handler, i64 0, i64 0)
  ret i64 %r
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
  EXPECT_NE(Source.find("__cxa_atexit(handler, 0, 0)"), NotFound) << Source;
  // The slot holds the symbol's address, whichever kind of object it names.
  EXPECT_NE(Source.find("(void*)&__cxa_atexit"), NotFound) << Source;
  EXPECT_EQ(Source.find("_cxa_atexit"), Source.find("__cxa_atexit") + 1)
      << Source;
}

TEST(CSymbolNames, ElfCxxLabelsLinkToTheMangledDefinitions) {
#if !defined(__linux__)
  GTEST_SKIP() << "links an ELF program on the host";
#else
  if (llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty())
    GTEST_SKIP() << "needs a GNU-style host C compiler";
  // The stubs define the symbols by their mangled names, as a C++ library
  // would; the emitted C must reach each one through its label.
  const std::string Stubs = R"(
#include <stdint.h>
unsigned called;
int64_t Foo_run(void);
void _ZN3BarC1Ev(void) { called |= 1; }
void _ZN3BarC2Ei(int value) { if (value == 7) called |= 2; }
void _ZdlPv(void *pointer) { if (!pointer) called |= 4; }
void __nd_probe_hook(void) { called |= 8; }
int main(void) { Foo_run(); return called == 15 ? 0 : 1; }
)";
  const std::string Source =
      emitHighC(cxxCaller(""), BinaryFormat::ELF, Arch::X64);
  llvm::SmallString<128> SourcePath, StubPath, BinaryPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-c-names", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-c-names-stubs", "c",
                                                  StubPath));
  llvm::FileRemover RemoveStubs(StubPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-c-names", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  for (const auto &[Path, Text] : {std::pair{SourcePath.str(), Source},
                                   std::pair{StubPath.str(), Stubs}}) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Out << Text;
  }
  std::string Message;
  const int Compiled =
      llvm::sys::ExecuteAndWait(NEVERD_RUNTIME_FIXTURE_COMPILER,
                                {NEVERD_RUNTIME_FIXTURE_COMPILER, "-std=c11",
                                 "-w", SourcePath, StubPath, "-o", BinaryPath},
                                std::nullopt, {}, 60, 0, &Message);
  ASSERT_EQ(Compiled, 0) << Message << '\n' << Source;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                      {}, 30, 0, &Message),
            0)
      << Message << '\n'
      << Source;
#endif
}

/// An executable image whose text holds `main` at 0x1150 and a handler.
BinaryImage startupImage() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x200;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x200, 0xC3);
  Img.Segments.push_back(std::move(Text));
  for (const auto &[Name, Addr] :
       {std::pair<const char *, va_t>{"main", 0x1150},
        {"on_exit_hook", 0x1180}}) {
    Symbol Sym;
    Sym.Name = Name;
    Sym.Addr = Addr;
    Sym.IsFunc = true;
    Img.Symbols.push_back(std::move(Sym));
  }
  return Img;
}

std::string emitWithImage(const std::vector<HighFunc> &Funcs,
                          const BinaryImage &Img) {
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.Image = &Img;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  EXPECT_TRUE(HighCEmitter().emit(Funcs, OS, Options));
  return Text;
}

ExprPtr address(va_t Addr) {
  return HighExpr::makeConst(Addr, 8, ConstantAddressProvenance::Address);
}

TEST(CSymbolNames, StartupPassesMainThroughItsPrototype) {
  const BinaryImage Img = startupImage();
  std::vector<ExprPtr> Args{address(0x1150),
                            HighExpr::makeConst(1, 8),
                            HighExpr::makeConst(0x7000, 8),
                            HighExpr::makeConst(0, 8),
                            HighExpr::makeConst(0, 8),
                            HighExpr::makeConst(0, 8),
                            HighExpr::makeConst(0, 8)};
  const std::string Source = emitWithImage(
      {function("_start", 0x1000,
                {callStatement("__libc_start_main", 0x2000, Args)})},
      Img);
  // The routine no header declares has its C library prototype, and the
  // address of main reads as main, converted to the parameter's type.
  EXPECT_NE(Source.find("extern int __libc_start_main(int (*)(int, char **, "
                        "char **), int, char **, void (*)(void), void "
                        "(*)(void), void (*)(void), void *)"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("extern int main();"), NotFound) << Source;
  EXPECT_NE(Source.find("__libc_start_main((int (*)(int, char **, char "
                        "**))main, 1, (char **)0x7000, 0, 0, 0, 0);"),
            NotFound)
      << Source;
  EXPECT_EQ(Source.find("0x1150"), NotFound) << Source;
  source_call_execution_test::compileAndRun(
      Source + "int main(void) { return 0; }\n",
      {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}

TEST(CSymbolNames, FunctionAddressesReadAsTheirFunctions) {
  const BinaryImage Img = startupImage();
  // An unprototyped callee takes the function's pointer as it is; a number
  // equal to its address stays a number.
  std::vector<HighFunc> Funcs{function(
      "install", 0x1000,
      {callStatement("register_hook", 0x2000, {address(0x1180)}),
       callStatement("register_hook", 0x2000,
                     {HighExpr::makeConst(
                         0x1180, 8, ConstantAddressProvenance::Scalar)})})};
  const std::string Source = emitWithImage(Funcs, Img);
  EXPECT_NE(Source.find("extern int on_exit_hook();"), NotFound) << Source;
  EXPECT_NE(Source.find("register_hook(on_exit_hook);"), NotFound) << Source;
  EXPECT_NE(Source.find("register_hook(0x1180);"), NotFound) << Source;
}

TEST(CSymbolNames, AFunctionWhoseAddressIsTakenIsDeclaredBeforeTheUse) {
  const BinaryImage Img = startupImage();
  // install passes on_exit_hook, whose body prints after it.
  const std::string Source = emitWithImage(
      {function("install", 0x1000,
                {callStatement("register_hook", 0x2000, {address(0x1180)})}),
       function("on_exit_hook", 0x1180, {})},
      Img);
  const size_t Declared = Source.find(" on_exit_hook(void);");
  const size_t Used = Source.find("register_hook(on_exit_hook);");
  ASSERT_NE(Declared, NotFound) << Source;
  ASSERT_NE(Used, NotFound) << Source;
  EXPECT_LT(Declared, Used) << Source;
  EXPECT_EQ(Source.find("extern int on_exit_hook"), NotFound) << Source;
  source_call_execution_test::compileAndRun(
      Source + "int register_hook(void *Hook) { return Hook == 0; }\n"
               "int main(void) { return (int)install(); }\n",
      {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}

TEST(CSymbolNames, PosixRoutinesTheArityTablesNameTakeTheirHeaders) {
  // Declared `extern int`, popen lost the upper half of its FILE * and
  // drand48 read its double from the integer register.
  const std::string Source =
      emitHighC({function("run", 0x1000,
                          {callStatement("popen", 0x2000,
                                         {HighExpr::makeConst(0, 8),
                                          HighExpr::makeConst(0, 8)}),
                           callStatement("drand48", 0x2010),
                           callStatement("srandom", 0x2020,
                                         {HighExpr::makeConst(1, 4)})})},
                BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find("#include <stdio.h>"), NotFound) << Source;
  EXPECT_NE(Source.find("#include <stdlib.h>"), NotFound) << Source;
  EXPECT_EQ(Source.find("extern int popen"), NotFound) << Source;
  EXPECT_EQ(Source.find("extern int drand48"), NotFound) << Source;
  EXPECT_EQ(Source.find("extern int srandom"), NotFound) << Source;
}

/// A function returning the result of the call \p Call, an integer.
HighFunc returnsCall(const char *Name, va_t Entry, ExprPtr Call) {
  Call->Type = NdType::makeInt(8, false);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Call);
  HighFunc F;
  F.Name = Name;
  F.Entry = Entry;
  F.ReturnType = NdType::makeInt(8, false);
  F.Body.push_back(std::move(Return));
  return F;
}

std::string emitFor(const std::vector<HighFunc> &Funcs, BinaryFormat Format,
                    Arch Target, const BinaryImage *Img = nullptr) {
  CEmitterOptions Options;
  Options.TheArch = Target;
  Options.Format = Format;
  Options.Image = Img;
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  EXPECT_TRUE(HighCEmitter().emit(Funcs, OS, Options));
  return Text;
}

TEST(CSymbolNames, CxxRuntimeRoutinesTakeTheirPrototypes) {
  const BinaryImage Img = startupImage();
  // A static object's destructor registers through __cxa_atexit, which no
  // header declares: the destructor passes as the function it is.
  const std::string Source = emitWithImage(
      {function("install", 0x1000,
                {callStatement("__cxa_atexit", 0x2000,
                               {address(0x1180), HighExpr::makeConst(0, 8),
                                HighExpr::makeConst(0, 8)}),
                 callStatement("__cxa_end_catch", 0x2010)})},
      Img);
  EXPECT_NE(Source.find("extern int __cxa_atexit(void (*)(void *), void *, "
                        "void *);"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("extern void __cxa_end_catch(void);"), NotFound)
      << Source;
  EXPECT_NE(Source.find("__cxa_atexit((void (*)(void *))on_exit_hook, 0, 0);"),
            NotFound)
      << Source;
  source_call_execution_test::compileAndRun(
      Source + "int on_exit_hook(void *Object) { return Object != 0; }\n"
               "void __cxa_end_catch(void) {}\n"
               "int main(void) { return (int)install(); }\n",
      {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}

TEST(CSymbolNames, APointerResultKeepsTheMachineInteger) {
  // __errno_location returns a pointer: declared `int` it lost the upper half
  // of the thread's errno address.
  const std::string Source =
      emitFor({returnsCall("errno_slot", 0x1000,
                           HighExpr::makeCall("__errno_location", 0x2000, {}))},
              BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find("extern int *__errno_location(void);"), NotFound)
      << Source;
  EXPECT_NE(Source.find("return (uintptr_t)__errno_location();"), NotFound)
      << Source;
  source_call_execution_test::compileAndRun(
      Source + "int main(void) {\n"
               "  int *Errno = (int *)errno_slot();\n"
               "  *Errno = 7;\n"
               "  return *Errno == 7 ? 0 : 1;\n"
               "}\n",
      {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}

TEST(CSymbolNames, HeaderFunctionsTakeFunctionsAsTheirParameterTypes) {
  const BinaryImage Img = startupImage();
  // qsort's header declares its comparison `int (*)(const void *, const void
  // *)`; a recovered function's own signature is not that type.
  const std::string Source = emitWithImage(
      {function(
           "sort_none", 0x1000,
           {callStatement("qsort", 0x2000,
                          {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                           HighExpr::makeConst(4, 8), address(0x1180)})}),
       function("on_exit_hook", 0x1180, {})},
      Img);
  EXPECT_NE(Source.find("qsort(0, 0, 4, (int (*)(const void *, const void "
                        "*))on_exit_hook);"),
            NotFound)
      << Source;
  source_call_execution_test::compileAndRun(
      Source + "int main(void) { return (int)sort_none(); }\n",
      {"-Werror=int-conversion", "-Werror=incompatible-pointer-types"});
}

TEST(CSymbolNames, WindowsRuntimeRoutinesTakeTheirPrototypesOnlyInPE) {
  const std::vector<HighFunc> Funcs{function(
      "startup", 0x1000,
      {callStatement(
           "_initterm", 0x2000,
           {HighExpr::makeConst(0x3000, 8), HighExpr::makeConst(0x3010, 8)}),
       callStatement("_lock", 0x2010,
                     {HighExpr::makeConst(1, 4), HighExpr::makeConst(2, 4)})})};
  const std::string PE = emitFor(Funcs, BinaryFormat::COFF, Arch::X64);
  EXPECT_NE(PE.find("extern void _initterm(void (**)(void), void "
                    "(**)(void));"),
            NotFound)
      << PE;
  EXPECT_NE(PE.find("_initterm((void (**)(void))0x3000, (void "
                    "(**)(void))0x3010);"),
            NotFound)
      << PE;
  EXPECT_NE(PE.find("extern void _lock(int);"), NotFound) << PE;
  // An ELF `_lock` is a routine of its own: both arguments stay.
  const std::string ELF = emitFor(Funcs, BinaryFormat::ELF, Arch::X64);
  EXPECT_EQ(ELF.find("_initterm(void (**)"), NotFound) << ELF;
  EXPECT_NE(ELF.find("_lock(1, 2);"), NotFound) << ELF;
}

TEST(CSymbolNames, WindowsAPIRoutinesAreStdcallOn32BitX86) {
  const std::vector<HighFunc> Funcs{
      function("install", 0x1000,
               {callStatement("SetUnhandledExceptionFilter", 0x2000,
                              {HighExpr::makeConst(0, 4)})})};
  const std::string X86 = emitFor(Funcs, BinaryFormat::COFF, Arch::X86);
  EXPECT_NE(X86.find("extern __attribute__((stdcall)) void "
                     "*SetUnhandledExceptionFilter(int32_t "
                     "(__attribute__((stdcall)) *)(void *));"),
            NotFound)
      << X86;
  const std::string X64 = emitFor(Funcs, BinaryFormat::COFF, Arch::X64);
  EXPECT_NE(X64.find("extern void *SetUnhandledExceptionFilter(int32_t "
                     "(*)(void *));"),
            NotFound)
      << X64;
}

TEST(CSymbolNames, FortifiedRoutinesAreVariadicWhereGlibcDeclaresThem) {
  const std::string Source =
      emitFor({function("report", 0x1000,
                        {callStatement("__printf_chk", 0x2000,
                                       {HighExpr::makeConst(1, 4),
                                        HighExpr::makeConst(0, 8),
                                        HighExpr::makeConst(5, 4)})})},
              BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find("extern int __printf_chk(int, const char *, ...);"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("__printf_chk(1, 0, 5);"), NotFound) << Source;
}

TEST(CSymbolNames, ACallMadeForItsEffectKeepsItsOwnType) {
  // Only a result something reads converts to the machine's integer.
  const std::string Source =
      emitFor({function("touch_errno", 0x1000,
                        {callStatement("__errno_location", 0x2000)})},
              BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find("    __errno_location();"), NotFound) << Source;
  EXPECT_EQ(Source.find("(uintptr_t)__errno_location"), NotFound) << Source;
}

TEST(CSymbolNames, AVoidRoutineLeavesNoResult) {
  // A PLT stub returns what the routine it jumps to returns, and
  // __cxa_finalize returns nothing: the result register holds no value, and
  // the stub returns nothing either.
  MedVar Result;
  Result.Kind = MedVar::Temp;
  Result.TheArch = Arch::X64;
  Result.Id = 3;
  Result.SSAVer = 1;
  Result.Size = 8;
  const TypeRef U64 = NdType::makeInt(8, false);
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeVar(Result, U64);
  Assign.Val =
      HighExpr::makeCall("__cxa_finalize", 0x2000, {HighExpr::makeConst(0, 8)});
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeVar(Result, U64);
  HighFunc Stub;
  Stub.Name = "finalize_stub";
  Stub.Entry = 0x1000;
  Stub.ReturnType = U64;
  Stub.Body = {Assign, Return};
  // MedIR settles that the stub returns no value (settleReturnContracts);
  // this HighIR stands for its outcome.
  Stub.ReturnsNoValue = true;
  const std::string Source = emitFor({Stub}, BinaryFormat::ELF, Arch::X64);
  EXPECT_NE(Source.find("extern void __cxa_finalize(void *);"), NotFound)
      << Source;
  EXPECT_NE(Source.find("    __cxa_finalize(0);"), NotFound) << Source;
  EXPECT_EQ(Source.find("= __cxa_finalize"), NotFound) << Source;
  EXPECT_EQ(Source.find("(int64_t)__cxa_finalize"), NotFound) << Source;
  EXPECT_NE(Source.find("void finalize_stub(void) {"), NotFound) << Source;
  EXPECT_EQ(Source.find("unknown"), NotFound) << Source;
}

/// main.main calls two instances of one Rust generic, a Go method, an
/// Objective-C method, a GCC clone, a C function and a C++ function that read
/// alike, and a symbol C reserves.
std::vector<HighFunc> otherLanguageCaller() {
  return {function(
      "main.main", 0x1000,
      {callStatement("_RINvNtCs4NRVxsYgnAr_4core3ptr9drop_glueNtNtCscdodAO9FK5_"
                     "5alloc6string6StringECs8vGhbR5OvgK_13rust_eh_probe",
                     0x2000),
       callStatement("_RINvNtCs4NRVxsYgnAr_4core3ptr9drop_glueNtNtCscdodAO9FK5_"
                     "5alloc3vec3VecECs8vGhbR5OvgK_13rust_eh_probe",
                     0x2010),
       callStatement("fmt.(*pp).doPrintf", 0x2020),
       callStatement("-[NSString length]", 0x2030),
       callStatement("foo.constprop.0", 0x2040),
       callStatement("_Z3foov", 0x2050), callStatement("foo", 0x2060),
       callStatement("int", 0x2070)})};
}

TEST(CSymbolNames, OtherLanguagesReadByTheirPathsAndLinkByTheirSymbols) {
  const std::string Source =
      emitHighC(otherLanguageCaller(), BinaryFormat::ELF, Arch::X64);
  // The definition reads by its path and names its symbol above.
  EXPECT_NE(Source.find("/* main.main */\n"), NotFound) << Source;
  EXPECT_NE(Source.find(" main_main("), NotFound) << Source;
  // Two instances of one generic stay distinct, each linking by its symbol
  // and read in Rust.
  EXPECT_NE(Source.find("core_ptr_drop_glue() __asm__(\"_RINvNtCs4NRVxsYgnAr_"
                        "4core3ptr9drop_glueNtNtCscdodAO9FK5_5alloc6string6S"
                        "tringECs8vGhbR5OvgK_13rust_eh_probe\"); /* core::ptr"
                        "::drop_glue::<alloc::string::String> */"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("core_ptr_drop_glue_2() __asm__("), NotFound) << Source;
  EXPECT_NE(Source.find("core_ptr_drop_glue();"), NotFound) << Source;
  EXPECT_NE(Source.find("core_ptr_drop_glue_2();"), NotFound) << Source;
  // A name its label already spells takes no comment.
  EXPECT_NE(Source.find("fmt_pp_doPrintf() __asm__(\"fmt.(*pp).doPrintf\");\n"),
            NotFound)
      << Source;
  EXPECT_NE(
      Source.find("_i_NSString__length() __asm__(\"-[NSString length]\");\n"),
      NotFound)
      << Source;
  EXPECT_NE(Source.find("foo_constprop_0() __asm__(\"foo.constprop.0\");\n"),
            NotFound)
      << Source;
  // The C function keeps its name, and the C++ function that reads like it
  // and a name C reserves link by their symbols.
  EXPECT_NE(Source.find("foo_2() __asm__(\"_Z3foov\"); /* foo() */"), NotFound)
      << Source;
  EXPECT_NE(Source.find("extern int foo();"), NotFound) << Source;
  EXPECT_NE(Source.find("nd_int() __asm__(\"int\");\n"), NotFound) << Source;
  // No byte of a name is escaped.
  EXPECT_EQ(Source.find("_x2E_"), NotFound) << Source;
}

TEST(CSymbolNames, LLVMCDeclaresOtherLanguagesByTheirSymbols) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
declare i64 @"fmt.(*pp).doPrintf"(i64)
declare i64 @_ZN4core3fmt5write17h0123456789abcdefE(i64)
define i64 @"main.main"(i64 %p) {
  %a = call i64 @"fmt.(*pp).doPrintf"(i64 %p)
  %b = call i64 @_ZN4core3fmt5write17h0123456789abcdefE(i64 %a)
  ret i64 %b
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
  EXPECT_NE(Source.find("fmt_pp_doPrintf(uint64_t) "
                        "__asm__(\"fmt.(*pp).doPrintf\");\n"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("core_fmt_write(uint64_t) __asm__(\"_ZN4core3fmt5write"
                        "17h0123456789abcdefE\"); /* core::fmt::write */"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("/* main.main */\n"), NotFound) << Source;
  EXPECT_NE(Source.find(" main_main("), NotFound) << Source;
}

TEST(CSymbolNames, MicrosoftNamesAreTheirOwnSymbolsOn32BitWindows) {
  // A C name takes the underscore 32-bit Windows adds; a decorated C++ or
  // fastcall name is the symbol already.
  EXPECT_EQ(symbolOfCName("printf", BinaryFormat::COFF, Arch::X86), "_printf");
  EXPECT_EQ(symbolOfCName("?f@@YAXXZ", BinaryFormat::COFF, Arch::X86),
            "?f@@YAXXZ");
  EXPECT_EQ(symbolOfCName("@f@8", BinaryFormat::COFF, Arch::X86), "@f@8");
  EXPECT_EQ(symbolOfCName("?f@@YAXXZ", BinaryFormat::COFF, Arch::X64),
            "?f@@YAXXZ");
}

/// A 32-bit PE whose import directory lists msvcrt's `_initterm`, `_exit`
/// and `exit`, in slots 0x2000, 0x2004 and 0x2008.
BinaryImage msvcrtImports() {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Format = BinaryFormat::COFF;
  for (const auto &[Name, Slot] :
       {std::pair{"_initterm", 0x2000}, std::pair{"_exit", 0x2004},
        std::pair{"exit", 0x2008}}) {
    Import Imp;
    Imp.Module = "msvcrt.dll";
    Imp.Name = Name;
    Imp.IATAddr = Slot;
    Img.Imports.push_back(std::move(Imp));
  }
  return Img;
}

TEST(CSymbolNames, ImportsKeepTheirCNamesOn32BitWindows) {
  // An import directory lists export names, which are C names: msvcrt's
  // `_initterm` is not `initterm`, and its `_exit` is not `exit`.  A COFF
  // symbol carries the underscore: the thunk `__amsg_exit` is `_amsg_exit`.
  const BinaryImage Img = msvcrtImports();
  const std::vector<HighFunc> Funcs{
      function("start", 0x1000,
               {callStatement("_initterm", 0x2000,
                              {HighExpr::makeConst(0x3000, 4),
                               HighExpr::makeConst(0x3010, 4)})}),
      function("quick", 0x1100,
               {callStatement("_exit", 0x2004, {HighExpr::makeConst(1, 4)})}),
      function("normal", 0x1200,
               {callStatement("exit", 0x2008, {HighExpr::makeConst(0, 4)})}),
      function(
          "fatal", 0x1300,
          {callStatement("__amsg_exit", 0x4000, {HighExpr::makeConst(2, 4)})})};
  const std::string Source =
      emitFor(Funcs, BinaryFormat::COFF, Arch::X86, &Img);
  EXPECT_NE(Source.find("_initterm((void (**)(void))0x3000"), NotFound)
      << Source;
  EXPECT_NE(Source.find("_exit(1);"), NotFound) << Source;
  EXPECT_NE(Source.find(" exit(0);"), NotFound) << Source;
  EXPECT_NE(Source.find(" _amsg_exit(2);"), NotFound) << Source;
  for (const char *Wrong : {" initterm(", "__amsg_exit("})
    EXPECT_EQ(Source.find(Wrong), NotFound) << Wrong << "\n" << Source;
}

TEST(CSymbolNames, LLVMCImportsKeepTheirCNamesOn32BitWindows) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
declare void @_initterm(i32, i32)
declare void @__amsg_exit(i32)
define void @start() {
  call void @_initterm(i32 12288, i32 12304)
  call void @__amsg_exit(i32 2)
  ret void
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  const BinaryImage Img = msvcrtImports();
  CEmitterOptions Options;
  Options.TheArch = Arch::X86;
  Options.Format = BinaryFormat::COFF;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options, nullptr, &Img));
  // The import keeps its C name; the symbol `__amsg_exit` reads as C.
  EXPECT_NE(Source.find(" _initterm("), NotFound) << Source;
  EXPECT_EQ(Source.find(" initterm("), NotFound) << Source;
  EXPECT_NE(Source.find(" _amsg_exit("), NotFound) << Source;
}

/// A thunk \p Name at 0x1000 that returns the call of its two parameters
/// through the import slot 0x2000, as `jmp [slot]` lifts.
HighFunc slotThunk(const char *Name, const char *ImportName, Arch Target) {
  const uint16_t Bytes = Target == Arch::X86 ? 4 : 8;
  std::vector<ExprPtr> Args;
  for (int I = 0; I < 2; ++I) {
    MedVar Var;
    Var.Kind = MedVar::Param;
    Var.Id = I;
    Var.Size = Bytes;
    Var.TheArch = Target;
    Args.push_back(HighExpr::makeVar(Var, NdType::makeInt(Bytes)));
  }
  HighFunc F;
  F.Name = Name;
  F.Entry = 0x1000;
  F.ReturnType = NdType::makeInt(Bytes, false);
  F.Params = {{"arg0", NdType::makeInt(Bytes)},
              {"arg1", NdType::makeInt(Bytes)}};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeCall(ImportName, 0x2000, std::move(Args));
  F.Body = {Return};
  return F;
}

TEST(CSymbolNames, AThunkNamedLikeItsImportCallsThroughTheSlot) {
  // A thunk the linker names like its import, `calloc: jmp [__imp_calloc]`,
  // does not call itself: it calls through the slot the loader binds, which
  // C declares as a function pointer named as the linker names the slot.
  // It returns what calloc returns; _initterm returns nothing, and neither
  // does its thunk.
  for (const auto &[Target, Thunk, ImportName, Slot, ReturnsValue] :
       {std::tuple{Arch::X64, "calloc", "calloc", "__imp_calloc", true},
        std::tuple{Arch::X86, "__initterm", "_initterm", "_imp___initterm",
                   false}}) {
    SCOPED_TRACE(Slot);
    BinaryImage Img;
    Img.Arch = Target;
    Img.Format = BinaryFormat::COFF;
    Import Imp;
    Imp.Module = "msvcrt.dll";
    Imp.Name = ImportName;
    Imp.IATAddr = 0x2000;
    Img.Imports.push_back(std::move(Imp));
    // MedIR settles whether the thunk returns a value
    // (settleReturnContracts); this HighIR stands for its outcome.
    HighFunc ThunkFunc = slotThunk(Thunk, ImportName, Target);
    ThunkFunc.ReturnsNoValue = !ReturnsValue;
    const std::string Source =
        emitFor({ThunkFunc}, BinaryFormat::COFF, Target, &Img);
    // The thunk calls through the slot, declared as a function pointer, and
    // never calls itself.
    EXPECT_NE(Source.find("(*" + std::string(Slot) + ")("), NotFound) << Source;
    EXPECT_EQ(Source.find("return " + std::string(Slot) + "(") != NotFound,
              ReturnsValue)
        << Source;
    if (!ReturnsValue) {
      EXPECT_NE(
          Source.find("void " +
                      cNameOfSymbol(Thunk, BinaryFormat::COFF, Target).str() +
                      "("),
          NotFound)
          << Source;
      EXPECT_NE(Source.find("    " + std::string(Slot) + "("), NotFound)
          << Source;
    }
    EXPECT_EQ(Source.find(
                  "return " +
                  cNameOfSymbol(Thunk, BinaryFormat::COFF, Target).str() + "("),
              NotFound)
        << Source;
  }
}

TEST(CSymbolNames, AWrapperNamedLikeItsImportCallsTheSlotTheImageNames) {
  // MinGW's own `__getmainargs` (0x1000) calls msvcrt's through a stub
  // (0x3000) whose slot the linker named `__imp____msvcrt_getmainargs`, after
  // the section symbol there; its `__imp____getmainargs` (0x4000) points to
  // the wrapper itself.  Neither that variable nor a label to
  // `___getmainargs` names the callee.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Format = BinaryFormat::COFF;
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x2100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x2100, 0xC3);
  Img.Segments.push_back(std::move(Text));
  Import Imp;
  Imp.Module = "msvcrt.dll";
  Imp.Name = "__getmainargs";
  Imp.IATAddr = 0x5000;
  Img.Imports.push_back(std::move(Imp));
  ASSERT_TRUE(Img.recordImportStub(0x3000, 0));
  for (const auto &[Name, Addr] :
       {std::pair<const char *, va_t>{".idata$5", 0x5000},
        {"__imp____msvcrt_getmainargs", 0x5000},
        {"__imp____getmainargs", 0x4000}}) {
    Symbol Sym;
    Sym.Name = Name;
    Sym.Addr = Addr;
    Img.Symbols.push_back(std::move(Sym));
  }
  HighFunc Wrapper = slotThunk("___getmainargs", "__getmainargs", Arch::X86);
  Wrapper.Body.front().RetVal->CallAddr = 0x3000;
  const std::string Source =
      emitFor({Wrapper}, BinaryFormat::COFF, Arch::X86, &Img);
  EXPECT_NE(Source.find("(*_imp____msvcrt_getmainargs)("), NotFound) << Source;
  EXPECT_NE(Source.find("return _imp____msvcrt_getmainargs("), NotFound)
      << Source;
  EXPECT_EQ(Source.find("_imp____getmainargs"), NotFound) << Source;
  EXPECT_EQ(Source.find("__asm__(\"___getmainargs\")"), NotFound) << Source;

  // Without the slot's own symbol, `__imp____getmainargs` is the wrapper's
  // pointer and no name for the slot: the callee keeps its own identifier.
  llvm::erase_if(Img.Symbols, [](const Symbol &Sym) {
    return Sym.Name == "__imp____msvcrt_getmainargs";
  });
  const std::string Unnamed =
      emitFor({Wrapper}, BinaryFormat::COFF, Arch::X86, &Img);
  EXPECT_EQ(Unnamed.find("_imp____getmainargs"), NotFound) << Unnamed;
  EXPECT_EQ(Unnamed.find("__asm__(\"___getmainargs\")"), NotFound) << Unnamed;
  EXPECT_NE(Unnamed.find("return __getmainargs_3000("), NotFound) << Unnamed;
}

TEST(CSymbolNames, ACallToAVariadicImportsStubKeepsItsArguments) {
  // MinGW's `fprintf: jmp [__imp_fprintf]` has no C signature that passes
  // `...` on, so it is defined without parameters; report(f, fmt, n) calls
  // it through the slot it jumps through and passes all three.
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::COFF;
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x200;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x200, 0xC3);
  Img.Segments.push_back(std::move(Text));
  Import Imp;
  Imp.Module = "msvcrt.dll";
  Imp.Name = "fprintf";
  Imp.IATAddr = 0x3000;
  Img.Imports.push_back(std::move(Imp));
  ASSERT_TRUE(Img.recordImportStub(0x1000, 0));
  std::vector<ExprPtr> Arguments;
  for (int I = 0; I < 3; ++I) {
    MedVar Var;
    Var.Kind = MedVar::Param;
    Var.Id = I;
    Var.Size = 8;
    Var.TheArch = Arch::X64;
    Arguments.push_back(HighExpr::makeVar(Var, NdType::makeInt(8)));
  }
  HighFunc Report =
      function("report", 0x1100, {callStatement("fprintf", 0x1000, Arguments)});
  Report.Params = {{"arg0", NdType::makeInt(8)},
                   {"arg1", NdType::makeInt(8)},
                   {"arg2", NdType::makeInt(8)}};
  const std::string Source = emitFor(
      {function("fprintf", 0x1000, {callStatement("fprintf", 0x3000)}), Report},
      BinaryFormat::COFF, Arch::X64, &Img);
  const size_t Body = Source.find(" report(");
  ASSERT_NE(Body, NotFound) << Source;
  EXPECT_NE(Source.find("__imp_fprintf((void *)(uintptr_t)arg0, "
                        "(void *)(uintptr_t)arg1, arg2)",
                        Body),
            NotFound)
      << Source;
}

TEST(CSymbolNames, ObjectsReadByTheirPathsWithTheirSymbolsBeside) {
  constexpr va_t Text = 0x1000, Data = 0x4000;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Segment Code;
  Code.Name = ".text";
  Code.VA = Text;
  Code.Data.assign(0x100, 0xC3);
  Code.Size = Code.Data.size();
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Segments.push_back(std::move(Code));
  Segment Values;
  Values.Name = ".data";
  Values.VA = Data;
  Values.Data.assign(0x40, 0);
  Values.Size = Values.Data.size();
  Values.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Values));
  for (const auto &[Name, Addr] : {std::pair{"_ZTV8QDomNode", Data},
                                   std::pair{"main.Flags", Data + 0x20}}) {
    Symbol Object;
    Object.Name = Name;
    Object.Addr = Addr;
    Object.Size = 8;
    Img.Symbols.push_back(std::move(Object));
  }
  HighFunc Reader = function("reader", Text, {});
  for (va_t Addr : {Data, Data + 0x20}) {
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreAddr = HighExpr::makeConst(Addr, 8);
    Store.StoreVal = HighExpr::makeConst(1, 8);
    Store.StoreVal->Type = NdType::makeInt(8, false);
    Reader.Body.insert(Reader.Body.begin(), std::move(Store));
  }
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::ELF;
  Options.Image = &Img;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit({Reader}, OS, Options));
  EXPECT_NE(Source.find("/* neverd.image: 0x4000 vtable for QDomNode */\n"),
            NotFound)
      << Source;
  EXPECT_NE(Source.find("QDomNode_vtable = 1;"), NotFound) << Source;
  EXPECT_NE(Source.find("/* neverd.image: 0x4020 main.Flags */\n"), NotFound)
      << Source;
  EXPECT_NE(Source.find("main_Flags = 1;"), NotFound) << Source;
}

TEST(CSymbolNames, OtherLanguageLabelsLinkToTheirSymbols) {
#if !defined(__linux__)
  GTEST_SKIP() << "links an ELF program on the host";
#else
  if (llvm::StringRef(NEVERD_RUNTIME_FIXTURE_COMPILER).empty())
    GTEST_SKIP() << "needs a GNU-style host C compiler";
  // The stubs define each callee under its symbol, as Go, Rust and
  // Objective-C objects would; the emitted C must reach each one through its
  // label.
  const std::string Stubs = R"(
#include <stdint.h>
unsigned called;
int64_t main_main(void);
#define DEFINE(Stub, Symbol, Bit)                                              \
  void Stub(void) __asm__(Symbol);                                             \
  void Stub(void) { called |= Bit; }
DEFINE(drop_string, "_RINvNtCs4NRVxsYgnAr_4core3ptr9drop_glueNtNtCscdodAO9FK5_"
                    "5alloc6string6StringECs8vGhbR5OvgK_13rust_eh_probe", 1)
DEFINE(drop_vec, "_RINvNtCs4NRVxsYgnAr_4core3ptr9drop_glueNtNtCscdodAO9FK5_"
                 "5alloc3vec3VecECs8vGhbR5OvgK_13rust_eh_probe", 2)
DEFINE(do_printf, "fmt.(*pp).doPrintf", 4)
DEFINE(length, "-[NSString length]", 8)
DEFINE(clone, "foo.constprop.0", 16)
DEFINE(cxx_foo, "_Z3foov", 32)
DEFINE(c_foo, "foo", 64)
DEFINE(keyword, "int", 128)
int main(void) { main_main(); return called == 255 ? 0 : 1; }
)";
  const std::string Source =
      emitHighC(otherLanguageCaller(), BinaryFormat::ELF, Arch::X64);
  llvm::SmallString<128> SourcePath, StubPath, BinaryPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-other-names", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-other-names-stubs",
                                                  "c", StubPath));
  llvm::FileRemover RemoveStubs(StubPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-other-names", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  for (const auto &[Path, Text] : {std::pair{SourcePath.str(), Source},
                                   std::pair{StubPath.str(), Stubs}}) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Out << Text;
  }
  std::string Message;
  const int Compiled =
      llvm::sys::ExecuteAndWait(NEVERD_RUNTIME_FIXTURE_COMPILER,
                                {NEVERD_RUNTIME_FIXTURE_COMPILER, "-std=c11",
                                 "-w", SourcePath, StubPath, "-o", BinaryPath},
                                std::nullopt, {}, 60, 0, &Message);
  ASSERT_EQ(Compiled, 0) << Message << '\n' << Source;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                      {}, 30, 0, &Message),
            0)
      << Message << '\n'
      << Source;
#endif
}

/// Clang's assembly for \p Source on \p Triple, or an error.
llvm::Expected<std::string> assemblyForTarget(const std::string &Source,
                                              llvm::StringRef Triple) {
  const std::string Compiler = NEVERD_TEST_CLANG;
  llvm::SmallString<128> SourcePath, AsmPath;
  if (llvm::sys::fs::createTemporaryFile("neverd-target-names", "c",
                                         SourcePath) ||
      llvm::sys::fs::createTemporaryFile("neverd-target-names", "s", AsmPath))
    return llvm::createStringError("cannot create temporary files");
  llvm::FileRemover RemoveSource(SourcePath);
  llvm::FileRemover RemoveAsm(AsmPath);
  {
    std::error_code Error;
    llvm::raw_fd_ostream Out(SourcePath, Error);
    if (Error)
      return llvm::createStringError(Error.message());
    Out << Source;
  }
  const std::string Target = ("--target=" + Triple).str();
  std::string Message;
  if (llvm::sys::ExecuteAndWait(Compiler,
                                {Compiler, "-std=gnu17", Target,
                                 "-ffreestanding", "-w", "-S", SourcePath, "-o",
                                 AsmPath},
                                std::nullopt, {}, 60, 0, &Message) != 0)
    return llvm::createStringError("clang failed: " + Message);
  auto Assembly = llvm::MemoryBuffer::getFile(AsmPath);
  if (!Assembly)
    return llvm::createStringError(Assembly.getError().message());
  return (*Assembly)->getBuffer().str();
}

TEST(CSymbolNames, MicrosoftImportsLinkByTheirDecoratedNames) {
  if (llvm::StringRef(NEVERD_TEST_CLANG).empty())
    GTEST_SKIP() << "needs clang";
  // A member function no MSVC rule describes, on 64-bit and 32-bit Windows.
  // 32-bit Windows decorates C names with an underscore, never C++ names.
  for (const auto &[Target, Triple, Symbol] :
       {std::tuple{Arch::X64, "x86_64-pc-windows-msvc",
                   "?Get@MemManager@Contoso@@QEAAPEAXXZ"},
        std::tuple{Arch::X86, "i686-pc-windows-msvc",
                   "?Get@MemManager@Contoso@@QAEPAXXZ"}}) {
    SCOPED_TRACE(Triple);
    const std::string Source =
        emitHighC({function("caller", 0x1000, {callStatement(Symbol, 0x2000)})},
                  BinaryFormat::COFF, Target);
    EXPECT_NE(Source.find((llvm::Twine("Contoso_MemManager_Get") +
                           "() __asm__(\"" + Symbol + "\")")
                              .str()),
              NotFound)
        << Source;
    EXPECT_NE(Source.find("Contoso::MemManager::Get(void)"), NotFound)
        << Source;
    auto Assembly = assemblyForTarget(Source, Triple);
    ASSERT_TRUE(static_cast<bool>(Assembly))
        << llvm::toString(Assembly.takeError()) << Source;
    EXPECT_NE(Assembly->find((llvm::Twine("\"") + Symbol + "\"").str()),
              NotFound)
        << *Assembly;
    EXPECT_EQ(Assembly->find("_?Get"), NotFound) << *Assembly;
  }
}

} // namespace
