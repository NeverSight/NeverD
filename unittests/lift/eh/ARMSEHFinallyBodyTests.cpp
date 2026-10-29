//===- ARMSEHFinallyBodyTests.cpp - ARM __finally funclets ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM and AArch64 termination handlers read the parent's callee-saved
/// registers, which the unwinder restores to their values where the exception
/// struck: the values buildSsa proves for the protected ranges, and the
/// `__finally` bodies HighC prints from them.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/ARM/ARMRegs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t Entry = 0x140001000;
constexpr va_t Callee = Entry + 0x40;
constexpr va_t Funclet = Entry + 0x80;
constexpr va_t OuterFunclet = Entry + 0x90;

enum class Parent {
  /// `x19 = sp + 0x10` before the range, which only calls foo.
  FrameAddress,
  /// The range also covers `x19 = sp + 0x10`: where the exception struck
  /// decides x19.
  WrittenInRange,
  /// `x19 = x0` and `x20 = 5` before the range; both are read after it.
  ArgumentAndConstant,
  /// x1, an argument register the parent writes before it reads, joins 1 and
  /// 2 at a PHI that x19 copies before the range.
  JoinedArgumentRegister,
  /// The finally range nested in an __except range.
  NestedInExcept,
  /// The finally range nested in another finally range, whose handler stores
  /// 7 through x19 while unwinding.
  NestedInFinally,
  /// xcpt4's bar1 at /O0: the frame the prologue leaves, which the handler
  /// takes as its establisher frame, holds the homed Counter pointer.
  EstablisherFrame,
  /// The same code under unwind codes that claim a larger frame.
  EstablisherFrameMismatch,
  /// clang-cl's shape: `sub sp,sp,#0x30; stp x29,x30,[sp,#0x10];
  /// add x29,sp,#0x10`, so the frame pointer is 0x10 above the stack pointer.
  FramePointerAboveTheStack,
};

enum class Handler {
  /// `if (abnormal) *(int *)x19 = 99`.
  StoresThroughX19,
  /// `if (abnormal) *(int *)x20 = 99`; the parent never sets x20.
  StoresThroughX20,
  /// `if (abnormal) **(int **)(x1 + 0x18) = 99`, through the establisher
  /// frame.
  StoresThroughTheFrame,
};

constexpr uint32_t Ret = 0xd65f03c0;

BinaryImage makeImage(Parent P, Handler H) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  std::vector<uint32_t> Words(0xa0 / 4, 0xd503201f); // nop
  std::vector<uint32_t> Body;
  ExceptionAddressRange Finally{Entry + 0x0c, Entry + 0x10};
  switch (P) {
  case Parent::FrameAddress:
  case Parent::WrittenInRange:
    // stp x19,x30,[sp,#-0x20]!; add x19,sp,#0x10; str wzr,[x19]; bl foo;
    // ldr w0,[x19]; ldp x19,x30,[sp],#0x20; ret
    Body = {0xa9be7bf3, 0x910043f3, 0xb900027f, 0x9400000d,
            0xb9400260, 0xa8c27bf3, Ret};
    if (P == Parent::WrittenInRange)
      Finally = {Entry + 0x04, Entry + 0x10};
    break;
  case Parent::ArgumentAndConstant:
    // stp x19,x30,[sp,#-0x20]!; mov x19,x0; mov x20,#5; bl foo;
    // add x0,x19,x20; ldp x19,x30,[sp],#0x20; ret
    Body = {0xa9be7bf3, 0xaa0003f3, 0xd28000b4, 0x9400000d,
            0x8b140260, 0xa8c27bf3, Ret};
    break;
  case Parent::NestedInExcept:
    // stp x19,x30,[sp,#-0x20]!; add x19,sp,#0x10; str wzr,[x19]; bl foo;
    // nop; b done | handler: str wzr,[x19] | done: ldr w0,[x19];
    // ldp x19,x30,[sp],#0x20; ret
    Body = {0xa9be7bf3, 0x910043f3, 0xb900027f, 0x9400000d, 0xd503201f,
            0x14000002, 0xb900027f, 0xb9400260, 0xa8c27bf3, Ret};
    break;
  case Parent::JoinedArgumentRegister:
    // stp x19,x30,[sp,#-0x20]!; cbz x0,two; mov x1,#1; b join |
    // two: mov x1,#2 | join: mov x19,x1; bl foo; mov x0,x19;
    // ldp x19,x30,[sp],#0x20; ret
    Body = {0xa9be7bf3, 0xb4000060, 0xd2800021, 0x14000002, 0xd2800041,
            0xaa0103f3, 0x9400000a, 0xaa1303e0, 0xa8c27bf3, Ret};
    Finally = {Entry + 0x18, Entry + 0x1c};
    break;
  case Parent::FramePointerAboveTheStack:
    // sub sp,sp,#0x30; stp x29,x30,[sp,#0x10]; add x29,sp,#0x10;
    // stur x1,[x29,#-8]; bl foo; ldur x8,[x29,#-8]; mov w9,#100;
    // str w9,[x8]; ldp x29,x30,[sp,#0x10]; add sp,sp,#0x30; ret
    Body = {0xd100c3ff, 0xa9017bfd, 0x910043fd, 0xf81f83a1,
            0x9400000c, 0xf85f83a8, 0x52800c89, 0xb9000109,
            0xa9417bfd, 0x9100c3ff, Ret};
    Finally = {Entry + 0x10, Entry + 0x14};
    break;
  case Parent::NestedInFinally:
    // stp x19,x30,[sp,#-0x20]!; add x19,sp,#0x10; str wzr,[x19]; bl foo;
    // nop; ldr w0,[x19]; ldp x19,x30,[sp],#0x20; ret
    Body = {0xa9be7bf3, 0x910043f3, 0xb900027f, 0x9400000d,
            0xd503201f, 0xb9400260, 0xa8c27bf3, Ret};
    break;
  case Parent::EstablisherFrame:
  case Parent::EstablisherFrameMismatch:
    // stp x29,x30,[sp,#-0x20]!; mov x29,sp; str x1,[sp,#0x18]; bl foo;
    // ldr x8,[sp,#0x18]; mov w9,#100; str w9,[x8]; ldp x29,x30,[sp],#0x20;
    // ret
    Body = {0xa9be7bfd, 0x910003fd, 0xf9000fe1, 0x9400000d, 0xf9400fe8,
            0x52800c89, 0xb9000109, 0xa8c27bfd, Ret};
    break;
  }
  std::copy(Body.begin(), Body.end(), Words.begin());
  const va_t End = Entry + Body.size() * 4;
  Words[(Callee - Entry) / 4] = Ret;
  // cbz w0,done; mov w8,#99; str w8,[x19 or x20] | done: ret
  const uint32_t Store =
      H == Handler::StoresThroughX19 ? 0xb9000268 : 0xb9000288;
  std::vector<uint32_t> HandlerCode = {0x34000060, 0x52800c68, Store, Ret};
  // ldr x8,[x1,#0x18]; cbz w0,done; mov w9,#99; str w9,[x8] | done: ret
  if (H == Handler::StoresThroughTheFrame)
    HandlerCode = {0xf9400c28, 0x34000060, 0x52800c69, 0xb9000109, Ret};
  std::copy(HandlerCode.begin(), HandlerCode.end(),
            Words.begin() + (Funclet - Entry) / 4);
  // cbz w0,done; mov w8,#7; str w8,[x19] | done: ret
  const uint32_t OuterCode[] = {0x34000060, 0x528000e8, 0xb9000268, Ret};
  std::copy(std::begin(OuterCode), std::end(OuterCode),
            Words.begin() + (OuterFunclet - Entry) / 4);

  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = Words.size() * 4;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  std::memcpy(Text.Data.data(), Words.data(), Text.Size);
  Img.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.VA = Entry;
  Code.Size = Words.size() * 4;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, End - Entry));
  Img.Symbols.push_back(Symbol::makeFunc(Callee, 4));
  Img.Symbols.push_back(Symbol::makeFunc(Funclet, 0x10));
  Img.Symbols.push_back(Symbol::makeFunc(OuterFunclet, 0x10));

  ExceptionFunction EH;
  EH.CodeRange = {Entry, End};
  EH.Encoding = ExceptionEncoding::ARM64Unpacked;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  if (P == Parent::FramePointerAboveTheStack) {
    // add_fp #0x10; save_fplr #0x10; alloc_s #0x30; end
    UnwindOperation AddFP;
    AddFP.Kind = UnwindOperationKind::AddFramePointer;
    AddFP.Register = 29;
    AddFP.StackOffset = 0x10;
    AddFP.RegisterClass = UnwindRegisterClass::GeneralPurpose;
    UnwindOperation Save;
    Save.Kind = UnwindOperationKind::SaveRegisterPair;
    Save.StackOffset = 0x10;
    Save.Register = 29;
    Save.RegisterClass = UnwindRegisterClass::GeneralPurpose;
    Save.RegisterMask = (1u << 29) | (1u << 30);
    UnwindOperation Alloc;
    Alloc.Kind = UnwindOperationKind::AllocateStack;
    Alloc.StackOffset = 0x30;
    UnwindOperation Done;
    Done.Kind = UnwindOperationKind::End;
    EH.UnwindOperations = {AddFP, Save, Alloc, Done};
  }
  if (P == Parent::EstablisherFrame || P == Parent::EstablisherFrameMismatch) {
    // save_fplr_x #0x20 (or #0x30); set_fp; end
    UnwindOperation Save;
    Save.Kind = UnwindOperationKind::SaveRegisterPairPreIndexed;
    Save.StackOffset = P == Parent::EstablisherFrame ? 0x20 : 0x30;
    Save.Register = 29;
    Save.RegisterClass = UnwindRegisterClass::GeneralPurpose;
    Save.RegisterMask = (1u << 29) | (1u << 30);
    UnwindOperation SetFP;
    SetFP.Kind = UnwindOperationKind::SetFramePointer;
    SetFP.Register = 29;
    SetFP.RegisterClass = UnwindRegisterClass::GeneralPurpose;
    UnwindOperation Done;
    Done.Kind = UnwindOperationKind::End;
    EH.UnwindOperations = {SetFP, Save, Done};
  }
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = Finally;
  Scope.Kind = SEHScopeKind::Finally;
  Scope.FilterOrFinallyVA = Funclet;
  Scope.HandlerVA = Funclet;
  EH.SEH->Scopes.push_back(Scope);
  if (P == Parent::NestedInFinally) {
    SEHScopeRecord Outer;
    Outer.GuardedRange = {Entry + 0x0c, Entry + 0x14};
    Outer.Kind = SEHScopeKind::Finally;
    Outer.FilterOrFinallyVA = OuterFunclet;
    Outer.HandlerVA = OuterFunclet;
    EH.SEH->Scopes.push_back(Outer);
  }
  if (P == Parent::NestedInExcept) {
    SEHScopeRecord Except;
    Except.GuardedRange = {Entry + 0x0c, Entry + 0x14};
    Except.Kind = SEHScopeKind::CatchAll;
    Except.HandlerVA = Entry + 0x18;
    Except.ContinuationVA = Entry + 0x18;
    EH.SEH->Scopes.push_back(Except);
  }
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

MedFunc convertParent(const BinaryImage &Img) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::AArch64));
  auto Low = CFGBuilder().build(Img, Dec, Img.Entry, "aarch64_finally_parent");
  return LowToMedConverter().convert(Low, Arch::AArch64, BinaryFormat::COFF);
}

const SEHFuncletRegisterValue *registerValue(const MedFunc &Med,
                                             uint64_t RegOff) {
  auto Registers = Med.SEHFuncletRegisters.find(Funclet);
  if (Registers == Med.SEHFuncletRegisters.end())
    return nullptr;
  auto It = Registers->second.find(RegOff);
  return It == Registers->second.end() ? nullptr : &It->second;
}

// The whole translation unit as HighC, which runs attachSEHFinallyBodies.
std::string decompileHighC(const BinaryImage &Img) {
  const Arch TargetArch = Img.Arch;
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions COpts;
  COpts.TheArch = TargetArch;
  COpts.Format = BinaryFormat::COFF;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COpts));
  OS.flush();
  return Source;
}

std::string parentText(const std::string &Source,
                       const std::string &Marker = "sub_140001000(") {
  const size_t At = Source.find(Marker);
  if (At == std::string::npos)
    return {};
  const size_t Brace = Source.find('{', At);
  int Depth = 0;
  for (size_t I = Brace; I < Source.size(); ++I) {
    if (Source[I] == '{')
      ++Depth;
    else if (Source[I] == '}' && --Depth == 0)
      return Source.substr(At, I - At + 1);
  }
  return Source.substr(At);
}

TEST(ARMSEHFinallyRegisters, AFrameAddressSetBeforeTheRangeIsPublished) {
  // x19 = entry SP - 0x20 + 0x10 throughout the range: the handler's x19.
  const MedFunc Med =
      convertParent(makeImage(Parent::FrameAddress, Handler::StoresThroughX19));
  const SEHFuncletRegisterValue *X19 = registerValue(Med, a64reg::X19);
  ASSERT_NE(X19, nullptr);
  EXPECT_EQ(X19->ValueKind, SEHFuncletRegisterValue::Kind::EntryStackOffset);
  EXPECT_EQ(X19->Offset, -0x10);
  // x20 holds the caller's value throughout, which has no stable form.
  EXPECT_EQ(registerValue(Med, a64reg::X20), nullptr);
}

TEST(ARMSEHFinallyRegisters, ARegisterWrittenInTheRangeIsNotPublished) {
  const MedFunc Med = convertParent(
      makeImage(Parent::WrittenInRange, Handler::StoresThroughX19));
  EXPECT_EQ(registerValue(Med, a64reg::X19), nullptr);
}

TEST(ARMSEHFinallyRegisters, AnIncomingArgumentAndAConstantArePublished) {
  const MedFunc Med = convertParent(
      makeImage(Parent::ArgumentAndConstant, Handler::StoresThroughX19));
  const SEHFuncletRegisterValue *X19 = registerValue(Med, a64reg::X19);
  ASSERT_NE(X19, nullptr);
  EXPECT_EQ(X19->ValueKind, SEHFuncletRegisterValue::Kind::EntryArgument);
  EXPECT_EQ(X19->RegOff, a64reg::X0);
  const SEHFuncletRegisterValue *X20 = registerValue(Med, a64reg::X20);
  ASSERT_NE(X20, nullptr);
  EXPECT_EQ(X20->ValueKind, SEHFuncletRegisterValue::Kind::Constant);
  EXPECT_EQ(X20->Value, 5u);
}

/// The local the parent's `str wzr,[x19]` clears, as HighC names it.
std::string clearedLocal(const std::string &P) {
  std::smatch M;
  static const std::regex Clear(R"(([A-Za-z_][A-Za-z0-9_.]*) = 0;)");
  return std::regex_search(P, M, Clear) ? M[1].str() : std::string();
}

TEST(ARMSEHFinallyBody, GuardsTheUnwindWorkThroughTheParentFrame) {
  // The handler stores 99 through x19 while unwinding. The clause stores it
  // to the local the parent's x19 addresses, under AbnormalTermination().
  const std::string P = parentText(decompileHighC(
      makeImage(Parent::FrameAddress, Handler::StoresThroughX19)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("} __finally {"), std::string::npos) << P;
  EXPECT_NE(P.find("/* finally handler @ 0x140001080 */"), std::string::npos)
      << P;
  EXPECT_NE(P.find("if (AbnormalTermination())"), std::string::npos) << P;
  const std::string Local = clearedLocal(P);
  ASSERT_FALSE(Local.empty()) << P;
  EXPECT_NE(P.find(Local + " = 99;"), std::string::npos) << P;
  EXPECT_EQ(P.find("unknown"), std::string::npos) << P;
}

TEST(ARMSEHFinallyBody, AParameterHeldInARegisterIsTheParameter) {
  // x19 holds the incoming x0: the handler's store goes through the
  // parent's parameter.
  const std::string P = parentText(decompileHighC(
      makeImage(Parent::ArgumentAndConstant, Handler::StoresThroughX19)));
  ASSERT_FALSE(P.empty());
  const size_t Guard = P.find("if (AbnormalTermination())");
  ASSERT_NE(Guard, std::string::npos) << P;
  EXPECT_TRUE(std::regex_search(P.substr(Guard),
                                std::regex(R"(\*\([^;]*\)arg0 = 99;)")))
      << P;
  EXPECT_EQ(P.find("unknown"), std::string::npos) << P;
}

TEST(ARMSEHFinallyBody, AFinallyNestedInAnExceptIsFilled) {
  const std::string P = parentText(decompileHighC(
      makeImage(Parent::NestedInExcept, Handler::StoresThroughX19)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("__except"), std::string::npos) << P;
  EXPECT_NE(P.find("if (AbnormalTermination())"), std::string::npos) << P;
  const std::string Local = clearedLocal(P);
  ASSERT_FALSE(Local.empty()) << P;
  EXPECT_NE(P.find(Local + " = 99;"), std::string::npos) << P;
}

TEST(ARMSEHFinallyBody, AFinallyAroundAnotherFinallyIsFilled) {
  // An inner __try/__finally resumes nowhere: the outer clause runs after it
  // on either exit, as C runs them.
  const std::string P = parentText(decompileHighC(
      makeImage(Parent::NestedInFinally, Handler::StoresThroughX19)));
  ASSERT_FALSE(P.empty());
  const std::string Local = clearedLocal(P);
  ASSERT_FALSE(Local.empty()) << P;
  EXPECT_NE(P.find(Local + " = 99;"), std::string::npos) << P;
  EXPECT_NE(P.find(Local + " = 7;"), std::string::npos) << P;
  const size_t First = P.find("if (AbnormalTermination())");
  ASSERT_NE(First, std::string::npos) << P;
  EXPECT_NE(P.find("if (AbnormalTermination())", First + 1), std::string::npos)
      << P;
}

TEST(ARMSEHFinallyBody, ARegisterWrittenInTheRangeKeepsTheComment) {
  const std::string P = parentText(decompileHighC(
      makeImage(Parent::WrittenInRange, Handler::StoresThroughX19)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("/* finally handler @ 0x140001080 */"), std::string::npos)
      << P;
  EXPECT_EQ(P.find("AbnormalTermination"), std::string::npos) << P;
}

TEST(ARMSEHFinallyBody, ARegisterTheParentNeverSetKeepsTheComment) {
  const std::string P = parentText(decompileHighC(
      makeImage(Parent::FrameAddress, Handler::StoresThroughX20)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("/* finally handler @ 0x140001080 */"), std::string::npos)
      << P;
  EXPECT_EQ(P.find("AbnormalTermination"), std::string::npos) << P;
}

TEST(ARMSEHFinallyRegisters, AJoinIsNotTheIncomingArgument) {
  // Version 0 of x1 may be the PHI that joins its two definitions rather
  // than the incoming argument: x19, its copy, has no stable value.
  const MedFunc Med = convertParent(
      makeImage(Parent::JoinedArgumentRegister, Handler::StoresThroughX19));
  EXPECT_EQ(registerValue(Med, a64reg::X19), nullptr);
}

TEST(ARMSEHFinallyRegisters, TheEstablisherFrameIsTheFrameTheUnwindCodesGive) {
  // The range runs on entry SP - 0x20, which the prologue's unwind codes
  // allocate: the stack pointer is published as the establisher frame.
  const MedFunc Med = convertParent(
      makeImage(Parent::EstablisherFrame, Handler::StoresThroughTheFrame));
  const SEHFuncletRegisterValue *Frame = registerValue(Med, a64reg::SP);
  ASSERT_NE(Frame, nullptr);
  EXPECT_EQ(Frame->ValueKind, SEHFuncletRegisterValue::Kind::EntryStackOffset);
  EXPECT_EQ(Frame->Offset, -0x20);
  // Unwind codes that claim another frame leave the frame unknown.
  EXPECT_EQ(
      registerValue(convertParent(makeImage(Parent::EstablisherFrameMismatch,
                                            Handler::StoresThroughTheFrame)),
                    a64reg::SP),
      nullptr);
}

TEST(ARMSEHFinallyRegisters, AFrameWhoseFramePointerIsNotItsStackPointer) {
  // The unwind codes allocate exactly the range's 0x30, but x29 sits 0x10
  // above sp: the two readings of the establisher frame disagree.
  const MedFunc Med = convertParent(makeImage(Parent::FramePointerAboveTheStack,
                                              Handler::StoresThroughTheFrame));
  EXPECT_EQ(registerValue(Med, a64reg::SP), nullptr);
}

TEST(ARMSEHFinallyBody, StoresThroughTheEstablisherFrame) {
  // bar1 at /O0: the handler reaches the homed Counter pointer through its
  // establisher frame, the parent's frame.
  const std::string P = parentText(decompileHighC(
      makeImage(Parent::EstablisherFrame, Handler::StoresThroughTheFrame)));
  ASSERT_FALSE(P.empty());
  const size_t Guard = P.find("if (AbnormalTermination())");
  ASSERT_NE(Guard, std::string::npos) << P;
  // The parent homes the pointer at entry SP - 8; the clause reads it there.
  const std::string Clause = P.substr(Guard);
  EXPECT_NE(Clause.find("var_m8"), std::string::npos) << P;
  EXPECT_NE(Clause.find("= 99;"), std::string::npos) << P;
  EXPECT_EQ(P.find("unknown"), std::string::npos) << P;
}

TEST(ARMSEHFinallyBody, AFrameTheUnwindCodesContradictKeepsTheComment) {
  const std::string P = parentText(decompileHighC(makeImage(
      Parent::EstablisherFrameMismatch, Handler::StoresThroughTheFrame)));
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("/* finally handler @ 0x140001080 */"), std::string::npos)
      << P;
  EXPECT_EQ(P.find("AbnormalTermination"), std::string::npos) << P;
}

constexpr va_t ThumbEntry = 0x401000;
constexpr va_t ThumbFunclet = ThumbEntry + 0x40;

/// xcpt4's bar1 shape as MSVC ARM compiles it: r4 addresses the parent's
/// local, and a frameless Thumb leaf handler with no unwind record of its
/// own stores through r4 while unwinding.
BinaryImage makeThumbImage() {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Bits = Bitness::Bits32;
  Img.Mode = InstructionMode::Thumb;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x400000;
  Img.Entry = ThumbEntry;
  std::vector<uint16_t> Halves(0x50 / 2, 0xbf00); // nop
  // push {r4,lr}; sub sp,#8; add r4,sp,#4; movs r0,#0; str r0,[r4]; nop;
  // ldr r0,[r4]; add sp,#8; pop {r4,pc}
  const uint16_t Parent[] = {0xb510, 0xb082, 0xac01, 0x2000, 0x6020,
                             0xbf00, 0x6820, 0xb002, 0xbd10};
  std::copy(std::begin(Parent), std::end(Parent), Halves.begin());
  // cbz r0,done; movs r1,#99; str r1,[r4] | done: bx lr
  const uint16_t HandlerCode[] = {0xb108, 0x2163, 0x6021, 0x4770};
  std::copy(std::begin(HandlerCode), std::end(HandlerCode),
            Halves.begin() + (ThumbFunclet - ThumbEntry) / 2);
  const va_t End = ThumbEntry + sizeof(Parent);

  Segment Text;
  Text.Name = ".text";
  Text.VA = ThumbEntry;
  Text.Size = Halves.size() * 2;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  std::memcpy(Text.Data.data(), Halves.data(), Text.Size);
  Img.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.VA = ThumbEntry;
  Code.Size = Halves.size() * 2;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  Img.Symbols.push_back(Symbol::makeFunc(ThumbEntry, End - ThumbEntry));

  ExceptionFunction EH;
  EH.CodeRange = {ThumbEntry, End};
  EH.Encoding = ExceptionEncoding::ARM32Unpacked;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {ThumbEntry + 0x08, ThumbEntry + 0x0c};
  Scope.Kind = SEHScopeKind::Finally;
  Scope.FilterOrFinallyVA = ThumbFunclet;
  Scope.HandlerVA = ThumbFunclet;
  EH.SEH->Scopes.push_back(Scope);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

TEST(ARMSEHFinallyRegisters, AThumbLeafHandlerIsAFunctionAndReadsTheFrame) {
  const BinaryImage Img = makeThumbImage();
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
  // Only the scope table names the handler.
  const auto Functions = FuncDetector().detect(Img, Dec);
  EXPECT_TRUE(llvm::any_of(
      Functions, [](const auto &F) { return F.first == ThumbFunclet; }));
  auto Low = CFGBuilder().build(Img, Dec, Img.Entry, "thumb_finally_parent");
  const MedFunc Med =
      LowToMedConverter().convert(Low, Arch::ARM, BinaryFormat::COFF);
  auto Registers = Med.SEHFuncletRegisters.find(ThumbFunclet);
  ASSERT_NE(Registers, Med.SEHFuncletRegisters.end());
  auto R4 = Registers->second.find(armreg::R4);
  ASSERT_NE(R4, Registers->second.end());
  EXPECT_EQ(R4->second.ValueKind,
            SEHFuncletRegisterValue::Kind::EntryStackOffset);
  EXPECT_EQ(R4->second.Offset, -0xc);
}

TEST(ARMSEHFinallyBody, AThumbHandlerFillsTheClause) {
  const std::string P =
      parentText(decompileHighC(makeThumbImage()), "sub_401000(");
  ASSERT_FALSE(P.empty());
  EXPECT_NE(P.find("/* finally handler @ 0x401040 */"), std::string::npos) << P;
  EXPECT_NE(P.find("if (AbnormalTermination())"), std::string::npos) << P;
  const std::string Local = clearedLocal(P);
  ASSERT_FALSE(Local.empty()) << P;
  EXPECT_NE(P.find(Local + " = 99;"), std::string::npos) << P;
  EXPECT_EQ(P.find("unknown"), std::string::npos) << P;
}

} // namespace
