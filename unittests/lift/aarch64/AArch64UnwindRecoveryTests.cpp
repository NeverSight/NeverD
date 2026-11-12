//===- AArch64UnwindRecoveryTests.cpp - Implicit Darwin frame proofs -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/AArch64UnwindRecovery.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/support/ISAEncoding.h"

namespace {
using namespace neverd;
using namespace aarch64_unwind;
using namespace dwarf_eh;
using Status = AArch64RAQueryStatus;
using Kind = AArch64RASigningKind;
#define UNWIND_PROOF_WORD(Name, Word) constexpr uint32_t Name = Word;
#include "AArch64UnwindRecoveryFixtures.def"

struct Frame {
  BinaryImage Image;
  ExceptionFunction Function;

  Frame(std::initializer_list<uint32_t> Words) {
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::MachO;
    Image.MachOCPUSubtype = macho_loader::arm64e::kCPUSubtype;
    auto &Text = Image.Segments.emplace_back();
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Size = Text.FileSz = Words.size() * sizeof(uint32_t);
    Text.Data.resize(Text.Size);
    size_t Offset = 0;
    for (auto Word : Words) {
      writeLE<uint32_t>(Text.Data.data() + Offset, Word);
      Offset += sizeof(Word);
    }
    auto &CIE = Image.ExceptionMetadata.CIEs.emplace_back();
    CIE.UsesArm64eReturnAddressABI = true;
    CIE.ReturnAddressRegister = kLR;
    CIE.CodeAlignmentFactor = 1;
    CIE.InitialInstructions = {{.Kind = CFIOpKind::DefCFA, .Register = kSP}};
    Function.CodeRange = {0, Text.Size};
    Function.Dwarf.emplace().AddressRange = Text.Size;
  }

  AArch64RAQueryResult at(size_t Instruction,
                          const AArch64UnwindRecoveryLimits &Limits = {}) {
    const auto Query = analyzeAArch64UnwindSigning(
        Image, Function, Instruction * sizeof(uint32_t), Limits);
    EXPECT_TRUE(Query.Metadata.HasImplicitDarwinSigning);
    EXPECT_TRUE(Query.Metadata.DwarfStateAtPC);
    if (Query.Metadata.DwarfStateAtPC)
      EXPECT_EQ(Query.Metadata.DwarfStateAtPC->Status, Status::Unknown);
    EXPECT_TRUE(Query.InstructionStateAtPC);
    return Query.InstructionStateAtPC.value_or(AArch64RAQueryResult{});
  }

  void saveRow(size_t Index) {
    auto &CFI = Function.Dwarf->Instructions;
    const auto Offset = Index * sizeof(uint32_t);
    CFI.push_back({.Kind = CFIOpKind::AdvanceLoc, .Offset = int64_t(Offset)});
    CFI.push_back({.Kind = CFIOpKind::DefCFAOffset,
                   .CodeOffset = Offset,
                   .Offset = 2 * kRegisterBytes});
    CFI.push_back({.Kind = CFIOpKind::Offset,
                   .CodeOffset = Offset,
                   .Register = kLR,
                   .Offset = -int(kRegisterBytes)});
  }
};

TEST(AArch64ImplicitUnwind,
     JoinsRequireAgreementAndUnreachableRowsStayUnknown) {
  for (const auto Branch : {IfZero, IfBitClear}) {
    Frame F{Branch, SignA, Ret};
    const auto Entry = F.at(0);
    ASSERT_TRUE(Entry.State);
    EXPECT_EQ(Entry.State->Kind, Kind::Unsigned);
    EXPECT_EQ(F.at(2).Status, Status::Unknown);
    Frame Both{Branch, Nop, SignA, Ret};
    const auto Result = Both.at(3);
    ASSERT_TRUE(Result.State);
    EXPECT_EQ(Result.State->Key, AArch64RAKey::A);
    EXPECT_EQ(Result.State->Kind, Kind::SP);
  }
  Frame Dead{Skip, SignB, Ret};
  EXPECT_EQ(Dead.at(1).Status, Status::Unknown);
  const auto Live = Dead.at(2);
  ASSERT_TRUE(Live.State);
  EXPECT_EQ(Live.State->Kind, Kind::Unsigned);
}

TEST(AArch64ImplicitUnwind, SavesSurviveCallsButNotOverlappingWrites) {
  Frame F{SignB, Push, Call, Pop, AuthB, Ret};
  F.saveRow(2);
  const auto R = F.at(3);
  ASSERT_TRUE(R.State);
  EXPECT_EQ(R.State->Kind, Kind::SP);
  EXPECT_EQ(R.State->Key, AArch64RAKey::B);
  for (const auto Store : {PartialStore, IndexedStore}) {
    Frame Corrupt{SignB, Push, Store, Ret};
    Corrupt.saveRow(2);
    EXPECT_EQ(Corrupt.at(3).Status, Status::Unknown);
  }
  Frame Register{SignB, SaveInX19, Call, RestoreFromX19, AuthB, Ret};
  EXPECT_EQ(Register.at(3).Status, Status::Unknown);
  const auto Restored = Register.at(5);
  ASSERT_TRUE(Restored.State);
  EXPECT_EQ(Restored.State->Kind, Kind::Unsigned);
}

TEST(AArch64ImplicitUnwind, AnUnprovedLRNeverBecomesAnUnsignedReturn) {
  for (const auto Rewrite : {ClobberLR, NarrowLR, ZeroSign, StripLR, AuthB}) {
    Frame F{Rewrite, Ret};
    EXPECT_EQ(F.at(1).Status, Status::Unknown);
  }
  Frame MovedSP{Allocate, SignB, Ret};
  EXPECT_EQ(MovedSP.at(2).Status, Status::Unknown);
  Frame WrongKey{SignA, AuthB, Ret};
  EXPECT_EQ(WrongKey.at(2).Status, Status::Unknown);
}

TEST(AArch64ImplicitUnwind, InstructionAddressRecipesSurviveImageSlides) {
  Frame Explicit{ExplicitSignB, ExplicitAuthBPrevious, Ret};
  auto Rows = recoverDarwinAArch64Unwind(Explicit.Image, Explicit.Function);
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  const auto &Signed = aarch64UnwindRowAt(*Rows, sizeof(uint32_t));
  ASSERT_TRUE(Signed.Signing.State);
  EXPECT_EQ(Signed.Signing.State->Recipe, AArch64RARecipe::NonHintPAuthLR);
  ASSERT_EQ(Signed.Signing.State->SigningPC, 0u);
  const auto Loaded = Explicit.Function.CodeRange.End;
  const auto Plan = prepareDarwinArm64eSystemCaller(
      Signed, Explicit.Function.CodeRange, Loaded);
  ASSERT_TRUE(Plan);
  EXPECT_EQ(Plan->Frame.SigningPC, Loaded);
  EXPECT_EQ(Plan->Frame.Signing, NDArm64eSignedIBWithCFAPC);
  const auto After = Explicit.at(2);
  ASSERT_TRUE(After.State);
  EXPECT_EQ(After.State->Kind, Kind::Unsigned);
  Frame Hint{kAArch64PACMEncoding, SignB, PreviousPCInX16,
             kAArch64PACMEncoding, AuthB, Ret};
  const auto Pending = Hint.at(2);
  ASSERT_TRUE(Pending.State);
  EXPECT_EQ(Pending.State->Recipe, AArch64RARecipe::HintPACM);
  EXPECT_EQ(Pending.State->SigningPC, sizeof(uint32_t));
  const auto Consumed = Hint.at(5);
  ASSERT_TRUE(Consumed.State);
  EXPECT_EQ(Consumed.State->Kind, Kind::Unsigned);
  Frame Page{kAArch64PACMEncoding, SignB, PageInX16, AddWordToX16,
             kAArch64PACMEncoding, AuthB, Ret};
  const auto PageRelative = Page.at(6);
  ASSERT_TRUE(PageRelative.State);
  EXPECT_EQ(PageRelative.State->Kind, Kind::Unsigned);
}

TEST(AArch64ImplicitUnwind, CompactRowsRecoverActualCarriersAtInterruption) {
  Frame F{SignB, Push, MoveFP, Call, Pop, AuthB, Ret};
  F.Function.Dwarf.reset();
  auto &Compact = F.Function.Compact.emplace();
  Compact.CodeRange = F.Function.CodeRange;
  Compact.NativeEncoding = macho_unwind::kARM64ModeFrame;
  ASSERT_TRUE(macho_unwind::decodeEncoding(Arch::AArch64,
                                           Compact.NativeEncoding, Compact));
  auto Rows = recoverDarwinAArch64Unwind(F.Image, F.Function);
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  const auto PlanAt = [&](size_t Index) {
    return prepareDarwinArm64eSystemCaller(
        aarch64UnwindRowAt(*Rows, Index * sizeof(uint32_t)), Compact.CodeRange,
        Compact.CodeRange.Begin);
  };
  // x29 was overwritten without a save. A signing query remains possible, but
  // handing the incomplete caller register set to the native walker must fail.
  const auto Query =
      analyzeAArch64UnwindSigning(F.Image, F.Function, 4 * sizeof(uint32_t));
  EXPECT_FALSE(Query.Metadata.DwarfStateAtPC);
  ASSERT_TRUE(Query.InstructionStateAtPC && Query.InstructionStateAtPC->State);
  EXPECT_EQ(Query.InstructionStateAtPC->State->Kind, Kind::SP);
  EXPECT_FALSE(PlanAt(4));
  // With the real frame-record spill, all caller state is accounted for.
  writeLE<uint32_t>(F.Image.Segments.front().Data.data() + sizeof(uint32_t),
                    PushFP);
  auto Complete = recoverDarwinAArch64Unwind(F.Image, F.Function);
  ASSERT_TRUE(bool(Complete)) << llvm::toString(Complete.takeError());
  const auto Plan = prepareDarwinArm64eSystemCaller(
      aarch64UnwindRowAt(*Complete, 4 * sizeof(uint32_t)), Compact.CodeRange,
      0);
  ASSERT_TRUE(Plan);
  EXPECT_TRUE(Plan->Frame.SavedLR);
  EXPECT_TRUE(Plan->Frame.SavedFP);
  EXPECT_EQ(Plan->Frame.CFAOffset, 2 * kRegisterBytes);
  EXPECT_EQ(Plan->Frame.LROffset, -int(kRegisterBytes));
  EXPECT_EQ(Plan->Frame.Signing, NDArm64eSignedIBWithCFA);
}

TEST(AArch64ImplicitUnwind, IncompleteFramesAndUnknownEdgesFailClosed) {
  for (auto Branch : {Indirect, LocalCall}) {
    Frame F{Branch, Nop, Ret};
    EXPECT_EQ(F.at(0).Status, Status::Unsupported);
  }
  Frame F{SignA, Ret};
  EXPECT_EQ(F.at(0, {.MaxInstructions = 1}).Status, Status::ResourceLimit);
  EXPECT_EQ(F.at(1, {.MaxTransitions = 1}).Status, Status::ResourceLimit);
  EXPECT_EQ(F.at(1, {.MaxInstructions = 2, .MaxTransitions = 2}).Status,
            Status::Known);
  F.Image.DataInCode.insert(sizeof(uint32_t));
  EXPECT_EQ(F.at(0).Status, Status::Unsupported);
  F.Image.DataInCode.clear();
  F.Image.ExceptionMetadata.CIEs.front().InitialInstructions.clear();
  EXPECT_EQ(F.at(0).Status, Status::Unsupported);
  F.Function.Dwarf->ParseStatus = ExceptionParseStatus::Malformed;
  const auto Incomplete = analyzeAArch64UnwindSigning(F.Image, F.Function, 0);
  ASSERT_TRUE(Incomplete.Metadata.DwarfStateAtPC);
  EXPECT_EQ(Incomplete.Metadata.DwarfStateAtPC->Status, Status::Malformed);
  EXPECT_FALSE(Incomplete.InstructionStateAtPC);
}
} // namespace
