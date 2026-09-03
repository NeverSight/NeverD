//===- NativeProcessReplayAdapterTests.cpp - Native replay boundary ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/safety/NativeProcessReplayAdapter.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <string>
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

NativeProcessReplayAdapterOptionsV1 options() {
  NativeProcessReplayAdapterOptionsV1 Options;
#ifdef _WIN32
  Options.ExecutablePath = R"(C:\authenticated\target)";
#else
  Options.ExecutablePath = "/authenticated/target";
#endif
  return Options;
}

void expectAllFalse(const ProcessReplayExecutorCapabilitiesV1 &Capabilities) {
  EXPECT_FALSE(Capabilities.NativeFormatAndArchitecture);
  EXPECT_FALSE(Capabilities.NoTranslatedExecution);
  EXPECT_FALSE(Capabilities.SelfContainedStaticExecutable);
  EXPECT_FALSE(Capabilities.PinnedTargetObject);
  EXPECT_FALSE(Capabilities.LoadedImageReauthentication);
  EXPECT_FALSE(Capabilities.DirectArgumentVector);
  EXPECT_FALSE(Capabilities.ExactEnvironmentBlock);
  EXPECT_FALSE(Capabilities.PrivateWorkingDirectory);
  EXPECT_FALSE(Capabilities.ExactDescriptorWhitelist);
  EXPECT_FALSE(Capabilities.SandboxBeforeTargetCode);
  EXPECT_FALSE(Capabilities.PrivilegeGainDenied);
  EXPECT_FALSE(Capabilities.NetworkDenied);
  EXPECT_FALSE(Capabilities.HostFilesystemReadIsolated);
  EXPECT_FALSE(Capabilities.FilesystemWritesDenied);
  EXPECT_FALSE(Capabilities.ProcessTreeContained);
  EXPECT_FALSE(Capabilities.ProcessTreeKillAndReap);
  EXPECT_FALSE(Capabilities.WallTimeLimit);
  EXPECT_FALSE(Capabilities.CpuTimeLimit);
  EXPECT_FALSE(Capabilities.MemoryLimit);
  EXPECT_FALSE(Capabilities.OutputLimit);
  EXPECT_FALSE(Capabilities.EnvironmentLookupInterposition);
  EXPECT_FALSE(Capabilities.StandardInputInterposition);
  EXPECT_FALSE(Capabilities.UniquePreCallOccurrenceAttestation);
}

TEST(NativeProcessReplayAdapter, HostAvailabilityNeverInventsCapabilities) {
  const NativeProcessReplayAvailabilityV1 Availability =
      queryNativeProcessReplayAvailabilityV1(minimalPlan(), {}, options());
  EXPECT_FALSE(Availability.Available);
  EXPECT_EQ(Availability.PlanReason, ProcessReplayValidationReason::None);
  EXPECT_EQ(Availability.ExecutionReason, ProcessReplayExecutionReason::None);
#ifdef __linux__
  EXPECT_EQ(Availability.Reason,
            NativeProcessReplayAvailabilityReasonV1::BackendIncomplete);
#else
  EXPECT_EQ(Availability.Reason,
            NativeProcessReplayAvailabilityReasonV1::UnsupportedHost);
#endif
  EXPECT_FALSE(Availability.Detail.empty());
  expectAllFalse(Availability.Capabilities);
}

TEST(NativeProcessReplayAdapter,
     InvalidOptionsPlanAndLimitsPrecedeHostAvailability) {
  NativeProcessReplayAdapterOptionsV1 InvalidOptions = options();
  InvalidOptions.Version = 2;
  NativeProcessReplayAvailabilityV1 Availability =
      queryNativeProcessReplayAvailabilityV1(minimalPlan(), {}, InvalidOptions);
  EXPECT_EQ(Availability.Reason,
            NativeProcessReplayAvailabilityReasonV1::InvalidOptions);
  expectAllFalse(Availability.Capabilities);

  InvalidOptions = options();
  InvalidOptions.ExecutablePath.clear();
  Availability =
      queryNativeProcessReplayAvailabilityV1(minimalPlan(), {}, InvalidOptions);
  EXPECT_EQ(Availability.Reason,
            NativeProcessReplayAvailabilityReasonV1::InvalidOptions);

  ProcessReplayPlanCandidateV1 InvalidPlan = minimalPlan();
  InvalidPlan.Version = 2;
  Availability =
      queryNativeProcessReplayAvailabilityV1(InvalidPlan, {}, options());
  EXPECT_EQ(Availability.Reason,
            NativeProcessReplayAvailabilityReasonV1::ExecutionRequestNotReady);
  EXPECT_EQ(Availability.PlanReason,
            ProcessReplayValidationReason::UnsupportedVersion);
  EXPECT_EQ(Availability.ExecutionReason,
            ProcessReplayExecutionReason::PlanNotReady);

  ProcessReplayExecutionLimitsV1 InvalidLimits;
  InvalidLimits.MaxProcesses = 2;
  Availability = queryNativeProcessReplayAvailabilityV1(
      minimalPlan(), InvalidLimits, options());
  EXPECT_EQ(Availability.Reason,
            NativeProcessReplayAvailabilityReasonV1::ExecutionRequestNotReady);
  EXPECT_EQ(Availability.ExecutionReason,
            ProcessReplayExecutionReason::InvalidExecutionLimits);
}

TEST(NativeProcessReplayAdapter,
     PhysicalCallAddressCollisionsAreUnattestableEvenWhenLabelsDiffer) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 2,
      ProcessReplayRequiredCapability::EnvironmentLookupInterposition);
  Plan.Environment.push_back({0, {'X'}, false, {}});
  ProcessReplayEnvironmentLookupEvent Lookup;
  Lookup.At = Plan.TargetOccurrence;
  Lookup.At.FuncEntry = 0x402000;
  Lookup.At.BlockId = 9;
  Lookup.At.OpIdx = 10;
  Lookup.At.OriginSeq = 11;
  Lookup.At.CallSiteId = 12;
  Lookup.EnvironmentId = 0;
  Plan.Events.push_back({0, Lookup});
  ASSERT_TRUE(validate(Plan).candidateReady());

  const NativeProcessReplayAvailabilityV1 Availability =
      queryNativeProcessReplayAvailabilityV1(Plan, {}, options());
  EXPECT_EQ(Availability.Reason,
            NativeProcessReplayAvailabilityReasonV1::ExecutionRequestNotReady);
  EXPECT_EQ(Availability.ExecutionReason,
            ProcessReplayExecutionReason::UnattestableOccurrenceMap);
  expectAllFalse(Availability.Capabilities);
}

TEST(NativeProcessReplayAdapter,
     RepeatedInvocationsOfOnePhysicalInputSiteRemainAttestable) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 2,
      ProcessReplayRequiredCapability::EnvironmentLookupInterposition);
  Plan.Environment.push_back({0, {'X'}, false, {}});

  ProcessReplayEnvironmentLookupEvent Lookup;
  Lookup.At.FuncEntry = 0x402000;
  Lookup.At.CallVA = 0x402020;
  Lookup.At.BlockId = 9;
  Lookup.At.OpIdx = 10;
  Lookup.At.OriginSeq = 11;
  Lookup.At.CallSiteId = 12;
  Lookup.At.Invocation = 0;
  Lookup.EnvironmentId = 0;
  Plan.Events.push_back({0, Lookup});
  Lookup.At.Invocation = 1;
  Plan.Events.push_back({1, Lookup});
  ASSERT_TRUE(validate(Plan).candidateReady());

  const NativeProcessReplayAvailabilityV1 Availability =
      queryNativeProcessReplayAvailabilityV1(Plan, {}, options());
  EXPECT_EQ(Availability.ExecutionReason, ProcessReplayExecutionReason::None);
#ifdef __linux__
  EXPECT_EQ(Availability.Reason,
            NativeProcessReplayAvailabilityReasonV1::BackendIncomplete);
#else
  EXPECT_EQ(Availability.Reason,
            NativeProcessReplayAvailabilityReasonV1::UnsupportedHost);
#endif
  expectAllFalse(Availability.Capabilities);
}

TEST(NativeProcessReplayAdapter,
     FactoryReturnsNoCallbacksWhileTheCompleteBackendIsUnavailable) {
  llvm::Expected<ProcessReplayExecutorOperationsV1> Operations =
      createNativeProcessReplayOperationsV1(minimalPlan(), {}, options());
  ASSERT_FALSE(static_cast<bool>(Operations));
  const std::string Detail = llvm::toString(Operations.takeError());
  EXPECT_NE(Detail.find("unavailable"), std::string::npos);
}

TEST(NativeProcessReplayAdapter, AvailabilityReasonNamesAreStable) {
  EXPECT_STREQ(toString(NativeProcessReplayAvailabilityReasonV1::None), "none");
  EXPECT_STREQ(
      toString(NativeProcessReplayAvailabilityReasonV1::InvalidOptions),
      "invalid_options");
  EXPECT_STREQ(
      toString(
          NativeProcessReplayAvailabilityReasonV1::ExecutionRequestNotReady),
      "execution_request_not_ready");
  EXPECT_STREQ(
      toString(NativeProcessReplayAvailabilityReasonV1::UnsupportedHost),
      "unsupported_host");
  EXPECT_STREQ(
      toString(NativeProcessReplayAvailabilityReasonV1::BackendIncomplete),
      "backend_incomplete");
  EXPECT_STREQ(
      toString(static_cast<NativeProcessReplayAvailabilityReasonV1>(0xffff)),
      "unknown");
}

} // namespace
