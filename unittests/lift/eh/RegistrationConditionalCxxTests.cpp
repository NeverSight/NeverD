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

#include "../../../lib/ir/low/X86/RegistrationConditionalFrame.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/low/LowIR.h"
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
                           bool ThrowingChild = false) {
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
    NestedCallVA = Entry + Text.size();
    CallTo(Child);
    Bytes({0x8b, 0x45, 8, 0x8b, 0x55, 0xe8});
    if (CaptureBeforeWrite)
      Bytes({0x89, 0x00}); // Capture remains invalid after the next store.
    StoreVA = Entry + Text.size();
    Bytes({0x89, 0x10}); // *output = saved ECX.
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
                bool CrossPopSpill = false) {
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
    Bytes({0x33, 0xc5, 0x50, 0x8d, 0x45, 0xf4, 0x64, 0xa3, 0, 0,
           0,    0,    0x89, 0x4d, 0xe8, 0xc7, 0x45, 0xec, 1, 0,
           0,    0,    0xc7, 0x45, 0xfc, 0,    0,    0,    0});
    if (CrossPopSpill)
      Bytes({0x83, 0xec, 12, 0xc7, 0x45, 0xd4, 0, 0, 0, 0, 0x6a, 0});
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
    EXPECT_EQ(Effect->Returns[0].MayAliasFrame, Alias);
    EXPECT_EQ(Effect->Returns[0].BorrowedOffset,
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
    EXPECT_TRUE(Effect->Returns[0].MayAliasFrame);
    EXPECT_FALSE(Effect->Returns[0].BorrowedOffset);
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
  EXPECT_EQ(Effect->Returns[0].BorrowedOffset, -28);
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
    EXPECT_TRUE(Effect->Returns.front().MayAliasFrame);
    EXPECT_EQ(Effect->Returns.front().BorrowedOffset,
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
  EXPECT_EQ(Effect->Returns.front().BorrowedOffset, -28);
}

TEST(RegistrationConditionalCxx, InitializedBytesDoNotEraseOverlappingTaint) {
  BoundCxxFixture F;
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
    EXPECT_EQ(Effect->Returns.front().BorrowedOffset, -28);
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

TEST(RegistrationConditionalCxx, ClosedThrowStillRequiresItsUnwindCleanup) {
  BoundCxxFixture F(BoundCxxFixture::ReturnMode::Alias, false, false, true);
  F.Parent.Registers[x86reg::RCX / x86reg::GeneralRegStride] = {};
  size_t Work = 0;
  const auto Effect = F.prove(Work);
  ASSERT_TRUE(Effect);
  ASSERT_EQ(Effect->Writes.size(), 1u);
  ASSERT_EQ(Effect->Returns.size(), 1u);
  EXPECT_EQ(Effect->Returns[0].BorrowedOffset, -28);
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
