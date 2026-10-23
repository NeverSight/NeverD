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
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
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
