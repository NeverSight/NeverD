//===- AArch64DwarfRewriteTests.cpp - RA CFI rewrite boundaries -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Address-stable metadata moves preserve every statically known RA recipe.
//===----------------------------------------------------------------------===//

#include "../language/AArch64DwarfTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/DwarfEHFrame.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/backend/codegen/SourcePatchCallback.h"
#include "neverd/loader/DWARF/EHFrame.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>

namespace {
using namespace neverd;
using namespace neverd::aarch64_dwarf_test;

std::vector<uint8_t> machOFrame(llvm::ArrayRef<uint8_t> Program,
                                const char *Augmentation = kOrdinary,
                                llvm::ArrayRef<uint8_t> Initial = {},
                                uint64_t Functions = 1) {
  using namespace llvm::MachO;
  std::vector<uint8_t> Bytes;
  for (uint64_t Index = 0; Index < Functions; ++Index) {
    if (Index)
      Bytes.resize(Bytes.size() - sizeof(uint32_t));
    const auto Next =
        frame(Program, Initial, Augmentation, kCodeAlign,
              kMachOBase + kMachOTextOffset + Index * kRange, false,
              kMachOBase + kMachOFrameOffset + Bytes.size());
    Bytes.insert(Bytes.end(), Next.begin(), Next.end());
  }
  std::vector<uint8_t> Binary(kMachOPageSize * 2, 0);
  auto *Header = reinterpret_cast<mach_header_64 *>(Binary.data());
  Header->magic = MH_MAGIC_64;
  Header->cputype = CPU_TYPE_ARM64;
  Header->cpusubtype = CPU_SUBTYPE_ARM64E;
  Header->filetype = MH_EXECUTE;
  Header->flags = MH_PIE;
  Header->ncmds = 2;
  Header->sizeofcmds = 2 * sizeof(segment_command_64) + 2 * sizeof(section_64);
  auto *Text = reinterpret_cast<segment_command_64 *>(Header + 1);
  Text->cmd = LC_SEGMENT_64;
  Text->cmdsize = sizeof(*Text) + 2 * sizeof(section_64);
  std::memcpy(Text->segname, kMachOTextSegment, sizeof(kMachOTextSegment));
  Text->vmaddr = kMachOBase;
  Text->vmsize = Text->filesize = kMachOPageSize;
  Text->maxprot = Text->initprot = VM_PROT_READ | VM_PROT_EXECUTE;
  Text->nsects = 2;
  auto *Code = reinterpret_cast<section_64 *>(Text + 1);
  std::memcpy(Code->sectname, kMachOTextSection, sizeof(kMachOTextSection));
  std::memcpy(Code->segname, kMachOTextSegment, sizeof(kMachOTextSegment));
  Code->addr = kMachOBase + kMachOTextOffset;
  Code->size = kRange * Functions;
  Code->offset = kMachOTextOffset;
  Code->align = kMachOSectionAlignment;
  Code->flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS;
  auto *EH = Code + 1;
  std::memcpy(EH->sectname, kMachOFrameSection, sizeof(kMachOFrameSection));
  std::memcpy(EH->segname, kMachOTextSegment, sizeof(kMachOTextSegment));
  EH->addr = kMachOBase + kMachOFrameOffset;
  EH->size = Bytes.size();
  EH->offset = kMachOFrameOffset;
  EH->align = kMachOSectionAlignment;
  auto *Linkedit = reinterpret_cast<segment_command_64 *>(EH + 1);
  Linkedit->cmd = LC_SEGMENT_64;
  Linkedit->cmdsize = sizeof(*Linkedit);
  std::memcpy(Linkedit->segname, kMachOLinkeditSegment,
              sizeof(kMachOLinkeditSegment));
  Linkedit->vmaddr = kMachOBase + kMachOPageSize;
  Linkedit->fileoff = kMachOPageSize;
  Linkedit->vmsize = Linkedit->filesize = kMachOPageSize;
  Linkedit->maxprot = Linkedit->initprot = VM_PROT_READ;
  std::copy(std::begin(kNopCode), std::end(kNopCode),
            Binary.begin() + kMachOTextOffset);
  std::copy(Bytes.begin(), Bytes.end(), Binary.begin() + kMachOFrameOffset);
  return Binary;
}

dwarf_eh::ParseResult parseMachOFrame(const std::vector<uint8_t> &Binary) {
  const auto Region = findMachOEHFrameRegion(Binary);
  EXPECT_TRUE(Region);
  if (!Region)
    return {};
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  return dwarf_eh::parseEHFrame(
      Image,
      {Binary.data() + Region->SectionFileOff,
       size_t(Region->AppendFileOff - Region->SectionFileOff),
       Region->SectionVA},
      {});
}

TEST(AArch64DwarfRewrite, NonHintAugmentationHasNoPayload) {
  for (const char *Augmentation :
       {kNonHint, kBKeyNonHint, kNonHintBeforeEncoding}) {
    auto Records =
        decodeDwarfEHFrameRecords(frame({}, {}, Augmentation), kFrame, true);
    ASSERT_TRUE(bool(Records)) << llvm::toString(Records.takeError());
    ASSERT_EQ(Records->size(), 1u);
    EXPECT_EQ(Records->front().BeginVA, kText);
    EXPECT_EQ(Records->front().EndVA, kText + kRange);
    auto Extra = decodeDwarfEHFrameRecords(
        frame({}, {}, Augmentation, kCodeAlign, kText, true), kFrame, true);
    EXPECT_FALSE(Extra);
    llvm::consumeError(Extra.takeError());
  }
}

TEST(AArch64DwarfRewrite, CallbackRejectsUnmappedSigningPCAndNonHintRecipes) {
  auto Prefix = relocateSourceCallbackPrefix(
      Arch::AArch64, InstructionMode::Default, kNopCode, kText, kCallback);
  ASSERT_TRUE(bool(Prefix)) << llvm::toString(Prefix.takeError());
  for (const char *Augmentation : {kOrdinary, kNonHint}) {
    const auto Parsed = decode(frame({}, {}, Augmentation));
    auto Result = encodeSourceCallbackDwarfFrame(*Prefix, Parsed.CIEs.front(),
                                                 Parsed.FDEs.front(), kFrame);
    EXPECT_EQ(bool(Result), llvm::StringRef(Augmentation) == kOrdinary);
    if (!Result)
      llvm::consumeError(Result.takeError());
  }
  for (uint64_t State : {kUnsignedState, kSignedWithSP, kSignedWithPC}) {
    const auto Parsed = decode(frame(absoluteState(State, 0)));
    auto Result = encodeSourceCallbackDwarfFrame(*Prefix, Parsed.CIEs.front(),
                                                 Parsed.FDEs.front(), kFrame);
    EXPECT_FALSE(Result);
    llvm::consumeError(Result.takeError());
  }
}

TEST(AArch64DwarfRewrite, MachOLayoutGrowthPreservesEverySigningRecipe) {
  struct Program {
    llvm::ArrayRef<uint8_t> Rules;
    llvm::ArrayRef<uint8_t> Initial;
    uint64_t SignedRow;
    std::optional<uint64_t> SigningOffset;
  };
  for (const char *Augmentation :
       {kOrdinary, kBKey, kNonHint, kBKeyNonHint, kNonHintBeforeEncoding})
    for (const auto &Case :
         {Program{{}, {}, 0, {}}, Program{kUnsignedIgnoredOffset, {}, 0, {}},
          Program{kSPIgnoredOffset, {}, 0, {}}, Program{{}, kInitialPC, 0, 0},
          Program{kStates, {}, kCodeAlign, 0},
          Program{kLegacy, {}, kCodeAlign * 2, kCodeAlign * 2},
          Program{kNested, kInitialSP, kCodeAlign * 2, 0},
          Program{kRestoreInitialRemember, kInitialRemember, 0, 0}}) {
      SCOPED_TRACE(Augmentation);
      auto Binary = machOFrame(Case.Rules, Augmentation, Case.Initial);
      const auto Original = Binary;
      const auto OldRegion = findMachOEHFrameRegion(Original);
      ASSERT_TRUE(OldRegion);
      const auto Before = parseMachOFrame(Binary);
      ASSERT_EQ(Before.ParseStatus, ExceptionParseStatus::Complete);
      ASSERT_EQ(Before.FDEs.size(), 1u);
      auto Grown =
          reserveMachOSourceUnwind(Binary, Arch::AArch64, kMachOPageSize, 0);
      ASSERT_TRUE(bool(Grown)) << llvm::toString(Grown.takeError());
      ASSERT_TRUE(*Grown);
      const auto Region = findMachOEHFrameRegion(Binary);
      ASSERT_TRUE(Region);
      EXPECT_NE(Region->SectionVA, kMachOBase + kMachOFrameOffset);
      const auto After = parseMachOFrame(Binary);
      ASSERT_EQ(After.ParseStatus, ExceptionParseStatus::Complete);
      ASSERT_EQ(After.CIEs.size(), 1u);
      ASSERT_EQ(After.FDEs.size(), 1u);
      EXPECT_EQ(After.CIEs.front().Augmentation,
                Before.CIEs.front().Augmentation);
      EXPECT_EQ(After.FDEs.front().InitialLocation,
                kMachOBase + kMachOTextOffset);
      EXPECT_EQ(After.FDEs.front().AddressRange, kRange);
      const auto OldBytes = llvm::ArrayRef<uint8_t>(Original).slice(
          OldRegion->SectionFileOff,
          OldRegion->AppendFileOff - OldRegion->SectionFileOff);
      const auto NewBytes = llvm::ArrayRef<uint8_t>(Binary).slice(
          Region->SectionFileOff,
          Region->AppendFileOff - Region->SectionFileOff);
      ASSERT_EQ(OldBytes.size(), NewBytes.size());
      const uint64_t LocationOffset = Before.FDEs.front().InitialLocationOffset;
      for (size_t Index = 0; Index < OldBytes.size(); ++Index)
        if (Index < LocationOffset ||
            Index >= LocationOffset + sizeof(uint64_t))
          EXPECT_EQ(NewBytes[Index], OldBytes[Index]);
      EXPECT_TRUE(std::equal(Original.begin() + kMachOTextOffset,
                             Original.begin() + kMachOTextOffset + kRange,
                             Binary.begin() + kMachOTextOffset));
      for (uint64_t Offset : {uint64_t(0), kCodeAlign, kCodeAlign * 2,
                              kCodeAlign * 3, kRange - 1}) {
        const auto A = query(Before, Offset);
        const auto B = query(After, Offset);
        ASSERT_EQ(A.Status, dwarf_eh::AArch64RAQueryStatus::Known);
        ASSERT_EQ(B.Status, dwarf_eh::AArch64RAQueryStatus::Known);
        ASSERT_TRUE(A.State);
        ASSERT_TRUE(B.State);
        EXPECT_EQ(*A.State, *B.State);
      }
      const auto Signed = query(After, Case.SignedRow);
      ASSERT_TRUE(Signed.State);
      const auto ExpectedPC =
          Case.SigningOffset
              ? std::optional<uint64_t>(kMachOBase + kMachOTextOffset +
                                        *Case.SigningOffset)
              : std::nullopt;
      EXPECT_EQ(Signed.State->SigningPC, ExpectedPC);
    }
}

TEST(AArch64DwarfRewrite,
     MachOLayoutGrowthRejectsUnprovedStateTransactionally) {
  const auto Reject = [](llvm::ArrayRef<uint8_t> Program,
                         const char *Augmentation, const char *Diagnostic,
                         llvm::ArrayRef<uint8_t> Initial = {}) {
    auto Binary = machOFrame(Program, Augmentation, Initial);
    const auto Before = Binary;
    auto Result =
        reserveMachOSourceUnwind(Binary, Arch::AArch64, kMachOPageSize, 0);
    ASSERT_FALSE(Result);
    EXPECT_NE(llvm::toString(Result.takeError()).find(Diagnostic),
              std::string::npos);
    EXPECT_EQ(Binary, Before);
  };
  Reject({}, kNonHint, kLayoutUnknownState, kInitialAdvance);
  Reject(kDynamic, kNonHint, kLayoutUnknownState);
  Reject(kLateUnderflow, kNonHint, kLayoutUnknownState);
  Reject(kReserved, kOrdinary, kLayoutRefusal);
  Reject(kExpression, kOrdinary, kLayoutRefusal);
  Reject(kOpaque, kOrdinary, kLayoutRefusal);
  Reject({}, kDuplicateNonHint, kLayoutRefusal);
  Reject({}, kSignalAugmentation, kLayoutRefusal);
  language_eh_test::ByteBuilder Location;
  Location.u8(kSetLocation);
  Location.u64(kMachOBase + kMachOTextOffset + kCodeAlign);
  Reject(Location.data(), kOrdinary, kLayoutRefusal);
}

TEST(AArch64DwarfRewrite, MachOStateComparisonBudgetIsCumulativeAndExact) {
  struct Program {
    llvm::ArrayRef<uint8_t> Rules;
    uint64_t Work;
  };
  for (const auto &Case :
       {Program{kStates, kStateReplayWork},
        Program{kStatesTrailingAdvance, kTrailingAdvanceReplayWork},
        Program{kStatesAdvanceToEnd, kEndAdvanceReplayWork}})
    for (uint64_t Functions : {uint64_t(1), uint64_t(2)})
      for (bool Enough : {false, true}) {
        auto Binary = machOFrame(Case.Rules, kNonHint, {}, Functions);
        const auto Before = Binary;
        MachOUnwindRelocationLimits Limits;
        Limits.MaxRAStateWork = Case.Work * Functions - !Enough;
        auto Result = reserveMachOSourceUnwind(Binary, Arch::AArch64,
                                               kMachOPageSize, 0, Limits);
        if (Enough) {
          ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
          EXPECT_TRUE(*Result);
        } else {
          ASSERT_FALSE(Result);
          EXPECT_NE(llvm::toString(Result.takeError()).find(kLayoutBudget),
                    std::string::npos);
          EXPECT_EQ(Binary, Before);
        }
      }
}

} // namespace
