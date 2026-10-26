//===- RegistrationConditionalCxxTests.cpp - Bound C++ borrows -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replay an authored guarded C++ helper with a stack output pointer, a
/// nested scalar call and an exceptional cleanup while preserving return
/// aliases.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationABIPrivate.h"
#include "../../../lib/ir/low/X86/RegistrationConditionalFrame.h"
#include "../../../lib/ir/low/X86/RegistrationSourceCall.h"
#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::registration_state;

constexpr va_t Entry = 0x401000;
constexpr va_t Handler = 0x401100;
constexpr va_t Cleanup = 0x401180;
constexpr va_t Child = 0x401200;
constexpr va_t Destructor = 0x401220;
constexpr va_t Checker = 0x401240;
constexpr va_t Personality = 0x401260;
constexpr va_t Caller = 0x401280;
constexpr va_t ThrowThunk = 0x4012a0;
constexpr va_t Outer = 0x401400;
constexpr va_t OuterCaller = 0x401600;
constexpr va_t OuterHandler = 0x401610;
constexpr va_t OuterCleanup = 0x401640;
constexpr va_t OuterDestructor = 0x401660;
constexpr va_t Table = 0x402000;
constexpr va_t Data = 0x403000;

struct BoundCxxFixture {
  enum class ReturnMode { Alias, Scalar, Joined, Partial };
  enum class OutputAccess {
    StoreWord,
    ReadWord,
    StoreByte,
    StoreByteThenReadWord,
    StoreWordThenReadWord,
    StoreTwoWordsThenReadSecond,
    ReadSecondWord,
    StoreOneArm,
    StoreOneArmThenReadWord,
    StoreBothArmsThenReadWord
  };
  BinaryImage Image;
  FrameState Parent;
  LowOp Call;
  va_t ReturnVA = 0;
  va_t StoreVA = 0;
  va_t NestedCallVA = 0;
  va_t ResultVA = 0;
  va_t OuterCallVA = 0;
  va_t OuterResultVA = 0;

  explicit BoundCxxFixture(ReturnMode Result = ReturnMode::Alias,
                           bool BorrowInChild = false,
                           bool CaptureBeforeWrite = false,
                           bool ThrowingChild = false,
                           bool RequireChildZero = false,
                           std::optional<uint8_t> ChildArgument = std::nullopt,
                           OutputAccess Access = OutputAccess::StoreWord) {
    std::vector<uint8_t> Text;
    auto Bytes = [&](std::initializer_list<uint8_t> Values) {
      Text.insert(Text.end(), Values);
    };
    auto Word = [&](uint32_t Value) {
      const auto At = Text.size();
      Text.resize(At + 4);
      writeLE<uint32_t>(Text.data() + At, Value);
    };
    auto CallTo = [&](va_t Target) {
      Bytes({0xe8});
      Word(uint32_t(Target - (Entry + Text.size() + 4)));
    };
    auto PadTo = [&](va_t Address) { Text.resize(Address - Entry, 0xcc); };
    Bytes({0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68});
    Word(Handler);
    Bytes({0x64, 0xa1, 0, 0, 0, 0, 0x50, 0x83, 0xec, 16, 0xa1});
    Word(Data);
    Bytes({0x33, 0xc5, 0x50, 0x8d, 0x45, 0xf4, 0x64, 0xa3, 0, 0, 0,
           0});                            // Publish node; cookie is EBP-32.
    Bytes({0x89, 0x4d, 0xe8,               // Save the full ECX scalar input.
           0xc7, 0x45, 0xec, 1, 0, 0, 0,   // Cleanup object EBP-20.
           0xc7, 0x45, 0xfc, 0, 0, 0, 0}); // Activate cleanup.
    if (BorrowInChild)
      Bytes({0x8b, 0x4d, 8});
    if (ThrowingChild)
      Bytes({0x85, 0xc9, 0x74, 5});
    if (ChildArgument)
      Bytes({0x6a, *ChildArgument});
    NestedCallVA = Entry + Text.size();
    CallTo(Child);
    if (RequireChildZero)
      Bytes({0x85, 0xc0, 0x74, 5, 0x8b, 0x55, 8, 0x89, 0x12});
    Bytes({0x8b, 0x45, 8, 0x8b, 0x55, 0xe8});
    if (CaptureBeforeWrite)
      Bytes({0x89, 0x00}); // Capture remains invalid after the next store.
    StoreVA = Entry + Text.size();
    switch (Access) {
    case OutputAccess::StoreWord:
      Bytes({0x89, 0x10}); // *output = saved ECX.
      break;
    case OutputAccess::ReadWord:
      Bytes({0x8b, 0x10});
      break;
    case OutputAccess::StoreByte:
      Bytes({0x88, 0x10});
      break;
    case OutputAccess::StoreByteThenReadWord:
      Bytes({0x88, 0x10, 0x8b, 0x10});
      break;
    case OutputAccess::StoreWordThenReadWord:
      Bytes({0x89, 0x10, 0x8b, 0x10});
      break;
    case OutputAccess::StoreTwoWordsThenReadSecond:
      Bytes({0x89, 0x10, 0x89, 0x50, 4, 0x8b, 0x50, 4});
      break;
    case OutputAccess::ReadSecondWord:
      Bytes({0x8b, 0x50, 4});
      break;
    case OutputAccess::StoreOneArm:
    case OutputAccess::StoreOneArmThenReadWord:
      Bytes({0x85, 0xd2, 0x74, 2, 0x89, 0x10});
      if (Access == OutputAccess::StoreOneArmThenReadWord)
        Bytes({0x8b, 0x10});
      break;
    case OutputAccess::StoreBothArmsThenReadWord:
      Bytes({0x85, 0xd2, 0x74, 4, 0x89, 0x10, 0xeb, 2, 0x89, 0x10, 0x8b, 0x10});
      break;
    }
    Bytes({0xc7, 0x45, 0xfc, 0xff, 0xff, 0xff, 0xff, 0x8b, 0x4d, 0xe0, 0x33,
           0xcd});
    CallTo(Checker);
    ResultVA = Entry + Text.size();
    if (Result == ReturnMode::Joined)
      Bytes({0x8b, 0x4d, 0xe8, 0x85, 0xc9, 0x74, 5, 0x8b, 0x45, 8, 0xeb, 2,
             0x31, 0xc0});
    else if (Result == ReturnMode::Partial)
      Bytes({0x8b, 0x45, 8, 0x66, 0xb8, 0, 0});
    else if (Result == ReturnMode::Alias)
      Bytes({0x8b, 0x45, 8});
    else
      Bytes({0x31, 0xc0, 0x90});
    Bytes({0x8b, 0x4d, 0xf4, 0x64, 0x89, 0x0d, 0, 0, 0, 0, 0x8b, 0xe5, 0x5d});
    ReturnVA = Entry + Text.size();
    Bytes({0xc2, 4, 0});
    PadTo(Handler);
    Bytes({0x8b, 0x54, 0x24, 8, 0x8d, 0x42, 12, 0x8b, 0x4a, 0xec, 0x33, 0xc8});
    CallTo(Checker);
    Bytes({0xb8});
    Word(Table);
    Bytes({0xe9});
    Word(uint32_t(Personality - (Entry + Text.size() + 4)));
    PadTo(Cleanup);
    Bytes({0x55, 0x83, 0xc5, 0, 0x8d, 0x4d, 0xec});
    CallTo(Destructor);
    Bytes({0x5d, 0xc3});
    PadTo(Child);
    if (ThrowingChild) {
      Bytes({0x55, 0x8b, 0xec, 0x83, 0xec, 4, 0xc7, 0x45, 0xfc, 7, 0, 0, 0,
             0x68});
      Word(Table + 0x80);
      Bytes({0x8d, 0x4d, 0xfc, 0x51});
      CallTo(ThrowThunk);
      Bytes({0x90, 0x8b, 0xe5, 0x5d, 0xc3});
    } else if (BorrowInChild)
      Bytes({0x8b, 1, 0x83, 0xc0, 1, 0x89, 1, 0x31, 0xc0, 0xc3});
    else
      Bytes({0xb8, 7, 0, 0, 0, 0xc3});
    PadTo(Destructor);
    Bytes({0xc7, 1, 0, 0, 0, 0, 0xc3});
    PadTo(Checker);
    Bytes({0x3b, 0x0d});
    Word(Data);
    Bytes({0x75, 1, 0xc3, 0xcc});
    PadTo(Personality);
    Bytes({0xff, 0x25});
    Word(Data + 4);
    PadTo(Caller);
    CallTo(Entry);
    Bytes({0xc3});
    if (ThrowingChild) {
      PadTo(ThrowThunk);
      Bytes({0xff, 0x25});
      Word(Data + 8);
    }
    PadTo(Entry + 0x300);

    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.DynInfo.SecurityCookieRVA = Data - Image.Base;
    auto SegmentAt = [&](va_t VA, std::vector<uint8_t> Bytes,
                         SegmentFlags Flags) {
      Segment S;
      S.VA = VA;
      S.Size = S.FileSz = Bytes.size();
      S.Flags = Flags;
      S.Data = std::move(Bytes);
      Section C;
      C.VA = VA;
      C.Size = C.FileSz = S.Size;
      C.Flags = Flags;
      Image.Sections.push_back(C);
      Image.Segments.push_back(std::move(S));
    };
    SegmentAt(Entry, std::move(Text),
              SegmentFlags::Readable | SegmentFlags::Executable);
    std::vector<uint8_t> Info(0x100);
    writeLE<uint32_t>(Info.data(), 0x19930522);
    writeLE<uint32_t>(Info.data() + 4, 1);
    writeLE<uint32_t>(Info.data() + 8, Table + 0x30);
    writeLE<uint32_t>(Info.data() + 0x30, UINT32_MAX);
    writeLE<uint32_t>(Info.data() + 0x34, Cleanup);
    if (ThrowingChild) {
      writeLE<uint32_t>(Info.data() + 0x8c, Table + 0x90);
      writeLE<uint32_t>(Info.data() + 0x90, 1);
      writeLE<uint32_t>(Info.data() + 0x94, Table + 0xa0);
      writeLE<uint32_t>(Info.data() + 0xa0, 1);
      writeLE<uint32_t>(Info.data() + 0xa4, Data + 0x80);
      writeLE<uint32_t>(Info.data() + 0xac, UINT32_MAX);
      writeLE<uint32_t>(Info.data() + 0xb4, 4);
    }
    SegmentAt(Table, std::move(Info), SegmentFlags::Readable);
    SegmentAt(Data, std::vector<uint8_t>(0x100),
              SegmentFlags::Readable | SegmentFlags::Writable);
    Import Runtime;
    Runtime.Module = "VCRUNTIME140.dll";
    Runtime.Name = "__CxxFrameHandler3";
    Runtime.IATAddr = Data + 4;
    Image.Imports.push_back(Runtime);
    if (ThrowingChild) {
      Runtime.Name = "_CxxThrowException";
      Runtime.IATAddr = Data + 8;
      Image.Imports.push_back(Runtime);
      Image.Segments.back().Data[0x88] = '.';
      Image.Segments.back().Data[0x89] = 'H';
    }
    for (const auto VA : {Entry, Handler, Cleanup, Child, Destructor, Checker,
                          Personality, Caller}) {
      auto S = Symbol::makeFunc(VA);
      S.Name = VA == Entry         ? "bound_cxx"
               : VA == Personality ? "__CxxFrameHandler3"
                                   : "authored_" + std::to_string(VA);
      Image.Symbols.push_back(S);
    }
    coff_loader::parseX86RegistrationExceptions(Image);
    for (auto &Register : Parent.Registers)
      Register = FrameValue::taint(true);
    Parent.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
        FrameValue::frame(-96);
    Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
        FrameValue::constant(17);
    Parent.Cells[-96] = FrameValue::frame(-28);
    Parent.Cells[-28] = FrameValue::constant(1);
    Call.Opcode = NdOp::CALL;
    Call.Addr = Caller;
    Call.Seq = 0;
    Call.addInput(NdVar::cst(Entry, 4));
  }

  void overwrite(va_t Address, std::initializer_list<uint8_t> Bytes) {
    std::copy(Bytes.begin(), Bytes.end(),
              Image.Segments.front().Data.begin() + Address - Entry);
  }

  void addOuter(bool ForwardBorrow = true, bool StoreResult = false,
                bool RepeatNested = false, va_t Wrapper = Outer,
                bool CrossPopSpill = false, bool RequireZero = false,
                std::optional<va_t> ImageWrite = std::nullopt,
                bool ZeroInput = false) {
    auto &Text = Image.Segments.front().Data;
    const auto Target = Call.Inputs[0].Offset;
    const va_t WrapperCaller = Wrapper + (OuterCaller - Outer);
    Text.resize(Wrapper - Entry, 0xcc);
    auto Bytes = [&](std::initializer_list<uint8_t> Values) {
      Text.insert(Text.end(), Values);
    };
    auto Word = [&](uint32_t Value) {
      const auto At = Text.size();
      Text.resize(At + 4);
      writeLE<uint32_t>(Text.data() + At, Value);
    };
    auto CallTo = [&](va_t Target) {
      Bytes({0xe8});
      Word(uint32_t(Target - (Entry + Text.size() + 4)));
    };
    Bytes({0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68});
    Word(Handler);
    Bytes({0x64, 0xa1, 0, 0, 0, 0, 0x50, 0x83, 0xec, 16, 0xa1});
    Word(Data);
    Bytes({0x33, 0xc5, 0x50, 0x8d, 0x45, 0xf4, 0x64, 0xa3, 0, 0, 0, 0});
    if (ZeroInput)
      Bytes({0x31, 0xc9});
    Bytes({0x89, 0x4d, 0xe8, 0xc7, 0x45, 0xec, 1, 0, 0, 0, 0xc7, 0x45, 0xfc, 0,
           0, 0, 0});
    if (CrossPopSpill)
      Bytes({0x83, 0xec, 12, 0xc7, 0x45, 0xd4, 0, 0, 0, 0, 0x6a, 0});
    if (ImageWrite) {
      Bytes({0xc7, 0x05});
      Word(*ImageWrite);
      Word(0);
    }
    if (ForwardBorrow)
      Bytes({0xff, 0x75, 8}); // Forward the ancestor's output borrow.
    else
      Bytes({0x8d, 0x45, 0xec, 0x50}); // Lend a private initialized word.
    if (CrossPopSpill)
      Bytes({0x8b, 0x45, 8, 0x89, 0x45, 0xd3}); // EBP-45 crosses returned ESP.
    OuterCallVA = Entry + Text.size();
    CallTo(Target);
    if (CrossPopSpill)
      Bytes({0x8b, 0x45, 0xd4}); // The three pointer bytes above ESP survive.
    if (RepeatNested) {
      Bytes({0x8b, 0x4d, 0xe8, 0xff, 0x75, 8});
      CallTo(Target);
    }
    OuterResultVA = Entry + Text.size();
    if (RequireZero)
      Bytes({0x85, 0xc0, 0x74, 5, 0x8b, 0x55, 8, 0x89,
             0x12}); // A nonzero result captures the borrowed output pointer.
    if (StoreResult)
      Bytes({0x8b, 0x55, 8, 0x89, 0x02}); // *outer_output = returned value.
    Bytes({0x89, 0x45, 0xe4, // Preserve the actual returned alias across GS.
           0xc7, 0x45, 0xfc, 0xff, 0xff, 0xff, 0xff, 0x8b, 0x4d, 0xe0, 0x33,
           0xcd});
    CallTo(Checker);
    Bytes({0x8b, 0x45, 0xe4, 0x8b, 0x4d, 0xf4, 0x64, 0x89, 0x0d, 0, 0, 0, 0,
           0x8b, 0xe5, 0x5d, 0xc2, 4, 0});
    Text.resize(WrapperCaller - Entry, 0xcc);
    CallTo(Wrapper);
    Bytes({0xc3});
    Image.Segments.front().Size = Image.Segments.front().FileSz = Text.size();
    Image.Sections.front().Size = Image.Sections.front().FileSz = Text.size();
    for (const auto VA : {Wrapper, WrapperCaller}) {
      auto S = Symbol::makeFunc(VA);
      S.Name = "authored_outer_" + std::to_string(VA);
      Image.Symbols.push_back(S);
    }
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
    Call.Addr = WrapperCaller;
    Call.Inputs[0] = NdVar::cst(Wrapper, 4);
  }

  void separateOuterCleanup() {
    auto &Text = Image.Segments.front().Data;
    Text.resize(OuterDestructor + 16 - Entry, 0xcc);
    auto Word = [&](va_t At, uint32_t Value) {
      writeLE<uint32_t>(Text.data() + At - Entry, Value);
    };
    std::copy_n(Text.begin() + Handler - Entry, 27,
                Text.begin() + OuterHandler - Entry);
    Word(Outer + 6, OuterHandler);
    Word(OuterHandler + 13, uint32_t(Checker - (OuterHandler + 17)));
    Word(OuterHandler + 18, Table + 0x100);
    Word(OuterHandler + 23, uint32_t(Personality - (OuterHandler + 27)));
    std::copy_n(Text.begin() + Cleanup - Entry, 14,
                Text.begin() + OuterCleanup - Entry);
    Word(OuterCleanup + 8, uint32_t(OuterDestructor - (OuterCleanup + 12)));
    std::copy_n(Text.begin() + Destructor - Entry, 7,
                Text.begin() + OuterDestructor - Entry);
    auto &Info = Image.Segments[1].Data;
    Info.resize(0x140);
    std::copy_n(Info.begin(), 0x40, Info.begin() + 0x100);
    writeLE<uint32_t>(Info.data() + 0x108, Table + 0x130);
    writeLE<uint32_t>(Info.data() + 0x134, OuterCleanup);
    for (unsigned I = 0; I != 2; ++I) {
      auto &Segment = Image.Segments[I];
      Segment.Size = Segment.FileSz = Segment.Data.size();
      Image.Sections[I].Size = Image.Sections[I].FileSz = Segment.Size;
    }
    for (const auto VA : {OuterHandler, OuterCleanup, OuterDestructor}) {
      auto S = Symbol::makeFunc(VA);
      S.Name = "authored_outer_" + std::to_string(VA);
      Image.Symbols.push_back(S);
    }
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
  }

  std::optional<BoundCxxCalleeEffect> prove(size_t &Work) const {
    return proveBoundCxxCallee(Image, Call, Parent, 4, Work);
  }
};

TEST(RegistrationConditionalCxx, DistinguishesReturnedBorrowFromScalar) {
  for (bool Alias : {false, true}) {
    SCOPED_TRACE(Alias);
    BoundCxxFixture F(Alias ? BoundCxxFixture::ReturnMode::Alias
                            : BoundCxxFixture::ReturnMode::Scalar);
    const auto *EH = F.Image.ExceptionMetadata.findFunction(Entry);
    ASSERT_TRUE(EH && EH->Registration && EH->Cxx);
    ASSERT_TRUE(coff_loader::getCheckedX86CxxHandlerABI(F.Image, *EH));
    ASSERT_TRUE(
        coff_loader::getCheckedX86CxxHandlerPointerSource(F.Image, *EH));
    size_t Work = 0;
    const auto Effect = F.prove(Work);
    ASSERT_TRUE(Effect);
    EXPECT_EQ(Effect->StackPopBytes, 4u);
    EXPECT_TRUE(Effect->Reads.empty());
    ASSERT_EQ(Effect->Writes.size(), 1u);
    EXPECT_EQ(Effect->Writes[0].Begin, -28);
    EXPECT_EQ(Effect->Writes[0].End, -24);
    ASSERT_EQ(Effect->Returns.size(), 1u);
    EXPECT_EQ(Effect->Returns[0].Address, F.ReturnVA);
    EXPECT_EQ(Effect->Returns[0].Value.MayBeFrame, Alias);
    EXPECT_EQ(Effect->Returns[0].Value.Offset,
              Alias ? std::optional<int32_t>(-28) : std::nullopt);
    EXPECT_GT(Work, 0u);
    ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
    size_t Exact = limits::kMaxRegistrationEHStateWork - Work;
    EXPECT_TRUE(F.prove(Exact));
    EXPECT_EQ(Exact, limits::kMaxRegistrationEHStateWork);
    size_t Limited = limits::kMaxRegistrationEHStateWork - Work + 1;
    EXPECT_FALSE(F.prove(Limited));
    EXPECT_EQ(Limited, limits::kMaxRegistrationEHStateWork);
    // This guarded source proof is not a context-free native personality ABI.
    EXPECT_FALSE(coff_loader::getCheckedX86CxxPersonalityABI(F.Image, *EH));
  }
}

TEST(RegistrationConditionalCxx, MissingBorrowDoesNotAnalyzeUnboundCalls) {
  for (bool Nested : {false, true}) {
    SCOPED_TRACE(Nested);
    BoundCxxFixture F;
    if (Nested)
      F.addOuter();
    F.Parent.Cells.erase(-28);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(F.Image));
    size_t UnboundWork = 0;
    RegistrationCallCalleeIndex Index(F.Image, &UnboundWork);
    CFGBuilder Builder;
    Builder.setRegistrationCalleeIndex(&Index);
    const auto Unbound = Builder.buildIntegerFrameProof(
        F.Image, Dec, F.Call.Inputs[0].Offset, "unbound-cxx-callee");
    ASSERT_TRUE(Unbound.hasCompleteLiftCoverage());
    ASSERT_TRUE(Unbound.RegistrationStates);
    ASSERT_GT(UnboundWork, 0u);
    size_t Work = 0;
    EXPECT_FALSE(F.prove(Work));
    // A missing invocation input is decidable from the physical body and
    // caller cells. It must not first pay for the unbound call/state closure.
    EXPECT_LT(Work, UnboundWork);
    size_t Limited = limits::kMaxRegistrationEHStateWork - Work;
    EXPECT_FALSE(F.prove(Limited));
    EXPECT_EQ(Limited, limits::kMaxRegistrationEHStateWork);
    F.Parent.Cells[-28] = FrameValue::constant(1);
    Work = 0;
    EXPECT_TRUE(F.prove(Work));
  }
}

TEST(RegistrationConditionalCxx, JoinedAndPartialReturnsKeepAliasTaint) {
  for (const auto Mode : {BoundCxxFixture::ReturnMode::Joined,
                          BoundCxxFixture::ReturnMode::Partial}) {
    SCOPED_TRACE(int(Mode));
    BoundCxxFixture F(Mode);
    F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
    size_t Work = 0;
    const auto Effect = F.prove(Work);
    ASSERT_TRUE(Effect);
    ASSERT_EQ(Effect->Returns.size(), 1u);
    EXPECT_TRUE(Effect->Returns[0].Value.MayBeFrame);
    EXPECT_FALSE(Effect->Returns[0].Value.Offset);
  }
}

TEST(RegistrationConditionalCxx, ComposesCheckedChildReadsAndMayWrites) {
  BoundCxxFixture F(BoundCxxFixture::ReturnMode::Alias, true);
  size_t Work = 0;
  const auto Effect = F.prove(Work);
  ASSERT_TRUE(Effect);
  ASSERT_EQ(Effect->Reads.size(), 1u);
  EXPECT_EQ(Effect->Reads[0].Begin, -28);
  EXPECT_EQ(Effect->Reads[0].End, -24);
  ASSERT_EQ(Effect->Writes.size(), 1u);
  EXPECT_EQ(Effect->Writes[0].Begin, -28);
  EXPECT_EQ(Effect->Writes[0].End, -24);
  ASSERT_EQ(Effect->Returns.size(), 1u);
  EXPECT_EQ(Effect->Returns[0].Value.Offset, -28);
  // A failed query does not populate a target-only cache for the next inputs.
  F.Parent.Cells[-28] = FrameValue::frame(-80);
  Work = 0;
  EXPECT_FALSE(F.prove(Work));
  F.Parent.Cells[-28] = FrameValue::constant(2);
  Work = 0;
  EXPECT_TRUE(F.prove(Work));
}

TEST(RegistrationConditionalCxx, AScalarOverwriteCannotEraseCapture) {
  BoundCxxFixture F(BoundCxxFixture::ReturnMode::Scalar, false, true);
  size_t Work = 0;
  EXPECT_FALSE(F.prove(Work));
}

TEST(RegistrationConditionalCxx, BindsNestedCxxBorrowAndReturnedAlias) {
  for (const bool Forward : {false, true}) {
    SCOPED_TRACE(Forward);
    BoundCxxFixture F;
    F.addOuter(Forward);
    size_t Work = 0;
    const auto Effect = F.prove(Work);
    ASSERT_TRUE(Effect);
    ASSERT_EQ(Effect->Objects.size(), 1u);
    EXPECT_EQ(Effect->Objects.front().Begin, -28);
    ASSERT_EQ(Effect->Returns.size(), 1u);
    EXPECT_TRUE(Effect->Returns.front().Value.MayBeFrame);
    EXPECT_EQ(Effect->Returns.front().Value.Offset,
              Forward ? std::optional<int32_t>(-28) : std::nullopt);
    ASSERT_EQ(Effect->Writes.size(), Forward ? 1u : 0u);
    if (Forward) {
      EXPECT_EQ(Effect->Writes.front().Begin, -28);
      EXPECT_EQ(Effect->Writes.front().End, -24);
    }
    ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
    size_t Limited = limits::kMaxRegistrationEHStateWork - Work + 1;
    EXPECT_FALSE(F.prove(Limited));
    EXPECT_EQ(Limited, limits::kMaxRegistrationEHStateWork);
    Work = 0;
    EXPECT_TRUE(F.prove(Work)); // Neither refusal nor success is cached.
  }
}

TEST(RegistrationConditionalCxx, NestedReturnCannotHideAnAlias) {
  BoundCxxFixture Scalar(BoundCxxFixture::ReturnMode::Scalar);
  Scalar.addOuter(true, true);
  size_t ScalarWork = 0;
  ASSERT_TRUE(Scalar.prove(ScalarWork));
  for (const auto Mode :
       {BoundCxxFixture::ReturnMode::Alias, BoundCxxFixture::ReturnMode::Joined,
        BoundCxxFixture::ReturnMode::Partial}) {
    SCOPED_TRACE(int(Mode));
    BoundCxxFixture F(Mode);
    F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
    F.addOuter(true, true);
    size_t Work = 0;
    EXPECT_FALSE(F.prove(Work));
  }
}

TEST(RegistrationConditionalCxx, PlainChildUsesTheCurrentScalarInvocation) {
  for (bool Nested : {false, true})
    for (unsigned Mode = 0; Mode != 5; ++Mode) {
      SCOPED_TRACE(::testing::Message() << Nested << " " << Mode);
      BoundCxxFixture F(BoundCxxFixture::ReturnMode::Scalar, false, false,
                        false, true);
      // Only ECX == 17 bypasses an unowned dereference. A target-only leaf
      // contract cannot prove this helper, even though the actual call can.
      F.overwrite(Child,
                  {0x83, 0xf9, 17, 0x74, 3, 0x8b, 2, 0xc3, 0x31, 0xc0, 0xc3});
      auto &Input = F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride];
      if (Mode == 1)
        Input = FrameValue::constant(18);
      else if (Mode == 2)
        Input = {};
      else if (Mode == 3)
        Input = FrameValue::taint(true);
      else if (Mode == 4)
        Input = FrameValue::imageAddress(Table);
      if (Nested)
        F.addOuter();
      size_t Work = 0;
      const auto Effect = F.prove(Work);
      EXPECT_EQ(bool(Effect), Mode == 0);
      if (!Effect)
        continue;
      ASSERT_EQ(Effect->Returns.size(), 1u);
      EXPECT_EQ(Effect->Returns.front().Value, FrameValue::constant(0));
      ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
      size_t Exact = limits::kMaxRegistrationEHStateWork - Work;
      EXPECT_TRUE(F.prove(Exact));
      EXPECT_EQ(Exact, limits::kMaxRegistrationEHStateWork);
      size_t Limited = limits::kMaxRegistrationEHStateWork - Work + 1;
      EXPECT_FALSE(F.prove(Limited));
      EXPECT_EQ(Limited, limits::kMaxRegistrationEHStateWork);
      Input = FrameValue::constant(18);
      Work = 0;
      EXPECT_FALSE(F.prove(Work));
      EXPECT_FALSE(getCheckedX86RegistrationObjectCalleeABI(F.Image, Child));
    }
}

TEST(RegistrationConditionalCxx, PlainChildOwnsItsExactStackCleanup) {
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    SCOPED_TRACE(Mode);
    BoundCxxFixture F(
        BoundCxxFixture::ReturnMode::Scalar, false, false, false, true,
        Mode == 2 ? std::nullopt : std::optional<uint8_t>(Mode == 1 ? 18 : 17));
    F.overwrite(Child, {0x8b, 0x44, 0x24, 4, 0x83, 0xf8, 17, 0x74, 5, 0x8b, 2,
                        0xc2, 4, 0, 0x31, 0xc0, 0xc2, 4, 0});
    if (Mode >= 3) {
      F.overwrite(Child + 12, {uint8_t(Mode == 3 ? 8 : 2)});
      F.overwrite(Child + 17, {uint8_t(Mode == 3 ? 8 : 2)});
    }
    size_t Work = 0;
    EXPECT_EQ(bool(F.prove(Work)), Mode == 0);
  }
}

TEST(RegistrationConditionalCxx, PlainChildRetainsImageReadAndReturnFacts) {
  for (unsigned Mode = 0; Mode != 4; ++Mode) {
    SCOPED_TRACE(Mode);
    BoundCxxFixture F(BoundCxxFixture::ReturnMode::Scalar, false, false, false,
                      true);
    F.overwrite(Child, {0xa1, 0x70, 0x20, 0x40, 0, 0xc3});
    if (Mode == 1)
      writeLE<uint32_t>(F.Image.Segments[1].Data.data() + 0x70, 1);
    else if (Mode == 2) {
      F.Image.Segments[1].Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Sections[1].Flags = F.Image.Segments[1].Flags;
    } else if (Mode == 3)
      F.overwrite(Child, {0xa3, 4, 0x30, 0x40, 0, 0x31, 0xc0, 0xc3});
    size_t Work = 0;
    const auto Effect = F.prove(Work);
    EXPECT_EQ(bool(Effect), Mode == 0);
    if (Effect)
      EXPECT_TRUE(llvm::any_of(Effect->ImageReads, [](const auto &Read) {
        return Read.Begin <= Table + 0x70 && Table + 0x74 <= Read.End;
      }));
  }
}

TEST(RegistrationConditionalCxx, PlainEffectCannotImportNestedRuntimeEffects) {
  BoundCxxFixture F;
  // ECX == 17 bypasses a closed RaiseException invocation. Its argument and
  // stack proof alone cannot grant the enclosing runtime's callback effects.
  F.overwrite(Child, {0x83, 0xf9, 17,   0x74, 17,   0x6a, 0,    0x6a, 0,
                      0x6a, 0,    0x68, 0x42, 0x42, 0x42, 0xe0, 0xff, 0x15,
                      8,    0x30, 0x40, 0,    0x31, 0xc0, 0xc3});
  Import Raise;
  Raise.Name = "RaiseException";
  Raise.Module = "kernel32.dll";
  Raise.IATAddr = Data + 8;
  F.Image.Imports.push_back(Raise);
  ASSERT_TRUE(F.Image.recordImportStorageSlot(
      Raise.IATAddr, Raise.Name, 0, ImportStorageEvidence::ImportDirectory));
  LowOp Call = F.Call;
  Call.Addr = F.NestedCallVA;
  Call.Inputs[0] = NdVar::cst(Child, 4);
  BoundPlainCalleeEffect Effect;
  for (unsigned Input : {17u, 18u, 17u}) {
    SCOPED_TRACE(Input);
    F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
        FrameValue::constant(Input);
    size_t Work = 0;
    std::optional<FrameValue> Returned;
    ASSERT_TRUE(registration_abi::hasConditionalCallFrame(
        F.Image, Call, F.Parent, Work, nullptr, &Returned));
    EXPECT_EQ(Returned, FrameValue::constant(0));
    Work = 0;
    EXPECT_EQ(registration_abi::hasConditionalCallFrame(
                  F.Image, Call, F.Parent, Work, nullptr, nullptr, &Effect),
              Input == 17);
    EXPECT_EQ(Effect.ReturnValue,
              Input == 17 ? FrameValue::constant(0) : FrameValue::taint(true));
    EXPECT_EQ(Effect.StackPopBytes, 0u);
    EXPECT_TRUE(Effect.ImageReads.empty());
  }
}

TEST(RegistrationConditionalCxx, ExactReturnedZeroClosesOnlyItsOwnBranch) {
  for (unsigned Depth : {1u, 2u})
    for (unsigned Mode = 0; Mode != 7; ++Mode) {
      SCOPED_TRACE(::testing::Message() << Depth << " " << Mode);
      BoundCxxFixture F(BoundCxxFixture::ReturnMode::Scalar);
      if (Mode == 1)
        F.overwrite(F.ResultVA, {0x31, 0xc0, 0x40}); // Known nonzero.
      else if (Mode >= 2) {
        F.overwrite(F.ResultVA, {0x8b, 0x45, 0xe8}); // Saved incoming ECX.
        auto &Input =
            F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride];
        if (Mode == 2)
          Input = FrameValue::constant(0);
        else if (Mode == 3)
          Input = {};
        else if (Mode == 4)
          Input = FrameValue::imageAddress(Table);
        else if (Mode == 5)
          Input = FrameValue::constant(Table);
        else
          Input = FrameValue::taint(true);
      }
      for (unsigned Level = 0; Level != Depth; ++Level)
        F.addOuter(true, false, false, Outer + Level * 0x280, false, true);
      size_t Work = 0;
      const auto Effect = F.prove(Work);
      EXPECT_EQ(bool(Effect), Mode == 0 || Mode == 2);
    }
}

TEST(RegistrationConditionalCxx, CallerIndexDoesNotCacheInvocationInputs) {
  BoundCxxFixture F(BoundCxxFixture::ReturnMode::Scalar);
  F.overwrite(F.ResultVA, {0x8b, 0x45, 0xe8}); // Return incoming ECX.
  F.addOuter(true, false, false, Outer, false, true);
  size_t TotalWork = 0;
  RegistrationCallCalleeIndex Index(F.Image, &TotalWork);
  for (uint32_t Value : {0u, 1u, 0u}) {
    SCOPED_TRACE(Value);
    F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
        FrameValue::constant(Value);
    const auto Before = TotalWork;
    size_t Work = 0;
    const auto Effect = proveBoundCxxCallee(F.Image, F.Call, F.Parent, 4, Work,
                                            {}, nullptr, &Index);
    EXPECT_EQ(bool(Effect), Value == 0);
    EXPECT_GT(Work, 0u);
    ASSERT_TRUE(Index.chargeStateWork(Work));
    EXPECT_GT(TotalWork, Before);
  }
  BoundCxxFixture Other;
  size_t ForeignWork = 0, Work = 0;
  RegistrationCallCalleeIndex Foreign(Other.Image, &ForeignWork);
  EXPECT_FALSE(proveBoundCxxCallee(F.Image, F.Call, F.Parent, 4, Work, {},
                                   nullptr, &Foreign));
  EXPECT_EQ(Work, 0u);
  EXPECT_EQ(ForeignWork, 0u);
}

TEST(RegistrationConditionalCxx,
     NestedReturnsKeepTheirInvocationAndAddressKind) {
  for (unsigned Depth : {0u, 1u, 2u})
    for (unsigned Mode = 0; Mode != 9; ++Mode) {
      SCOPED_TRACE(::testing::Message() << Depth << " " << Mode);
      BoundCxxFixture F(Mode == 7   ? BoundCxxFixture::ReturnMode::Partial
                        : Mode == 8 ? BoundCxxFixture::ReturnMode::Joined
                                    : BoundCxxFixture::ReturnMode::Scalar);
      auto Expected = FrameValue::constant(0);
      auto &Input = F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride];
      if (Mode >= 1 && Mode <= 3) {
        Expected = Mode == 1   ? FrameValue::constant(Table)
                   : Mode == 2 ? FrameValue::imageAddress(Table)
                               : FrameValue{};
        Input = Expected;
        F.overwrite(F.ResultVA, {0x8b, 0x45, 0xe8});
      } else if (Mode >= 4) {
        Expected = FrameValue::taint(true);
        Expected.FrameOnlyFromCall = true;
        if (Mode == 4)
          F.overwrite(F.ResultVA, {0x8b, 0xc3, 0x90}); // Incoming saved EBX.
        else if (Mode == 5)
          F.overwrite(F.ResultVA, {0x8b, 0x45, 4}); // Callee return PC.
        else if (Mode == 6)
          F.overwrite(F.ResultVA,
                      {0x8d, 0x45, 0xec}); // Retired private object.
        else if (Mode == 8)
          Input = {}; // Both the scalar and borrowed return arms are reached.
      }
      for (unsigned Level = 0; Level != Depth; ++Level)
        F.addOuter(true, false, false, Outer + Level * 0x280);
      size_t Work = 0;
      const auto Effect = F.prove(Work);
      ASSERT_TRUE(Effect);
      ASSERT_EQ(Effect->Returns.size(), 1u);
      EXPECT_EQ(Effect->Returns.front().Value, Expected);
      ASSERT_GT(Work, 0u);
      ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
      size_t Exact = limits::kMaxRegistrationEHStateWork - Work;
      EXPECT_TRUE(F.prove(Exact));
      EXPECT_EQ(Exact, limits::kMaxRegistrationEHStateWork);
      size_t Short = limits::kMaxRegistrationEHStateWork - Work + 1;
      EXPECT_FALSE(F.prove(Short));
      EXPECT_EQ(Short, limits::kMaxRegistrationEHStateWork);
    }
}

TEST(RegistrationConditionalCxx, ReturnedFactsRequireACompleteScalarCarrier) {
  for (const auto Output :
       {NdVar::reg(x86reg::RAX, 4), NdVar::tmp(80, 4),
        NdVar::reg(x86reg::RAX, 2), NdVar::reg(x86reg::RCX, 4),
        NdVar::reg(x86reg::RDX, 4), NdVar::tmp(80, 2), NdVar::tmp(80, 8)}) {
    SCOPED_TRACE(::testing::Message() << int(Output.Space) << " "
                                      << Output.Offset << " " << Output.Size);
    BoundCxxFixture F(BoundCxxFixture::ReturnMode::Scalar);
    F.addOuter(true, false, false, Outer, false, true);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(F.Image));
    size_t Work = 0;
    RegistrationCallCalleeIndex Index(F.Image, &Work);
    CFGBuilder Builder;
    Builder.setRegistrationCalleeIndex(&Index);
    auto Function =
        Builder.buildIntegerFrameProof(F.Image, Dec, Outer, "bound-parent");
    ASSERT_TRUE(Function.hasCompleteLiftCoverage());
    ASSERT_TRUE(Function.ExceptionMetadata);
    const auto HandlerABI = coff_loader::getCheckedX86CxxHandlerABI(
        F.Image, *Function.ExceptionMetadata);
    ASSERT_TRUE(HandlerABI);
    unsigned Changed = 0;
    for (auto &Block : Function.Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.Addr == F.OuterCallVA) {
          Op.Output = Output;
          ++Changed;
        }
    ASSERT_EQ(Changed, 1u);
    const auto Callees = Index.contracts(Function);
    const auto Cleanups = Index.cleanupContracts(Function);
    const auto Stacks = Index.stackContracts(Function);
    ASSERT_TRUE(Callees && Cleanups && Stacks);
    ConditionalFrame Inputs;
    Inputs.Arguments.emplace(8, FrameValue::frame(76));
    Inputs.Objects = {{76, 80}};
    Inputs.ObjectCells.emplace(76, FrameValue::constant(1));
    Inputs.Registers.emplace(x86reg::RCX / x86reg::GeneralRegStride,
                             FrameValue::constant(17));
    Inputs.ProveCxxCall =
        [&](const LowFunc &Caller, const LowOp &Call, const FrameState &Frame,
            const std::set<int32_t> &Initialized,
            const BoundCxxBorrowStorage &Storage,
            size_t &Used) -> std::optional<BoundCxxCalleeEffect> {
      if (!checkedRegistrationDirectCall(F.Image, Caller, Call, Used))
        return std::nullopt;
      return proveBoundCxxCallee(F.Image, Call, Frame, 0, Used, Inputs.Objects,
                                 &Initialized, nullptr, &Storage);
    };
    RegistrationStateSolver Solver(Function, Data, 0, &*Callees, &*Cleanups,
                                   &*Stacks, nullptr, nullptr, &Inputs, nullptr,
                                   nullptr, &*HandlerABI);
    const auto States = Solver.run(Work);
    const bool FullResult =
        Output.Size == 4 && (Output.isTemp() || Output.Offset == x86reg::RAX);
    EXPECT_EQ(States.Complete && Inputs.Complete, FullResult);
  }
}

TEST(RegistrationConditionalCxx, ReturnFactsCoverEveryReachedExit) {
  for (bool Nested : {false, true})
    for (unsigned Mode = 0; Mode != 6; ++Mode) {
      SCOPED_TRACE(::testing::Message() << Nested << " " << Mode);
      BoundCxxFixture F(BoundCxxFixture::ReturnMode::Scalar);
      if (Mode == 1 || Mode == 5)
        F.overwrite(F.ReturnVA,
                    {0x85, uint8_t(Mode == 1 ? 0xdb : 0xc0), 0x75, 3, 0xc2, 4,
                     0, 0xff, 0xe2}); // An unproved indirect tail.
      if (Mode == 2)
        F.overwrite(F.ReturnVA, {0xff, 0xe2, 0x90}); // No ordinary return.
      if (Mode == 3)
        F.overwrite(F.ReturnVA,
                    {0x85, 0xdb, 0x75, 3, 0xc2, 4, 0, 0xb8, 1, 0, 0, 0, 0xc2, 4,
                     0}); // A second, differing return occurrence.
      if (Mode == 4)
        F.overwrite(F.ReturnVA, {0x85, 0xdb, 0x75, 3, 0xc2, 4, 0, 0xeb,
                                 0xfe}); // A closed nonreturning loop.
      F.Image.ExceptionMetadata = {};
      coff_loader::parseX86RegistrationExceptions(F.Image);
      if (Nested)
        F.addOuter(true, false, false, Outer, false, true);
      size_t Work = 0;
      const auto Effect = F.prove(Work);
      const bool Valid = Mode != 1 && Mode != 2 && (!Nested || Mode != 3);
      ASSERT_EQ(bool(Effect), Valid);
      if (!Effect)
        continue;
      ASSERT_EQ(Effect->Returns.size(), Mode == 3 ? 2u : 1u);
      EXPECT_EQ(Effect->Returns.front().Value, FrameValue::constant(0));
      if (Mode == 3)
        EXPECT_EQ(Effect->Returns.back().Value, FrameValue::constant(1));
    }
}

TEST(RegistrationConditionalCxx, NestedMayWriteKeepsInitializationSeparate) {
  BoundCxxFixture F;
  F.addOuter(true, false, true);
  size_t Work = 0;
  const auto Effect = F.prove(Work);
  ASSERT_TRUE(Effect);
  ASSERT_EQ(Effect->Writes.size(), 1u);
  EXPECT_EQ(Effect->Writes.front().Begin, -28);
  EXPECT_EQ(Effect->Writes.front().End, -24);
  ASSERT_EQ(Effect->Returns.size(), 1u);
  EXPECT_EQ(Effect->Returns.front().Value.Offset, -28);
}

TEST(RegistrationConditionalCxx, InitializedBytesDoNotEraseOverlappingTaint) {
  BoundCxxFixture F;
  F.overwrite(F.StoreVA,
              {0x8b, 0x10}); // Read the incoming word before any write.
  F.Parent.Cells.erase(-28);
  std::set<int32_t> Initialized;
  for (const auto Begin : {-96, -28})
    for (int32_t Byte = Begin; Byte != Begin + 4; ++Byte)
      Initialized.insert(Byte);
  auto Prove = [&] {
    size_t Work = 0;
    return proveBoundCxxCallee(F.Image, F.Call, F.Parent, 4, Work, {},
                               &Initialized);
  };
  ASSERT_TRUE(Prove()); // Initialized scalar with no retained value cell.
  Initialized.erase(-25);
  EXPECT_FALSE(Prove());
  Initialized.insert(-25);
  ASSERT_TRUE(Prove());
  F.Parent.Cells[-29] = FrameValue::frame(-80);
  EXPECT_FALSE(Prove()); // Three bytes still contain a possible frame alias.
}

TEST(RegistrationConditionalCxx, OutputBorrowChecksEveryReadAndReturnPath) {
  using Access = BoundCxxFixture::OutputAccess;
  for (const auto Mode :
       {Access::StoreWord, Access::ReadWord, Access::StoreByte,
        Access::StoreByteThenReadWord, Access::StoreWordThenReadWord,
        Access::StoreOneArm, Access::StoreOneArmThenReadWord,
        Access::StoreBothArmsThenReadWord}) {
    SCOPED_TRACE(unsigned(Mode));
    BoundCxxFixture F(BoundCxxFixture::ReturnMode::Alias, false, false, false,
                      false, std::nullopt, Mode);
    F.Parent.Cells.erase(-28);
    F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
    const std::set<int32_t> Initialized{-96, -95, -94, -93};
    auto Prove = [&](size_t &Work) {
      return proveBoundCxxCallee(F.Image, F.Call, F.Parent, 4, Work, {},
                                 &Initialized);
    };
    size_t Work = 0;
    const auto Effect = Prove(Work);
    const bool Accepted = Mode != Access::ReadWord &&
                          Mode != Access::StoreByteThenReadWord &&
                          Mode != Access::StoreOneArmThenReadWord;
    ASSERT_EQ(bool(Effect), Accepted);
    if (!Effect)
      continue;
    EXPECT_TRUE(Effect->RequiredInitializedInputs.empty());
    ASSERT_EQ(Effect->Objects.size(), 1u);
    EXPECT_EQ(Effect->Objects.front().Begin, -28);
    EXPECT_EQ(Effect->Objects.front().End, -24);
    ASSERT_FALSE(Effect->Writes.empty());
    if (Mode == Access::StoreOneArm)
      EXPECT_TRUE(Effect->InitializedOnReturn.empty());
    else {
      ASSERT_EQ(Effect->InitializedOnReturn.size(), 1u);
      EXPECT_EQ(Effect->InitializedOnReturn.front().Begin, -28);
      EXPECT_EQ(Effect->InitializedOnReturn.front().End,
                Mode == Access::StoreByte ? -27 : -24);
    }
    ASSERT_GT(Work, 0u);
    ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
    size_t Exact = limits::kMaxRegistrationEHStateWork - Work;
    EXPECT_TRUE(Prove(Exact));
    EXPECT_EQ(Exact, limits::kMaxRegistrationEHStateWork);
    size_t Limited = limits::kMaxRegistrationEHStateWork - Work + 1;
    EXPECT_FALSE(Prove(Limited));
    EXPECT_EQ(Limited, limits::kMaxRegistrationEHStateWork);
  }
}

TEST(RegistrationConditionalCxx,
     NestedOutputBorrowRetainsDefiniteInitialization) {
  BoundCxxFixture F(BoundCxxFixture::ReturnMode::Alias, false, false, false,
                    false, std::nullopt,
                    BoundCxxFixture::OutputAccess::StoreWordThenReadWord);
  F.addOuter(true, false, true);
  F.Parent.Cells.erase(-28);
  const std::set<int32_t> Initialized{-96, -95, -94, -93};
  size_t Work = 0;
  const auto Effect =
      proveBoundCxxCallee(F.Image, F.Call, F.Parent, 4, Work, {}, &Initialized);
  ASSERT_TRUE(Effect);
  EXPECT_TRUE(Effect->RequiredInitializedInputs.empty());
  ASSERT_EQ(Effect->InitializedOnReturn.size(), 1u);
  EXPECT_EQ(Effect->InitializedOnReturn.front().Begin, -28);
  EXPECT_EQ(Effect->InitializedOnReturn.front().End, -24);
  ASSERT_FALSE(Effect->Reads.empty());
}

TEST(RegistrationConditionalCxx,
     OutputInitializationRequiresEveryNormalReturn) {
  for (bool BothWrite : {false, true}) {
    SCOPED_TRACE(BothWrite);
    BoundCxxFixture F(BoundCxxFixture::ReturnMode::Alias, false, false, false,
                      false, std::nullopt,
                      BoundCxxFixture::OutputAccess::StoreOneArm);
    auto &Text = F.Image.Segments.front().Data;
    const va_t Tail = F.StoreVA + 6;
    const std::vector<uint8_t> Epilogue(Text.begin() + Tail - Entry,
                                        Text.begin() + F.ReturnVA + 3 - Entry);
    // Split the paths before the original join. Each path has its own
    // complete cookie/chain epilogue and physical RET, but only one may write.
    const va_t Second = F.ReturnVA + 7;
    ASSERT_LE(Second + Epilogue.size() + (BothWrite ? 2 : 0), Handler);
    ASSERT_LE(Second - (F.StoreVA + 4), 127u);
    Text[F.StoreVA + 3 - Entry] = uint8_t(Second - (F.StoreVA + 4));
    if (BothWrite)
      F.overwrite(Second, {0x89, 0x10});
    const va_t Copy = Second + (BothWrite ? 2 : 0);
    std::copy(Epilogue.begin(), Epilogue.end(), Text.begin() + Copy - Entry);
    const va_t CookieCall = Copy + F.ResultVA - 5 - Tail;
    ASSERT_EQ(Text[CookieCall - Entry], 0xe8);
    writeLE<uint32_t>(Text.data() + CookieCall + 1 - Entry,
                      uint32_t(Checker - (CookieCall + 5)));
    F.Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(F.Image);
    F.Parent.Cells.erase(-28);
    F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
    const std::set<int32_t> Initialized{-96, -95, -94, -93};
    size_t Work = 0;
    const auto Effect = proveBoundCxxCallee(F.Image, F.Call, F.Parent, 4, Work,
                                            {}, &Initialized);
    ASSERT_TRUE(Effect);
    ASSERT_EQ(Effect->Returns.size(), 2u);
    ASSERT_FALSE(Effect->Writes.empty());
    EXPECT_TRUE(Effect->RequiredInitializedInputs.empty());
    ASSERT_EQ(Effect->InitializedOnReturn.size(), BothWrite ? 1u : 0u);
    if (BothWrite) {
      EXPECT_EQ(Effect->InitializedOnReturn.front().Begin, -28);
      EXPECT_EQ(Effect->InitializedOnReturn.front().End, -24);
    }
  }
}

TEST(RegistrationConditionalCxx, RootInvocationDoesNotGrantNativeCallABI) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BoundCxxFixture F(
        BoundCxxFixture::ReturnMode::Scalar, false, false, false, false,
        std::nullopt,
        Mutation == 3 ? BoundCxxFixture::OutputAccess::ReadWord
                      : BoundCxxFixture::OutputAccess::StoreWordThenReadWord);
    if (Mutation >= 5) {
      Import Extra;
      Extra.Module = "authored.dll";
      Extra.Name = "unused_by_parent";
      Extra.IATAddr = Data + 12;
      F.Image.Imports.push_back(Extra);
    }
    F.addOuter(false, false, false, Outer, false, false,
               Mutation >= 5
                   ? std::optional<va_t>{Data + (Mutation == 5 ? 12 : 16)}
                   : std::nullopt);
    auto &Text = F.Image.Segments.front().Data;
    const std::vector<uint8_t> Save{0x89, 0x4d, 0xe8};
    const auto Saved = std::search(Text.begin() + Outer - Entry,
                                   Text.begin() + F.OuterCallVA - Entry,
                                   Save.begin(), Save.end());
    ASSERT_NE(Saved, Text.begin() + F.OuterCallVA - Entry);
    // Give the callee a scalar register, without an invented root input.
    F.overwrite(Entry + (Saved - Text.begin()), {0x31, 0xc9, 0x90});
    // The cleanup owns EBP-20. Its separate output at EBP-28 is uninitialized.
    // Mutations instead lend the state slot or the outgoing argument word.
    F.overwrite(F.OuterCallVA - 4, {0x8d, 0x45,
                                    uint8_t(Mutation == 1   ? 0xfc
                                            : Mutation == 2 ? 0xdc
                                                            : 0xe4),
                                    0x50});
    const std::vector<uint8_t> Return{0x8b, 0xe5, 0x5d, 0xc2, 4, 0};
    const auto Ret = std::search(Text.begin() + F.OuterResultVA - Entry,
                                 Text.begin() + OuterCaller - Entry,
                                 Return.begin(), Return.end());
    ASSERT_NE(Ret, Text.begin() + OuterCaller - Entry);
    // A zero-argument parent cannot hide the missing callee native ABI behind
    // an unrelated rejection of its own RET cleanup.
    F.overwrite(Entry + (Ret - Text.begin()) + 3, {0xc3, 0x90, 0x90});
    if (Mutation == 4)
      F.overwrite(F.StoreVA, {0xcc});
    F.Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(F.Image);
    Decoder Decode;
    ASSERT_TRUE(Decode.init(F.Image));
    size_t Work = 0;
    RegistrationCallCalleeIndex Index(F.Image, &Work);
    CFGBuilder Builder;
    Builder.setRegistrationCalleeIndex(&Index);
    auto Function = Builder.buildIntegerFrameProof(F.Image, Decode, Outer,
                                                   "authored-root-borrow");
    ASSERT_TRUE(Function.hasCompleteLiftCoverage());
    ASSERT_TRUE(Function.RegistrationStates);
    auto &States = *Function.RegistrationStates;
    const bool Valid = Mutation == 0 || Mutation == 6;
    EXPECT_EQ(States.Complete, Valid);
    EXPECT_EQ(States.SourceCallFrameEffectsComplete, Valid);
    EXPECT_FALSE(States.CallFrameEffectsComplete);
    EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Function, F.Image));
    for (const auto &Effect : States.CallFrameEffects)
      EXPECT_NE(Effect.Address, F.OuterCallVA);
    if (Valid) {
      EXPECT_TRUE(States.SecurityCookiesComplete);
      EXPECT_TRUE(States.RegistrationLifetimeComplete);
      // Summary flags cannot replace the missing physical call receipt.
      States.CallFrameEffectsComplete = true;
      EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Function, F.Image));
    }
    if (Mutation == 0) {
      ASSERT_GT(Work, 0u);
      ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
      for (unsigned Short : {0u, 1u}) {
        SCOPED_TRACE(Short);
        size_t Budget = limits::kMaxRegistrationEHStateWork - Work + Short;
        RegistrationCallCalleeIndex LimitedIndex(F.Image, &Budget);
        CFGBuilder LimitedBuilder;
        LimitedBuilder.setRegistrationCalleeIndex(&LimitedIndex);
        const auto Limited = LimitedBuilder.buildIntegerFrameProof(
            F.Image, Decode, Outer, "authored-root-budget");
        ASSERT_TRUE(Limited.RegistrationStates);
        EXPECT_EQ(Limited.RegistrationStates->SourceCallFrameEffectsComplete,
                  Short == 0);
        EXPECT_FALSE(Limited.RegistrationStates->CallFrameEffectsComplete);
        EXPECT_EQ(Budget, limits::kMaxRegistrationEHStateWork);
      }
    }
  }
}

TEST(RegistrationConditionalCxx, RootBorrowExtentRequiresCallerStorage) {
  struct Case {
    const char *Name;
    int8_t Output;
    bool ReadOnly;
    bool Initialized;
    int8_t PointerStore;
    uint8_t SecondStore;
    bool Accepted;
  };
  for (bool Nested : {false, true}) {
    SCOPED_TRACE(Nested);
    for (const auto &C : {
             Case{"write then read second field", -28, false, false, 0, 4,
                  true},
             Case{"second field is SavedESP", -20, false, false, 0, 4, false},
             Case{"outgoing argument", -36, false, false, 0, 4, false},
             Case{"cookie", -32, false, false, 0, 4, false},
             Case{"below live stack", -40, false, false, 0, 4, false},
             Case{"uninitialized second field", -28, true, false, 0, 4, false},
             Case{"initialized second field", -28, true, true, 0, 4, true},
             Case{"adjacent frame pointer", -28, false, false, -24, 4, false},
             Case{"overlapping pointer bytes", -28, false, false, -22, 4,
                  false},
             Case{"store reaches SavedESP", -28, false, true, 0, 12, false},
             Case{"neighboring scalar storage", -24, false, true, 0, 4, true},
         }) {
      SCOPED_TRACE(C.Name);
      BoundCxxFixture F(
          BoundCxxFixture::ReturnMode::Scalar, false, false, false, false,
          std::nullopt,
          C.ReadOnly
              ? BoundCxxFixture::OutputAccess::ReadSecondWord
              : BoundCxxFixture::OutputAccess::StoreTwoWordsThenReadSecond);
      if (Nested)
        F.addOuter();
      const va_t Root = Nested ? Outer + 0x800 : Outer;
      const va_t RootCaller = Root + (OuterCaller - Outer);
      F.addOuter(false, false, false, Root, false, false, std::nullopt, true);
      auto &Text = F.Image.Segments.front().Data;
      const std::vector<uint8_t> Save{0x89, 0x4d, 0xe8};
      const auto Saved = std::search(Text.begin() + Root - Entry,
                                     Text.begin() + F.OuterCallVA - Entry,
                                     Save.begin(), Save.end());
      ASSERT_NE(Saved, Text.begin() + F.OuterCallVA - Entry);
      const auto SaveVA = Entry + (Saved - Text.begin());
      if (C.PointerStore)
        F.overwrite(SaveVA, {0x89, 0x6d, uint8_t(C.PointerStore)});
      else if (!C.Initialized)
        F.overwrite(SaveVA, {0x90, 0x90, 0x90});
      F.overwrite(F.OuterCallVA - 4, {0x8d, 0x45, uint8_t(C.Output), 0x50});
      if (!C.ReadOnly)
        F.overwrite(F.StoreVA + 4, {C.SecondStore});
      const std::vector<uint8_t> Return{0x8b, 0xe5, 0x5d, 0xc2, 4, 0};
      const auto Ret = std::search(Text.begin() + F.OuterResultVA - Entry,
                                   Text.begin() + RootCaller - Entry,
                                   Return.begin(), Return.end());
      ASSERT_NE(Ret, Text.begin() + RootCaller - Entry);
      F.overwrite(Entry + (Ret - Text.begin()) + 3, {0xc3, 0x90, 0x90});
      F.Image.ExceptionMetadata = {};
      coff_loader::parseX86RegistrationExceptions(F.Image);
      Decoder Decode;
      ASSERT_TRUE(Decode.init(F.Image));
      CFGBuilder Builder;
      const auto Function = Builder.buildIntegerFrameProof(
          F.Image, Decode, Root, "authored-root-storage");
      ASSERT_TRUE(Function.hasCompleteLiftCoverage());
      ASSERT_TRUE(Function.RegistrationStates);
      const auto &States = *Function.RegistrationStates;
      EXPECT_EQ(States.Complete, C.Accepted);
      EXPECT_EQ(States.SourceCallFrameEffectsComplete, C.Accepted);
      EXPECT_FALSE(States.CallFrameEffectsComplete);
      EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Function, F.Image));
      for (const auto &Effect : States.CallFrameEffects)
        EXPECT_NE(Effect.Address, F.OuterCallVA);
    }
  }
}

TEST(RegistrationConditionalCxx, WordBorrowDoesNotInventAdjacentStorage) {
  BoundCxxFixture F(BoundCxxFixture::ReturnMode::Scalar, false, false, false,
                    false, std::nullopt,
                    BoundCxxFixture::OutputAccess::StoreTwoWordsThenReadSecond);
  F.Parent.Cells[-24] = FrameValue::constant(0);
  size_t Work = 0;
  EXPECT_FALSE(F.prove(Work));
}

TEST(RegistrationConditionalCxx, DiscardingOpaqueReadDoesNotProveItsEffects) {
  for (bool Nested : {false, true}) {
    for (bool Borrowed : {true, false}) {
      for (bool Discard : {true, false}) {
        SCOPED_TRACE(Nested);
        SCOPED_TRACE(Borrowed);
        SCOPED_TRACE(Discard);
        BoundCxxFixture F(
            BoundCxxFixture::ReturnMode::Scalar, false, false, false, false,
            std::nullopt,
            BoundCxxFixture::OutputAccess::StoreBothArmsThenReadWord);
        F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
        // An opaque address can reach a frame pointer outside this borrow.
        // Even a discarded read still lacks mapped-storage and exception
        // resumption evidence: a fault callback can write before returning.
        F.Parent.Cells[-48] = FrameValue::frame(-80);
        F.overwrite(F.StoreVA, {0x8b, uint8_t(Borrowed ? 0x10 : 0x12),
                                uint8_t(Discard ? 0x31 : 0x90),
                                uint8_t(Discard ? 0xd2 : 0x90), 0x89, 0x10,
                                0x90, 0x90, 0x90, 0x90, 0x90, 0x90});
        F.Image.ExceptionMetadata = {};
        coff_loader::parseX86RegistrationExceptions(F.Image);
        if (Nested)
          F.addOuter();
        size_t Work = 0;
        const auto Effect = F.prove(Work);
        ASSERT_EQ(bool(Effect), Borrowed);
        if (Effect) {
          ASSERT_EQ(Effect->Reads.size(), 1u);
          EXPECT_EQ(Effect->Reads.front().Begin, -28);
          EXPECT_EQ(Effect->Reads.front().End, -24);
          ASSERT_EQ(Effect->Writes.size(), 1u);
          EXPECT_EQ(Effect->Writes.front().Begin, -28);
          EXPECT_EQ(Effect->Writes.front().End, -24);
          ASSERT_EQ(Effect->Returns.size(), 1u);
          EXPECT_FALSE(Effect->Returns.front().Value.MayBeFrame);
        }
      }
    }
  }
}

TEST(RegistrationConditionalCxx, ImageReadsRequireCurrentImmutableStorage) {
  constexpr va_t ReadData = 0x404000;
  for (bool Wide : {false, true}) {
    for (bool Nested : {false, true}) {
      for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
        SCOPED_TRACE(Wide);
        SCOPED_TRACE(Nested);
        SCOPED_TRACE(Mutation);
        BoundCxxFixture F(
            BoundCxxFixture::ReturnMode::Scalar, false, false, false, false,
            std::nullopt,
            BoundCxxFixture::OutputAccess::StoreBothArmsThenReadWord);
        Segment Storage;
        Storage.VA = ReadData;
        Storage.Size = Storage.FileSz = 32;
        Storage.Flags = SegmentFlags::Readable;
        Storage.Data.resize(Storage.Size);
        Section StorageSection;
        StorageSection.VA = Storage.VA;
        StorageSection.Size = StorageSection.FileSz = Storage.Size;
        StorageSection.Flags = Storage.Flags;
        const unsigned Width = Wide ? 8 : 4;
        const va_t Address = ReadData + (Mutation == 7 ? 30 : 0);
        F.overwrite(F.StoreVA,
                    {uint8_t(Wide ? 0xdd : 0x8b), uint8_t(Wide ? 0x05 : 0x15),
                     uint8_t(Address), uint8_t(Address >> 8),
                     uint8_t(Address >> 16), uint8_t(Address >> 24), 0x89, 0x10,
                     0x90, 0x90, 0x90, 0x90});
        if (Wide)
          F.overwrite(F.StoreVA + 6, {0xdd, 0xd8, 0x31, 0xd2, 0x89, 0x10});
        if (Mutation == 1 || Mutation == 8)
          StorageSection.Flags = Storage.Flags =
              SegmentFlags::Readable | SegmentFlags::Writable;
        if (Mutation == 2)
          StorageSection.Flags = Storage.Flags =
              SegmentFlags::Readable | SegmentFlags::Executable;
        if (Mutation == 3)
          StorageSection.FileSz = Storage.FileSz = 0;
        F.Image.Segments.push_back(Storage);
        F.Image.Sections.push_back(StorageSection);
        if (Mutation == 4)
          F.Image.Segments.push_back(Storage);
        if (Mutation == 5) {
          StorageSection.VA += 1;
          StorageSection.Size = StorageSection.FileSz = 4;
          F.Image.Sections.push_back(StorageSection);
        }
        if (Mutation == 6)
          F.Image.DataPtrRelocSlots.insert(ReadData + 1);
        if (Mutation == 8) {
          // Writable mapped data still cannot supply a closed source write.
          F.overwrite(F.StoreVA, {0x89, 0x15});
          F.overwrite(F.StoreVA + 6, {0x31, 0xd2, 0x89, 0x10, 0x90, 0x90});
        }
        F.Image.ExceptionMetadata = {};
        coff_loader::parseX86RegistrationExceptions(F.Image);
        if (Nested)
          F.addOuter();
        size_t Work = 0;
        const auto Effect = F.prove(Work);
        ASSERT_EQ(bool(Effect), Mutation == 0);
        if (!Effect)
          continue;
        EXPECT_TRUE(llvm::any_of(Effect->ImageReads, [&](const auto &Read) {
          return Read.Begin <= ReadData && ReadData + Width <= Read.End;
        }));
        ASSERT_EQ(Effect->Writes.size(), 1u);
        EXPECT_EQ(Effect->Writes.front().Begin, -28);
        EXPECT_EQ(Effect->Writes.front().End, -24);
        ASSERT_GT(Work, 0u);
        ASSERT_LT(Work, limits::kMaxRegistrationEHStateWork);
        for (unsigned Short : {0u, 1u}) {
          size_t Budget = limits::kMaxRegistrationEHStateWork - Work + Short;
          EXPECT_EQ(bool(F.prove(Budget)), Short == 0);
          EXPECT_EQ(Budget, limits::kMaxRegistrationEHStateWork);
        }
      }
    }
  }
}

TEST(RegistrationConditionalCxx, StackPopPreservesOverlappingPointerBytes) {
  BoundCxxFixture F;
  F.overwrite(F.ReturnVA,
              {0xc2, 8, 0}); // Second argument is intentionally unused.
  F.addOuter(true, true, false, Outer, true);
  F.overwrite(F.OuterCallVA - 6, {0x31, 0xc0, 0x90});
  size_t Work = 0;
  ASSERT_TRUE(F.prove(Work)); // An overlapping scalar spill is harmless.
  F.overwrite(F.OuterCallVA - 6, {0x8b, 0x45, 8});
  Work = 0;
  EXPECT_FALSE(F.prove(Work));
}

TEST(RegistrationConditionalCxx, NestedThrowRetainsBothCleanupObligations) {
  for (const auto Target : {Destructor, OuterDestructor}) {
    SCOPED_TRACE(Target);
    BoundCxxFixture F(BoundCxxFixture::ReturnMode::Alias, false, false, true);
    F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
    F.addOuter();
    F.separateOuterCleanup();
    size_t Work = 0;
    const auto Effect = F.prove(Work);
    ASSERT_TRUE(Effect);
    ASSERT_EQ(Effect->Writes.size(), 1u);
    EXPECT_EQ(Effect->Writes.front().Begin, -28);
    ASSERT_EQ(Effect->Returns.size(), 1u);
    EXPECT_EQ(Effect->Returns.front().Value.Offset, -28);
    F.overwrite(Target, {0x0f, 0x0b});
    Work = 0;
    EXPECT_FALSE(F.prove(Work));
  }
}

TEST(RegistrationConditionalCxx, BoundsAcyclicNestingWithoutCachingInputs) {
  BoundCxxFixture F;
  const auto Depth = limits::ir::kRegistrationCxxMaxBoundCallDepth;
  for (unsigned I = 1; I < Depth; ++I)
    F.addOuter(true, false, false, Outer + (I - 1) * 0x280);
  size_t Work = 0;
  ASSERT_TRUE(F.prove(Work));
  F.addOuter(true, false, false, Outer + (Depth - 1) * 0x280);
  ASSERT_LE(Entry + F.Image.Segments.front().Size, Table);
  ASSERT_TRUE(F.Image.ExceptionMetadata.findFunction(F.Call.Inputs[0].Offset));
  Work = 0;
  EXPECT_FALSE(F.prove(Work));
  BoundCxxFixture Fresh;
  Work = 0;
  EXPECT_TRUE(Fresh.prove(Work));
}

TEST(RegistrationConditionalCxx, ReplaysNestedCallsAndTheirAdministration) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BoundCxxFixture F;
    F.addOuter(false);
    size_t Work = 0;
    ASSERT_TRUE(F.prove(Work));
    switch (Mutation) {
    case 0: // The helper cannot borrow its caller's live try-state word.
      F.overwrite(F.OuterCallVA - 2, {0xfc});
      break;
    case 1: // Declared stack facts cannot hide a changed physical return.
      F.overwrite(F.ReturnVA, {0xc2, 8, 0});
      break;
    case 2: // A recursive invocation cannot prove itself.
      writeLE<uint32_t>(F.Image.Segments.front().Data.data() + F.OuterCallVA +
                            1 - Entry,
                        uint32_t(Outer - (F.OuterCallVA + 5)));
      break;
    case 3: // No initialized outgoing pointer remains after this edit.
      F.overwrite(F.OuterCallVA - 4, {0x83, 0xec, 4, 0x90});
      break;
    case 4:
      F.overwrite(F.OuterCallVA, {0xe9});
      break;
    case 5:
      F.overwrite(Child, {0x64, 0xa1, 0, 0, 0, 0, 0x8b, 0x40, 20, 0xa3, 0x20,
                          0x30, 0x40, 0, 0xc3});
      break;
    case 6:
      F.overwrite(Destructor,
                  {0xc7, 0x05, 4, 0x30, 0x40, 0, 0, 0, 0, 0, 0x31, 0xc0, 0xc3});
      break;
    }
    Work = 0;
    EXPECT_FALSE(F.prove(Work));
  }
}

TEST(RegistrationConditionalCxx, PlainObjectQueriesRejectRecursiveCxxEntries) {
  BoundCxxFixture F;
  F.addOuter(false);
  writeLE<uint32_t>(F.Image.Segments.front().Data.data() + F.OuterCallVA + 1 -
                        Entry,
                    uint32_t(Outer - (F.OuterCallVA + 5)));
  size_t Work = 0;
  EXPECT_FALSE(
      getCheckedX86RegistrationInitializerCalleeABI(F.Image, Outer, &Work));
  EXPECT_FALSE(getCheckedX86RegistrationObjectCalleeABI(F.Image, Outer, &Work));
  EXPECT_FALSE(getCheckedX86RegistrationMemberCalleeABI(F.Image, Outer, &Work));
  EXPECT_FALSE(getCheckedX86CalleeStackPop(F.Image, Outer, &Work));
  EXPECT_LT(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationConditionalCxx, ClosedThrowStillRequiresItsUnwindCleanup) {
  BoundCxxFixture F(BoundCxxFixture::ReturnMode::Alias, false, false, true);
  F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
  size_t Work = 0;
  const auto Effect = F.prove(Work);
  ASSERT_TRUE(Effect);
  ASSERT_EQ(Effect->Writes.size(), 1u);
  ASSERT_EQ(Effect->Returns.size(), 1u);
  EXPECT_EQ(Effect->Returns[0].Value.Offset, -28);
  F.overwrite(Destructor, {0x64, 0xa1, 0, 0, 0, 0, 0x8b, 0x40, 20, 0xa3, 0x20,
                           0x30, 0x40, 0, 0xc3});
  Work = 0;
  EXPECT_FALSE(F.prove(Work));
}

TEST(RegistrationConditionalCxx, ChildCannotRewriteRuntimeDispatchEvidence) {
  for (const auto Target : {Child, Destructor}) {
    SCOPED_TRACE(Target);
    BoundCxxFixture F;
    size_t Work = 0;
    ASSERT_TRUE(F.prove(Work));
    // The source call has a valid scalar/global-write ABI, but overwrites
    // the personality IAT while this helper publishes its incoming pointer.
    F.overwrite(Target,
                {0xc7, 0x05, 4, 0x30, 0x40, 0, 0, 0, 0, 0, 0x31, 0xc0, 0xc3});
    Work = 0;
    EXPECT_FALSE(F.prove(Work));
  }
}

TEST(RegistrationConditionalCxx, RejectsCaptureAndIncompleteCurrentEvidence) {
  for (unsigned Mutation = 0; Mutation != 16; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BoundCxxFixture F;
    size_t BaselineWork = 0;
    ASSERT_TRUE(F.prove(BaselineWork));
    switch (Mutation) {
    case 0: // Scalar overwrite cannot launder an unknown full ECX input.
      F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] =
          FrameValue::taint(true);
      break;
    case 1:
      F.Parent.Cells.erase(-28);
      break;
    case 2: // Borrow overlaps the incoming carrier.
      F.Parent.Cells[-96] = FrameValue::frame(-96);
      break;
    case 3:
      F.Parent.Cells[-28] = FrameValue::frame(-80);
      break;
    case 4:
      F.overwrite(F.ReturnVA, {0xc2, 8, 0});
      break;
    case 5:
      F.overwrite(Handler + 9, {0xed}); // Changed guard slot.
      break;
    case 6:
      F.overwrite(F.StoreVA, {0x89, 0x00}); // *output = output.
      break;
    case 7: // Ambient capture through FS0 and the incoming pointer cell.
    case 8: {
      const auto Target = Mutation == 7 ? Child : Destructor;
      F.overwrite(Target, {0x64, 0xa1, 0, 0, 0, 0, 0x8b, 0x40, 20, 0xa3, 0x20,
                           0x30, 0x40, 0, 0xc3});
      break;
    }
    case 9:
      ++F.Call.Addr;
      break;
    case 10:
      F.Image.Segments.front().Flags =
          F.Image.Segments.front().Flags | SegmentFlags::Writable;
      break;
    case 11:
      F.Image.Segments[1].Data[0x30] = 0; // Cyclic cleanup state.
      break;
    case 12:
      F.overwrite(Child, {0x0f, 0x0b}); // Incomplete child.
      break;
    case 13:
      F.Parent.Cells[-96] = FrameValue::frame(0); // Saved caller EBP.
      F.Parent.Cells[0] = FrameValue::constant(1);
      break;
    case 14:
      F.Image.Format = BinaryFormat::ELF;
      break;
    case 15:
      F.Call.Seq = -1;
      break;
    }
    size_t Work = 0;
    EXPECT_FALSE(F.prove(Work));
  }
}
} // namespace
