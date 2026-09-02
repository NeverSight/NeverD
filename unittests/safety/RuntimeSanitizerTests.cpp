//===- RuntimeSanitizerTests.cpp - Strict guard planner tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/safety/RuntimeSanitizer.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace neverd;
using namespace neverd::safety;
using namespace neverd::safety_callsite_md;

namespace {

Finding finding(llvm::StringRef Sink = "memcpy",
                std::optional<uint64_t> Capacity = 5) {
  Finding F;
  F.Origin = Track::Hunt;
  F.Class = VulnClass::BufferOverflow;
  F.FuncEntry = 0x401000;
  F.CallVA = 0x401020;
  F.Sink = Sink.str();
  F.Name = Sink.str();
  F.Kind = SinkKind::Copy;
  F.ArgIndex = 2;
  F.Capacity = Capacity;
  F.CapacityKind = Capacity ? CapacityPrecision::TypedBufferExact
                            : CapacityPrecision::Unknown;
  F.CapacityExact = Capacity.has_value();
  F.BlockId = 1;
  F.OpIdx = 2;
  F.OriginSeq = 3;
  F.CallSiteId = 4;
  return F;
}

SafetyCallsiteRecord record(SemanticKind Kind = SemanticKind::Memcpy) {
  SafetyCallsiteRecord R;
  R.Occurrence = {/*FuncEntry=*/0x401000,
                  /*CallVA=*/0x401020,
                  /*BlockId=*/1,
                  /*OpIdx=*/2,
                  /*OriginSeq=*/3,
                  /*CallSiteId=*/4};
  R.Kind = Kind;
  R.DestinationOperandIndex = 0;
  R.LengthOperandIndex = 2;
  R.ElementBytes = 1;
  return R;
}

bool hasReason(const RuntimeSanitizerPlan &Plan,
               RuntimeSanitizerUnsupportedReason Reason) {
  for (const RuntimeSanitizerUnsupported &Unsupported : Plan.Unsupported)
    if (Unsupported.Reason == Reason)
      return true;
  return false;
}

void expectOneGuard(const Finding &F, const SafetyCallsiteRecord &R,
                    uint64_t Capacity,
                    BinaryFormat Format = BinaryFormat::ELF) {
  const RuntimeSanitizerPlan Plan = planRuntimeSanitizer(Format, {F}, {R});
  ASSERT_TRUE(Plan.Complete);
  ASSERT_TRUE(Plan.Unsupported.empty());
  ASSERT_EQ(Plan.Guards.size(), 1u);
  const RuntimeSanitizerGuard &Guard = Plan.Guards.front();
  EXPECT_EQ(Guard.Version, kRuntimeSanitizerPlanVersion);
  EXPECT_EQ(Guard.Occurrence, R.Occurrence);
  EXPECT_EQ(Guard.Kind, R.Kind);
  EXPECT_EQ(Guard.RemainingCapacity, Capacity);
  EXPECT_EQ(Guard.DestinationOperandIndex, R.DestinationOperandIndex);
  EXPECT_EQ(Guard.LengthOperandIndex, R.LengthOperandIndex);
  EXPECT_EQ(Guard.ElementBytes, R.ElementBytes);
}

TEST(RuntimeSanitizerPlanner, EmptyInputsAreACompleteEmptyPlan) {
  const RuntimeSanitizerPlan Plan =
      planRuntimeSanitizer(BinaryFormat::ELF, {}, {});
  EXPECT_TRUE(Plan.Complete);
  EXPECT_TRUE(Plan.Guards.empty());
  EXPECT_TRUE(Plan.Unsupported.empty());
}

TEST(RuntimeSanitizerPlanner, UsesExactRemainingCapacityWithoutRebasing) {
  for (const uint64_t Remaining : {uint64_t{8}, uint64_t{5}, uint64_t{0},
                                   std::numeric_limits<uint64_t>::max()}) {
    SCOPED_TRACE(Remaining);
    expectOneGuard(finding("memcpy", Remaining), record(), Remaining);
  }
}

TEST(RuntimeSanitizerPlanner, GuardsEveryStaticVerdict) {
  for (const Verdict StaticVerdict :
       {Verdict::Safe, Verdict::Unsafe, Verdict::Unknown}) {
    SCOPED_TRACE(toString(StaticVerdict));
    Finding F = finding();
    F.TheVerdict = StaticVerdict;
    expectOneGuard(F, record(), 5);
  }
}

TEST(RuntimeSanitizerPlanner, PlansCountedWriteSemanticMatrix) {
  struct Case {
    const char *Sink;
    const char *StatedName;
    SemanticKind Kind;
    uint32_t Destination;
    uint32_t Length;
    uint32_t ElementBytes;
    BinaryFormat Format;
  };
  const Case Cases[] = {
      {"___imp_memcpy@12", "___imp_memcpy@12", SemanticKind::Memcpy, 0, 2, 1,
       BinaryFormat::ELF},
      {"__aeabi_memcpy4", "__aeabi_memcpy4", SemanticKind::Memcpy, 0, 2, 1,
       BinaryFormat::ELF},
      {"__memcpy_chk", "__memcpy_chk", SemanticKind::Memcpy, 0, 2, 1,
       BinaryFormat::ELF},
      {"memmove", "memmove", SemanticKind::Memmove, 0, 2, 1, BinaryFormat::ELF},
      {"__aeabi_memmove8", "__aeabi_memmove8", SemanticKind::Memmove, 0, 2, 1,
       BinaryFormat::ELF},
      {"__memmove_chk", "__memmove_chk", SemanticKind::Memmove, 0, 2, 1,
       BinaryFormat::ELF},
      {"memset", "memset", SemanticKind::Memset, 0, 2, 1, BinaryFormat::ELF},
      {"__memset_chk", "__memset_chk", SemanticKind::Memset, 0, 2, 1,
       BinaryFormat::ELF},
      {"__aeabi_memset4", "__aeabi_memset4", SemanticKind::Memset, 0, 1, 1,
       BinaryFormat::ELF},
      {"bzero", "bzero", SemanticKind::Bzero, 0, 1, 1, BinaryFormat::ELF},
      {"__aeabi_memclr8", "__aeabi_memclr8", SemanticKind::Bzero, 0, 1, 1,
       BinaryFormat::ELF},
      {"bcopy", "bcopy", SemanticKind::Bcopy, 1, 2, 1, BinaryFormat::ELF},
      {"wmemcpy", "wmemcpy", SemanticKind::Memcpy, 0, 2, 2, BinaryFormat::COFF},
      {"wmemmove", "wmemmove", SemanticKind::Memmove, 0, 2, 4,
       BinaryFormat::ELF},
  };

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Sink);
    Finding F = finding(C.Sink);
    F.Name = C.StatedName;
    F.ArgIndex = static_cast<int>(C.Length);
    SafetyCallsiteRecord R = record(C.Kind);
    R.DestinationOperandIndex = C.Destination;
    R.LengthOperandIndex = C.Length;
    R.ElementBytes = C.ElementBytes;
    expectOneGuard(F, R, 5, C.Format);
  }
}

TEST(RuntimeSanitizerPlanner, RejectsUnknownAndInexactCapacity) {
  Finding Unknown = finding("memcpy", std::nullopt);
  RuntimeSanitizerPlan Plan =
      planRuntimeSanitizer(BinaryFormat::ELF, {Unknown}, {record()});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::UnknownCapacity));

  Finding Inexact = finding();
  Inexact.CapacityKind = CapacityPrecision::ContainerUpperBound;
  Inexact.CapacityExact = false;
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {Inexact}, {record()});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::InexactCapacity));

  Finding Storage = finding();
  Storage.CapacityKind = CapacityPrecision::StorageExact;
  Storage.CapacityExact = true; // A stale legacy projection cannot widen it.
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {Storage}, {record()});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::InexactCapacity));
}

TEST(RuntimeSanitizerPlanner, CapacityPrecisionNamesAreStable) {
  EXPECT_STREQ(toString(CapacityPrecision::Unknown), "unknown");
  EXPECT_STREQ(toString(CapacityPrecision::ContainerUpperBound),
               "container_upper_bound");
  EXPECT_STREQ(toString(CapacityPrecision::StorageExact), "storage_exact");
  EXPECT_STREQ(toString(CapacityPrecision::TypedBufferExact),
               "typed_buffer_exact");
}

TEST(RuntimeSanitizerPlanner, RejectsNonNativeFormatContext) {
  const RuntimeSanitizerPlan Plan =
      planRuntimeSanitizer(BinaryFormat::EVM, {finding()}, {record()});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(hasReason(
      Plan, RuntimeSanitizerUnsupportedReason::UnsupportedBinaryFormat));
}

TEST(RuntimeSanitizerPlanner, BindsAllSixOccurrenceFieldsInBothDirections) {
  for (unsigned Field = 0; Field != 6; ++Field) {
    SCOPED_TRACE(Field);
    SafetyCallsiteRecord R = record();
    switch (Field) {
    case 0:
      ++R.Occurrence.FuncEntry;
      break;
    case 1:
      ++R.Occurrence.CallVA;
      break;
    case 2:
      ++R.Occurrence.BlockId;
      break;
    case 3:
      ++R.Occurrence.OpIdx;
      break;
    case 4:
      ++R.Occurrence.OriginSeq;
      break;
    case 5:
      ++R.Occurrence.CallSiteId;
      break;
    }
    const RuntimeSanitizerPlan Plan =
        planRuntimeSanitizer(BinaryFormat::ELF, {finding()}, {R});
    EXPECT_FALSE(Plan.Complete);
    EXPECT_TRUE(
        hasReason(Plan, RuntimeSanitizerUnsupportedReason::MissingMetadata));
    EXPECT_TRUE(
        hasReason(Plan, RuntimeSanitizerUnsupportedReason::StaleMetadata));
  }
}

TEST(RuntimeSanitizerPlanner, RejectsInvalidFindingAndMetadataIdentity) {
  Finding InvalidFinding = finding();
  InvalidFinding.OriginSeq = -1;
  RuntimeSanitizerPlan Plan =
      planRuntimeSanitizer(BinaryFormat::ELF, {InvalidFinding}, {record()});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::InvalidIdentity));

  SafetyCallsiteRecord InvalidRecord = record();
  InvalidRecord.Occurrence.CallSiteId = 0;
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {}, {InvalidRecord});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::InvalidIdentity));
}

TEST(RuntimeSanitizerPlanner, RejectsMissingDuplicateAndStaleJoinState) {
  RuntimeSanitizerPlan Plan =
      planRuntimeSanitizer(BinaryFormat::ELF, {finding()}, {});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::MissingMetadata));

  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {}, {record()});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::StaleMetadata));

  std::array<Finding, 2> Findings = {finding(), finding()};
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, Findings, {record()});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(hasReason(
      Plan, RuntimeSanitizerUnsupportedReason::DuplicateAnalyzedSite));

  std::array<SafetyCallsiteRecord, 2> Records = {record(), record()};
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {finding()}, Records);
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::DuplicateMetadata));
}

TEST(RuntimeSanitizerPlanner, RejectsSemanticOperandAndElementDisagreement) {
  SafetyCallsiteRecord R = record(SemanticKind::Memmove);
  RuntimeSanitizerPlan Plan =
      planRuntimeSanitizer(BinaryFormat::ELF, {finding()}, {R});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::SemanticKindMismatch));

  R = record();
  R.DestinationOperandIndex = 1;
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {finding()}, {R});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::OperandMismatch));

  R = record();
  R.LengthOperandIndex = 1;
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {finding()}, {R});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::OperandMismatch));

  R = record();
  R.ElementBytes = 2;
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {finding()}, {R});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::ElementWidthMismatch));

  Finding Wide = finding("wmemcpy");
  R = record();
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {Wide}, {R});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::ElementWidthMismatch));
}

TEST(RuntimeSanitizerPlanner, RejectsFindingLengthOperandDisagreement) {
  Finding F = finding();
  F.ArgIndex = 1;
  const RuntimeSanitizerPlan Plan =
      planRuntimeSanitizer(BinaryFormat::ELF, {F}, {record()});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(
      hasReason(Plan, RuntimeSanitizerUnsupportedReason::OperandMismatch));
}

TEST(RuntimeSanitizerPlanner, ClassifiesUnsupportedStringFormatAndUafSites) {
  Finding String = finding("strcpy");
  String.ArgIndex = 1;
  RuntimeSanitizerPlan Plan =
      planRuntimeSanitizer(BinaryFormat::ELF, {String}, {});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(hasReason(
      Plan, RuntimeSanitizerUnsupportedReason::UnsupportedStringWrite));

  Finding Format = finding("snprintf");
  Format.Class = VulnClass::FormatString;
  Format.Kind = SinkKind::Format;
  Format.ArgIndex = 2;
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {Format}, {});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(hasReason(
      Plan, RuntimeSanitizerUnsupportedReason::UnsupportedFormatWrite));

  Finding Uaf = finding("free");
  Uaf.Origin = Track::Audit;
  Uaf.Class = VulnClass::UseAfterFree;
  Uaf.ArgIndex = 0;
  Plan = planRuntimeSanitizer(BinaryFormat::ELF, {Uaf}, {});
  EXPECT_FALSE(Plan.Complete);
  EXPECT_TRUE(hasReason(
      Plan, RuntimeSanitizerUnsupportedReason::UnsupportedUseAfterFree));
}

TEST(RuntimeSanitizerPlanner, RetainsValidGuardsButNeverClaimsPartialSuccess) {
  Finding String = finding("strcpy");
  String.FuncEntry = 0x402000;
  String.CallVA = 0x402010;
  String.CallSiteId = 5;
  String.ArgIndex = 1;
  const std::array<Finding, 2> Findings = {finding(), String};
  const RuntimeSanitizerPlan Plan =
      planRuntimeSanitizer(BinaryFormat::ELF, Findings, {record()});

  EXPECT_FALSE(Plan.Complete);
  ASSERT_EQ(Plan.Guards.size(), 1u);
  EXPECT_TRUE(hasReason(
      Plan, RuntimeSanitizerUnsupportedReason::UnsupportedStringWrite));
}

TEST(RuntimeSanitizerPlanner, UnsupportedReasonNamesAreStable) {
  EXPECT_STREQ(toString(RuntimeSanitizerUnsupportedReason::InvalidIdentity),
               "invalid_identity");
  EXPECT_STREQ(
      toString(RuntimeSanitizerUnsupportedReason::DuplicateAnalyzedSite),
      "duplicate_analyzed_site");
  EXPECT_STREQ(toString(RuntimeSanitizerUnsupportedReason::DuplicateMetadata),
               "duplicate_metadata");
  EXPECT_STREQ(toString(RuntimeSanitizerUnsupportedReason::MissingMetadata),
               "missing_metadata");
  EXPECT_STREQ(toString(RuntimeSanitizerUnsupportedReason::StaleMetadata),
               "stale_metadata");
  EXPECT_STREQ(toString(RuntimeSanitizerUnsupportedReason::UnknownCapacity),
               "unknown_capacity");
  EXPECT_STREQ(toString(RuntimeSanitizerUnsupportedReason::InexactCapacity),
               "inexact_capacity");
  EXPECT_STREQ(
      toString(RuntimeSanitizerUnsupportedReason::UnsupportedBinaryFormat),
      "unsupported_binary_format");
  EXPECT_STREQ(
      toString(RuntimeSanitizerUnsupportedReason::UnsupportedStringWrite),
      "unsupported_string_write");
  EXPECT_STREQ(
      toString(RuntimeSanitizerUnsupportedReason::UnsupportedFormatWrite),
      "unsupported_format_write");
  EXPECT_STREQ(
      toString(RuntimeSanitizerUnsupportedReason::UnsupportedUseAfterFree),
      "unsupported_use_after_free");
  EXPECT_STREQ(
      toString(RuntimeSanitizerUnsupportedReason::SemanticKindMismatch),
      "semantic_kind_mismatch");
  EXPECT_STREQ(toString(RuntimeSanitizerUnsupportedReason::OperandMismatch),
               "operand_mismatch");
  EXPECT_STREQ(
      toString(RuntimeSanitizerUnsupportedReason::ElementWidthMismatch),
      "element_width_mismatch");
}

} // namespace
