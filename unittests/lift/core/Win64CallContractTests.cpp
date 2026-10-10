//===- Win64CallContractTests.cpp - Microsoft x64 import arguments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// MSVC calls a C runtime import through its import address slot
// (`call [__imp_fputs]`); MinGW calls a thunk that jumps there
// (`jmp [__imp_fputs]`).  A wrapper that passes its own RCX and RDX on leaves
// them unwritten.  The import's prototype says it reads them, so they are the
// wrapper's parameters and the call's arguments.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Text = 0x140001000;
constexpr va_t Iat = 0x140003000;

void put(std::vector<uint8_t> &Code, va_t At, std::vector<uint8_t> Bytes) {
  std::copy(Bytes.begin(), Bytes.end(), Code.begin() + (At - Text));
}

std::vector<uint8_t> rel32(va_t Next, va_t To) {
  const uint32_t Delta = static_cast<uint32_t>(To - Next);
  return {static_cast<uint8_t>(Delta), static_cast<uint8_t>(Delta >> 8),
          static_cast<uint8_t>(Delta >> 16), static_cast<uint8_t>(Delta >> 24)};
}

/// The slot of import \p Index.
constexpr va_t slot(size_t Index) { return Iat + 8 * Index; }

/// `call [rip + Slot]`, or `jmp` when \p Jump, at \p At.
std::vector<uint8_t> throughSlot(va_t At, va_t Slot, bool Jump = false) {
  std::vector<uint8_t> Bytes = {0xFF, static_cast<uint8_t>(Jump ? 0x25 : 0x15)};
  for (uint8_t B : rel32(At + 6, Slot))
    Bytes.push_back(B);
  return Bytes;
}

/// `sub rsp, 28h`, the calls \p Calls, `add rsp, 28h` and `ret`: a function
/// that passes its own argument registers on to each callee.  The first call
/// starts 4 bytes in.
std::vector<uint8_t> framed(const std::vector<std::vector<uint8_t>> &Calls) {
  std::vector<uint8_t> Bytes = {0x48, 0x83, 0xEC, 0x28};
  for (const auto &Call : Calls)
    Bytes.insert(Bytes.end(), Call.begin(), Call.end());
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x28, 0xC3})
    Bytes.push_back(B);
  return Bytes;
}

/// A PE32+ image with \p Code at Text, the given function symbols, and the
/// msvcrt.dll imports \p Imports in consecutive slots at Iat.
BinaryImage
makeImage(std::vector<uint8_t> Code,
          const std::vector<std::pair<va_t, const char *>> &Functions,
          const std::vector<const char *> &Imports) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Text;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  Segment Idata;
  Idata.Name = ".idata";
  Idata.VA = Iat;
  Idata.Size = Idata.FileSz = 8 * Imports.size();
  Idata.Data.resize(Idata.Size);
  Idata.Flags = SegmentFlags::Readable;
  Img.Segments.push_back(std::move(Idata));
  for (size_t I = 0; I < Imports.size(); ++I) {
    Import Imp;
    Imp.Module = "msvcrt.dll";
    Imp.Name = Imports[I];
    Imp.IATAddr = slot(I);
    Img.Imports.push_back(std::move(Imp));
  }
  for (const auto &[Entry, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  return Img;
}

PipelineResult run(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                   std::set<va_t> Entries) {
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = std::move(Entries);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  return Result;
}

/// HighC for the functions at \p Entries of \p Img.
std::string liftEntries(const BinaryImage &Img, std::set<va_t> Entries) {
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, std::move(Entries));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  return Source;
}

/// The definition of function \p Name in \p Source: the line naming it that
/// opens a body, not a declaration before it.
std::string body(const std::string &Source, const std::string &Name) {
  const std::string Head = " " + Name + "(";
  for (size_t At = Source.find(Head); At != std::string::npos;
       At = Source.find(Head, At + 1)) {
    const size_t LineEnd = Source.find('\n', At);
    if (LineEnd == std::string::npos || Source[LineEnd - 1] != '{')
      continue;
    const size_t Close = Source.find("\n}", LineEnd);
    if (Close == std::string::npos)
      return {};
    const size_t Start = Source.rfind('\n', At) + 1;
    return Source.substr(Start, Close + 2 - Start);
  }
  return {};
}

unsigned family(uint64_t RegOff) { return static_cast<unsigned>(RegOff / 8); }

TEST(Win64CallContract, ACallThroughTheImportSlotPassesTheCallersArguments) {
  // wrap(s, f) calls fputs through its slot without writing RCX or RDX.
  constexpr va_t Wrap = Text;
  std::vector<uint8_t> Code(0x20, 0xCC);
  put(Code, Wrap, framed({throughSlot(Wrap + 4, slot(0))}));
  const BinaryImage Img = makeImage(Code, {{Wrap, "wrap"}}, {"fputs"});
  const std::string Source = liftEntries(Img, {Wrap});
  const std::string Body = body(Source, "wrap");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(
      Body.find("fputs((void *)(uintptr_t)arg0, (void *)(uintptr_t)arg1)"),
      std::string::npos)
      << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(Win64CallContract, LLVMImportCallKeepsUnwrittenLeadingArgument) {
  // run_pipe(command): RCX is forwarded, only the second argument is set.
  constexpr va_t Wrap = Text;
  std::vector<uint8_t> Code(0x20, 0xCC);
  put(Code, Wrap,
      framed({{0xBA, 0x45, 0x23, 0x01, 0x00}, throughSlot(Wrap + 9, slot(0))}));
  BinaryImage Img = makeImage(Code, {{Wrap, "wrap"}}, {"_popen"});
  ASSERT_TRUE(Img.recordImportStorageSlot(slot(0), "_popen", 0,
                                          ImportStorageEvidence::PointerTable));
  for (bool NoOpt : {true, false}) {
    SCOPED_TRACE(NoOpt);
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.LiftMode = true;
    Opts.SourceProjection = true;
    Opts.NoOpt = NoOpt;
    Opts.OnlyFunctionEntries = {Wrap};
    auto Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    auto *Caller = Result.LlvmModule->getFunction("wrap");
    auto *Callee = Result.LlvmModule->getFunction("_popen");
    ASSERT_NE(Caller, nullptr);
    ASSERT_NE(Callee, nullptr);
    ASSERT_EQ(Caller->arg_size(), 1u);
    ASSERT_EQ(Callee->arg_size(), 2u);
    unsigned Calls = 0;
    for (auto &Block : *Caller)
      for (auto &Instruction : Block)
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
            Call && Call->getCalledFunction() == Callee) {
          ++Calls;
          EXPECT_EQ(Call->getFunctionType(), Callee->getFunctionType());
          ASSERT_EQ(Call->arg_size(), 2u);
          EXPECT_EQ(Call->getArgOperand(0), Caller->getArg(0));
          auto *Second =
              llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(1));
          ASSERT_NE(Second, nullptr);
          EXPECT_EQ(Second->getZExtValue(), 0x12345u);
        }
    EXPECT_EQ(Calls, 1u);
  }
}

TEST(Win64CallContract, AThunkJumpingThroughTheSlotForwardsTheArguments) {
  // A MinGW thunk `jmp [__imp_fputs]`, called by wrap(s, f) without writing
  // RCX or RDX: both read the parameters fputs reads.
  constexpr va_t Wrap = Text, Thunk = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> Call = {0xE8};
  for (uint8_t B : rel32(Wrap + 4 + 5, Thunk))
    Call.push_back(B);
  put(Code, Wrap, framed({Call}));
  put(Code, Thunk, throughSlot(Thunk, slot(0), /*Jump=*/true));
  const BinaryImage Img =
      makeImage(Code, {{Wrap, "wrap"}, {Thunk, "fputs_thunk"}}, {"fputs"});
  const std::string Source = liftEntries(Img, {Wrap, Thunk});
  const std::string ThunkBody = body(Source, "fputs_thunk");
  ASSERT_FALSE(ThunkBody.empty()) << Source;
  EXPECT_NE(
      ThunkBody.find("fputs((void *)(uintptr_t)arg0, (void *)(uintptr_t)arg1)"),
      std::string::npos)
      << ThunkBody;
  const std::string WrapBody = body(Source, "wrap");
  ASSERT_FALSE(WrapBody.empty()) << Source;
  EXPECT_NE(WrapBody.find("fputs_thunk(arg0, arg1)"), std::string::npos)
      << WrapBody;
  for (const std::string &Body : {ThunkBody, WrapBody})
    EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(Win64CallContract, AThunkForwardsTheStackArgumentsItsImportReads) {
  // A MinGW thunk `jmp [__imp___getmainargs]`, called by wrap(a, b, c, d, e)
  // with its own arguments: __getmainargs reads a fifth on the stack, which
  // wrap stores at [rsp+20h] and the thunk leaves where its caller put it.
  constexpr va_t Wrap = Text, Thunk = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> Body = {
      0x48, 0x83, 0xEC, 0x38,       // sub rsp, 38h
      0x48, 0x8B, 0x44, 0x24, 0x60, // mov rax, [rsp+60h]
      0x48, 0x89, 0x44, 0x24, 0x20, // mov [rsp+20h], rax
      0xE8};                        // call Thunk
  for (uint8_t B : rel32(Wrap + Body.size() + 4, Thunk))
    Body.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x38, 0xC3}) // add rsp, 38h; ret
    Body.push_back(B);
  put(Code, Wrap, Body);
  put(Code, Thunk, throughSlot(Thunk, slot(0), /*Jump=*/true));
  BinaryImage Img = makeImage(
      Code, {{Wrap, "wrap"}, {Thunk, "getmainargs_thunk"}}, {"__getmainargs"});
  // The PE loader records the thunk as the import's stub.
  ASSERT_TRUE(Img.recordImportStub(Thunk, 0));
  const std::string Source = liftEntries(Img, {Wrap, Thunk});
  const std::string ThunkBody = body(Source, "getmainargs_thunk");
  ASSERT_FALSE(ThunkBody.empty()) << Source;
  EXPECT_NE(ThunkBody.find("arg4)"), std::string::npos) << ThunkBody;
  const std::string WrapBody = body(Source, "wrap");
  ASSERT_FALSE(WrapBody.empty()) << Source;
  // A call to the stub names the import.
  EXPECT_NE(WrapBody.find("__getmainargs("), std::string::npos) << WrapBody;
  EXPECT_NE(WrapBody.find("arg4)"), std::string::npos) << WrapBody;
  for (const std::string &Body : {ThunkBody, WrapBody})
    EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(Win64CallContract, TheRuntimesOwnPrototypeFixesWhatItsSlotReads) {
  // One function calls each import through its slot.  calloc reads RCX and
  // RDX by the C library's arity, _initterm by the Windows C runtime's own
  // prototype, and _lock reads RCX alone as that runtime declares it.
  // __getmainargs reads all four registers and a fifth argument on the
  // stack.  _pipe(fds, size, mode) has no fixed reads: POSIX pipe(fds) is
  // another function.
  const std::vector<const char *> Imports = {"calloc", "_initterm", "_lock",
                                             "__getmainargs", "_pipe"};
  constexpr va_t Caller = Text;
  std::vector<uint8_t> Code(0x48, 0xCC);
  std::vector<std::vector<uint8_t>> Calls;
  for (size_t I = 0; I < Imports.size(); ++I)
    Calls.push_back(throughSlot(Caller + 4 + 6 * I, slot(I)));
  put(Code, Caller, framed(Calls));
  const BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, Imports);
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {Caller});
  const auto &Reads = Result.CallEntryReadGPRs;
  for (size_t I = 0; I < 2; ++I) {
    SCOPED_TRACE(Imports[I]);
    ASSERT_TRUE(Reads.count(slot(I)));
    EXPECT_EQ(Reads.at(slot(I))[family(x86reg::RCX)], 8u);
    EXPECT_EQ(Reads.at(slot(I))[family(x86reg::RDX)], 8u);
    EXPECT_EQ(Reads.at(slot(I))[family(x86reg::R8)], 0u);
  }
  ASSERT_TRUE(Reads.count(slot(2)));
  EXPECT_EQ(Reads.at(slot(2))[family(x86reg::RCX)], 8u);
  EXPECT_EQ(Reads.at(slot(2))[family(x86reg::RDX)], 0u);
  ASSERT_TRUE(Reads.count(slot(3)));
  EXPECT_EQ(Reads.at(slot(3))[family(x86reg::R9)], 8u);
  // Stack arguments count positions, the registers' included.
  ASSERT_TRUE(Result.CallEntryStackArgs.count(slot(3)));
  EXPECT_EQ(Result.CallEntryStackArgs.at(slot(3)), 5);
  EXPECT_EQ(Result.CallEntryStackArgs.at(slot(0)), 0);
  EXPECT_FALSE(Reads.count(slot(4)));
}

TEST(Win64CallContract, TheStartupsKernel32CallsReadWhatTheyDeclare) {
  // MSVC's startup code calls these through their slots.  IsDebuggerPresent
  // takes no argument, GetModuleHandleW one, and RtlVirtualUnwind four in
  // registers and four on the stack.
  const std::vector<const char *> Imports = {
      "IsDebuggerPresent", "GetModuleHandleW", "RtlVirtualUnwind"};
  constexpr va_t Caller = Text;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<std::vector<uint8_t>> Calls;
  for (size_t I = 0; I < Imports.size(); ++I)
    Calls.push_back(throughSlot(Caller + 4 + 6 * I, slot(I)));
  put(Code, Caller, framed(Calls));
  const BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, Imports);
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {Caller});
  const auto &Reads = Result.CallEntryReadGPRs;
  if (Reads.count(slot(0)))
    for (unsigned Bytes : Reads.at(slot(0)))
      EXPECT_EQ(Bytes, 0u);
  ASSERT_TRUE(Reads.count(slot(1)));
  EXPECT_EQ(Reads.at(slot(1))[family(x86reg::RCX)], 8u);
  EXPECT_EQ(Reads.at(slot(1))[family(x86reg::RDX)], 0u);
  ASSERT_TRUE(Reads.count(slot(2)));
  EXPECT_EQ(Reads.at(slot(2))[family(x86reg::R9)], 8u);
  ASSERT_TRUE(Result.CallEntryStackArgs.count(slot(2)));
  EXPECT_EQ(Result.CallEntryStackArgs.at(slot(2)), 8);
  const std::string Source = body(liftEntries(Img, {Caller}), "caller");
  EXPECT_NE(Source.find("IsDebuggerPresent();"), std::string::npos) << Source;
}

TEST(Win64CallContract, ACallNamesTheFunctionNotItsObjectsSection) {
  // MinGW links each object's code with the section's own symbol where the
  // object's first function starts: `.text` comes first in the symbol table,
  // then _setargv.  A call from a function decompiled alone names _setargv,
  // as the function decompiled alone is named.
  constexpr va_t Caller = Text;
  constexpr va_t Callee = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> Call = {0xE8}; // call rel32
  for (uint8_t B : rel32(Caller + 9, Callee))
    Call.push_back(B);
  put(Code, Caller, framed({Call}));
  put(Code, Callee, {0x31, 0xC0, 0xC3}); // xor eax, eax; ret
  BinaryImage Img = makeImage(Code, {{Caller, "caller"}}, {"calloc"});
  Symbol Section;
  Section.Name = ".text";
  Section.Addr = Callee;
  Img.Symbols.push_back(std::move(Section));
  Symbol Function;
  Function.Name = "_setargv";
  Function.Addr = Callee;
  Function.IsFunc = true;
  Img.Symbols.push_back(std::move(Function));
  ASSERT_EQ(Img.getFunctionNameAt(Callee), "_setargv");
  const std::string Source = body(liftEntries(Img, {Caller}), "caller");
  EXPECT_NE(Source.find("_setargv("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("text("), std::string::npos) << Source;
}

TEST(Win64CallContract, ADataImportReadsAsItsSlot) {
  // MinGW reads msvcrt's _commode through `.refptr.__imp__commode`, a
  // pointer to the import's slot in read-only data; MSVC reads the slot
  // itself.  Both read the address the loader binds to the slot.
  constexpr va_t Refptr = 0x140004000;
  constexpr va_t ViaRefptr = Text;
  constexpr va_t Direct = Text + 0x20;
  std::vector<uint8_t> Code(0x40, 0xCC);
  std::vector<uint8_t> Indirect = {0x48, 0x8B, 0x05}; // mov rax, [rip + Refptr]
  for (uint8_t B : rel32(ViaRefptr + 7, Refptr))
    Indirect.push_back(B);
  for (uint8_t B : {0x48, 0x8B, 0x00, 0xC3}) // mov rax, [rax]; ret
    Indirect.push_back(B);
  put(Code, ViaRefptr, Indirect);
  std::vector<uint8_t> Slot = {0x48, 0x8B, 0x05}; // mov rax, [rip + slot]
  for (uint8_t B : rel32(Direct + 7, slot(0)))
    Slot.push_back(B);
  Slot.push_back(0xC3); // ret
  put(Code, Direct, Slot);
  BinaryImage Img = makeImage(
      Code, {{ViaRefptr, "p_commode"}, {Direct, "read_commode"}}, {"_commode"});
  Segment Rdata;
  Rdata.Name = ".rdata";
  Rdata.VA = Refptr;
  Rdata.Size = Rdata.FileSz = 8;
  Rdata.Flags = SegmentFlags::Readable;
  const va_t SlotVA = slot(0);
  Rdata.Data.resize(8);
  std::memcpy(Rdata.Data.data(), &SlotVA, sizeof(SlotVA));
  Img.Segments.push_back(std::move(Rdata));
  const std::string Source = liftEntries(Img, {ViaRefptr, Direct});
  EXPECT_NE(Source.find("extern void *__imp__commode;"), std::string::npos)
      << Source;
  for (const char *Name : {"p_commode", "read_commode"}) {
    SCOPED_TRACE(Name);
    const std::string Body = body(Source, Name);
    EXPECT_NE(Body.find("__imp__commode;"), std::string::npos) << Source;
    EXPECT_EQ(Body.find("0x140003000"), std::string::npos) << Source;
  }
  EXPECT_EQ(Source.find("refptr"), std::string::npos) << Source;
}

TEST(Win64CallContract, AFloatArgumentTakesItsSlotsVectorRegister) {
  // scale(x, n) = x * n reads x in xmm0, the first slot's vector register,
  // and n in edx, the second slot's integer one. twice(x) = x + x;
  // quadruple(x) doubles twice(x), passing x on in xmm0.
  constexpr va_t Scale = Text, Twice = Text + 0x10, Quadruple = Text + 0x20;
  std::vector<uint8_t> Code(0x40, 0xCC);
  put(Code, Scale,
      {0xF3, 0x0F, 0x2A, 0xCA, // cvtsi2ss xmm1, edx
       0xF3, 0x0F, 0x59, 0xC1, // mulss xmm0, xmm1
       0xC3});                 // ret
  put(Code, Twice,
      {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
       0xC3});                 // ret
  std::vector<uint8_t> QuadrupleCode = {0x48, 0x83, 0xEC, 0x28, // sub rsp, 28h
                                        0xE8};                  // call twice
  for (uint8_t B : rel32(Quadruple + 9, Twice))
    QuadrupleCode.push_back(B);
  for (uint8_t B : {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
                    0x48, 0x83, 0xC4, 0x28, // add rsp, 28h
                    0xC3})                  // ret
    QuadrupleCode.push_back(B);
  put(Code, Quadruple, QuadrupleCode);
  const BinaryImage Img = makeImage(
      Code, {{Scale, "scale"}, {Twice, "twice"}, {Quadruple, "quadruple"}}, {});
  const std::string Source = liftEntries(Img, {Scale, Twice, Quadruple});
  EXPECT_NE(Source.find("float scale(float arg0, int32_t arg1)"),
            std::string::npos)
      << Source;
  const std::string Body = body(Source, "quadruple");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("double quadruple(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("twice(arg0)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
}

TEST(Win64CallContract, AFloatArgumentReachesItsImportsSlot) {
  // sin_twice(x) = sin(x + x) through the import's slot: sin's double
  // parameter takes the first slot's vector register.
  constexpr va_t SinTwice = Text;
  std::vector<uint8_t> Code(0x20, 0xCC);
  std::vector<uint8_t> SinTwiceCode = {0x48, 0x83, 0xEC, 0x28,  // sub rsp, 28h
                                       0xF2, 0x0F, 0x58, 0xC0}; // addsd
  for (uint8_t B : throughSlot(SinTwice + 8, slot(0)))
    SinTwiceCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x28, // add rsp, 28h
                    0xC3})                  // ret
    SinTwiceCode.push_back(B);
  put(Code, SinTwice, SinTwiceCode);
  const BinaryImage Img = makeImage(Code, {{SinTwice, "sin_twice"}}, {"sin"});
  const std::string Body = body(liftEntries(Img, {SinTwice}), "sin_twice");
  ASSERT_FALSE(Body.empty());
  EXPECT_NE(Body.find("double sin_twice(double arg0)"), std::string::npos)
      << Body;
  // ADD's numerical bits and MXCSR complete together before the import.
  EXPECT_NE(Body.find("neverd_x86_fp_add_state_f64("), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("return sin("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
}
} // namespace
