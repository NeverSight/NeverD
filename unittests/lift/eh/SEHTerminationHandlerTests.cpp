//===- SEHTerminationHandlerTests.cpp - x64 __finally funclets -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows x64 termination handlers: the certified EstablisherFrame that an
/// out-of-line `__finally` callback receives, and the `__finally` bodies the C
/// routes print from it.
///
//===----------------------------------------------------------------------===//

#include "../core/SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t Entry = 0x140001000;
constexpr va_t Funclet = Entry + 0x80;

/// xcpt4's bar1 as MSVC x64 /O2 compiles it: the parent homes its Counter
/// argument, guards one call, and stores 100 on the normal path; the funclet
/// stores 99 or 100 through the home slot that `[rdx + 0x38]` names.
enum class Shape {
  Plain,
  /// An __except arm nested in the guarded range: the dispatcher resumes it
  /// with the SP of its own scope, which then joins the proof.
  NestedExcept,
  /// `push rax` between the prologue and the protected call.
  MovesSP,
  /// A termination handler that ignores the abnormal flag: it increments the
  /// parent's Counter the same way on either exit.
  Flagless,
};

struct ParentImage {
  BinaryImage Img;
  ExceptionFunction *Parent = nullptr;
};

void place(std::vector<uint8_t> &Data, size_t At,
           std::initializer_list<uint8_t> Bytes) {
  std::copy(Bytes.begin(), Bytes.end(), Data.begin() + At);
}

UnwindOperation unwindOp(UnwindOperationKind Kind, uint8_t Offset,
                         uint32_t StackOffset = 0, uint8_t Register = 0) {
  UnwindOperation Op;
  Op.Kind = Kind;
  Op.CodeOffset = Offset;
  Op.StackOffset = StackOffset;
  Op.Register = Register;
  return Op;
}

BinaryImage makeTerminationImage(Shape Kind) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0xa0;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  auto &D = Text.Data;
  // mov [rsp+0x10],rdx; push rbx; sub rsp,0x20
  place(D, 0x00, {0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83, 0xec, 0x20});
  va_t End = Entry + 0x1f;
  ExceptionAddressRange Guarded{Entry + 0x0d, Entry + 0x13};
  switch (Kind) {
  case Shape::Plain:
    // mov rbx,rdx; call foo; nop; mov dword [rbx],100; add rsp,0x20;
    // pop rbx; ret
    place(D, 0x0a,
          {0x48, 0x8b, 0xda, 0xe8, 0x2e, 0,    0,    0,    0x90, 0xc7, 0x03,
           0x64, 0,    0,    0,    0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
    break;
  case Shape::NestedExcept:
    // mov rbx,rdx; call foo; nop; jmp copy | handler: mov dword [rbx],99;
    // jmp copy | copy: mov dword [rbx],100; add rsp,0x20; pop rbx; ret
    place(D, 0x0a, {0x48, 0x8b, 0xda, 0xe8, 0x2e, 0, 0, 0, 0x90, 0xeb, 0x13});
    place(D, 0x20, {0xc7, 0x03, 0x63, 0, 0, 0, 0xeb, 0x00});
    place(D, 0x28,
          {0xc7, 0x03, 0x64, 0, 0, 0, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
    End = Entry + 0x34;
    Guarded = {Entry + 0x0d, Entry + 0x28};
    break;
  case Shape::MovesSP:
    // push rax; mov rbx,rdx; call foo; nop; pop rax; mov dword [rbx],100;
    // add rsp,0x20; pop rbx; ret
    place(D, 0x0a, {0x50, 0x48, 0x8b, 0xda, 0xe8, 0x2d, 0,   0,
                    0,    0x90, 0x58, 0xc7, 0x03, 0x64, 0,   0,
                    0,    0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
    End = Entry + 0x21;
    Guarded = {Entry + 0x0e, Entry + 0x14};
    break;
  case Shape::Flagless:
    // mov rbx,rdx; call foo; nop; mov dword [rbx],7; add rsp,0x20; pop rbx; ret
    place(D, 0x0a,
          {0x48, 0x8b, 0xda, 0xe8, 0x2e, 0,    0,    0,    0x90, 0xc7, 0x03,
           0x07, 0,    0,    0,    0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
    End = Entry + 0x1f;
    Guarded = {Entry + 0x0d, Entry + 0x13};
    break;
  }
  // foo: ret
  place(D, 0x40, {0xc3});
  if (Kind == Shape::Flagless)
    // A flag-independent handler: push rbp; sub rsp,0x20; mov rbp,rdx;
    // mov rax,[rbp+0x38]; mov dword [rax],7; add rsp,0x20; pop rbp; ret
    place(D, 0x80, {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xea,
                    0x48, 0x8b, 0x45, 0x38, 0xc7, 0x00, 0x07, 0,    0,
                    0,    0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3});
  else
    // The termination handler: push rbp; sub rsp,0x20; mov rbp,rdx;
    // movzx eax,cl; neg eax; sbb ecx,ecx; add ecx,100; mov rax,[rbp+0x38];
    // mov [rax],ecx; add rsp,0x20; pop rbp; ret
    place(D, 0x80,
          {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xea, 0x0f, 0xb6,
           0xc1, 0xf7, 0xd8, 0x1b, 0xc9, 0x83, 0xc1, 0x64, 0x48, 0x8b, 0x45,
           0x38, 0x89, 0x08, 0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3});
  Img.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.VA = Entry;
  Code.Size = 0xa0;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, End - Entry));
  Img.Symbols.push_back(Symbol::makeFunc(Entry + 0x40, 1));
  Img.Symbols.push_back(Symbol::makeFunc(Funclet, 0x1f));

  ExceptionFunction Parent;
  Parent.CodeRange = {Entry, End};
  Parent.Encoding = ExceptionEncoding::X64UnwindV1;
  Parent.UnwindVersion = 1;
  Parent.UnwindFlags = 3;
  Parent.PrologueSize = 0x0a;
  Parent.Personality = ExceptionPersonality::CSpecificHandler;
  Parent.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::AllocateSmall, 0x0a, 0x20));
  Parent.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::PushNonVolatile, 6, 0, 3));
  Parent.SEH.emplace();
  if (Kind == Shape::NestedExcept) {
    SEHScopeRecord Except;
    Except.GuardedRange = {Entry + 0x0d, Entry + 0x13};
    Except.Kind = SEHScopeKind::CatchAll;
    Except.HandlerVA = Entry + 0x20;
    Except.ContinuationVA = Entry + 0x20;
    Parent.SEH->Scopes.push_back(Except);
  }
  SEHScopeRecord Finally;
  Finally.GuardedRange = Guarded;
  Finally.Kind = SEHScopeKind::Finally;
  Finally.FilterOrFinallyVA = Funclet;
  Finally.HandlerVA = Funclet;
  Parent.SEH->Scopes.push_back(Finally);
  Img.ExceptionMetadata.Functions.push_back(std::move(Parent));

  ExceptionFunction Handler;
  Handler.CodeRange = {Funclet, Funclet + 0x1f};
  Handler.Encoding = ExceptionEncoding::X64UnwindV1;
  Handler.UnwindVersion = 1;
  Handler.PrologueSize = 6;
  Handler.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::AllocateSmall, 6, 0x20));
  Handler.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::PushNonVolatile, 2, 0, 5));
  Img.ExceptionMetadata.Functions.push_back(std::move(Handler));
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

LowFunc decodeParent(const BinaryImage &Img) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::X64));
  return CFGBuilder().build(Img, Dec, Entry, "bar1");
}

std::string
terminationFrameError(const LowFunc &Low, va_t Handler,
                      size_t Work = limits::kMaxSEHEstablisherFrameWork) {
  auto Bytes = proveLowSEHTerminationFrame(Low, Handler,
                                           getTargetRegInfo(Arch::X64), Work);
  if (Bytes)
    return "proved " + std::to_string(*Bytes);
  return llvm::toString(Bytes.takeError());
}

TEST(SEHTerminationFrame, ProloguePushAndAllocationLocateTheEstablisher) {
  // `push rbx; sub rsp, 0x20`: the funclet's `[rdx + 0x38]` is the parent's
  // `[entry SP + 0x10]`, where it homed Counter.
  const BinaryImage Img = makeTerminationImage(Shape::Plain);
  const LowFunc Low = decodeParent(Img);
  size_t Work = limits::kMaxSEHEstablisherFrameWork;
  auto Bytes = proveLowSEHTerminationFrame(Low, Funclet,
                                           getTargetRegInfo(Arch::X64), Work);
  ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
  EXPECT_EQ(*Bytes, 0x28u);
  EXPECT_EQ(collectLowSEHTerminationFrames(Low, Arch::X64),
            (std::map<va_t, uint64_t>{{Funclet, 0x28}}));
  // MedIR, and through it both C routes, carry the certified bytes.
  const MedFunc Med =
      LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  EXPECT_EQ(Med.SEHTerminationFrameBytes,
            (std::map<va_t, uint64_t>{{Funclet, 0x28}}));
}

TEST(SEHTerminationFrame, AgreesWithTheInFunctionHandlerFrame) {
  // The __except arm nested in the finally's range is resumed with the SP its
  // own scope establishes; both proofs count the same allocations.
  const BinaryImage Img = makeTerminationImage(Shape::NestedExcept);
  const LowFunc Low = decodeParent(Img);
  EXPECT_EQ(terminationFrameError(Low, Funclet), "proved 40");
  size_t Work = limits::kMaxSEHEstablisherFrameWork;
  auto Except = proveLowSEHEstablisherFrame(Low, Entry + 0x20,
                                            getTargetRegInfo(Arch::X64),
                                            /*OrdinaryEntry=*/false, Work);
  ASSERT_TRUE(bool(Except)) << llvm::toString(Except.takeError());
  EXPECT_EQ(Except->FrameBytes, 0x28u);
}

TEST(SEHTerminationFrame, RejectsAnSPWriteOnThePathIntoTheScope) {
  const BinaryImage Img = makeTerminationImage(Shape::MovesSP);
  const LowFunc Low = decodeParent(Img);
  EXPECT_NE(terminationFrameError(Low, Funclet)
                .find("SP is not stable from prologue through protected scope"),
            std::string::npos)
      << terminationFrameError(Low, Funclet);
  EXPECT_TRUE(collectLowSEHTerminationFrames(Low, Arch::X64).empty());
}

TEST(SEHTerminationFrame, RejectsUnwindDataTheDecodedPrologueContradicts) {
  BinaryImage Img = makeTerminationImage(Shape::Plain);
  Img.ExceptionMetadata.Functions.front().UnwindOperations.front().StackOffset =
      0x30;
  Img.ExceptionMetadata.rebuildIndex();
  const LowFunc Low = decodeParent(Img);
  EXPECT_NE(terminationFrameError(Low, Funclet)
                .find("decoded prologue disagrees with unwind allocation"),
            std::string::npos)
      << terminationFrameError(Low, Funclet);
}

TEST(SEHTerminationFrame, RejectsAFrameRegister) {
  // The establisher of a frame-register function is that register less its
  // scaled offset, not the prologue's SP.
  BinaryImage Img = makeTerminationImage(Shape::Plain);
  ExceptionFunction &Parent = Img.ExceptionMetadata.Functions.front();
  Parent.FrameRegister = 5;
  Parent.UnwindOperations.insert(
      Parent.UnwindOperations.begin(),
      unwindOp(UnwindOperationKind::SetFramePointer, 0x0a));
  Img.ExceptionMetadata.rebuildIndex();
  const LowFunc Low = decodeParent(Img);
  EXPECT_NE(terminationFrameError(Low, Funclet)
                .find("unsupported unwind or frame-register contract"),
            std::string::npos)
      << terminationFrameError(Low, Funclet);
}

TEST(SEHTerminationFrame, RejectsAHandlerThatTerminatesNoScope) {
  const BinaryImage Img = makeTerminationImage(Shape::Plain);
  const LowFunc Low = decodeParent(Img);
  EXPECT_NE(terminationFrameError(Low, Funclet + 0x10)
                .find("handler terminates no scope of this function"),
            std::string::npos);
  // A block of the parent is no out-of-line callback.
  EXPECT_NE(terminationFrameError(Low, Entry + 0x13)
                .find("unsupported unwind or frame-register contract"),
            std::string::npos);
}

TEST(SEHTerminationFrame, ExhaustedWorkRefuses) {
  const BinaryImage Img = makeTerminationImage(Shape::Plain);
  const LowFunc Low = decodeParent(Img);
  // A tiny budget cannot complete the proof, so it refuses rather than
  // certifying a frame.
  size_t Work = 3;
  auto Bytes = proveLowSEHTerminationFrame(Low, Funclet,
                                           getTargetRegInfo(Arch::X64), Work);
  EXPECT_FALSE(bool(Bytes)) << "proved " << *Bytes;
  if (!Bytes)
    llvm::consumeError(Bytes.takeError());
}

// The whole translation unit as HighC, which runs attachSEHFinallyBodies.
std::string decompileHighC(const BinaryImage &Img) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions COpts;
  COpts.TheArch = Arch::X64;
  COpts.Format = BinaryFormat::COFF;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COpts));
  OS.flush();
  return Source;
}

// The parent function's text alone (the funclet still prints standalone in a
// whole-unit view).
std::string parentText(const std::string &Source) {
  const std::string Marker = "sub_140001000(";
  const size_t At = Source.find(Marker);
  if (At == std::string::npos)
    return {};
  size_t Brace = Source.find('{', At);
  int Depth = 0;
  for (size_t I = Brace; I < Source.size(); ++I) {
    if (Source[I] == '{')
      ++Depth;
    else if (Source[I] == '}' && --Depth == 0)
      return Source.substr(At, I - At + 1);
  }
  return Source.substr(At);
}

TEST(SEHFinallyBody, GuardsTheAbnormalWorkAndKeepsTheNormalCopy) {
  // bar1's termination handler stores 99 on an unwind and 100 on normal exit.
  // The projected `__finally` runs the abnormal store only while unwinding and
  // keeps the compiler's inline normal-path copy, so it never runs twice.
  const std::string P =
      parentText(decompileHighC(makeTerminationImage(Shape::Plain)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("} __finally {"), std::string::npos) << P;
  EXPECT_NE(P.find("/* finally handler @ 0x140001080 */"), std::string::npos)
      << P;
  EXPECT_NE(P.find("if (AbnormalTermination())"), std::string::npos) << P;
  // The abnormal store (99) and the surviving normal-path copy (100), through
  // the homed Counter argument rather than a raw frame slot.
  EXPECT_NE(P.find("= 99"), std::string::npos) << P;
  EXPECT_NE(P.find("= 100"), std::string::npos) << P;
  EXPECT_NE(P.find("arg1"), std::string::npos) << P;
  EXPECT_EQ(P.find("var_"), std::string::npos) << P;
  EXPECT_EQ(P.find("rdx"), std::string::npos) << P;
}

TEST(SEHFinallyBody, FlagIndependentHandlerFillsTheClauseAndDropsTheCopy) {
  // A handler that does not read the abnormal flag runs the same on both
  // exits, so the clause holds the body itself and the inline copy is removed.
  const std::string P =
      parentText(decompileHighC(makeTerminationImage(Shape::Flagless)));
  ASSERT_FALSE(P.empty());
  const size_t Finally = P.find("} __finally {");
  ASSERT_NE(Finally, std::string::npos) << P;
  EXPECT_EQ(P.find("AbnormalTermination"), std::string::npos) << P;
  // The store appears once, inside the finally; the inline normal-path copy
  // was proved equal and removed, so it is not repeated after the try.
  size_t Count = 0;
  for (size_t At = P.find("= 7"); At != std::string::npos;
       At = P.find("= 7", At + 1))
    ++Count;
  EXPECT_EQ(Count, 1u) << P;
  EXPECT_NE(P.find("= 7", Finally), std::string::npos) << P;
}

TEST(SEHFinallyBody, FrameRegisterHandlerKeepsTheComment) {
  BinaryImage Img = makeTerminationImage(Shape::Plain);
  ExceptionFunction &Parent = Img.ExceptionMetadata.Functions.front();
  Parent.FrameRegister = 5;
  Parent.UnwindOperations.insert(
      Parent.UnwindOperations.begin(),
      unwindOp(UnwindOperationKind::SetFramePointer, 0x0a));
  Img.ExceptionMetadata.rebuildIndex();
  const std::string P = parentText(decompileHighC(Img));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("/* finally handler @ 0x140001080 */"), std::string::npos)
      << P;
  EXPECT_EQ(P.find("AbnormalTermination"), std::string::npos) << P;
}

TEST(SEHFinallyBody, LocalUnwindInTheProtectedBodyKeepsTheComment) {
  // A `return` out of a `__try` guarded by a finally compiles to
  // `_local_unwind`, which runs the handler with the flag set; C would read it
  // as an ordinary call followed by a normal exit, so the clause stays out of
  // line until that call is a structured jump.
  BinaryImage Img = makeTerminationImage(Shape::Plain);
  for (Symbol &S : Img.Symbols)
    if (S.Addr == Entry + 0x40) {
      S.Name = "_local_unwind";
      S.Origin = NameOrigin::Stated;
    }
  const std::string P = parentText(decompileHighC(Img));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("_local_unwind"), std::string::npos) << P;
  EXPECT_NE(P.find("/* finally handler @ 0x140001080 */"), std::string::npos)
      << P;
  EXPECT_EQ(P.find("AbnormalTermination"), std::string::npos) << P;
}

TEST(SEHFinallyBody, NestedExceptInTheProtectedBodyKeepsTheComment) {
  // The finally guards a `__try`/`__except`; a filled finally could run on the
  // goto that leaves the inner handler, so the body is left out of line.
  const std::string P =
      parentText(decompileHighC(makeTerminationImage(Shape::NestedExcept)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("/* finally handler @ 0x140001080 */"), std::string::npos)
      << P;
  EXPECT_EQ(P.find("AbnormalTermination"), std::string::npos) << P;
}

/// A `return` out of a `__try` guarded by a `__finally`, as MSVC x64 compiles
/// it: the return calls the runtime's local unwind with the frame and the
/// epilogue, which runs the termination handler with the flag set and
/// resumes there.  The parent homes Counter, then
/// `__try { if (Flag) return; foo(); } __finally { ... }`.
enum class LocalUnwindShape {
  /// The handler stores 99 on an unwind and 100 on a normal exit; the normal
  /// path keeps its inline copy (the 100 store).
  Guarded,
  /// The handler ignores the flag: it stores 7 on either exit.
  Flagless,
  /// The handler adds the flag to the counter, so the normal path has no
  /// copy; `__try { foo(); if (Flag) return; }` returns from the end of the
  /// protected body, where the try ends anyway.
  NoCopy,
  /// The handler branches on the flag, a shape the x64 projection keeps out
  /// of line.
  Branchy,
  /// The Guarded parent whose `__finally` returns as well: the handler
  /// local-unwinds the parent's frame to the same epilogue.
  ReturningFinally,
};

constexpr va_t LocalUnwindStub = Entry + 0x180;

BinaryImage makeLocalUnwindImage(LocalUnwindShape Kind) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0x181;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  auto &D = Text.Data;
  // mov [rsp+0x10],rdx; push rbx; sub rsp,0x20; mov rbx,rdx
  place(D, 0x00,
        {0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
         0xda});
  va_t End = 0;
  ExceptionAddressRange Guarded;
  if (Kind == LocalUnwindShape::NoCopy) {
    // call foo; test ecx,ecx; jz done; mov rcx,rsp; lea rdx,[done];
    // call _local_unwind; nop | done: add rsp,0x20; pop rbx; ret
    place(D, 0x0d,
          {0xe8, 0x2e, 0,    0,    0,    0x85, 0xc9, 0x74, 0x10, 0x48, 0x8b,
           0xcc, 0x48, 0x8d, 0x15, 0x06, 0,    0,    0,    0xe8, 0x5b, 0x01,
           0,    0,    0x90, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
    Guarded = {Entry + 0x0d, Entry + 0x26};
    End = Entry + 0x2c;
  } else {
    // test ecx,ecx; jz work; mov rcx,rsp; lea rdx,[done];
    // call _local_unwind; nop | work: call foo; nop |
    // mov dword [rbx],100 (or 7) | done: add rsp,0x20; pop rbx; ret
    place(D, 0x0d, {0x85, 0xc9, 0x74, 0x10, 0x48, 0x8b, 0xcc, 0x48, 0x8d,
                    0x15, 0x12, 0,    0,    0,    0xe8, 0x60, 0x01, 0,
                    0,    0x90, 0xe8, 0x1a, 0,    0,    0,    0x90});
    place(D, 0x27,
          {0xc7, 0x03,
           Kind == LocalUnwindShape::Flagless ? uint8_t(7) : uint8_t(100), 0, 0,
           0, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3});
    Guarded = {Entry + 0x0d, Entry + 0x27};
    End = Entry + 0x33;
  }
  // foo: ret
  place(D, 0x40, {0xc3});
  switch (Kind) {
  case LocalUnwindShape::Guarded:
    // push rbp; sub rsp,0x20; mov rbp,rdx; movzx eax,cl; neg eax;
    // sbb ecx,ecx; add ecx,100; mov rax,[rbp+0x38]; mov [rax],ecx;
    // add rsp,0x20; pop rbp; ret
    place(D, 0x80,
          {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xea, 0x0f, 0xb6,
           0xc1, 0xf7, 0xd8, 0x1b, 0xc9, 0x83, 0xc1, 0x64, 0x48, 0x8b, 0x45,
           0x38, 0x89, 0x08, 0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3});
    break;
  case LocalUnwindShape::Flagless:
    // push rbp; sub rsp,0x20; mov rbp,rdx; mov rax,[rbp+0x38];
    // mov dword [rax],7; add rsp,0x20; pop rbp; ret
    place(D, 0x80, {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xea,
                    0x48, 0x8b, 0x45, 0x38, 0xc7, 0x00, 0x07, 0,    0,
                    0,    0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3});
    break;
  case LocalUnwindShape::NoCopy:
    // push rbp; sub rsp,0x20; mov rbp,rdx; movzx eax,cl;
    // mov rdx,[rbp+0x38]; add [rdx],eax; add rsp,0x20; pop rbp; ret
    place(D, 0x80, {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
                    0xea, 0x0f, 0xb6, 0xc1, 0x48, 0x8b, 0x55, 0x38,
                    0x01, 0x02, 0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3});
    break;
  case LocalUnwindShape::Branchy:
    // push rbp; sub rsp,0x20; mov rbp,rdx; test cl,cl; jz done;
    // mov rax,[rbp+0x38]; mov dword [rax],99;
    // done: add rsp,0x20; pop rbp; ret
    place(D, 0x80, {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xea, 0x84,
                    0xc9, 0x74, 0x0a, 0x48, 0x8b, 0x45, 0x38, 0xc7, 0x00, 0x63,
                    0,    0,    0,    0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3});
    break;
  case LocalUnwindShape::ReturningFinally:
    // push rbp; sub rsp,0x20; mov rbp,rdx; mov rcx,rdx; lea rdx,[done];
    // call _local_unwind; nop; add rsp,0x20; pop rbp; ret
    place(D, 0x80,
          {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xea, 0x48, 0x8b,
           0xca, 0x48, 0x8d, 0x15, 0x9a, 0xff, 0xff, 0xff, 0xe8, 0xe8, 0,
           0,    0,    0x90, 0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3});
    break;
  }
  // The local unwind import's stub.
  D[0x180] = 0xc3;
  Img.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.VA = Entry;
  Code.Size = 0x181;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, End - Entry));
  Img.Symbols.push_back(Symbol::makeFunc(Entry + 0x40, 1));
  Img.Symbols.push_back(Symbol::makeFunc(Funclet, 0x20));
  Import LocalUnwind;
  LocalUnwind.Name = "_local_unwind";
  LocalUnwind.Module = "VCRUNTIME140.dll";
  Img.Imports.push_back(LocalUnwind);
  EXPECT_TRUE(Img.recordImportStub(LocalUnwindStub, 0));

  ExceptionFunction Parent;
  Parent.CodeRange = {Entry, End};
  Parent.Encoding = ExceptionEncoding::X64UnwindV1;
  Parent.UnwindVersion = 1;
  Parent.UnwindFlags = 3;
  Parent.PrologueSize = 0x0a;
  Parent.Personality = ExceptionPersonality::CSpecificHandler;
  Parent.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::AllocateSmall, 0x0a, 0x20));
  Parent.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::PushNonVolatile, 6, 0, 3));
  Parent.SEH.emplace();
  SEHScopeRecord Finally;
  Finally.GuardedRange = Guarded;
  Finally.Kind = SEHScopeKind::Finally;
  Finally.FilterOrFinallyVA = Funclet;
  Finally.HandlerVA = Funclet;
  Parent.SEH->Scopes.push_back(Finally);
  Img.ExceptionMetadata.Functions.push_back(std::move(Parent));

  ExceptionFunction Handler;
  Handler.CodeRange = {Funclet, Funclet + 0x20};
  Handler.Encoding = ExceptionEncoding::X64UnwindV1;
  Handler.UnwindVersion = 1;
  Handler.PrologueSize = 6;
  Handler.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::AllocateSmall, 6, 0x20));
  Handler.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::PushNonVolatile, 2, 0, 5));
  Img.ExceptionMetadata.Functions.push_back(std::move(Handler));
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

/// How the saved-frame parent below treats its GS cookie.
enum class CookieShape {
  /// The GS data places the cookie, which the prologue masks with the frame.
  Masked,
  /// The same code without GS data: nothing places the masked frame.
  Undeclared,
  /// The GS data places the cookie, but the prologue stores the frame there
  /// unmasked.
  Unmasked,
};

/// The Guarded shape with a GS cookie and MSVC's saved-frame slot: the
/// prologue saves the frame at [rsp+0x30] and the local unwind reloads it.
/// The parent homes Counter, `__try { if (Flag) return; foo(); }
/// __finally { *Counter = abnormal ? 99 : 100; }`, and checks its cookie at
/// [rsp+0x38] before it returns.
BinaryImage makeCookieImage(CookieShape Kind) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0x181;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  auto &D = Text.Data;
  // mov [rsp+0x10],rdx; push rbx; sub rsp,0x40; mov rbx,rdx;
  // mov rax,[cookie]; xor rax,rsp (or mov rax,rsp); mov [rsp+0x38],rax;
  // mov [rsp+0x30],rsp
  place(D, 0x00,
        {0x48, 0x89,
         0x54, 0x24,
         0x10, 0x53,
         0x48, 0x83,
         0xec, 0x40,
         0x48, 0x8b,
         0xda, 0x48,
         0x8b, 0x05,
         0x5c, 0x01,
         0,    0,
         0x48, Kind == CookieShape::Unmasked ? uint8_t(0x8b) : uint8_t(0x33),
         0xc4, 0x48,
         0x89, 0x44,
         0x24, 0x38,
         0x48, 0x89,
         0x64, 0x24,
         0x30});
  // test ecx,ecx; jz work; mov rcx,[rsp+0x30]; lea rdx,[done];
  // call _local_unwind; nop | work: call foo; nop | mov dword [rbx],100 |
  // done: mov rcx,[rsp+0x38]; xor rcx,rsp; call check; add rsp,0x40;
  // pop rbx; ret
  place(D, 0x21,
        {0x85, 0xc9, 0x74, 0x12, 0x48, 0x8b, 0x4c, 0x24, 0x30, 0x48, 0x8d,
         0x15, 0x12, 0,    0,    0,    0xe8, 0x4a, 0x01, 0,    0,    0x90,
         0xe8, 0x24, 0,    0,    0,    0x90, 0xc7, 0x03, 0x64, 0,    0,
         0,    0x48, 0x8b, 0x4c, 0x24, 0x38, 0x48, 0x33, 0xcc, 0xe8, 0x18,
         0,    0,    0,    0x48, 0x83, 0xc4, 0x40, 0x5b, 0xc3});
  // foo: ret | check: ret
  place(D, 0x60, {0xc3});
  place(D, 0x68, {0xc3});
  // The Guarded handler, which finds Counter's home 0x58 above its frame.
  place(D, 0x80,
        {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xea, 0x0f, 0xb6,
         0xc1, 0xf7, 0xd8, 0x1b, 0xc9, 0x83, 0xc1, 0x64, 0x48, 0x8b, 0x45,
         0x58, 0x89, 0x08, 0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3});
  // The cookie, then the local unwind import's stub.
  place(D, 0x170, {0x32, 0xa2, 0xdf, 0x2d, 0x99, 0x2b, 0, 0});
  D[0x180] = 0xc3;
  Img.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.VA = Entry;
  Code.Size = 0x181;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, 0x56));
  Img.Symbols.push_back(Symbol::makeFunc(Entry + 0x60, 1));
  Img.Symbols.push_back(Symbol::makeFunc(Entry + 0x68, 1));
  Img.Symbols.push_back(Symbol::makeFunc(Funclet, 0x20));
  Import LocalUnwind;
  LocalUnwind.Name = "_local_unwind";
  LocalUnwind.Module = "VCRUNTIME140.dll";
  Img.Imports.push_back(LocalUnwind);
  EXPECT_TRUE(Img.recordImportStub(LocalUnwindStub, 0));

  ExceptionFunction Parent;
  Parent.CodeRange = {Entry, Entry + 0x56};
  Parent.Encoding = ExceptionEncoding::X64UnwindV1;
  Parent.UnwindVersion = 1;
  Parent.UnwindFlags = 3;
  Parent.PrologueSize = 0x0a;
  Parent.Personality = ExceptionPersonality::CSpecificHandler;
  Parent.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::AllocateSmall, 0x0a, 0x40));
  Parent.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::PushNonVolatile, 6, 0, 3));
  Parent.SEH.emplace();
  SEHScopeRecord Finally;
  Finally.GuardedRange = {Entry + 0x21, Entry + 0x3d};
  Finally.Kind = SEHScopeKind::Finally;
  Finally.FilterOrFinallyVA = Funclet;
  Finally.HandlerVA = Funclet;
  Parent.SEH->Scopes.push_back(Finally);
  if (Kind != CookieShape::Undeclared) {
    GSCookieInfo Cookie;
    Cookie.ParseStatus = ExceptionParseStatus::Complete;
    Cookie.CookieOffset = 0x38;
    Parent.GSCookie = Cookie;
  }
  Img.ExceptionMetadata.Functions.push_back(std::move(Parent));

  ExceptionFunction Handler;
  Handler.CodeRange = {Funclet, Funclet + 0x20};
  Handler.Encoding = ExceptionEncoding::X64UnwindV1;
  Handler.UnwindVersion = 1;
  Handler.PrologueSize = 6;
  Handler.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::AllocateSmall, 6, 0x20));
  Handler.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::PushNonVolatile, 2, 0, 5));
  Img.ExceptionMetadata.Functions.push_back(std::move(Handler));
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

/// Who receives the frame in makeSetjmpImage.
enum class FrameTaker {
  /// The runtime's `__intrinsic_setjmpex`.
  Setjmp,
  /// A function of the image that reads it.
  Reader,
  /// A function of the image that reads only the jump buffer, with the frame
  /// in R8: the unread-register proof never covers RDX, which a callee that
  /// leaves it alone would hand back as the high half of a result.
  Ignorer,
};

/// The Guarded shape with MSVC's saved-frame slot, and a setjmp that takes
/// the frame first: `setjmp(Buffer)` passes the jump buffer and the
/// EstablisherFrame, as MSVC x64 calls `__intrinsic_setjmpex`.  \p Taker
/// says what the call enters.
BinaryImage makeSetjmpImage(FrameTaker Taker) {
  const bool Authenticated = Taker == FrameTaker::Setjmp;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0x181;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  auto &D = Text.Data;
  // mov [rsp+0x10],rdx; push rbx; sub rsp,0x40; mov rbx,rdx;
  // mov [rsp+0x30],rsp; mov [rsp+0x28],ecx; lea rcx,[rsp+0x38];
  // mov rdx,rsp; call setjmp; mov ecx,[rsp+0x28]
  place(D, 0x00, {0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83, 0xec, 0x40,
                  0x48, 0x8b, 0xda, 0x48, 0x89, 0x64, 0x24, 0x30, 0x89, 0x4c,
                  0x24, 0x28, 0x48, 0x8d, 0x4c, 0x24, 0x38, 0x48, 0x8b, 0xd4,
                  0xe8, 0x55, 0x01, 0,    0,    0x8b, 0x4c, 0x24, 0x28});
  if (Taker != FrameTaker::Setjmp) {
    // call the image's function at 0x68 instead
    D[0x1f] = 0x45;
    D[0x20] = 0;
  }
  if (Taker == FrameTaker::Ignorer) {
    // mov r8,rsp
    D[0x1b] = 0x4c;
    D[0x1d] = 0xc4;
  }
  // test ecx,ecx; jz work; mov rcx,[rsp+0x30]; lea rdx,[done];
  // call _local_unwind; nop | work: call foo; nop | mov dword [rbx],100 |
  // done: add rsp,0x40; pop rbx; ret
  place(D, 0x27, {0x85, 0xc9, 0x74, 0x12, 0x48, 0x8b, 0x4c, 0x24, 0x30, 0x48,
                  0x8d, 0x15, 0x12, 0,    0,    0,    0xe8, 0x44, 0x01, 0,
                  0,    0x90, 0xe8, 0x1e, 0,    0,    0,    0x90, 0xc7, 0x03,
                  0x64, 0,    0,    0,    0x48, 0x83, 0xc4, 0x40, 0x5b, 0xc3});
  // foo: ret | reader: mov rax,rdx; ret (or the ignorer: mov rax,rcx; ret)
  place(D, 0x60, {0xc3});
  place(D, 0x68,
        {0x48, 0x89,
         Taker == FrameTaker::Ignorer ? uint8_t(0xc8) : uint8_t(0xd0), 0xc3});
  // The Guarded handler, which finds Counter's home 0x58 above its frame.
  place(D, 0x80,
        {0x40, 0x55, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xea, 0x0f, 0xb6,
         0xc1, 0xf7, 0xd8, 0x1b, 0xc9, 0x83, 0xc1, 0x64, 0x48, 0x8b, 0x45,
         0x58, 0x89, 0x08, 0x48, 0x83, 0xc4, 0x20, 0x5d, 0xc3});
  // The setjmp and local unwind imports' stubs.
  D[0x178] = 0xc3;
  D[0x180] = 0xc3;
  Img.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.VA = Entry;
  Code.Size = 0x181;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, 0x4f));
  Img.Symbols.push_back(Symbol::makeFunc(Entry + 0x60, 1));
  Img.Symbols.push_back(Symbol::makeFunc(Entry + 0x68, 4));
  Img.Symbols.push_back(Symbol::makeFunc(Funclet, 0x20));
  Import LocalUnwind;
  LocalUnwind.Name = "_local_unwind";
  LocalUnwind.Module = "VCRUNTIME140.dll";
  Img.Imports.push_back(LocalUnwind);
  EXPECT_TRUE(Img.recordImportStub(LocalUnwindStub, 0));
  Import Setjmp;
  Setjmp.Name = "__intrinsic_setjmpex";
  Setjmp.Module = "VCRUNTIME140.dll";
  Img.Imports.push_back(Setjmp);
  EXPECT_TRUE(Img.recordImportStub(Entry + 0x178, 1));

  ExceptionFunction Parent;
  Parent.CodeRange = {Entry, Entry + 0x4f};
  Parent.Encoding = ExceptionEncoding::X64UnwindV1;
  Parent.UnwindVersion = 1;
  Parent.UnwindFlags = 3;
  Parent.PrologueSize = 0x0a;
  Parent.Personality = ExceptionPersonality::CSpecificHandler;
  Parent.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::AllocateSmall, 0x0a, 0x40));
  Parent.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::PushNonVolatile, 6, 0, 3));
  Parent.SEH.emplace();
  SEHScopeRecord Finally;
  Finally.GuardedRange = {Entry + 0x27, Entry + 0x43};
  Finally.Kind = SEHScopeKind::Finally;
  Finally.FilterOrFinallyVA = Funclet;
  Finally.HandlerVA = Funclet;
  Parent.SEH->Scopes.push_back(Finally);
  Img.ExceptionMetadata.Functions.push_back(std::move(Parent));

  ExceptionFunction Handler;
  Handler.CodeRange = {Funclet, Funclet + 0x20};
  Handler.Encoding = ExceptionEncoding::X64UnwindV1;
  Handler.UnwindVersion = 1;
  Handler.PrologueSize = 6;
  Handler.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::AllocateSmall, 6, 0x20));
  Handler.UnwindOperations.push_back(
      unwindOp(UnwindOperationKind::PushNonVolatile, 2, 0, 5));
  Img.ExceptionMetadata.Functions.push_back(std::move(Handler));
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

size_t occurrences(const std::string &Text, const std::string &Needle) {
  size_t Count = 0;
  for (size_t At = Text.find(Needle); At != std::string::npos;
       At = Text.find(Needle, At + 1))
    ++Count;
  return Count;
}

TEST(SEHLocalUnwind, AReturnOutOfTheTryRunsTheFilledFinallyOnce) {
  // The local unwind becomes the jump it is. The clause runs the handler's
  // unwind work, as C runs a `__finally` for a return out of its `__try`; the
  // compiler's normal-path copy stays for the normal exit. Each exit runs the
  // handler once: the clause on the return, the copy on the fall-through.
  const std::string Full =
      decompileHighC(makeLocalUnwindImage(LocalUnwindShape::Guarded));
  const std::string P = parentText(Full);
  ASSERT_FALSE(P.empty()) << Full;
  EXPECT_EQ(P.find("_local_unwind"), std::string::npos) << P;
  EXPECT_EQ(P.find("0x14000102D"), std::string::npos) << P;
  EXPECT_EQ(P.find("sub_140001080("), std::string::npos) << P;
  const size_t Try = P.find("__try {");
  const size_t Finally = P.find("} __finally {");
  ASSERT_NE(Try, std::string::npos) << P;
  ASSERT_NE(Finally, std::string::npos) << P;
  const size_t Jump = std::min(P.find("goto ", Try), P.find("return", Try));
  EXPECT_LT(Jump, Finally) << P;
  EXPECT_NE(P.find("if (AbnormalTermination())", Finally), std::string::npos)
      << P;
  EXPECT_EQ(occurrences(P, "= 99"), 1u) << P;
  EXPECT_GT(P.find("= 99"), Finally) << P;
  EXPECT_EQ(occurrences(P, "= 100"), 1u) << P;
  EXPECT_GT(P.find("= 100"), Finally) << P;
}

TEST(SEHLocalUnwind, AFlagIndependentHandlerRunsInTheClauseOnEitherExit) {
  // The clause holds the whole body and the normal-path copy goes: the
  // return and the fall-through each run it once, through the clause.
  const std::string P = parentText(
      decompileHighC(makeLocalUnwindImage(LocalUnwindShape::Flagless)));
  ASSERT_FALSE(P.empty());
  EXPECT_EQ(P.find("_local_unwind"), std::string::npos) << P;
  EXPECT_EQ(P.find("sub_140001080("), std::string::npos) << P;
  const size_t Finally = P.find("} __finally {");
  ASSERT_NE(Finally, std::string::npos) << P;
  EXPECT_EQ(P.find("AbnormalTermination"), std::string::npos) << P;
  EXPECT_EQ(occurrences(P, "= 7"), 1u) << P;
  EXPECT_GT(P.find("= 7"), Finally) << P;
  EXPECT_EQ(P.find("__leave"), std::string::npos) << P;
}

TEST(SEHLocalUnwind, AJumpToWhereTheTryEndsStaysAJump) {
  // The unwind resumes right after the try. Falling through, or `__leave`,
  // would end the try normally and skip the clause; the jump must stay.
  const std::string P = parentText(
      decompileHighC(makeLocalUnwindImage(LocalUnwindShape::NoCopy)));
  ASSERT_FALSE(P.empty());
  EXPECT_EQ(P.find("_local_unwind"), std::string::npos) << P;
  EXPECT_EQ(P.find("sub_140001080("), std::string::npos) << P;
  EXPECT_EQ(P.find("__leave"), std::string::npos) << P;
  const size_t Try = P.find("__try {");
  const size_t Finally = P.find("} __finally {");
  ASSERT_NE(Try, std::string::npos) << P;
  ASSERT_NE(Finally, std::string::npos) << P;
  const size_t Jump = std::min(P.find("goto ", Try), P.find("return", Try));
  EXPECT_LT(Jump, Finally) << P;
  EXPECT_NE(P.find("if (AbnormalTermination())", Finally), std::string::npos)
      << P;
}

TEST(SEHLocalUnwind, AnUnfilledFinallyRunsAsAnExplicitHandlerCall) {
  // A handler shape the clause cannot hold keeps its address comment; the
  // jump then calls the handler itself, as the runtime did, with the
  // abnormal flag set and the frame, so it still runs exactly once.
  const std::string P = parentText(
      decompileHighC(makeLocalUnwindImage(LocalUnwindShape::Branchy)));
  ASSERT_FALSE(P.empty());
  EXPECT_EQ(P.find("_local_unwind"), std::string::npos) << P;
  EXPECT_EQ(P.find("AbnormalTermination"), std::string::npos) << P;
  EXPECT_EQ(occurrences(P, "sub_140001080(1, "), 1u) << P;
  const size_t Call = P.find("sub_140001080(1, ");
  const size_t Finally = P.find("} __finally {");
  ASSERT_NE(Finally, std::string::npos) << P;
  EXPECT_LT(Call, Finally) << P;
  EXPECT_NE(P.find("/* finally handler @ 0x140001080 */"), std::string::npos)
      << P;
}

TEST(SEHLocalUnwind, AGSCookieMasksTheFrameItsDataPlaces) {
  // The prologue stores the frame XORed with the cookie in the slot the GS
  // data names, and the epilogue XORs it with the frame again before the
  // check: the frame itself reaches no memory and no callee, so the
  // saved-frame slot still certifies and the return becomes its jump.
  const std::string Full = decompileHighC(makeCookieImage(CookieShape::Masked));
  const std::string P = parentText(Full);
  ASSERT_FALSE(P.empty()) << Full;
  EXPECT_EQ(P.find("_local_unwind"), std::string::npos) << P;
  EXPECT_EQ(P.find("sub_140001080("), std::string::npos) << P;
  const size_t Finally = P.find("} __finally {");
  ASSERT_NE(Finally, std::string::npos) << P;
  EXPECT_NE(P.find("if (AbnormalTermination())", Finally), std::string::npos)
      << P;
}

TEST(SEHLocalUnwind, AMaskedFrameNoGSDataPlacesKeepsTheCall) {
  // Without GS data, the masked frame lands in an ordinary slot, where any
  // reader could unmask it: the call keeps its runtime form.
  const std::string P =
      parentText(decompileHighC(makeCookieImage(CookieShape::Undeclared)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("_local_unwind"), std::string::npos) << P;
}

TEST(SEHLocalUnwind, AnUnmaskedFrameInTheCookieSlotKeepsTheCall) {
  // The GS data names the slot, but the prologue stores the frame there as
  // it is: the slot is written another way.
  const std::string P =
      parentText(decompileHighC(makeCookieImage(CookieShape::Unmasked)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("_local_unwind"), std::string::npos) << P;
}

TEST(SEHLocalUnwind, AnAuthenticatedSetjmpTakesTheFrame) {
  // The runtime's setjmp keeps the frame in the jump buffer, where only
  // longjmp reads it to unwind back here: the saved-frame slot certifies.
  const std::string P =
      parentText(decompileHighC(makeSetjmpImage(FrameTaker::Setjmp)));
  ASSERT_FALSE(P.empty());
  EXPECT_EQ(P.find("_local_unwind"), std::string::npos) << P;
}

TEST(SEHLocalUnwind, AFrameAFunctionOfTheImageReadsKeepsTheCall) {
  // The same frame handed to code of the image that reads it may reach the
  // saved-frame slot: the call keeps its runtime form.
  const std::string P =
      parentText(decompileHighC(makeSetjmpImage(FrameTaker::Reader)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("_local_unwind"), std::string::npos) << P;
}

TEST(SEHLocalUnwind, AFrameItsCalleeNeverObservesPassesNothing) {
  // The frame left in R8 for a function of the image whose register
  // summary proves it never observes R8 reaches nothing there.
  const std::string P =
      parentText(decompileHighC(makeSetjmpImage(FrameTaker::Ignorer)));
  ASSERT_FALSE(P.empty());
  EXPECT_EQ(P.find("_local_unwind"), std::string::npos) << P;
}

TEST(SEHLocalUnwind, AFinallyThatUnwindsToTheTargetKeepsItARoot) {
  // Only the parent's own exit names its epilogue in the Guarded shape, so
  // the exit owns it and no proof over the decoded CFG takes it as an
  // independent entry.  A `__finally` that returns too local-unwinds to the
  // same epilogue from the handler: the target stays a root.
  for (const auto &[Kind, Owns] :
       {std::pair{LocalUnwindShape::Guarded, true},
        std::pair{LocalUnwindShape::ReturningFinally, false}}) {
    SCOPED_TRACE(Owns);
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    const BinaryImage Img = makeLocalUnwindImage(Kind);
    auto Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    const auto Parent = llvm::find_if(
        Result.HighFuncs, [](const HighFunc &F) { return F.Entry == Entry; });
    ASSERT_NE(Parent, Result.HighFuncs.end());
    ASSERT_EQ(Parent->SEHLocalUnwindExits.size(), 1u);
    EXPECT_EQ(Parent->SEHLocalUnwindExits.front().Target, Entry + 0x2d);
    EXPECT_EQ(Parent->SEHLocalUnwindExits.front().OwnsTarget, Owns);
  }
}

} // namespace
