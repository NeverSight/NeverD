//===- RegistrationCleanupABITests.cpp - PE32 cleanup call relays --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check saved runtime EBP, adjusted object coordinates and leaf call bounds.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

namespace {
using namespace neverd;

struct CleanupCallImage {
  static constexpr va_t Entry = 0x401000;
  static constexpr va_t Leaf = Entry + 0x80;
  BinaryImage Image;
  unsigned Address = 0;
  unsigned Call = 0;
  unsigned End = 0;

  CleanupCallImage(bool WideAdjustment = false, bool WideObject = false,
                   int32_t Adjustment = 12, int32_t Object = -44) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = Entry;
    Segment Text;
    Text.Name = ".text";
    Text.VA = Entry;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(0x100, 0xcc);
    auto &Bytes = Text.Data;
    Bytes[0] = 0x55;
    Bytes[1] = WideAdjustment ? 0x81 : 0x83;
    Bytes[2] = 0xc5;
    if (WideAdjustment)
      writeLE<int32_t>(Bytes.data() + 3, Adjustment);
    else
      Bytes[3] = uint8_t(Adjustment);
    Address = WideAdjustment ? 7 : 4;
    Bytes[Address] = 0x8d;
    Bytes[Address + 1] = WideObject ? 0x8d : 0x4d;
    if (WideObject)
      writeLE<int32_t>(Bytes.data() + Address + 2, Object);
    else
      Bytes[Address + 2] = uint8_t(Object);
    Call = Address + (WideObject ? 6 : 3);
    Bytes[Call] = 0xe8;
    writeLE<uint32_t>(Bytes.data() + Call + 1,
                      uint32_t(Leaf - (Entry + Call + 5)));
    Bytes[Call + 5] = 0x5d;
    Bytes[Call + 6] = 0xc3;
    End = Call + 7;
    Bytes[0x80] = 0x8b; // mov eax, [ecx]
    Bytes[0x81] = 0x01;
    Bytes[0x82] = 0xc3;
    Text.Size = Text.FileSz = Bytes.size();
    Image.Segments.push_back(Text);
    Section Code;
    Code.Name = Text.Name;
    Code.VA = Text.VA;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    Image.Sections.push_back(Code);
  }
};

TEST(RegistrationCleanupABI, PreservesRuntimeEBPAroundAdjustedLeafCalls) {
  for (bool WideAdjustment : {false, true})
    for (bool WideObject : {false, true})
      for (int32_t Adjustment : {-12, 0, 12}) {
        CleanupCallImage F(WideAdjustment, WideObject, Adjustment);
        const auto Proof =
            getCheckedX86RegistrationCleanupRelayABI(F.Image, F.Entry);
        ASSERT_TRUE(Proof);
        ASSERT_EQ(Proof->Calls.size(), 1u);
        EXPECT_EQ(Proof->Calls[0].ObjectFrameOffset, Adjustment - 44);
        EXPECT_EQ(Proof->EndAddress, F.Entry + F.End);
        EXPECT_EQ(Proof->Calls[0].Leaf.Target, F.Leaf);
        ASSERT_EQ(Proof->Calls[0].Leaf.ECXReads.size(), 1u);
        EXPECT_EQ(Proof->Calls[0].Leaf.ECXReads[0].Begin, 0);
        EXPECT_EQ(Proof->Calls[0].Leaf.ECXReads[0].End, 4);
      }
}

TEST(RegistrationCleanupABI, RetainsEachObjectInACombinedCleanup) {
  for (bool ChangeSecondCall : {false, true}) {
    CleanupCallImage F;
    auto &Bytes = F.Image.Segments[0].Data;
    const unsigned Second = F.End - 2;
    Bytes[Second] = 0x8d;
    Bytes[Second + 1] = 0x4d;
    Bytes[Second + 2] = uint8_t(-48);
    Bytes[Second + 3] = ChangeSecondCall ? 0xe9 : 0xe8;
    writeLE<uint32_t>(Bytes.data() + Second + 4,
                      uint32_t(F.Leaf - (F.Entry + Second + 8)));
    Bytes[Second + 8] = 0x5d;
    Bytes[Second + 9] = 0xc3;
    const auto Proof =
        getCheckedX86RegistrationCleanupRelayABI(F.Image, F.Entry);
    if (ChangeSecondCall) {
      EXPECT_FALSE(Proof);
      continue;
    }
    ASSERT_TRUE(Proof);
    ASSERT_EQ(Proof->Calls.size(), 2u);
    EXPECT_EQ(Proof->EndAddress, F.Entry + Second + 10);
    EXPECT_EQ(Proof->Calls[0].ObjectFrameOffset, -32);
    EXPECT_EQ(Proof->Calls[1].ObjectFrameOffset, -36);
    EXPECT_EQ(Proof->Calls[0].Leaf.Target, F.Leaf);
    EXPECT_EQ(Proof->Calls[1].Leaf.Target, F.Leaf);
  }
}

TEST(RegistrationCleanupABI, RejectsChangedFrameCallAndReturnContracts) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CleanupCallImage F;
    auto &Bytes = F.Image.Segments[0].Data;
    switch (Mutation) {
    case 0:
      Bytes[0] = 0x53; // push ebx
      break;
    case 1:
      Bytes[2] = 0xc4; // add esp, 12
      break;
    case 2:
      Bytes[F.Address + 1] = 0x45; // lea eax, [ebp-44]
      break;
    case 3:
      Bytes[F.Call] = 0xe9;
      break;
    case 4:
      Bytes[F.End - 2] = 0x5b;
      break;
    case 5:
      Bytes[F.End - 1] = 0xc2;
      break;
    case 6:
      writeLE<uint32_t>(Bytes.data() + F.Call + 1, 0x40000000);
      break;
    case 7:
      Bytes[0x82] = 0xc2; // callee pops caller storage
      Bytes[0x83] = 4;
      Bytes[0x84] = 0;
      break;
    case 8:
      F.Image.BaseRelocations.push_back({F.Entry + F.Call + 1, 3});
      break;
    case 9:
      F.Image.Segments[0].Flags =
          F.Image.Segments[0].Flags | SegmentFlags::Writable;
      break;
    }
    EXPECT_FALSE(getCheckedX86RegistrationCleanupRelayABI(F.Image, F.Entry));
  }
}

TEST(RegistrationCleanupABI, RejectsOverflowAndExhaustedWork) {
  for (int32_t Adjustment : {INT32_MIN, INT32_MAX}) {
    CleanupCallImage F(true, true, Adjustment, Adjustment < 0 ? -1 : 1);
    EXPECT_FALSE(getCheckedX86RegistrationCleanupRelayABI(F.Image, F.Entry));
  }
  CleanupCallImage F;
  size_t Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(
      getCheckedX86RegistrationCleanupRelayABI(F.Image, F.Entry, &Work));
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationCleanupABI, ChecksReturnsAndEffectsFromTheCalledEntry) {
  for (bool Reachable : {false, true}) {
    CleanupCallImage F;
    auto &Bytes = F.Image.Segments[0].Data;
    Bytes[0x90] = 0xc2; // An independently addressed ret 4.
    Bytes[0x91] = 4;
    Bytes[0x92] = 0;
    writeLE<uint32_t>(Bytes.data() + 0xb0, F.Entry + 0x90);
    F.Image.CodePtrRelocSlots.insert(F.Entry + 0xb0);
    F.Image.KnownCodeRanges.emplace_back(F.Leaf, F.Entry + 0xc0);
    if (Reachable) {
      Bytes[0x80] = 0xeb;
      Bytes[0x81] = 0x0e;
    }
    const auto Proof = getCheckedX86RegistrationLeafCalleeABI(F.Image, F.Leaf);
    if (Reachable) {
      EXPECT_FALSE(Proof);
      continue;
    }
    ASSERT_TRUE(Proof);
    ASSERT_EQ(Proof->CodeRanges.size(), 1u);
    EXPECT_EQ(Proof->CodeRanges[0].Begin, F.Leaf);
    EXPECT_EQ(Proof->CodeRanges[0].End, F.Leaf + 3);
    EXPECT_EQ(Proof->StackPopBytes, 0u);
  }
}
} // namespace
