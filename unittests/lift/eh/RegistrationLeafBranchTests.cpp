//===- RegistrationLeafBranchTests.cpp - PE32 feasible leaf effects -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep leaf effects conditional on current all-path branch evidence.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstdint>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t TextVA = 0x401000;
constexpr va_t DataVA = 0x403000;

BinaryImage branchImage(const std::vector<uint8_t> &Code) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.Entry = TextVA;
  Segment Text;
  Text.Name = ".text";
  Text.VA = TextVA;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = Code;
  Text.Data.resize(0x81, 0xcc);
  Text.Data[0x80] = 0xc3;
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(Text);
  Segment Data;
  Data.Name = ".data";
  Data.VA = DataVA;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(16);
  Data.Size = Data.FileSz = Data.Data.size();
  Image.Segments.push_back(Data);
  for (const auto &Segment : Image.Segments) {
    Section Section;
    Section.Name = Segment.Name;
    Section.VA = Segment.VA;
    Section.Size = Section.FileSz = Segment.Size;
    Section.Flags = Segment.Flags;
    Image.Sections.push_back(Section);
  }
  Image.Symbols.push_back(Symbol::makeFunc(TextVA, Code.size()));
  Image.Symbols.push_back(Symbol::makeFunc(TextVA + 0x80, 1));
  return Image;
}
} // namespace

TEST(RegistrationLeafBranch, EffectsFollowTakenAndFallthroughEvidence) {
  for (bool TakenIsExit : {false, true})
    for (unsigned Condition : {0, 1, 2})
      for (unsigned Effect : {0, 1, 2}) {
        SCOPED_TRACE(::testing::Message()
                     << TakenIsExit << ':' << Condition << ':' << Effect);
        // A mutable zero-filled word is still unknown at this invocation.
        std::vector<uint8_t> Code =
            Condition == 2 ? std::vector<uint8_t>{0xa1, 0, 0, 0, 0}
                           : std::vector<uint8_t>{0xb8, 0, 0, 0, 0};
        writeLE<uint32_t>(Code.data() + 1, Condition == 2 ? DataVA : Condition);
        Code.insert(Code.end(), {0x85, 0xc0, 0x74, 0}); // TEST EAX; JZ.
        const size_t Displacement = Code.size() - 1;
        const size_t Fallthrough = Code.size();
        if (!TakenIsExit)
          Code.insert(Code.end(), {0x31, 0xc0, 0xc3});
        const size_t EffectOffset = Code.size();
        if (Effect == 0)
          Code.insert(Code.end(), {0xff, 0xd2}); // Opaque CALL EDX.
        else if (Effect == 1) {
          Code.insert(Code.end(), {0xe8, 0, 0, 0, 0});
          writeLE<uint32_t>(Code.data() + EffectOffset + 1,
                            0x80 - (EffectOffset + 5));
        } else {
          Code.insert(Code.end(), {0xa3, 0, 0, 0, 0}); // Image write.
          writeLE<uint32_t>(Code.data() + EffectOffset + 1, DataVA + 4);
        }
        const size_t ExitOffset = Code.size();
        Code.insert(Code.end(), {0x31, 0xc0, 0xc3});
        Code[Displacement] =
            (TakenIsExit ? ExitOffset : EffectOffset) - Fallthrough;
        const auto Image = branchImage(Code);
        const bool EffectReached =
            Condition == 2 || (TakenIsExit ? Condition != 0 : Condition == 0);
        for (auto Result : {RegistrationLeafResult::Observed,
                            RegistrationLeafResult::Discarded}) {
          size_t Work = 0;
          const auto Leaf = getCheckedX86RegistrationLeafCalleeABI(
              Image, Image.Entry, &Work, Result);
          ASSERT_EQ(bool(Leaf), Effect == 2 || !EffectReached);
          if (!Leaf)
            continue;
          EXPECT_TRUE(Leaf->HasIndependentScalarReturn);
          EXPECT_EQ(Leaf->ImageReads.size(), Condition == 2 ? 1u : 0u);
          EXPECT_EQ(Leaf->ImageWrites.size(), EffectReached ? 1u : 0u);
          if (EffectReached) {
            EXPECT_EQ(Leaf->ImageWrites.front().Begin, DataVA + 4);
            EXPECT_EQ(Leaf->ImageWrites.front().End, DataVA + 8);
          }
          if (Condition == 2) {
            EXPECT_EQ(Leaf->ImageReads.front().Begin, DataVA);
            EXPECT_EQ(Leaf->ImageReads.front().End, DataVA + 4);
          }
          Work = limits::kMaxRegistrationEHStateWork;
          EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(
              Image, Image.Entry, &Work, Result));
        }
      }
}

TEST(RegistrationLeafBranch, BackedgesReopenAFormerlyConstantChoice) {
  for (bool Reload : {false, true}) {
    SCOPED_TRACE(Reload);
    // The first visit cannot reach the call. The backedge supplies either a
    // different constant or a mutable loaded value; neither keeps that proof.
    std::vector<uint8_t> Code = {
        0x31, 0xc0, 0x85, 0xc0, 0x75, 7,    uint8_t(Reload ? 0xa1 : 0xb8),
        0,    0,    0,    0,    0xeb, 0xf5, 0xff,
        0xd2, 0x31, 0xc0, 0xc3};
    writeLE<uint32_t>(Code.data() + 7, Reload ? DataVA : 1);
    const auto Image = branchImage(Code);
    for (auto Result :
         {RegistrationLeafResult::Observed, RegistrationLeafResult::Discarded})
      EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(Image, Image.Entry,
                                                          nullptr, Result));
  }
}
