//===- RegistrationRealignedCleanupABITests.cpp - PE32 cleanup frames ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind generated cleanup coordinates to each parent, including cached relays.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

namespace {
using namespace neverd;

struct RealignedCleanupImage {
  static constexpr va_t Entry = 0x401000;
  static constexpr va_t Leaf = Entry + 0x80;
  BinaryImage Image;
  unsigned Saved = 0;
  unsigned Object = 0;
  unsigned Call = 0;
  unsigned End = 0;
  int32_t BaseOffset;

  RealignedCleanupImage(unsigned Wide = 0, unsigned Calls = 1,
                        int32_t Base = -60, int32_t SavedOffset = 40,
                        int32_t ObjectOffset = 20)
      : BaseOffset(Base) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = Entry;
    Segment Text;
    Text.Name = ".text";
    Text.VA = Entry;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    auto &Bytes = Text.Data;
    auto Word = [&](int32_t Value) {
      const size_t Offset = Bytes.size();
      Bytes.resize(Offset + 4);
      writeLE<int32_t>(Bytes.data() + Offset, Value);
    };
    auto Frame = [&](uint8_t Opcode, uint8_t ModRM, int32_t Offset,
                     bool WideOffset) {
      Bytes.push_back(Opcode);
      Bytes.push_back(ModRM + (WideOffset ? 0x40 : 0));
      if (WideOffset)
        Word(Offset);
      else
        Bytes.push_back(uint8_t(Offset));
    };
    Bytes.push_back(0x55);             // Save runtime EBP.
    Frame(0x8d, 0x75, Base, Wide & 1); // lea esi, [ebp+base]
    Saved = Bytes.size();
    Frame(0x8b, 0x6e, SavedOffset, Wide & 2); // mov ebp, [esi+saved]
    for (unsigned I = 0; I < Calls; ++I) {
      Object = Bytes.size();
      Frame(0x8d, 0x4e, ObjectOffset + 4 * I, Wide & 4);
      Call = Bytes.size();
      Bytes.push_back(0xe8);
      Word(Leaf - (Entry + Call + 5));
    }
    Bytes.push_back(0x5d);
    Bytes.push_back(0xc3);
    End = Bytes.size();
    Bytes.resize(0x100, 0xcc);
    Bytes[0x80] = 0x8b; // A returning leaf reads only the borrowed object.
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

  LowFunc parent() const {
    LowFunc Parent;
    auto &EH = Parent.ExceptionMetadata.emplace();
    auto &Chain = EH.Registration.emplace();
    Chain.SeededTryLevel = -1;
    Chain.RegistrationOffset = -12;
    Chain.TryLevelOffset = -4;
    Chain.RealignedFrame = RegistrationRealignedFrame{
        6, 0x40090f, 16, uint32_t(-BaseOffset + 4), BaseOffset, -20};
    EH.Cxx.emplace().UnwindMap = {
        {-1, Entry, CxxUnwindAction::ActionKind::Direct}};
    return Parent;
  }
};

TEST(RegistrationRealignedCleanupABI, ProjectsOrderedObjectsFromRuntimeEBP) {
  for (unsigned Wide = 0; Wide < 8; ++Wide)
    for (unsigned Calls : {1, 2}) {
      SCOPED_TRACE(Wide);
      SCOPED_TRACE(Calls);
      RealignedCleanupImage F(Wide, Calls);
      const auto Proof =
          getCheckedX86RegistrationCleanupRelayABI(F.Image, F.Entry);
      ASSERT_TRUE(Proof);
      ASSERT_TRUE(Proof->RealignedParent);
      EXPECT_EQ(Proof->RealignedParent->BaseOffset, -60);
      EXPECT_EQ(Proof->RealignedParent->SavedParentFrameOffset, -20);
      EXPECT_TRUE(Proof->matchesParentFrame(
          *F.parent().ExceptionMetadata->Registration));
      EXPECT_EQ(Proof->EndAddress, F.Entry + F.End);
      ASSERT_EQ(Proof->Calls.size(), Calls);
      for (unsigned I = 0; I < Calls; ++I) {
        EXPECT_EQ(Proof->Calls[I].ObjectFrameOffset, -40 + 4 * int(I));
        EXPECT_EQ(Proof->Calls[I].Leaf.Target, F.Leaf);
        EXPECT_EQ(Proof->Calls[I].Leaf.ECXReads,
                  (std::vector<RegistrationObjectExtent>{{0, 4}}));
      }
    }
  RealignedCleanupImage Large(7, 2, -476, 456, 340);
  const auto Proof =
      getCheckedX86RegistrationCleanupRelayABI(Large.Image, Large.Entry);
  ASSERT_TRUE(Proof);
  ASSERT_EQ(Proof->Calls.size(), 2u);
  EXPECT_EQ(Proof->Calls[0].ObjectFrameOffset, -136);
  EXPECT_EQ(Proof->Calls[1].ObjectFrameOffset, -132);
}

TEST(RegistrationRealignedCleanupABI, BindsCachedRelayToEachDispatchFrame) {
  for (bool MismatchFirst : {false, true})
    for (unsigned Mutation = 0; Mutation < 7; ++Mutation) {
      SCOPED_TRACE(Mutation);
      RealignedCleanupImage F;
      auto Good = F.parent();
      auto Bad = F.parent();
      auto &Chain = *Bad.ExceptionMetadata->Registration;
      switch (Mutation) {
      case 0:
        ++Chain.RealignedFrame->BaseOffset;
        break;
      case 1:
        ++Chain.RealignedFrame->SavedParentFrameOffset;
        break;
      case 2:
        Chain.RealignedFrame->BaseRegister = 7;
        break;
      case 3:
        Chain.RealignedFrame.reset();
        break;
      case 4:
        Chain.RegistrationOffset = -24;
        break;
      case 5:
        Chain.SeededTryLevel = -2;
        break;
      case 6:
        Bad.ExceptionMetadata->Registration.reset();
        break;
      }
      RegistrationCallCalleeIndex Index(F.Image);
      for (bool Mismatch : {MismatchFirst, !MismatchFirst, MismatchFirst}) {
        const auto Contracts = Index.cleanupContracts(Mismatch ? Bad : Good);
        ASSERT_TRUE(Contracts);
        EXPECT_EQ(Contracts->size(), Mismatch ? 0u : 1u);
      }
    }
}

TEST(RegistrationRealignedCleanupABI, RejectsChangedFrameOrLeafEffects) {
  for (unsigned Mutation = 0; Mutation < 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    RealignedCleanupImage F(7, 2);
    auto &Text = F.Image.Segments[0];
    auto &Bytes = Text.Data;
    switch (Mutation) {
    case 0:
      Bytes[0] = 0x56; // Save ESI instead of runtime EBP.
      break;
    case 1:
      Bytes[2] = 0xbd; // Establish EDI instead of ESI.
      break;
    case 2:
      Bytes[F.Saved] = 0x89; // Overwrite the saved entry frame.
      break;
    case 3:
      Bytes[F.Saved + 1] = 0xad; // Load EBP from runtime EBP, not ESI.
      break;
    case 4:
      Bytes[F.Object + 1] = 0x8d; // Object uses restored entry EBP.
      break;
    case 5:
      Bytes[F.Call] = 0xe9; // Tail jump would strand the saved runtime EBP.
      break;
    case 6:
      Bytes[F.End - 2] = 0x5e;
      break;
    case 7:
      Bytes[F.End - 1] = 0xc2;
      break;
    case 8:
      F.Image.BaseRelocations.push_back({F.Entry + 3, 3});
      break;
    case 9:
      F.Image.BaseRelocations.push_back({F.Entry + F.Saved + 2, 3});
      break;
    case 10:
      Text.Flags = Text.Flags | SegmentFlags::Writable;
      break;
    case 11:
      Bytes[0x80] = 0x31; // Leaf destroys the local-frame anchor.
      Bytes[0x81] = 0xf6;
      break;
    case 12:
      Bytes[0x80] = 0x31; // Leaf destroys entry EBP.
      Bytes[0x81] = 0xed;
      break;
    case 13:
      Bytes[0x82] = 0xc2; // Leaf consumes caller storage.
      Bytes[0x83] = 4;
      Bytes[0x84] = 0;
      break;
    case 14:
      F.Image.Sections[0].FileSz = F.End - 1;
      break;
    }
    EXPECT_FALSE(getCheckedX86RegistrationCleanupRelayABI(F.Image, F.Entry));
  }
}

TEST(RegistrationRealignedCleanupABI,
     RejectsCoordinateOverflowAndWorkExhaustion) {
  for (int32_t Base : {INT32_MIN, INT32_MAX}) {
    const int32_t Overflow = Base < 0 ? -1 : 1;
    for (bool Saved : {false, true}) {
      RealignedCleanupImage F(7, 1, Base, Saved ? Overflow : 0,
                              Saved ? 0 : Overflow);
      EXPECT_FALSE(getCheckedX86RegistrationCleanupRelayABI(F.Image, F.Entry));
    }
  }
  RealignedCleanupImage F;
  size_t Work = limits::kMaxRegistrationEHStateWork - 2;
  EXPECT_FALSE(
      getCheckedX86RegistrationCleanupRelayABI(F.Image, F.Entry, &Work));
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
}
} // namespace
