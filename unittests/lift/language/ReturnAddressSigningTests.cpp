//===- ReturnAddressSigningTests.cpp - Unified signing query coverage ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguish completed-prologue recipes, exact-PC state and absent evidence.
//===----------------------------------------------------------------------===//

#include "AArch64DwarfTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/MachOExceptions.h"
#include "neverd/loader/ReturnAddressSigning.h"

#include <iterator>
#include <utility>

namespace {
using namespace neverd;
using namespace neverd::aarch64_dwarf_test;
using namespace neverd::dwarf_eh;

#define NEVERD_RA_FIXTURE_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define NEVERD_RA_FIXTURE_BYTES(Name, ...)                                     \
  constexpr uint8_t Name[] = {__VA_ARGS__};
#include "ReturnAddressSigningFixtures.def"

BinaryImage image(llvm::ArrayRef<uint8_t> Program = kStates) {
  auto Frames = decode(frame(Program, {}, kBKeyNonHint));
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.ExceptionMetadata.CIEs = std::move(Frames.CIEs);
  Image.ExceptionMetadata.ParseStatus = Frames.ParseStatus;
  ExceptionFunction Function;
  Function.CodeRange = {kText, kText + kRange};
  Function.Dwarf = std::move(Frames.FDEs.front());
  Image.ExceptionMetadata.Functions.push_back(std::move(Function));
  return Image;
}

void addCompact(ExceptionFunction &Function, uint32_t Encoding = kPCCompact) {
  CompactUnwindEntry Entry;
  EXPECT_TRUE(macho_unwind::decodeEncoding(Arch::AArch64, Encoding, Entry));
  Entry.NativeEncoding = Encoding;
  Entry.CodeRange = Function.CodeRange;
  Function.Compact = std::move(Entry);
}

AArch64UnwindSigningQuery
queryImage(const BinaryImage &Image, uint64_t Offset = 0,
           const AArch64UnwindSigningLimits &Limits = {}) {
  return queryAArch64UnwindSigning(
      Image, Image.ExceptionMetadata.Functions.front(), kText + Offset, Limits);
}

TEST(ReturnAddressSigning, Arm64eUsesTheImplicitBKeyWithoutChangingTheCIE) {
  const auto Bytes = frame(kInitialSP);
  for (auto Format : {BinaryFormat::ELF, BinaryFormat::MachO})
    for (uint32_t Subtype :
         {0u, kArm64eSubtype, kArm64eX1Subtype, kArm64eVersionedSubtype}) {
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      Image.Format = Format;
      Image.MachOCPUSubtype = Subtype;
      auto Frames = dwarf_eh::parseEHFrame(
          Image, {Bytes.data(), Bytes.size(), kFrame}, {});
      ASSERT_EQ(Frames.ParseStatus, ExceptionParseStatus::Complete);
      ASSERT_EQ(Frames.CIEs.size(), 1u);
      EXPECT_FALSE(Frames.CIEs.front().HasPointerAuth);
      EXPECT_EQ(Frames.CIEs.front().Augmentation, kOrdinary);
      EXPECT_EQ(Frames.CIEs.front().UsesArm64eReturnAddressABI,
                Image.isArm64e());
      Image.ExceptionMetadata.CIEs = std::move(Frames.CIEs);
      ExceptionFunction Function;
      Function.CodeRange = {kText, kText + kRange};
      Function.Dwarf = std::move(Frames.FDEs.front());
      const auto Result = queryAArch64UnwindSigning(Image, Function, kText);
      ASSERT_TRUE(Result.DwarfStateAtPC && Result.DwarfStateAtPC->State);
      EXPECT_EQ(Result.DwarfStateAtPC->State->Kind, AArch64RASigningKind::SP);
      EXPECT_EQ(Result.DwarfStateAtPC->State->Key,
                Image.isArm64e() ? AArch64RAKey::B : AArch64RAKey::A);
    }
}

TEST(ReturnAddressSigning, ExactPCStateDoesNotBorrowTheCompactRecipe) {
  auto Image = image();
  auto &Function = Image.ExceptionMetadata.Functions.front();
  addCompact(Function);
  const auto At = queryImage(Image, kCodeAlign);
  EXPECT_EQ(At.QueryPC, kText + kCodeAlign);
  ASSERT_TRUE(At.DwarfStateAtPC && At.DwarfStateAtPC->State);
  EXPECT_EQ(At.DwarfStateAtPC->Status, AArch64RAQueryStatus::Known);
  EXPECT_EQ(At.DwarfStateAtPC->State->SigningPC, kText);
  EXPECT_EQ(At.DwarfStateAtPC->State->Recipe, AArch64RARecipe::NonHintPAuthLR);
  ASSERT_TRUE(At.CompactRecipe && At.CompactRecipe->State);
  EXPECT_EQ(At.CompactRecipe->State->SigningPC, kText + kCodeAlign);

  const auto After = queryImage(Image, kCodeAlign * 2);
  ASSERT_TRUE(After.DwarfStateAtPC && After.DwarfStateAtPC->State);
  EXPECT_EQ(After.DwarfStateAtPC->State->Kind, AArch64RASigningKind::Unsigned);
  EXPECT_FALSE(After.DwarfStateAtPC->State->SigningPC);
  ASSERT_TRUE(After.CompactRecipe && After.CompactRecipe->State);
  EXPECT_EQ(After.CompactRecipe->State->Kind, AArch64RASigningKind::SPAndPC);

  Function.Dwarf->AddressRange = kShortRange;
  const auto PastDwarf = queryImage(Image, kShortRange);
  ASSERT_TRUE(PastDwarf.DwarfStateAtPC && PastDwarf.CompactRecipe);
  EXPECT_EQ(PastDwarf.DwarfStateAtPC->Status, AArch64RAQueryStatus::OutOfRange);
  EXPECT_FALSE(PastDwarf.DwarfStateAtPC->State);
  EXPECT_EQ(PastDwarf.CompactRecipe->Status, AArch64RAQueryStatus::Known);
  const auto End = queryImage(Image, kRange);
  ASSERT_TRUE(End.DwarfStateAtPC && End.CompactRecipe);
  EXPECT_EQ(End.DwarfStateAtPC->Status, AArch64RAQueryStatus::OutOfRange);
  EXPECT_EQ(End.CompactRecipe->Status, AArch64RAQueryStatus::OutOfRange);
}

TEST(ReturnAddressSigning, MissingAndUnprovedSourcesRemainDistinct) {
  auto Image = image(kDynamic);
  auto &Function = Image.ExceptionMetadata.Functions.front();
  addCompact(Function);
  const auto Dynamic = queryImage(Image);
  ASSERT_TRUE(Dynamic.DwarfStateAtPC && Dynamic.CompactRecipe);
  EXPECT_EQ(Dynamic.DwarfStateAtPC->Status, AArch64RAQueryStatus::Unknown);
  EXPECT_FALSE(Dynamic.DwarfStateAtPC->State);
  EXPECT_EQ(Dynamic.CompactRecipe->Status, AArch64RAQueryStatus::Known);
  Function.Dwarf.reset();
  addCompact(Function, kOrdinaryCompact);
  const auto Ordinary = queryImage(Image);
  EXPECT_FALSE(Ordinary.DwarfStateAtPC);
  ASSERT_TRUE(Ordinary.CompactRecipe);
  EXPECT_EQ(Ordinary.CompactRecipe->Status, AArch64RAQueryStatus::Unknown);
  EXPECT_FALSE(Ordinary.CompactRecipe->State);
  Function.Compact.reset();
  const auto Absent = queryImage(Image);
  EXPECT_FALSE(Absent.DwarfStateAtPC);
  EXPECT_FALSE(Absent.CompactRecipe);
}

TEST(ReturnAddressSigning, AddressZeroIsNotMissingQueryOrSigningEvidence) {
  auto Image = image(kInitialSP);
  auto &Function = Image.ExceptionMetadata.Functions.front();
  Function.CodeRange = {0, kRange};
  Function.Dwarf->InitialLocation = 0;
  addCompact(Function, kZeroPCCompact);
  const auto Query = queryAArch64UnwindSigning(Image, Function, 0);
  EXPECT_EQ(Query.QueryPC, 0u);
  ASSERT_TRUE(Query.DwarfStateAtPC && Query.DwarfStateAtPC->State);
  EXPECT_EQ(Query.DwarfStateAtPC->Status, AArch64RAQueryStatus::Known);
  EXPECT_EQ(Query.DwarfStateAtPC->State->Kind, AArch64RASigningKind::SP);
  ASSERT_TRUE(Query.CompactRecipe && Query.CompactRecipe->State);
  EXPECT_EQ(Query.CompactRecipe->State->SigningPC, 0u);
}

TEST(ReturnAddressSigning, CompactSigningPCUsesTheExactValidatedRecipeRange) {
  struct Case {
    uint32_t Encoding;
    uint32_t Offset;
  };
  constexpr Case Cases[] = {
#define NEVERD_RA_FIXTURE_CASE(Encoding, Offset) {Encoding, Offset},
#include "ReturnAddressSigningFixtures.def"
  };
  for (const auto &Case : Cases) {
    auto Image = image();
    auto &Function = Image.ExceptionMetadata.Functions.front();
    Function.Dwarf.reset();
    addCompact(Function, Case.Encoding);
    const auto Query = queryImage(Image);
    ASSERT_TRUE(Query.CompactRecipe && Query.CompactRecipe->State);
    EXPECT_EQ(Query.CompactRecipe->Status, AArch64RAQueryStatus::Known);
    EXPECT_EQ(Query.CompactRecipe->State->SigningPC, kText + Case.Offset);
    EXPECT_EQ(Query.CompactRecipe->State->Key, AArch64RAKey::B);
    EXPECT_EQ(Query.CompactRecipe->State->Recipe,
              AArch64RARecipe::NonHintPAuthLR);
  }
  auto Image = image();
  auto &Function = Image.ExceptionMetadata.Functions.front();
  Function.Dwarf.reset();
  addCompact(Function);
  Function.Compact->CodeRange.Begin += kCodeAlign;
  const auto Query = queryImage(Image, kCodeAlign);
  ASSERT_TRUE(Query.CompactRecipe && Query.CompactRecipe->State);
  EXPECT_EQ(Query.CompactRecipe->State->SigningPC, kText + kCodeAlign * 2);
}

TEST(ReturnAddressSigning, InvalidOrStaleCompactEvidenceCannotBecomeKnown) {
  auto Image = image();
  auto &Function = Image.ExceptionMetadata.Functions.front();
  Function.Dwarf.reset();
  const auto Expect = [&](AArch64RAQueryStatus Status) {
    const auto Query = queryImage(Image);
    ASSERT_TRUE(Query.CompactRecipe);
    EXPECT_EQ(Query.CompactRecipe->Status, Status);
    EXPECT_FALSE(Query.CompactRecipe->State);
  };
  addCompact(Function);
  Function.Compact->ReturnAddressSigningOffset = kStaleOffset;
  Expect(AArch64RAQueryStatus::Malformed);
  addCompact(Function);
  Function.Compact->ReturnAddressSigningOffset.reset();
  Expect(AArch64RAQueryStatus::Malformed);
  addCompact(Function);
  Function.Compact->NativeEncoding = kReservedCompact;
  Expect(AArch64RAQueryStatus::Malformed);
  addCompact(Function);
  Function.Compact->SemanticStatus = CompactUnwindSemanticStatus::Partial;
  Expect(AArch64RAQueryStatus::Unsupported);
  addCompact(Function);
  Function.Compact->CodeRange.End = kText + kPartialInstruction;
  Expect(AArch64RAQueryStatus::Malformed);
  addCompact(Function);
  Function.CodeRange = {kLastAlignedAddress, 0};
  Function.Compact->CodeRange = Function.CodeRange;
  const auto Overflow =
      queryAArch64UnwindSigning(Image, Function, kLastAlignedAddress);
  ASSERT_TRUE(Overflow.CompactRecipe);
  EXPECT_EQ(Overflow.CompactRecipe->Status, AArch64RAQueryStatus::Malformed);
}

TEST(ReturnAddressSigning, CIEIdentityAndBothWorkBudgetsArePreserved) {
  auto Image = image();
  auto &CIEs = Image.ExceptionMetadata.CIEs;
  AArch64UnwindSigningLimits Limits;
  Limits.MaxCIEs = CIEs.size();
  Limits.Dwarf.MaxInstructions =
      CIEs.front().InitialInstructions.size() +
      Image.ExceptionMetadata.Functions.front().Dwarf->Instructions.size();
  EXPECT_EQ(queryImage(Image, 0, Limits).DwarfStateAtPC->Status,
            AArch64RAQueryStatus::Known);
  --Limits.MaxCIEs;
  EXPECT_EQ(queryImage(Image, 0, Limits).DwarfStateAtPC->Status,
            AArch64RAQueryStatus::ResourceLimit);
  ++Limits.MaxCIEs;
  --Limits.Dwarf.MaxInstructions;
  EXPECT_EQ(queryImage(Image, 0, Limits).DwarfStateAtPC->Status,
            AArch64RAQueryStatus::ResourceLimit);
  CIEs.push_back(CIEs.front());
  EXPECT_EQ(queryImage(Image).DwarfStateAtPC->Status,
            AArch64RAQueryStatus::Malformed);
  CIEs.clear();
  EXPECT_EQ(queryImage(Image).DwarfStateAtPC->Status,
            AArch64RAQueryStatus::Malformed);
}

TEST(ReturnAddressSigning, WholeProgramValidationAndArchitectureRemainVisible) {
  auto Late = image(kLateUnderflow);
  EXPECT_EQ(queryImage(Late).DwarfStateAtPC->Status,
            AArch64RAQueryStatus::Malformed);
  auto Reserved = image(kReserved);
  EXPECT_EQ(queryImage(Reserved).DwarfStateAtPC->Status,
            AArch64RAQueryStatus::Unsupported);
  auto Other = image();
  addCompact(Other.ExceptionMetadata.Functions.front());
  Other.Arch = Arch::X64;
  const auto Query = queryImage(Other);
  ASSERT_TRUE(Query.DwarfStateAtPC && Query.CompactRecipe);
  EXPECT_EQ(Query.DwarfStateAtPC->Status, AArch64RAQueryStatus::Unsupported);
  EXPECT_EQ(Query.CompactRecipe->Status, AArch64RAQueryStatus::Unsupported);
  EXPECT_FALSE(Query.DwarfStateAtPC->State);
  EXPECT_FALSE(Query.CompactRecipe->State);
}

TEST(ReturnAddressSigning, IncompleteOwnerProvenanceRejectsBothSources) {
  enum class Provenance {
    Function,
    Structural,
    Language,
    Image,
    ImageStructural
  };
  for (auto Source :
       {Provenance::Function, Provenance::Structural, Provenance::Language,
        Provenance::Image, Provenance::ImageStructural}) {
    auto Image = image();
    auto &Function = Image.ExceptionMetadata.Functions.front();
    addCompact(Function);
    if (Source == Provenance::Function)
      Function.ParseStatus = ExceptionParseStatus::Partial;
    else if (Source == Provenance::Structural)
      Function.establishDecodeProvenance().Structural.ParseStatus =
          ExceptionParseStatus::Partial;
    else if (Source == Provenance::Language)
      Function.establishDecodeProvenance().Language.ParseStatus =
          ExceptionParseStatus::Partial;
    else if (Source == Provenance::Image)
      Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Partial;
    else
      Image.ExceptionMetadata.structuralDecodeState().ParseStatus =
          ExceptionParseStatus::Partial;
    const auto Query = queryImage(Image);
    ASSERT_TRUE(Query.DwarfStateAtPC && Query.CompactRecipe);
    EXPECT_EQ(Query.DwarfStateAtPC->Status, AArch64RAQueryStatus::Unsupported);
    EXPECT_EQ(Query.CompactRecipe->Status, AArch64RAQueryStatus::Unsupported);
    EXPECT_FALSE(Query.DwarfStateAtPC->State);
    EXPECT_FALSE(Query.CompactRecipe->State);
  }
}

TEST(ReturnAddressSigning,
     DarwinSectionFailureCannotRegrantLocalCompleteState) {
  for (bool Truncated : {false, true}) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::MachO;
    Image.Base = kText;
    Segment Text;
    Text.Name = kMachOTextSegment;
    Text.VA = kText;
    Text.Size = kRange * 2;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Segments.push_back(std::move(Text));
    Section Frames;
    Frames.Name = kMachOFrameSection;
    Frames.VA = kFrame;
    Frames.Data = frame(kStates);
    Frames.Data.resize(Frames.Data.size() - sizeof(uint32_t));
    if (Truncated)
      Frames.Data.insert(Frames.Data.end(), std::begin(kTruncatedRecord),
                         std::end(kTruncatedRecord));
    else {
      const auto Unknown =
          frame(kOpaque, {}, kOrdinary, kCodeAlign, kText + kRange);
      Frames.Data.insert(Frames.Data.end(), Unknown.begin(), Unknown.end());
    }
    Frames.Size = Frames.Data.size();
    Image.Sections.push_back(std::move(Frames));
    macho_unwind::parseDarwinExceptions(Image);
    const auto *Function = Image.ExceptionMetadata.findFunction(kText);
    ASSERT_NE(Function, nullptr);
    ASSERT_TRUE(Function->Dwarf);
    ASSERT_EQ(Function->Dwarf->ParseStatus, ExceptionParseStatus::Complete);
    const auto *CIE =
        Image.ExceptionMetadata.findCIE(Function->Dwarf->CIESectionOffset);
    ASSERT_NE(CIE, nullptr);
    EXPECT_EQ(evaluateAArch64ReturnAddressState(Image.Arch, *CIE,
                                                *Function->Dwarf, kText)
                  .Status,
              AArch64RAQueryStatus::Known);
    const auto Query = queryAArch64UnwindSigning(Image, *Function, kText);
    ASSERT_TRUE(Query.DwarfStateAtPC);
    EXPECT_NE(Query.DwarfStateAtPC->Status, AArch64RAQueryStatus::Known);
    EXPECT_FALSE(Query.DwarfStateAtPC->State);
  }
}

} // namespace
