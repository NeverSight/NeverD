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

  void addHandler(size_t Begin, size_t End, size_t Handler) {
    const auto LSDAVA = Function.CodeRange.End;
    Function.Personality = ExceptionPersonality::GxxPersonalityV0;
    Function.Dwarf->LSDAVA = LSDAVA;
    if (!Function.Itanium)
      Function.Itanium.emplace().LSDAVA = LSDAVA;
    Function.Itanium->CallSites.push_back(
        {.GuardedRange = {Begin * sizeof(uint32_t), End * sizeof(uint32_t)},
         .LandingPadVA = Handler * sizeof(uint32_t)});
  }
};

TEST(AArch64ImplicitUnwind, VectorSavesFollowLaneWidthAndPartialOverwrites) {
  for (bool Quad : {false, true}) {
    Frame F{SignB,
            Quad ? PushQ8Q9 : PushD8D9,
            ClearQ8,
            FillQ9,
            ByteInUpperLane,
            ByteBelowLR,
            Quad ? PopQ8Q9 : PopD8D9,
            AuthB,
            Ret};
    auto Rows = recoverDarwinAArch64Unwind(F.Image, F.Function);
    ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
    const auto PlanAt = [&](unsigned Index) {
      return prepareDarwinArm64eSystemCaller(
          aarch64UnwindRowAt(*Rows, Index * sizeof(uint32_t)),
          F.Function.CodeRange, 0);
    };
    const auto At = PlanAt(4);
    ASSERT_TRUE(At);
    EXPECT_TRUE(At->Vectors.Complete);
    EXPECT_EQ(At->Vectors.LowMask, 3u);
    EXPECT_EQ(At->Vectors.HighMask, Quad ? 3u : 0u);
    const int Width = kRegisterBytes * (Quad ? 2 : 1);
    EXPECT_EQ(At->Vectors.LowOffsets[0], -2 * Width);
    EXPECT_EQ(At->Vectors.LowOffsets[1], -Width);
    if (Quad)
      EXPECT_EQ(At->Vectors.HighOffsets[0], -2 * Width + int(kRegisterBytes));
    const auto Partial = PlanAt(5);
    ASSERT_TRUE(Partial);
    EXPECT_EQ(Partial->Vectors.Complete, Quad);
    EXPECT_EQ(Partial->Vectors.HighMask, Quad ? 2u : 0u);
    const auto Overwritten = PlanAt(6);
    ASSERT_TRUE(Overwritten);
    EXPECT_FALSE(Overwritten->Vectors.Complete);
    const auto Restored = PlanAt(7);
    ASSERT_TRUE(Restored);
    EXPECT_FALSE(Restored->Vectors.Complete);
  }
}

TEST(AArch64ImplicitUnwind, DwarfVectorRulesSurviveRestoreState) {
  Frame F{SignB, PushQ8Q9, ClearQ8, Nop, Nop, Nop, PopQ8Q9, AuthB, Ret};
  constexpr unsigned V8 = kDwarfV0 + NDArm64eFirstPreservedD;
  constexpr int Depth = 4 * kRegisterBytes;
  const auto PC = [](unsigned Index) { return Index * sizeof(uint32_t); };
  F.Function.Dwarf->Instructions = {
      {.Kind = CFIOpKind::DefCFAOffset, .CodeOffset = PC(2), .Offset = Depth},
      {.Kind = CFIOpKind::Offset,
       .CodeOffset = PC(2),
       .Register = V8,
       .Offset = -Depth},
      {.Kind = CFIOpKind::RememberState, .CodeOffset = PC(3)},
      {.Kind = CFIOpKind::Undefined, .CodeOffset = PC(4), .Register = V8},
      {.Kind = CFIOpKind::RestoreState, .CodeOffset = PC(5)},
      {.Kind = CFIOpKind::Restore, .CodeOffset = PC(7), .Register = V8}};
  auto Rows = recoverDarwinAArch64Unwind(F.Image, F.Function);
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  for (unsigned Index : {3u, 4u, 5u, 7u}) {
    auto Plan = prepareDarwinArm64eSystemCaller(
        aarch64UnwindRowAt(*Rows, PC(Index)), F.Function.CodeRange, 0);
    ASSERT_TRUE(Plan);
    EXPECT_EQ(Plan->Vectors.Complete, Index != 4);
    EXPECT_EQ(Plan->Vectors.LowMask, Index == 3 || Index == 5 ? 1u : 0u);
    EXPECT_EQ(Plan->Vectors.HighMask, Index == 7 ? 0u : 1u);
  }
}

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

TEST(AArch64ImplicitUnwind, SynchronousDwarfLeavesPartialFrameRowsImplicit) {
  Frame F{SignB, PushFP, MoveFP, Call, PopFP, AuthB, Ret};
  F.saveRow(3);
  auto &CFI = F.Function.Dwarf->Instructions;
  const auto BodyOffset = 3 * sizeof(uint32_t);
  CFI.push_back({.Kind = CFIOpKind::DefCFARegister,
                 .CodeOffset = BodyOffset,
                 .Register = kFP});
  CFI.push_back({.Kind = CFIOpKind::Offset,
                 .CodeOffset = BodyOffset,
                 .Register = kFP,
                 .Offset = -2 * int(kRegisterBytes)});
  auto Rows = recoverDarwinAArch64Unwind(F.Image, F.Function);
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  for (auto Index : {2u, 3u, 5u}) {
    const auto &Row = aarch64UnwindRowAt(*Rows, Index * sizeof(uint32_t));
    ASSERT_TRUE(Row.Signing.State);
    EXPECT_EQ(Row.Signing.State->Kind, Kind::SP);
    const auto Plan =
        prepareDarwinArm64eSystemCaller(Row, F.Function.CodeRange, 0);
    ASSERT_TRUE(Plan);
    EXPECT_EQ(Plan->Frame.CFAFromFP, Index == 3);
    EXPECT_EQ(Plan->Frame.CFAOffset, Index == 5 ? 0u : 2 * kRegisterBytes);
    EXPECT_EQ(Plan->Frame.SavedLR, Index == 3);
  }
  CFI.push_back({.Kind = CFIOpKind::Undefined,
                 .CodeOffset = BodyOffset,
                 .Register = kLR});
  EXPECT_EQ(F.at(5).Status, Status::Unknown);
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

TEST(AArch64ImplicitUnwind, LandingPadsJoinOnlySurvivingFrameSaves) {
  Frame F{SignB, PushFP, Call, PartialStore, Call, Ret, Ret};
  F.saveRow(2);
  F.addHandler(2, 3, 6);
  const auto Single = F.at(6);
  ASSERT_TRUE(Single.State);
  EXPECT_EQ(Single.State->Kind, Kind::SP);
  EXPECT_EQ(Single.State->Key, AArch64RAKey::B);
  F.addHandler(4, 5, 6);
  EXPECT_EQ(F.at(6).Status, Status::Unknown);
  // A protected non-call write cannot vouch for the earlier stack contents.
  F.Function.Itanium->CallSites.pop_back();
  F.Function.Itanium->CallSites.front().GuardedRange.End = 4 * sizeof(uint32_t);
  EXPECT_EQ(F.at(6).Status, Status::Unknown);

  Frame Live{SignA, Call, Ret, Ret};
  Live.addHandler(1, 2, 3);
  EXPECT_EQ(Live.at(3).Status, Status::Unknown);
}

TEST(AArch64ImplicitUnwind, NarrowStoresInvalidateOnlyOverlappingSaves) {
  for (auto Word : {ByteBelowLR, ByteInsideLR, HalfAcrossLR}) {
    Frame F{SignB, PushFP, Word, Call, Ret, Ret};
    F.saveRow(2);
    F.addHandler(3, 4, 5);
    F.Function.Personality = ExceptionPersonality::ObjCPersonalityV0;
    const auto Row = F.at(5);
    if (Word == ByteBelowLR) {
      ASSERT_TRUE(Row.State);
      EXPECT_EQ(Row.State->Kind, Kind::SP);
    } else {
      EXPECT_EQ(Row.Status, Status::Unknown);
    }
  }
}

TEST(AArch64ImplicitUnwind, CompilerTrapEndsOnlyItsReachablePath) {
  Frame F{SignB, CompilerTrap, Ret};
  ASSERT_TRUE(F.at(1).State);
  EXPECT_EQ(F.at(2).Status, Status::Unknown);
  Frame Debug{SignB, DebugBreak, Ret};
  EXPECT_EQ(Debug.at(1).Status, Status::Unsupported);
}

TEST(AArch64ImplicitUnwind, NonReturningCallsStillReachTheirLandingPads) {
  Frame F{SignB, PushFP, Skip, Ret, Call};
  F.saveRow(2);
  F.addHandler(4, 5, 3);
  const auto Target = 4 * sizeof(uint32_t) + CallDisplacement;
  F.Image.UserNoReturn[Target] = true;
  const auto FromThrow = F.at(3);
  ASSERT_TRUE(FromThrow.State);
  EXPECT_EQ(FromThrow.State->Key, AArch64RAKey::B);
  EXPECT_EQ(FromThrow.State->Kind, Kind::SP);
  F.Image.UserNoReturn[Target] = false;
  EXPECT_EQ(F.at(3).Status, Status::Unsupported);
}

TEST(AArch64ImplicitUnwind, LandingPadsPopOnlyTheirOutgoingArgumentArea) {
  Frame F{SignB, PushFP, Allocate, Call, Ret, Ret};
  F.addHandler(3, 4, 5);
  const auto CallOffset = 3 * sizeof(uint32_t);
  F.Function.Dwarf->Instructions = {
      {.Kind = CFIOpKind::AdvanceLoc, .Offset = int64_t(CallOffset)},
      {.Kind = CFIOpKind::GnuArgsSize,
       .CodeOffset = CallOffset,
       .Offset = 2 * kRegisterBytes}};
  auto Rows = recoverDarwinAArch64Unwind(F.Image, F.Function);
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  for (unsigned Index : {4u, 5u}) {
    const auto &Row = aarch64UnwindRowAt(*Rows, Index * sizeof(uint32_t));
    EXPECT_EQ(Row.CFARegister, kSP);
    EXPECT_EQ(Row.CFAOffset, (Index == 4 ? 4 : 2) * kRegisterBytes);
    ASSERT_TRUE(Row.Signing.State);
    EXPECT_EQ(Row.Signing.State->Kind, Kind::SP);
  }
  // A return saved in the released area cannot authorize the handler.
  F.Function.Dwarf->Instructions.back().Offset = 4 * kRegisterBytes;
  EXPECT_EQ(F.at(5).Status, Status::Unknown);
}

TEST(AArch64ImplicitUnwind, RequiresAnOwnedAddressBasedCxxLandingPadTable) {
  Frame F{SignB, PushFP, Call, Ret, Ret};
  F.saveRow(2);
  F.addHandler(2, 3, 4);
  F.Function.Itanium->IsCallSiteAddressForm = false;
  EXPECT_EQ(F.at(0).Status, Status::Unsupported);
  F.Function.Itanium->IsCallSiteAddressForm = true;
  F.Function.Personality = ExceptionPersonality::Unknown;
  EXPECT_EQ(F.at(0).Status, Status::Unsupported);
  F.Function.Personality = ExceptionPersonality::GxxPersonalityV0;
  F.addHandler(2, 3, 4);
  EXPECT_EQ(F.at(0).Status, Status::Unsupported);
  F.Function.Itanium->CallSites.pop_back();
  auto &Site = F.Function.Itanium->CallSites.front();
  ++Site.LandingPadVA;
  EXPECT_EQ(F.at(0).Status, Status::Unsupported);
  Site.LandingPadVA = F.Function.CodeRange.End;
  EXPECT_EQ(F.at(0).Status, Status::Unsupported);
  Site.LandingPadVA -= sizeof(uint32_t);
  F.Function.Dwarf->Instructions.push_back({.Kind = CFIOpKind::GnuArgsSize,
                                            .CodeOffset = 2 * sizeof(uint32_t),
                                            .Offset = -1});
  EXPECT_EQ(F.at(0).Status, Status::Unsupported);
  F.Function.Dwarf->Instructions.pop_back();
  ++F.Function.Itanium->LSDAVA;
  EXPECT_EQ(F.at(0).Status, Status::Unsupported);
  F.Function.Itanium.reset();
  EXPECT_EQ(F.at(0).Status, Status::Unsupported);
}
} // namespace
