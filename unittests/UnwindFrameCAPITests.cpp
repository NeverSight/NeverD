//===- UnwindFrameCAPITests.cpp - Public unwind signing query ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Public session load/query/free calls cover exact PCs and all query statuses
/// without source-library archives, compiler tools or private session access.
//===----------------------------------------------------------------------===//

#include "UnwindFrameTestsDetail.h"

namespace {
using namespace neverd;
using namespace neverd::aarch64_dwarf_test;
using namespace neverd::unwind_frame_test;

class UnwindFrameCAPI : public UnwindFrameTest {};

TEST_F(UnwindFrameCAPI, LegacyNullAndNonAArch64ResponsesRemainCompatible) {
  EXPECT_EQ(take(neverd_unwind_frame_json(nullptr, kImageBase)), kJSONNull);
  EXPECT_EQ(take(neverd_unwind_frame_json(Session, kImageBase)), kJSONNull);
  ImageOptions Empty;
  Empty.Dwarf = Empty.Compact = false;
  ASSERT_NO_FATAL_FAILURE(load({}, Empty));
  EXPECT_TRUE(query().getAsNull().has_value());
  ImageOptions Other;
  Other.AArch64 = false;
  Other.DwarfFallback = true;
  ASSERT_NO_FATAL_FAILURE(load({}, Other));
  const auto Result = query();
  const auto *Frame = Result.getAsObject();
  ASSERT_NE(Frame, nullptr);
  EXPECT_EQ(Frame->getString(kBegin), kEntryAddress);
  EXPECT_EQ(Frame->getString(kEnd), kEndAddress);
  EXPECT_EQ(Frame->getString(kEncoding), kCompactEncodingName);
  EXPECT_EQ(Frame->getBoolean(kLanguageData), false);
  EXPECT_EQ(Frame->get(kReturnAddress), nullptr);
  EXPECT_EQ(Frame->get(kPersonality), nullptr);
}

TEST_F(UnwindFrameCAPI,
       ExactPCAndCompactRecipeRetainDifferentSigningLocations) {
  ASSERT_NO_FATAL_FAILURE(load());
  const auto At = query(kCodeAlign);
  const auto *Frame = At.getAsObject();
  ASSERT_NE(Frame, nullptr);
  EXPECT_EQ(Frame->getString(kBegin), kEntryAddress);
  EXPECT_EQ(Frame->getString(kEnd), kEndAddress);
  EXPECT_EQ(Frame->getString(kEncoding), kCompactEncodingName);
  EXPECT_EQ(Frame->getBoolean(kLanguageData), false);
  const auto *Signing = signing(At);
  ASSERT_NE(Signing, nullptr);
  EXPECT_EQ(Signing->getString(kQueryPC), kSignedAddress);
  const auto *Dwarf = Signing->getObject(kDwarfState);
  const auto *Compact = Signing->getObject(kCompactRecipe);
  ASSERT_NE(Dwarf, nullptr);
  ASSERT_NE(Compact, nullptr);
  EXPECT_EQ(Dwarf->getString(kStatus), kKnown);
  EXPECT_EQ(Dwarf->getString(kKind), kSPPC);
  EXPECT_EQ(Dwarf->getString(kKey), kBKeyText);
  EXPECT_EQ(Dwarf->getString(kRecipe), kNonHintText);
  EXPECT_EQ(Dwarf->getString(kSigningPC), kEntryAddress);
  EXPECT_EQ(Compact->getString(kStatus), kKnown);
  EXPECT_EQ(Compact->getString(kKind), kSPPC);
  EXPECT_EQ(Compact->getString(kSigningPC), kSignedAddress);
  const auto Before = query();
  const auto *BeforeSigning = signing(Before);
  ASSERT_NE(BeforeSigning, nullptr);
  const auto *BeforeState = BeforeSigning->getObject(kDwarfState);
  ASSERT_NE(BeforeState, nullptr);
  EXPECT_EQ(BeforeState->getString(kKind), kSP);
  EXPECT_EQ(BeforeState->get(kRecipe), nullptr);
  EXPECT_EQ(BeforeState->get(kSigningPC), nullptr);
  const auto After = query(kCodeAlign * 2);
  const auto *AfterSigning = signing(After);
  ASSERT_NE(AfterSigning, nullptr);
  EXPECT_EQ(AfterSigning->getString(kQueryPC), kUnsignedAddress);
  const auto *AfterState = AfterSigning->getObject(kDwarfState);
  ASSERT_NE(AfterState, nullptr);
  EXPECT_EQ(AfterState->getString(kKind), kUnsigned);
  EXPECT_EQ(AfterState->get(kKey), nullptr);
  EXPECT_EQ(AfterState->get(kRecipe), nullptr);
  EXPECT_EQ(AfterState->get(kSigningPC), nullptr);
  EXPECT_TRUE(query(kRange).getAsNull().has_value());
}

TEST_F(UnwindFrameCAPI,
       EveryUnavailableStatusRemainsTypedWithoutAnUnsignedState) {
  const std::vector<uint8_t> Deep(kRememberDepthOverflow, kRememberState);
  struct StatusCase {
    llvm::ArrayRef<uint8_t> Program;
    const char *Status;
  };
  for (const auto &Case :
       {StatusCase{kDynamic, kUnknown}, StatusCase{kReserved, kUnsupported},
        StatusCase{kLateUnderflow, kMalformed},
        StatusCase{Deep, kResourceLimit}}) {
    SCOPED_TRACE(Case.Status);
    ASSERT_NO_FATAL_FAILURE(load(Case.Program));
    const auto Result = query();
    const auto *Signing = signing(Result);
    ASSERT_NE(Signing, nullptr);
    const auto *Dwarf = Signing->getObject(kDwarfState);
    ASSERT_NE(Dwarf, nullptr);
    EXPECT_EQ(Dwarf->getString(kStatus), Case.Status);
    EXPECT_TRUE(Dwarf->getString(kDiagnostic).has_value());
    EXPECT_EQ(Dwarf->get(kKind), nullptr);
    EXPECT_EQ(Dwarf->get(kKey), nullptr);
    EXPECT_EQ(Dwarf->get(kRecipe), nullptr);
    EXPECT_EQ(Dwarf->get(kSigningPC), nullptr);
    const auto *Compact = Signing->getObject(kCompactRecipe);
    ASSERT_NE(Compact, nullptr);
    EXPECT_EQ(Compact->getString(kStatus),
              llvm::StringRef(Case.Status) == kUnsupported ? kUnsupported
                                                           : kKnown);
  }
  ImageOptions Short;
  Short.DwarfRange = kShortDwarfRange;
  ASSERT_NO_FATAL_FAILURE(load(kStates, Short));
  const auto Result = query(kShortDwarfRange);
  const auto *Signing = signing(Result);
  ASSERT_NE(Signing, nullptr);
  const auto *Dwarf = Signing->getObject(kDwarfState);
  ASSERT_NE(Dwarf, nullptr);
  EXPECT_EQ(Dwarf->getString(kStatus), kOutOfRange);
  EXPECT_EQ(Dwarf->get(kKind), nullptr);
  const auto *Compact = Signing->getObject(kCompactRecipe);
  ASSERT_NE(Compact, nullptr);
  EXPECT_EQ(Compact->getString(kStatus), kKnown);
}

TEST_F(UnwindFrameCAPI, AbsentDwarfAndOrdinaryCompactDoNotInventUnsignedState) {
  ImageOptions Ordinary;
  Ordinary.Dwarf = false;
  Ordinary.CompactEncoding = kOrdinaryCompact;
  ASSERT_NO_FATAL_FAILURE(load({}, Ordinary));
  const auto Result = query();
  const auto *Signing = signing(Result);
  ASSERT_NE(Signing, nullptr);
  const auto *Absent = Signing->get(kDwarfState);
  ASSERT_NE(Absent, nullptr);
  EXPECT_TRUE(Absent->getAsNull().has_value());
  const auto *Compact = Signing->getObject(kCompactRecipe);
  ASSERT_NE(Compact, nullptr);
  EXPECT_EQ(Compact->getString(kStatus), kUnknown);
  EXPECT_EQ(Compact->get(kKind), nullptr);
  EXPECT_EQ(Compact->get(kSigningPC), nullptr);
  ImageOptions DwarfOnly;
  DwarfOnly.Compact = false;
  ASSERT_NO_FATAL_FAILURE(load(kStates, DwarfOnly));
  const auto DwarfResult = query(kCodeAlign);
  const auto *DwarfSigning = signing(DwarfResult);
  ASSERT_NE(DwarfSigning, nullptr);
  const auto *Missing = DwarfSigning->get(kCompactRecipe);
  ASSERT_NE(Missing, nullptr);
  EXPECT_TRUE(Missing->getAsNull().has_value());
  const auto *Dwarf = DwarfSigning->getObject(kDwarfState);
  ASSERT_NE(Dwarf, nullptr);
  EXPECT_EQ(Dwarf->getString(kStatus), kKnown);
}

TEST_F(UnwindFrameCAPI, AZeroSigningPCRemainsAPresentHexadecimalAddress) {
  const auto Program = absoluteState(
      kSignedWithPC, -int64_t((kImageBase + kCodeOffset) / kCodeAlign));
  ASSERT_NO_FATAL_FAILURE(load(Program));
  const auto Result = query();
  const auto *Signing = signing(Result);
  ASSERT_NE(Signing, nullptr);
  const auto *Dwarf = Signing->getObject(kDwarfState);
  ASSERT_NE(Dwarf, nullptr);
  EXPECT_EQ(Dwarf->getString(kStatus), kKnown);
  EXPECT_EQ(Dwarf->getString(kSigningPC), kZeroAddress);
}

} // namespace
