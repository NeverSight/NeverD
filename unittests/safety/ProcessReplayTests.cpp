//===- ProcessReplayTests.cpp - Process replay plan validation tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/safety/ProcessReplay.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace neverd;
using namespace neverd::safety::process_replay;

namespace {

ProcessReplayPlanCandidateV1 minimalPlan() {
  ProcessReplayPlanCandidateV1 Plan;
  Plan.Target.Format = BinaryFormat::ELF;
  Plan.Target.Architecture = Arch::X64;
  Plan.Target.Bits = Bitness::Bits64;
  Plan.Target.Mode = InstructionMode::Default;
  Plan.Target.Endianness = ProcessReplayEndianness::Little;
  Plan.Target.Relocatable = false;
  Plan.Target.Base = 0x400000;
  Plan.Target.Entry = 0x401000;
  Plan.Target.SHA256 = std::array<uint8_t, 32>{1};

  Plan.TargetOccurrence.FuncEntry = 0x401100;
  Plan.TargetOccurrence.CallVA = 0x401120;
  Plan.TargetOccurrence.BlockId = 1;
  Plan.TargetOccurrence.OpIdx = 2;
  Plan.TargetOccurrence.OriginSeq = 3;
  Plan.TargetOccurrence.CallSiteId = 4;
  Plan.TargetOccurrence.Invocation = 0;

  Plan.RequiredCapabilities = {
      ProcessReplayRequiredCapability::ArgumentVectorInjection,
      ProcessReplayRequiredCapability::EnvironmentIsolation,
      ProcessReplayRequiredCapability::TargetIdentityAuthentication,
      ProcessReplayRequiredCapability::TargetOccurrenceAttestation};
  Plan.Arguments.push_back({0, {'p', 'r', 'o', 'g'}});
  return Plan;
}

ProcessReplayOccurrence occurrence(uint64_t CallVA, uint64_t Invocation) {
  ProcessReplayOccurrence Occurrence;
  Occurrence.FuncEntry = 0x402000;
  Occurrence.CallVA = CallVA;
  Occurrence.BlockId = 5;
  Occurrence.OpIdx = 6;
  Occurrence.OriginSeq = 7;
  Occurrence.CallSiteId = static_cast<uint32_t>(CallVA & 0xffffu);
  Occurrence.Invocation = Invocation;
  return Occurrence;
}

ProcessReplayEvent stdinEvent(uint32_t Id, uint64_t Invocation, uint64_t Offset,
                              uint64_t Requested, std::vector<uint8_t> Bytes,
                              ProcessReplayReadOutcome Outcome, bool EOFAfter) {
  ProcessReplayStdinReadEvent Read;
  Read.At = occurrence(0x402030, Invocation);
  Read.ResourceId = 0;
  Read.Offset = Offset;
  Read.RequestedBytes = Requested;
  Read.ReturnedBytes = Bytes.size();
  Read.Bytes = std::move(Bytes);
  Read.Outcome = Outcome;
  Read.EOFAfter = EOFAfter;
  ProcessReplayEvent Event;
  Event.Id = Id;
  Event.Payload = std::move(Read);
  return Event;
}

ProcessReplayEvent environmentEvent(uint32_t Id, uint64_t Invocation,
                                    uint32_t EnvironmentId) {
  ProcessReplayEnvironmentLookupEvent Lookup;
  Lookup.At = occurrence(0x402010, Invocation);
  Lookup.EnvironmentId = EnvironmentId;
  ProcessReplayEvent Event;
  Event.Id = Id;
  Event.Payload = std::move(Lookup);
  return Event;
}

ProcessReplayPlanCandidateV1 stdinPlan() {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 2,
      ProcessReplayRequiredCapability::StandardInputInterposition);
  Plan.Resources.push_back({0, ProcessReplayResourceKind::StandardInput});
  return Plan;
}

TEST(ProcessReplayValidation, AcceptsOnlyATypedPlanCandidate) {
  const ProcessReplayValidation Result = validate(minimalPlan());
  EXPECT_TRUE(Result.candidateReady());
  EXPECT_EQ(Result.Reason, ProcessReplayValidationReason::None);
  EXPECT_STREQ(toString(Result.Reason), "none");
  EXPECT_EQ(Result.LiteralBytes, 5u); // argv[0] plus its implicit NUL.
  EXPECT_EQ(kProcessReplayAdapter, std::string_view("process-replay-v1"));
  EXPECT_EQ(kProcessReplaySchemaVersion, 1u);
}

TEST(ProcessReplayValidation, RejectsSentinelTargetAddresses) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.Target.Entry = std::numeric_limits<uint64_t>::max();
  ProcessReplayValidation Result = validate(Plan);
  EXPECT_FALSE(Result.candidateReady());
  EXPECT_EQ(Result.Reason, ProcessReplayValidationReason::InvalidTargetAddress);

  Plan = minimalPlan();
  Plan.Target.Base = std::numeric_limits<uint64_t>::max();
  Result = validate(Plan);
  EXPECT_EQ(Result.Reason, ProcessReplayValidationReason::InvalidTargetAddress);
}

TEST(ProcessReplayValidation, BindsEveryTargetIdentityField) {
  for (unsigned Field = 0; Field != 9; ++Field) {
    SCOPED_TRACE(Field);
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    switch (Field) {
    case 0:
      Plan.Target.Format.reset();
      break;
    case 1:
      Plan.Target.Architecture.reset();
      break;
    case 2:
      Plan.Target.Bits.reset();
      break;
    case 3:
      Plan.Target.Mode.reset();
      break;
    case 4:
      Plan.Target.Endianness.reset();
      break;
    case 5:
      Plan.Target.Relocatable.reset();
      break;
    case 6:
      Plan.Target.Base.reset();
      break;
    case 7:
      Plan.Target.Entry.reset();
      break;
    case 8:
      Plan.Target.SHA256.reset();
      break;
    }
    EXPECT_EQ(validate(Plan).Reason,
              ProcessReplayValidationReason::IncompleteTargetIdentity);
  }
}

TEST(ProcessReplayValidation, RestrictsThePlanOnlyTargetSurface) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.Version = 2;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedVersion);

  Plan = minimalPlan();
  Plan.Target.Format = BinaryFormat::EVM;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedTargetFormat);
  Plan.Target.Format = BinaryFormat::Unknown;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedTargetFormat);

  Plan = minimalPlan();
  Plan.Target.Architecture = Arch::X86;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedTargetArchitecture);
  Plan.Target.Architecture = Arch::Unknown;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedTargetArchitecture);

  Plan = minimalPlan();
  Plan.Target.Bits = Bitness::Bits32;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedTargetBitness);

  Plan = minimalPlan();
  Plan.Target.Mode = InstructionMode::Thumb;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedTargetMode);

  Plan = minimalPlan();
  Plan.Target.Endianness = ProcessReplayEndianness::Big;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedTargetEndianness);

  Plan = minimalPlan();
  Plan.Target.Relocatable = true;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::RelocatableTarget);
}

TEST(ProcessReplayValidation, RequiresEveryOccurrenceCoordinate) {
  for (unsigned Field = 0; Field != 7; ++Field) {
    SCOPED_TRACE(Field);
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    switch (Field) {
    case 0:
      Plan.TargetOccurrence.FuncEntry.reset();
      break;
    case 1:
      Plan.TargetOccurrence.CallVA.reset();
      break;
    case 2:
      Plan.TargetOccurrence.BlockId.reset();
      break;
    case 3:
      Plan.TargetOccurrence.OpIdx.reset();
      break;
    case 4:
      Plan.TargetOccurrence.OriginSeq.reset();
      break;
    case 5:
      Plan.TargetOccurrence.CallSiteId.reset();
      break;
    case 6:
      Plan.TargetOccurrence.Invocation.reset();
      break;
    }
    EXPECT_EQ(validate(Plan).Reason,
              ProcessReplayValidationReason::IncompleteOccurrence);
  }
}

TEST(ProcessReplayValidation, RejectsInvalidOccurrenceCoordinates) {
  const uint32_t InvalidIndex =
      static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) + 1u;
  for (unsigned Field = 0; Field != 7; ++Field) {
    SCOPED_TRACE(Field);
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    switch (Field) {
    case 0:
      Plan.TargetOccurrence.FuncEntry = std::numeric_limits<uint64_t>::max();
      break;
    case 1:
      Plan.TargetOccurrence.CallVA = std::numeric_limits<uint64_t>::max();
      break;
    case 2:
      Plan.TargetOccurrence.BlockId = InvalidIndex;
      break;
    case 3:
      Plan.TargetOccurrence.OpIdx = InvalidIndex;
      break;
    case 4:
      Plan.TargetOccurrence.OriginSeq = InvalidIndex;
      break;
    case 5:
      Plan.TargetOccurrence.CallSiteId = 0;
      break;
    case 6:
      Plan.TargetOccurrence.Invocation =
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u;
      break;
    }
    EXPECT_EQ(validate(Plan).Reason,
              ProcessReplayValidationReason::InvalidOccurrence);
  }
}

TEST(ProcessReplayValidation, DoesNotUseNumericZeroAsFieldAbsence) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.Target.Base = 0;
  Plan.Target.Entry = 0;
  Plan.Target.SHA256 = std::array<uint8_t, 32>{};
  Plan.TargetOccurrence.FuncEntry = 0;
  Plan.TargetOccurrence.CallVA = 0;
  EXPECT_TRUE(validate(Plan).candidateReady());
}

TEST(ProcessReplayValidation, AcceptsOnlyThePosixNativeTargetMatrix) {
  for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::MachO})
    for (Arch Architecture : {Arch::X64, Arch::AArch64}) {
      SCOPED_TRACE(static_cast<unsigned>(Format));
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      ProcessReplayPlanCandidateV1 Plan = minimalPlan();
      Plan.Target.Format = Format;
      Plan.Target.Architecture = Architecture;
      EXPECT_TRUE(validate(Plan).candidateReady());
    }

  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.Target.Format = BinaryFormat::COFF;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedTargetFormat);
}

TEST(ProcessReplayValidation, ValidatesCanonicalArgvAndItsImplicitTerminators) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.Arguments.push_back({1, {}});
  ProcessReplayValidation Result = validate(Plan);
  ASSERT_TRUE(Result.candidateReady());
  EXPECT_EQ(Result.LiteralBytes, 6u);

  Plan.Arguments[1].Index = 2;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::NonCanonicalArgumentOrder);

  Plan = minimalPlan();
  Plan.Arguments.front().Bytes.push_back(0);
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidArgument);

  Plan = minimalPlan();
  Plan.Arguments.clear();
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::MissingArgumentVector);
}

TEST(ProcessReplayValidation, EnforcesArgumentAndLiteralByteBudgetsAtBoundary) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  ProcessReplayValidationLimits Limits;
  Limits.MaxArguments = 1;
  Limits.MaxLiteralBytes = 5;
  ProcessReplayValidation Result = validate(Plan, Limits);
  ASSERT_TRUE(Result.candidateReady());
  EXPECT_EQ(Result.LiteralBytes, 5u);

  Limits.MaxLiteralBytes = 4;
  Result = validate(Plan, Limits);
  EXPECT_EQ(Result.Reason,
            ProcessReplayValidationReason::LiteralByteBudgetExceeded);

  Limits.MaxLiteralBytes = 5;
  Limits.MaxArguments = 0;
  EXPECT_EQ(validate(Plan, Limits).Reason,
            ProcessReplayValidationReason::ArgumentLimitExceeded);

  Limits = {};
  Limits.MaxArguments = kProcessReplayMaxArguments + 1;
  EXPECT_EQ(validate(Plan, Limits).Reason,
            ProcessReplayValidationReason::InvalidLimits);
  Limits = {};
  Limits.MaxLiteralBytes = kProcessReplayMaxLiteralBytes + 1;
  EXPECT_EQ(validate(Plan, Limits).Reason,
            ProcessReplayValidationReason::InvalidLimits);
}

TEST(ProcessReplayValidation,
     ValidatesExactPresentAndAbsentEnvironmentEntries) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.Environment.push_back({/*Id=*/0, /*Name=*/{'M', 'O', 'D', 'E'},
                              /*Present=*/true,
                              /*Value=*/{'s', 'a', 'f', 'e'}});
  Plan.Environment.push_back({/*Id=*/1, /*Name=*/{'S', 'E', 'C', 'R', 'E', 'T'},
                              /*Present=*/false, /*Value=*/{}});
  ProcessReplayValidation Result = validate(Plan);
  ASSERT_TRUE(Result.candidateReady());
  // argv[0]\0 + MODE=safe\0 + the retained negative lookup name SECRET\0.
  EXPECT_EQ(Result.LiteralBytes, 22u);

  Plan.Arguments.clear();
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::MissingArgumentVector);
}

TEST(ProcessReplayValidation, RejectsAmbiguousEnvironmentState) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.Environment.push_back({1, {'A'}, true, {'1'}});
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::NonCanonicalEnvironmentOrder);

  Plan = minimalPlan();
  Plan.Environment.push_back({0, {}, true, {'1'}});
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidEnvironmentName);
  Plan.Environment.front().Name = {'A', 0, 'B'};
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidEnvironmentName);
  Plan.Environment.front().Name = {'A', '=', 'B'};
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidEnvironmentName);

  Plan = minimalPlan();
  Plan.Environment.push_back({0, {'A'}, true, {'1', 0}});
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidEnvironmentValue);
  Plan.Environment.front() = {0, {'A'}, false, {'1'}};
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidEnvironmentValue);

  Plan = minimalPlan();
  Plan.Environment.push_back({0, {'A'}, true, {'1'}});
  Plan.Environment.push_back({1, {'A'}, false, {}});
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::DuplicateEnvironmentName);
}

TEST(ProcessReplayValidation,
     RejectsAbsentEnvironmentValuesBeforeChargingOrScanningThem) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.Arguments.front().Bytes.clear();
  Plan.Environment.push_back(
      {0,
       {'A'},
       false,
       std::vector<uint8_t>(kProcessReplayMaxLiteralBytes + 1, 'x')});

  ProcessReplayValidationLimits Limits;
  // Only argv[0]'s terminator fits.  A contradictory absent value must be
  // rejected in O(1) before either its name is charged or its bytes scanned.
  Limits.MaxLiteralBytes = 1;
  const ProcessReplayValidation Result = validate(Plan, Limits);
  EXPECT_EQ(Result.Reason,
            ProcessReplayValidationReason::InvalidEnvironmentValue);
  EXPECT_EQ(Result.LiteralBytes, 1u);
}

TEST(ProcessReplayValidation, EnforcesEnvironmentCountAndByteBudgets) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.Environment.push_back({0, {'A'}, true, {'B'}});

  ProcessReplayValidationLimits Limits;
  Limits.MaxArguments = 1;
  Limits.MaxEnvironmentEntries = 1;
  Limits.MaxLiteralBytes = 9; // argv[0]\0 + A=B\0
  EXPECT_TRUE(validate(Plan, Limits).candidateReady());
  Limits.MaxLiteralBytes = 8;
  EXPECT_EQ(validate(Plan, Limits).Reason,
            ProcessReplayValidationReason::LiteralByteBudgetExceeded);

  Limits.MaxLiteralBytes = 9;
  Limits.MaxEnvironmentEntries = 0;
  EXPECT_EQ(validate(Plan, Limits).Reason,
            ProcessReplayValidationReason::EnvironmentLimitExceeded);

  Limits = {};
  Limits.MaxEnvironmentEntries = kProcessReplayMaxEnvironmentEntries + 1;
  EXPECT_EQ(validate(Plan, Limits).Reason,
            ProcessReplayValidationReason::InvalidLimits);
}

TEST(ProcessReplayValidation, AcceptsOrderedEnvironmentAndRepeatedStdinEvents) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.RequiredCapabilities = {
      ProcessReplayRequiredCapability::ArgumentVectorInjection,
      ProcessReplayRequiredCapability::EnvironmentIsolation,
      ProcessReplayRequiredCapability::EnvironmentLookupInterposition,
      ProcessReplayRequiredCapability::StandardInputInterposition,
      ProcessReplayRequiredCapability::TargetIdentityAuthentication,
      ProcessReplayRequiredCapability::TargetOccurrenceAttestation};
  Plan.Environment.push_back(
      {0, {'K', 'E', 'Y'}, true, {'V', 'A', 'L', 'U', 'E'}});
  Plan.Resources.push_back({0, ProcessReplayResourceKind::StandardInput});
  Plan.Events.push_back(environmentEvent(0, 0, 0));
  Plan.Events.push_back(stdinEvent(1, 0, 0, 4, {'a', 'b'},
                                   ProcessReplayReadOutcome::Data, false));
  Plan.Events.push_back(environmentEvent(2, 1, 0));
  Plan.Events.push_back(stdinEvent(3, 1, 2, 4, {'c', 'd', 'e'},
                                   ProcessReplayReadOutcome::Data, true));
  Plan.Events.push_back(
      stdinEvent(4, 2, 5, 4, {}, ProcessReplayReadOutcome::EndOfFile, true));

  const ProcessReplayValidation Result = validate(Plan);
  ASSERT_TRUE(Result.candidateReady()) << Result.Detail;
  EXPECT_EQ(Result.EventCount, 5u);
  EXPECT_EQ(Result.LiteralBytes, 20u);
}

TEST(ProcessReplayValidation, RequiresAnExactCanonicalCapabilitySet) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.RequiredCapabilities.pop_back();
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::RequiredCapabilityMismatch);

  Plan = minimalPlan();
  Plan.RequiredCapabilities.push_back(
      ProcessReplayRequiredCapability::EnvironmentIsolation);
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::NonCanonicalCapabilityOrder);

  Plan = minimalPlan();
  Plan.RequiredCapabilities.front() =
      static_cast<ProcessReplayRequiredCapability>(0xff);
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedRequiredCapability);

  Plan = minimalPlan();
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 2,
      ProcessReplayRequiredCapability::StandardInputInterposition);
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::RequiredCapabilityMismatch);
}

TEST(ProcessReplayValidation, RejectsAStdinRangeBeforeCursorComparison) {
  ProcessReplayPlanCandidateV1 Plan = stdinPlan();
  Plan.Events.push_back(stdinEvent(0, 0, std::numeric_limits<uint64_t>::max(),
                                   1, {'x'}, ProcessReplayReadOutcome::Data,
                                   false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::ArithmeticOverflow);
}

TEST(ProcessReplayValidation, RequiresFullIdentityOnEveryInputEvent) {
  for (unsigned Field = 0; Field != 7; ++Field) {
    SCOPED_TRACE(Field);
    ProcessReplayPlanCandidateV1 Plan = stdinPlan();
    Plan.Events.push_back(
        stdinEvent(0, 0, 0, 1, {'x'}, ProcessReplayReadOutcome::Data, false));
    ProcessReplayOccurrence &At =
        std::get<ProcessReplayStdinReadEvent>(Plan.Events.front().Payload).At;
    switch (Field) {
    case 0:
      At.FuncEntry.reset();
      break;
    case 1:
      At.CallVA.reset();
      break;
    case 2:
      At.BlockId.reset();
      break;
    case 3:
      At.OpIdx.reset();
      break;
    case 4:
      At.OriginSeq.reset();
      break;
    case 5:
      At.CallSiteId.reset();
      break;
    case 6:
      At.Invocation.reset();
      break;
    }
    EXPECT_EQ(validate(Plan).Reason,
              ProcessReplayValidationReason::IncompleteOccurrence);
  }
}

TEST(ProcessReplayValidation, EnforcesEventOrderUniquenessAndInvocation) {
  ProcessReplayPlanCandidateV1 Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(1, 0, 0, 1, {'x'}, ProcessReplayReadOutcome::Data, false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::NonCanonicalEventOrder);

  Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 1, 0, 1, {'x'}, ProcessReplayReadOutcome::Data, false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvocationOutOfOrder);

  Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 1, {'x'}, ProcessReplayReadOutcome::Data, false));
  Plan.Events.push_back(
      stdinEvent(1, 0, 1, 1, {'y'}, ProcessReplayReadOutcome::Data, false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::DuplicateEventOccurrence);

  Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 1, {'x'}, ProcessReplayReadOutcome::Data, false));
  Plan.Events.push_back(
      stdinEvent(1, 2, 1, 1, {'y'}, ProcessReplayReadOutcome::Data, false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvocationOutOfOrder);
}

TEST(ProcessReplayValidation, SeparatesInputAndTargetDynamicOccurrences) {
  ProcessReplayPlanCandidateV1 Plan = stdinPlan();
  ProcessReplayEvent Event =
      stdinEvent(0, 0, 0, 1, {'x'}, ProcessReplayReadOutcome::Data, false);
  std::get<ProcessReplayStdinReadEvent>(Event.Payload).At =
      Plan.TargetOccurrence;
  Plan.Events.push_back(std::move(Event));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::TargetOccurrenceCollision);
}

TEST(ProcessReplayValidation, ValidatesResourceAndEnvironmentReferences) {
  ProcessReplayPlanCandidateV1 Plan = stdinPlan();
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnreferencedResource);

  Plan = minimalPlan();
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 2,
      ProcessReplayRequiredCapability::StandardInputInterposition);
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 1, {'x'}, ProcessReplayReadOutcome::Data, false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::DanglingResource);

  Plan = stdinPlan();
  Plan.Resources.front().Id = 1;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::NonCanonicalResourceOrder);

  Plan = stdinPlan();
  Plan.Resources.front().Kind = static_cast<ProcessReplayResourceKind>(0xff);
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedResourceKind);

  Plan = stdinPlan();
  Plan.Resources.push_back({1, ProcessReplayResourceKind::StandardInput});
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::DuplicateResource);

  Plan = stdinPlan();
  Plan.Events.push_back({0, std::monostate{}});
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::UnsupportedEventKind);

  Plan = minimalPlan();
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 2,
      ProcessReplayRequiredCapability::EnvironmentLookupInterposition);
  Plan.Events.push_back(environmentEvent(0, 0, 0));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::DanglingEnvironment);

  Plan.Environment.push_back({0, {'M', 'I', 'S', 'S'}, false, {}});
  EXPECT_TRUE(validate(Plan).candidateReady());
}

TEST(ProcessReplayValidation, PreservesZeroDataEofAndShortReadBoundaries) {
  ProcessReplayPlanCandidateV1 Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 0, {}, ProcessReplayReadOutcome::ZeroLength, false));
  Plan.Events.push_back(stdinEvent(1, 1, 0, 4, {'a', 'b'},
                                   ProcessReplayReadOutcome::Data, false));
  Plan.Events.push_back(
      stdinEvent(2, 2, 2, 1, {}, ProcessReplayReadOutcome::EndOfFile, true));
  Plan.Events.push_back(
      stdinEvent(3, 3, 2, 0, {}, ProcessReplayReadOutcome::ZeroLength, true));
  Plan.Events.push_back(
      stdinEvent(4, 4, 2, 1, {}, ProcessReplayReadOutcome::EndOfFile, true));
  const ProcessReplayValidation Result = validate(Plan);
  ASSERT_TRUE(Result.candidateReady()) << Result.Detail;
  EXPECT_EQ(Result.EventCount, 5u);
  EXPECT_EQ(Result.LiteralBytes, 7u);
}

TEST(ProcessReplayValidation, RejectsInconsistentReadResultsAndOutcomes) {
  ProcessReplayPlanCandidateV1 Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 1, {'x'}, ProcessReplayReadOutcome::Data, false));
  std::get<ProcessReplayStdinReadEvent>(Plan.Events.front().Payload)
      .ReturnedBytes = 0;
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidReadResult);

  Plan = stdinPlan();
  Plan.Events.push_back(stdinEvent(0, 0, 0, 1, {'x', 'y'},
                                   ProcessReplayReadOutcome::Data, false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidReadResult);

  Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 1, {}, ProcessReplayReadOutcome::Data, false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidReadResult);

  Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 1, {}, ProcessReplayReadOutcome::EndOfFile, false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidReadResult);

  Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 1, {}, ProcessReplayReadOutcome::ZeroLength, false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidReadResult);

  Plan = stdinPlan();
  Plan.Events.push_back(stdinEvent(
      0, 0, 0, 0, {}, static_cast<ProcessReplayReadOutcome>(0xff), false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidReadOutcome);
}

TEST(ProcessReplayValidation, EnforcesMonotonicOffsetAndEofState) {
  ProcessReplayPlanCandidateV1 Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 1, 1, {'x'}, ProcessReplayReadOutcome::Data, false));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::StdinOffsetMismatch);

  Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 0, {}, ProcessReplayReadOutcome::ZeroLength, true));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidEOFTransition);

  Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 1, {}, ProcessReplayReadOutcome::EndOfFile, true));
  Plan.Events.push_back(
      stdinEvent(1, 1, 0, 1, {'x'}, ProcessReplayReadOutcome::Data, true));
  EXPECT_EQ(validate(Plan).Reason,
            ProcessReplayValidationReason::InvalidEOFTransition);
}

TEST(ProcessReplayValidation, EnforcesEventResourceAndRequestBudgets) {
  ProcessReplayPlanCandidateV1 Plan = stdinPlan();
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 1, {'x'}, ProcessReplayReadOutcome::Data, false));
  ProcessReplayValidationLimits Limits;
  Limits.MaxResources = 1;
  Limits.MaxEvents = 1;
  Limits.MaxReadRequestBytes = 1;
  Limits.MaxTotalReadRequestBytes = 1;
  Limits.MaxLiteralBytes = 6;
  EXPECT_TRUE(validate(Plan, Limits).candidateReady());

  ProcessReplayValidationLimits Mutated = Limits;
  Mutated.MaxResources = 0;
  EXPECT_EQ(validate(Plan, Mutated).Reason,
            ProcessReplayValidationReason::ResourceLimitExceeded);
  Mutated = Limits;
  Mutated.MaxEvents = 0;
  EXPECT_EQ(validate(Plan, Mutated).Reason,
            ProcessReplayValidationReason::EventLimitExceeded);
  Mutated = Limits;
  Mutated.MaxReadRequestBytes = 0;
  EXPECT_EQ(validate(Plan, Mutated).Reason,
            ProcessReplayValidationReason::ReadRequestLimitExceeded);
  Mutated = Limits;
  Mutated.MaxTotalReadRequestBytes = 0;
  EXPECT_EQ(validate(Plan, Mutated).Reason,
            ProcessReplayValidationReason::TotalReadRequestBudgetExceeded);
  Mutated = Limits;
  Mutated.MaxLiteralBytes = 5;
  EXPECT_EQ(validate(Plan, Mutated).Reason,
            ProcessReplayValidationReason::LiteralByteBudgetExceeded);

  Mutated = {};
  Mutated.MaxResources = kProcessReplayMaxResources + 1;
  EXPECT_EQ(validate(Plan, Mutated).Reason,
            ProcessReplayValidationReason::InvalidLimits);
  Mutated = {};
  Mutated.MaxEvents = kProcessReplayMaxEvents + 1;
  EXPECT_EQ(validate(Plan, Mutated).Reason,
            ProcessReplayValidationReason::InvalidLimits);
  Mutated = {};
  Mutated.MaxReadRequestBytes = kProcessReplayMaxReadRequestBytes + 1;
  EXPECT_EQ(validate(Plan, Mutated).Reason,
            ProcessReplayValidationReason::InvalidLimits);
  Mutated = {};
  Mutated.MaxTotalReadRequestBytes = kProcessReplayMaxTotalReadRequestBytes + 1;
  EXPECT_EQ(validate(Plan, Mutated).Reason,
            ProcessReplayValidationReason::InvalidLimits);
}

TEST(ProcessReplayValidation, ExposesStableTypedReasonNames) {
  struct Case {
    ProcessReplayValidationReason Reason;
    const char *Name;
  };
  const Case Cases[] = {
      {ProcessReplayValidationReason::None, "none"},
      {ProcessReplayValidationReason::UnsupportedVersion,
       "unsupported_version"},
      {ProcessReplayValidationReason::IncompleteTargetIdentity,
       "incomplete_target_identity"},
      {ProcessReplayValidationReason::UnsupportedTargetFormat,
       "unsupported_target_format"},
      {ProcessReplayValidationReason::UnsupportedTargetArchitecture,
       "unsupported_target_architecture"},
      {ProcessReplayValidationReason::UnsupportedTargetBitness,
       "unsupported_target_bitness"},
      {ProcessReplayValidationReason::UnsupportedTargetMode,
       "unsupported_target_mode"},
      {ProcessReplayValidationReason::UnsupportedTargetEndianness,
       "unsupported_target_endianness"},
      {ProcessReplayValidationReason::RelocatableTarget, "relocatable_target"},
      {ProcessReplayValidationReason::IncompleteOccurrence,
       "incomplete_occurrence"},
      {ProcessReplayValidationReason::InvalidOccurrence, "invalid_occurrence"},
      {ProcessReplayValidationReason::InvalidLimits, "invalid_limits"},
      {ProcessReplayValidationReason::ArgumentLimitExceeded,
       "argument_limit_exceeded"},
      {ProcessReplayValidationReason::NonCanonicalArgumentOrder,
       "noncanonical_argument_order"},
      {ProcessReplayValidationReason::InvalidArgument, "invalid_argument"},
      {ProcessReplayValidationReason::LiteralByteBudgetExceeded,
       "literal_byte_budget_exceeded"},
      {ProcessReplayValidationReason::ArithmeticOverflow,
       "arithmetic_overflow"},
      {ProcessReplayValidationReason::InvalidTargetAddress,
       "invalid_target_address"},
      {ProcessReplayValidationReason::EnvironmentLimitExceeded,
       "environment_limit_exceeded"},
      {ProcessReplayValidationReason::NonCanonicalEnvironmentOrder,
       "noncanonical_environment_order"},
      {ProcessReplayValidationReason::InvalidEnvironmentName,
       "invalid_environment_name"},
      {ProcessReplayValidationReason::InvalidEnvironmentValue,
       "invalid_environment_value"},
      {ProcessReplayValidationReason::DuplicateEnvironmentName,
       "duplicate_environment_name"},
      {ProcessReplayValidationReason::ResourceLimitExceeded,
       "resource_limit_exceeded"},
      {ProcessReplayValidationReason::EventLimitExceeded,
       "event_limit_exceeded"},
      {ProcessReplayValidationReason::ReadRequestLimitExceeded,
       "read_request_limit_exceeded"},
      {ProcessReplayValidationReason::TotalReadRequestBudgetExceeded,
       "total_read_request_budget_exceeded"},
      {ProcessReplayValidationReason::NonCanonicalResourceOrder,
       "noncanonical_resource_order"},
      {ProcessReplayValidationReason::UnsupportedResourceKind,
       "unsupported_resource_kind"},
      {ProcessReplayValidationReason::DuplicateResource, "duplicate_resource"},
      {ProcessReplayValidationReason::UnreferencedResource,
       "unreferenced_resource"},
      {ProcessReplayValidationReason::NonCanonicalEventOrder,
       "noncanonical_event_order"},
      {ProcessReplayValidationReason::UnsupportedEventKind,
       "unsupported_event_kind"},
      {ProcessReplayValidationReason::DuplicateEventOccurrence,
       "duplicate_event_occurrence"},
      {ProcessReplayValidationReason::InvocationOutOfOrder,
       "invocation_out_of_order"},
      {ProcessReplayValidationReason::DanglingEnvironment,
       "dangling_environment"},
      {ProcessReplayValidationReason::DanglingResource, "dangling_resource"},
      {ProcessReplayValidationReason::InvalidReadOutcome,
       "invalid_read_outcome"},
      {ProcessReplayValidationReason::InvalidReadResult, "invalid_read_result"},
      {ProcessReplayValidationReason::StdinOffsetMismatch,
       "stdin_offset_mismatch"},
      {ProcessReplayValidationReason::InvalidEOFTransition,
       "invalid_eof_transition"},
      {ProcessReplayValidationReason::MissingArgumentVector,
       "missing_argument_vector"},
      {ProcessReplayValidationReason::UnsupportedRequiredCapability,
       "unsupported_required_capability"},
      {ProcessReplayValidationReason::NonCanonicalCapabilityOrder,
       "noncanonical_capability_order"},
      {ProcessReplayValidationReason::RequiredCapabilityMismatch,
       "required_capability_mismatch"},
      {ProcessReplayValidationReason::TargetOccurrenceCollision,
       "target_occurrence_collision"},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    EXPECT_STREQ(toString(C.Reason), C.Name);
  }
  EXPECT_STREQ(toString(static_cast<ProcessReplayValidationReason>(0xffff)),
               "unknown");
}

} // namespace
