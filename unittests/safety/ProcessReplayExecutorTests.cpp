//===- ProcessReplayExecutorTests.cpp - Replay coordinator tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/safety/ProcessReplayExecutor.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace neverd;
using namespace neverd::safety::process_replay;

namespace {

llvm::Error testError(const char *Message) {
  return llvm::createStringError(llvm::errc::io_error, "%s", Message);
}

enum class AllocationFailureKind {
  BadAlloc,
  LengthError,
};

[[noreturn]] void throwAllocationFailure(AllocationFailureKind Kind) {
  if (Kind == AllocationFailureKind::BadAlloc)
    throw std::bad_alloc();
  throw std::length_error("injected diagnostic allocation failure");
}

ProcessReplayExecutorCapabilitiesV1 completeCapabilities() {
  ProcessReplayExecutorCapabilitiesV1 Capabilities;
  Capabilities.NativeFormatAndArchitecture = true;
  Capabilities.NoTranslatedExecution = true;
  Capabilities.SelfContainedStaticExecutable = true;
  Capabilities.PinnedTargetObject = true;
  Capabilities.LoadedImageReauthentication = true;
  Capabilities.DirectArgumentVector = true;
  Capabilities.ExactEnvironmentBlock = true;
  Capabilities.PrivateWorkingDirectory = true;
  Capabilities.ExactDescriptorWhitelist = true;
  Capabilities.SandboxBeforeTargetCode = true;
  Capabilities.PrivilegeGainDenied = true;
  Capabilities.NetworkDenied = true;
  Capabilities.HostFilesystemReadIsolated = true;
  Capabilities.FilesystemWritesDenied = true;
  Capabilities.ProcessTreeContained = true;
  Capabilities.ProcessTreeKillAndReap = true;
  Capabilities.WallTimeLimit = true;
  Capabilities.CpuTimeLimit = true;
  Capabilities.MemoryLimit = true;
  Capabilities.OutputLimit = true;
  Capabilities.EnvironmentLookupInterposition = true;
  Capabilities.StandardInputInterposition = true;
  Capabilities.UniquePreCallOccurrenceAttestation = true;
  return Capabilities;
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

ProcessReplayPlanCandidateV1 minimalPlan(uint64_t TargetInvocation = 0) {
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
  Plan.TargetOccurrence.Invocation = TargetInvocation;

  Plan.RequiredCapabilities = {
      ProcessReplayRequiredCapability::ArgumentVectorInjection,
      ProcessReplayRequiredCapability::EnvironmentIsolation,
      ProcessReplayRequiredCapability::TargetIdentityAuthentication,
      ProcessReplayRequiredCapability::TargetOccurrenceAttestation};
  Plan.Arguments.push_back({0, {'p', 'r', 'o', 'g'}});
  return Plan;
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

ProcessReplayEvent stdinEvent(uint32_t Id, uint64_t Invocation, uint64_t Offset,
                              uint64_t RequestedBytes,
                              std::vector<uint8_t> Bytes,
                              ProcessReplayReadOutcome Outcome, bool EOFAfter) {
  ProcessReplayStdinReadEvent Read;
  Read.At = occurrence(0x402030, Invocation);
  Read.ResourceId = 0;
  Read.Offset = Offset;
  Read.RequestedBytes = RequestedBytes;
  Read.ReturnedBytes = Bytes.size();
  Read.Bytes = std::move(Bytes);
  Read.Outcome = Outcome;
  Read.EOFAfter = EOFAfter;
  ProcessReplayEvent Event;
  Event.Id = Id;
  Event.Payload = std::move(Read);
  return Event;
}

ProcessReplayBackendEventV1 targetHit(const ProcessReplayOccurrence &At) {
  ProcessReplayBackendEventV1 Event;
  Event.Payload = ProcessReplayTargetSiteHitV1{At, true};
  return Event;
}

ProcessReplayBackendEventV1
environmentRequest(const ProcessReplayOccurrence &At,
                   std::vector<uint8_t> Name) {
  ProcessReplayBackendEventV1 Event;
  Event.Payload = ProcessReplayEnvironmentLookupRequestV1{At, std::move(Name)};
  return Event;
}

ProcessReplayBackendEventV1 stdinRequest(const ProcessReplayOccurrence &At,
                                         uint64_t RequestedBytes) {
  ProcessReplayBackendEventV1 Event;
  Event.Payload = ProcessReplayStdinReadRequestV1{At, 0, 0, RequestedBytes};
  return Event;
}

ProcessReplayBackendEventV1 output(ProcessReplayOutputStreamV1 Stream,
                                   std::vector<uint8_t> Bytes) {
  ProcessReplayBackendEventV1 Event;
  Event.Payload = ProcessReplayOutputChunkV1{Stream, std::move(Bytes)};
  return Event;
}

ProcessReplayTargetAuthenticationV1
authenticationFor(const ProcessReplayPlanCandidateV1 &Plan) {
  ProcessReplayTargetAuthenticationV1 Authentication;
  Authentication.Identity = Plan.Target;
  Authentication.ByteSize = 4096;
  Authentication.ObjectIdentitySHA256 = std::array<uint8_t, 32>{2};
  Authentication.RegularFile = true;
  Authentication.Executable = true;
  Authentication.NativeFormatAndArchitecture = true;
  Authentication.SelfContainedStaticExecutable = true;
  return Authentication;
}

struct FakeExecutor {
  ProcessReplayExecutorCapabilitiesV1 Capabilities = completeCapabilities();
  ProcessReplayTargetAuthenticationV1 Source;
  ProcessReplayTargetAuthenticationV1 Loaded;
  std::deque<ProcessReplayBackendEventV1> Events;
  std::deque<ProcessReplayOutputChunkV1> DrainChunks;
  bool DrainSecurelyDiscarded = false;
  std::vector<ProcessReplayInputReplyV1> Replies;
  std::vector<std::string> Calls;
  std::optional<ProcessReplayLaunchRequestV1> LaunchRequest;

  explicit FakeExecutor(const ProcessReplayPlanCandidateV1 &Plan)
      : Source(authenticationFor(Plan)), Loaded(Source) {}

  ProcessReplayExecutorOperationsV1 operations() {
    ProcessReplayExecutorOperationsV1 Operations;
    Operations.Capabilities = Capabilities;
    Operations.Begin = [this] {
      Calls.push_back("begin");
      return llvm::Error::success();
    };
    Operations.AuthenticatePinnedTarget =
        [this]() -> llvm::Expected<ProcessReplayTargetAuthenticationV1> {
      Calls.push_back("auth-source");
      return Source;
    };
    Operations.PrepareLaunch =
        [this](const ProcessReplayLaunchRequestV1 &Request) {
          Calls.push_back("prepare");
          LaunchRequest = Request;
          return llvm::Error::success();
        };
    Operations.LaunchStopped = [this] {
      Calls.push_back("launch");
      return llvm::Error::success();
    };
    Operations.ObserveAppliedIsolation =
        [this]() -> llvm::Expected<ProcessReplayExecutorCapabilitiesV1> {
      Calls.push_back("observe");
      return Capabilities;
    };
    Operations.AuthenticateLoadedTarget =
        [this]() -> llvm::Expected<ProcessReplayTargetAuthenticationV1> {
      Calls.push_back("auth-loaded");
      return Loaded;
    };
    Operations.Resume = [this] {
      Calls.push_back("resume");
      return llvm::Error::success();
    };
    Operations.NextEvent = [this](std::chrono::steady_clock::time_point)
        -> llvm::Expected<ProcessReplayBackendEventV1> {
      Calls.push_back("next");
      if (Events.empty())
        return testError("fake event queue exhausted");
      ProcessReplayBackendEventV1 Event = std::move(Events.front());
      Events.pop_front();
      return Event;
    };
    Operations.ReplyAndResume = [this](const ProcessReplayInputReplyV1 &Reply) {
      Calls.push_back("reply");
      Replies.push_back(Reply);
      return llvm::Error::success();
    };
    Operations.TerminateTree = [this] {
      Calls.push_back("terminate");
      return llvm::Error::success();
    };
    Operations.ReapTree = [this] {
      Calls.push_back("reap");
      return llvm::Error::success();
    };
    Operations.DrainOutput = [this](std::chrono::steady_clock::time_point)
        -> llvm::Expected<ProcessReplayOutputDrainResultV1> {
      Calls.push_back("drain");
      if (DrainChunks.empty())
        return DrainSecurelyDiscarded
                   ? ProcessReplayOutputDrainResultV1{ProcessReplayOutputSecurelyDiscardedV1{}}
                   : ProcessReplayOutputDrainResultV1{
                         ProcessReplayOutputEndOfFileV1{}};
      ProcessReplayOutputChunkV1 Chunk = std::move(DrainChunks.front());
      DrainChunks.pop_front();
      return ProcessReplayOutputDrainResultV1{std::move(Chunk)};
    };
    Operations.CleanupAfterReap = [this] {
      Calls.push_back("cleanup");
      return llvm::Error::success();
    };
    Operations.RetainContainmentForUnreaped = [this] {
      Calls.push_back("retain");
      return llvm::Error::success();
    };
    return Operations;
  }
};

TEST(ProcessReplayExecutor,
     PureExecutionRequestValidationDoesNotRequireNativeOperations) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  EXPECT_TRUE(validateProcessReplayExecutionRequest(Plan, {}).ready());

  Plan.Version = 2;
  ProcessReplayExecutionValidationV1 Validation =
      validateProcessReplayExecutionRequest(Plan, {});
  EXPECT_EQ(Validation.PlanReason,
            ProcessReplayValidationReason::UnsupportedVersion);
  EXPECT_EQ(Validation.Reason, ProcessReplayExecutionReason::PlanNotReady);

  Plan = minimalPlan();
  ProcessReplayExecutionLimitsV1 Limits;
  Limits.MaxProcesses = 2;
  Validation = validateProcessReplayExecutionRequest(Plan, Limits);
  EXPECT_EQ(Validation.Reason,
            ProcessReplayExecutionReason::InvalidExecutionLimits);
}

TEST(ProcessReplayExecutor, PreflightFailsBeforeAnyCallback) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  ProcessReplayExecutorOperationsV1 Operations = Fake.operations();

  Plan.Arguments.clear();
  ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Operations);
  EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::PlanNotReady);
  EXPECT_EQ(Result.Receipt.PlanValidationReason,
            ProcessReplayValidationReason::MissingArgumentVector);
  EXPECT_TRUE(Fake.Calls.empty());

  Plan = minimalPlan();
  Plan.Arguments.resize(kProcessReplayMaxArguments + 1);
  Result = executeProcessReplay(Plan, {}, Operations);
  EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::PlanNotReady);
  EXPECT_EQ(Result.Receipt.PlanValidationReason,
            ProcessReplayValidationReason::ArgumentLimitExceeded);
  EXPECT_TRUE(Fake.Calls.empty());

  Plan = minimalPlan();
  Plan.Environment.push_back(
      {0,
       std::vector<uint8_t>(kProcessReplayMaxLiteralBytes + 1, 'A'),
       true,
       {}});
  Result = executeProcessReplay(Plan, {}, Operations);
  EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::PlanNotReady);
  EXPECT_EQ(Result.Receipt.PlanValidationReason,
            ProcessReplayValidationReason::LiteralByteBudgetExceeded);
  EXPECT_TRUE(Fake.Calls.empty());

  Plan = minimalPlan();
  Fake.Capabilities.HostFilesystemReadIsolated = false;
  Operations = Fake.operations();
  Result = executeProcessReplay(Plan, {}, Operations);
  EXPECT_EQ(Result.Reason,
            ProcessReplayExecutionReason::InsufficientCapabilities);
  EXPECT_TRUE(Fake.Calls.empty());

  Fake.Capabilities = completeCapabilities();
  Operations = Fake.operations();
  ProcessReplayExecutionLimitsV1 Limits;
  Limits.MaxProtocolBytes = kProcessReplayExecutionMaxProtocolBytes + 1;
  Result = executeProcessReplay(Plan, Limits, Operations);
  EXPECT_EQ(Result.Reason,
            ProcessReplayExecutionReason::InvalidExecutionLimits);
  EXPECT_TRUE(Fake.Calls.empty());
}

TEST(ProcessReplayExecutor,
     ZeroInputTranscriptRequiresEveryInputInterpositionBeforeBegin) {
  enum class MissingInterposition {
    EnvironmentLookup,
    StandardInput,
  };

  for (MissingInterposition Missing : {MissingInterposition::EnvironmentLookup,
                                       MissingInterposition::StandardInput}) {
    SCOPED_TRACE(Missing == MissingInterposition::EnvironmentLookup
                     ? "environment-lookup"
                     : "standard-input");
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    ASSERT_TRUE(Plan.Events.empty());
    FakeExecutor Fake(Plan);
    if (Missing == MissingInterposition::EnvironmentLookup)
      Fake.Capabilities.EnvironmentLookupInterposition = false;
    else
      Fake.Capabilities.StandardInputInterposition = false;

    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, {}, Fake.operations());
    EXPECT_EQ(Result.Reason,
              ProcessReplayExecutionReason::InsufficientCapabilities);
    EXPECT_FALSE(Result.Receipt.Complete);
    EXPECT_TRUE(Fake.Calls.empty());
  }
}

TEST(ProcessReplayExecutor,
     PreBeginExceptionsFailStablyWithoutExecutorCallbacks) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  for (bool DuringValidation : {true, false}) {
    SCOPED_TRACE(DuringValidation);
    FakeExecutor Fake(Plan);
    detail::ProcessReplayPreBeginHooksForTestingV1 Hooks;
    if (DuringValidation)
      Hooks.BeforeSourceValidation = [] { throw std::bad_alloc(); };
    else
      Hooks.BeforeSnapshotCopy = [] {
        throw std::runtime_error("snapshot exception");
      };

    const ProcessReplayExecutionResultV1 Result =
        detail::executeProcessReplayForTesting(Plan, {}, Fake.operations(),
                                               Hooks);
    EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::SnapshotCopyFailed);
    EXPECT_FALSE(Result.Receipt.Complete);
    EXPECT_FALSE(Result.Receipt.TargetAttested);
    EXPECT_FALSE(Result.Receipt.TargetCallExecuted);
    EXPECT_TRUE(Fake.Calls.empty());
  }
}

TEST(ProcessReplayExecutor,
     AllocationExceptionsFromEveryExecutionCallbackNeverEscape) {
  enum class Stage {
    Begin,
    AuthenticatePinnedTarget,
    PrepareLaunch,
    LaunchStopped,
    ObserveAppliedIsolation,
    AuthenticateLoadedTarget,
    Resume,
    NextEvent,
    ReplyAndResume,
  };
  struct Case {
    Stage At;
    const char *Name;
    AllocationFailureKind Failure;
    bool PossibleLaunch;
  };
  const Case Cases[] = {
      {Stage::Begin, "begin", AllocationFailureKind::BadAlloc, false},
      {Stage::AuthenticatePinnedTarget, "auth-source",
       AllocationFailureKind::LengthError, false},
      {Stage::PrepareLaunch, "prepare", AllocationFailureKind::BadAlloc, false},
      {Stage::LaunchStopped, "launch", AllocationFailureKind::LengthError,
       true},
      {Stage::ObserveAppliedIsolation, "observe",
       AllocationFailureKind::BadAlloc, true},
      {Stage::AuthenticateLoadedTarget, "auth-loaded",
       AllocationFailureKind::LengthError, true},
      {Stage::Resume, "resume", AllocationFailureKind::BadAlloc, true},
      {Stage::NextEvent, "next", AllocationFailureKind::LengthError, true},
      {Stage::ReplyAndResume, "reply", AllocationFailureKind::BadAlloc, true},
  };

  for (const Case &TestCase : Cases) {
    SCOPED_TRACE(TestCase.Name);
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    if (TestCase.At == Stage::ReplyAndResume) {
      Plan.RequiredCapabilities.insert(
          Plan.RequiredCapabilities.begin() + 2,
          ProcessReplayRequiredCapability::EnvironmentLookupInterposition);
      Plan.Environment.push_back({0, {'K'}, true, {'V'}});
      Plan.Events.push_back(environmentEvent(0, 0, 0));
    }
    FakeExecutor Fake(Plan);
    if (TestCase.At == Stage::ReplyAndResume) {
      const auto &Lookup =
          std::get<ProcessReplayEnvironmentLookupEvent>(Plan.Events[0].Payload);
      Fake.Events.push_back(environmentRequest(Lookup.At, {'K'}));
    } else {
      Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
    }
    ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
    unsigned CallbackCalls = 0;

    switch (TestCase.At) {
    case Stage::Begin:
      Operations.Begin = [&]() -> llvm::Error {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::AuthenticatePinnedTarget:
      Operations.AuthenticatePinnedTarget =
          [&]() -> llvm::Expected<ProcessReplayTargetAuthenticationV1> {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::PrepareLaunch:
      Operations.PrepareLaunch =
          [&](const ProcessReplayLaunchRequestV1 &) -> llvm::Error {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::LaunchStopped:
      Operations.LaunchStopped = [&]() -> llvm::Error {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::ObserveAppliedIsolation:
      Operations.ObserveAppliedIsolation =
          [&]() -> llvm::Expected<ProcessReplayExecutorCapabilitiesV1> {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::AuthenticateLoadedTarget:
      Operations.AuthenticateLoadedTarget =
          [&]() -> llvm::Expected<ProcessReplayTargetAuthenticationV1> {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::Resume:
      Operations.Resume = [&]() -> llvm::Error {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::NextEvent:
      Operations.NextEvent = [&](std::chrono::steady_clock::time_point)
          -> llvm::Expected<ProcessReplayBackendEventV1> {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::ReplyAndResume:
      Operations.ReplyAndResume =
          [&](const ProcessReplayInputReplyV1 &) -> llvm::Error {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    }

    ProcessReplayExecutionResultV1 Result;
    EXPECT_NO_THROW(Result = executeProcessReplay(Plan, {}, Operations));
    EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::CallbackException);
    EXPECT_EQ(CallbackCalls, 1u);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "cleanup"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "retain"), 0);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "terminate"),
              TestCase.PossibleLaunch ? 1 : 0);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "reap"),
              TestCase.PossibleLaunch ? 1 : 0);
    EXPECT_EQ(Result.Receipt.Termination.TreeFullyReaped,
              TestCase.PossibleLaunch);
  }
}

TEST(ProcessReplayExecutor, ExecutesTheImplicitTerminalMarkerInExactOrder) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.Arguments.push_back({1, {0xff, 'x'}});
  Plan.Environment.push_back({0, {'K'}, true, {}});
  Plan.Environment.push_back({1, {'M'}, false, {}});
  FakeExecutor Fake(Plan);
  Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
  ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
  Operations.PrepareLaunch =
      [&Fake, &Plan](const ProcessReplayLaunchRequestV1 &Request) {
        Fake.Calls.push_back("prepare");
        Fake.LaunchRequest = Request;
        // executeProcessReplay owns a validated snapshot, so caller mutation
        // during a callback cannot retarget the in-flight execution.
        Plan.TargetOccurrence.Invocation = 99;
        return llvm::Error::success();
      };

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Operations);
  ASSERT_TRUE(Result.succeeded()) << Result.Detail << Result.TeardownDetail;
  EXPECT_EQ(Fake.Calls, (std::vector<std::string>{
                            "begin", "auth-source", "prepare", "launch",
                            "observe", "auth-loaded", "resume", "next",
                            "terminate", "reap", "drain", "cleanup"}));
  ASSERT_TRUE(Fake.LaunchRequest);
  ASSERT_EQ(Fake.LaunchRequest->Arguments.size(), 2u);
  EXPECT_EQ(Fake.LaunchRequest->Arguments[1],
            (std::vector<uint8_t>{0xff, 'x'}));
  ASSERT_EQ(Fake.LaunchRequest->Environment.size(), 1u);
  EXPECT_EQ(Fake.LaunchRequest->Environment[0].Name,
            (std::vector<uint8_t>{'K'}));
  EXPECT_TRUE(Fake.LaunchRequest->Environment[0].Value.empty());
  EXPECT_FALSE(Fake.LaunchRequest->UseShell);
  EXPECT_FALSE(Fake.LaunchRequest->SearchPath);
  EXPECT_TRUE(Fake.LaunchRequest->ClearInheritedEnvironment);
  EXPECT_TRUE(Fake.LaunchRequest->PrivateEmptyWorkingDirectory);
  EXPECT_TRUE(Fake.LaunchRequest->DenyHostFilesystemReadsExceptPinnedTarget);
  EXPECT_TRUE(Fake.LaunchRequest->InheritOnlyStandardDescriptors);
  EXPECT_TRUE(Fake.LaunchRequest->StopBeforeTargetCall);
  EXPECT_TRUE(Fake.LaunchRequest->RequireSelfContainedStaticExecutable);
  EXPECT_EQ(Result.Receipt.Termination.Kind,
            ProcessReplayTerminationKindV1::StoppedBeforeTargetCall);
  EXPECT_TRUE(Result.Receipt.Termination.TreeFullyReaped);
  EXPECT_TRUE(Result.Receipt.SourceIdentityMatched);
  EXPECT_TRUE(Result.Receipt.LoadedIdentityMatched);
  EXPECT_TRUE(Result.Receipt.StableObjectIdentityMatched);
  EXPECT_TRUE(Result.Receipt.TargetAttested);
  EXPECT_FALSE(Result.Receipt.TargetCallExecuted);
}

TEST(ProcessReplayExecutor, ReplaysEnvironmentShortReadsAndStickyEOFExactly) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 2,
      ProcessReplayRequiredCapability::EnvironmentLookupInterposition);
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 3,
      ProcessReplayRequiredCapability::StandardInputInterposition);
  Plan.Environment.push_back({0, {'K'}, true, {'V'}});
  Plan.Resources.push_back({0, ProcessReplayResourceKind::StandardInput});
  Plan.Events.push_back(environmentEvent(0, 0, 0));
  Plan.Events.push_back(stdinEvent(1, 0, 0, 4, {'a', 'b'},
                                   ProcessReplayReadOutcome::Data, false));
  Plan.Events.push_back(
      stdinEvent(2, 1, 2, 4, {'c'}, ProcessReplayReadOutcome::Data, true));
  Plan.Events.push_back(
      stdinEvent(3, 2, 3, 4, {}, ProcessReplayReadOutcome::EndOfFile, true));
  Plan.Events.push_back(
      stdinEvent(4, 3, 3, 4, {}, ProcessReplayReadOutcome::EndOfFile, true));
  Plan.Events.push_back(
      stdinEvent(5, 4, 3, 0, {}, ProcessReplayReadOutcome::ZeroLength, true));
  ASSERT_TRUE(validate(Plan).candidateReady());

  FakeExecutor Fake(Plan);
  const auto &Lookup =
      std::get<ProcessReplayEnvironmentLookupEvent>(Plan.Events[0].Payload);
  Fake.Events.push_back(environmentRequest(Lookup.At, {'K'}));
  for (size_t Index = 1; Index < Plan.Events.size(); ++Index) {
    const auto &Read =
        std::get<ProcessReplayStdinReadEvent>(Plan.Events[Index].Payload);
    Fake.Events.push_back(stdinRequest(Read.At, Read.RequestedBytes));
  }
  Fake.Events.push_back(targetHit(Plan.TargetOccurrence));

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Fake.operations());
  ASSERT_TRUE(Result.succeeded()) << Result.Detail << Result.TeardownDetail;
  ASSERT_EQ(Fake.Replies.size(), 6u);
  const auto &EnvironmentReply =
      std::get<ProcessReplayEnvironmentLookupReplyV1>(Fake.Replies[0].Payload);
  EXPECT_TRUE(EnvironmentReply.Present);
  EXPECT_EQ(EnvironmentReply.Value, (std::vector<uint8_t>{'V'}));

  const auto &FirstRead =
      std::get<ProcessReplayStdinReadReplyV1>(Fake.Replies[1].Payload);
  EXPECT_EQ(FirstRead.ReturnedBytes, 2u);
  EXPECT_EQ(FirstRead.Bytes, (std::vector<uint8_t>{'a', 'b'}));
  EXPECT_FALSE(FirstRead.EOFAfter);
  const auto &SecondRead =
      std::get<ProcessReplayStdinReadReplyV1>(Fake.Replies[2].Payload);
  EXPECT_EQ(SecondRead.ReturnedBytes, 1u);
  EXPECT_TRUE(SecondRead.EOFAfter);
  for (size_t Index : {size_t{3}, size_t{4}}) {
    const auto &EOFReply =
        std::get<ProcessReplayStdinReadReplyV1>(Fake.Replies[Index].Payload);
    EXPECT_EQ(EOFReply.Outcome, ProcessReplayReadOutcome::EndOfFile);
    EXPECT_EQ(EOFReply.ReturnedBytes, 0u);
    EXPECT_TRUE(EOFReply.EOFAfter);
  }
  const auto &ZeroLength =
      std::get<ProcessReplayStdinReadReplyV1>(Fake.Replies[5].Payload);
  EXPECT_EQ(ZeroLength.Outcome, ProcessReplayReadOutcome::ZeroLength);
  EXPECT_TRUE(ZeroLength.EOFAfter);
  EXPECT_EQ(Result.Receipt.MatchedInputEventCount, 6u);
}

TEST(ProcessReplayExecutor, RejectsTargetBeforeInputTranscriptWithoutReply) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 2,
      ProcessReplayRequiredCapability::EnvironmentLookupInterposition);
  Plan.Environment.push_back({0, {'K'}, false, {}});
  Plan.Events.push_back(environmentEvent(0, 0, 0));
  FakeExecutor Fake(Plan);
  Fake.Events.push_back(targetHit(Plan.TargetOccurrence));

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Fake.operations());
  EXPECT_EQ(Result.Reason,
            ProcessReplayExecutionReason::TargetBeforeTranscriptComplete);
  EXPECT_FALSE(Result.Receipt.TargetAttested);
  EXPECT_TRUE(Fake.Replies.empty());
  EXPECT_TRUE(Result.Receipt.Termination.TreeFullyReaped);
}

TEST(ProcessReplayExecutor,
     ResumesLowerTargetInvocationsAndStopsAtTheExactOne) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan(2);
  FakeExecutor Fake(Plan);
  for (uint64_t Invocation = 0; Invocation != 3; ++Invocation) {
    ProcessReplayOccurrence At = Plan.TargetOccurrence;
    At.Invocation = Invocation;
    Fake.Events.push_back(targetHit(At));
  }

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Fake.operations());
  ASSERT_TRUE(Result.succeeded()) << Result.Detail << Result.TeardownDetail;
  EXPECT_EQ(Result.Receipt.TargetSiteVisitsObserved, 3u);
  EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "resume"), 3);
}

TEST(ProcessReplayExecutor,
     RejectsEachIncompleteRuntimeTargetOccurrenceWithoutDereferencingIt) {
  enum class MissingField {
    FuncEntry,
    CallVA,
    BlockId,
    OpIdx,
    OriginSeq,
    CallSiteId,
    Invocation,
  };
  const std::pair<MissingField, const char *> Cases[] = {
      {MissingField::FuncEntry, "func-entry"},
      {MissingField::CallVA, "call-va"},
      {MissingField::BlockId, "block-id"},
      {MissingField::OpIdx, "op-index"},
      {MissingField::OriginSeq, "origin-sequence"},
      {MissingField::CallSiteId, "call-site-id"},
      {MissingField::Invocation, "invocation"},
  };

  for (const auto &[Field, Name] : Cases) {
    SCOPED_TRACE(Name);
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    FakeExecutor Fake(Plan);
    ProcessReplayOccurrence Malformed = Plan.TargetOccurrence;
    switch (Field) {
    case MissingField::FuncEntry:
      Malformed.FuncEntry.reset();
      break;
    case MissingField::CallVA:
      Malformed.CallVA.reset();
      break;
    case MissingField::BlockId:
      Malformed.BlockId.reset();
      break;
    case MissingField::OpIdx:
      Malformed.OpIdx.reset();
      break;
    case MissingField::OriginSeq:
      Malformed.OriginSeq.reset();
      break;
    case MissingField::CallSiteId:
      Malformed.CallSiteId.reset();
      break;
    case MissingField::Invocation:
      Malformed.Invocation.reset();
      break;
    }
    Fake.Events.push_back(targetHit(Malformed));

    ProcessReplayExecutionResultV1 Result;
    EXPECT_NO_THROW(Result = executeProcessReplay(Plan, {}, Fake.operations()));
    EXPECT_EQ(Result.Reason,
              ProcessReplayExecutionReason::TargetOccurrenceMismatch);
    EXPECT_FALSE(Result.Receipt.TargetAttested);
    EXPECT_TRUE(Result.Receipt.Termination.TreeFullyReaped);
  }
}

TEST(ProcessReplayExecutor, RejectsAStdinRequestMismatchWithoutResponding) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 2,
      ProcessReplayRequiredCapability::StandardInputInterposition);
  Plan.Resources.push_back({0, ProcessReplayResourceKind::StandardInput});
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 4, {'x'}, ProcessReplayReadOutcome::Data, false));
  FakeExecutor Fake(Plan);
  const auto &Read =
      std::get<ProcessReplayStdinReadEvent>(Plan.Events[0].Payload);
  Fake.Events.push_back(stdinRequest(Read.At, 5));

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Fake.operations());
  EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::StdinRequestMismatch);
  EXPECT_TRUE(Fake.Replies.empty());
  EXPECT_EQ(Result.Receipt.MatchedInputEventCount, 0u);
}

TEST(ProcessReplayExecutor, RejectsImpossibleMinimumBudgetsBeforeBegin) {
  {
    ProcessReplayPlanCandidateV1 Plan = minimalPlan(2);
    FakeExecutor Fake(Plan);
    ProcessReplayExecutionLimitsV1 Limits;
    Limits.MaxControlEvents = 2;
    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, Limits, Fake.operations());
    EXPECT_EQ(Result.Reason,
              ProcessReplayExecutionReason::InsufficientExecutionBudget);
    EXPECT_TRUE(Fake.Calls.empty());
  }

  {
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    Plan.RequiredCapabilities.insert(
        Plan.RequiredCapabilities.begin() + 2,
        ProcessReplayRequiredCapability::EnvironmentLookupInterposition);
    Plan.Environment.push_back({0, {'K'}, true, {'v', 'a', 'l'}});
    Plan.Events.push_back(environmentEvent(0, 0, 0));
    FakeExecutor Fake(Plan);
    const auto &Lookup =
        std::get<ProcessReplayEnvironmentLookupEvent>(Plan.Events[0].Payload);
    Fake.Events.push_back(environmentRequest(Lookup.At, {'K'}));
    ProcessReplayExecutionLimitsV1 Limits;
    const uint64_t MinimumProtocolBytes =
        kProcessReplayBackendEventProtocolBytes +
        static_cast<uint64_t>(Plan.Environment[0].Name.size()) +
        kProcessReplayEnvironmentReplyProtocolBytes + 3 +
        kProcessReplayBackendEventProtocolBytes +
        kProcessReplayTargetHitProtocolBytes;
    Limits.MaxProtocolBytes = MinimumProtocolBytes - 1;

    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, Limits, Fake.operations());
    EXPECT_EQ(Result.Reason,
              ProcessReplayExecutionReason::InsufficientExecutionBudget);
    EXPECT_TRUE(Fake.Replies.empty());
    EXPECT_TRUE(Fake.Calls.empty());
  }

  {
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    Plan.RequiredCapabilities.insert(
        Plan.RequiredCapabilities.begin() + 2,
        ProcessReplayRequiredCapability::StandardInputInterposition);
    Plan.Resources.push_back({0, ProcessReplayResourceKind::StandardInput});
    Plan.Events.push_back(
        stdinEvent(0, 0, 0, 4, {'x'}, ProcessReplayReadOutcome::Data, false));
    FakeExecutor Fake(Plan);
    const auto &Read =
        std::get<ProcessReplayStdinReadEvent>(Plan.Events[0].Payload);
    Fake.Events.push_back(stdinRequest(Read.At, Read.RequestedBytes));
    ProcessReplayExecutionLimitsV1 Limits;
    constexpr uint64_t MinimumProtocolBytes =
        kProcessReplayBackendEventProtocolBytes +
        kProcessReplayStdinRequestProtocolBytes +
        kProcessReplayStdinReplyProtocolBytes + 1 +
        kProcessReplayBackendEventProtocolBytes +
        kProcessReplayTargetHitProtocolBytes;
    Limits.MaxProtocolBytes = MinimumProtocolBytes - 1;

    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, Limits, Fake.operations());
    EXPECT_EQ(Result.Reason,
              ProcessReplayExecutionReason::InsufficientExecutionBudget);
    EXPECT_TRUE(Fake.Replies.empty());
    EXPECT_TRUE(Fake.Calls.empty());
  }
}

TEST(ProcessReplayExecutor, ChargesReplyPayloadsOnTheDynamicPath) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  Plan.RequiredCapabilities.insert(
      Plan.RequiredCapabilities.begin() + 2,
      ProcessReplayRequiredCapability::StandardInputInterposition);
  Plan.Resources.push_back({0, ProcessReplayResourceKind::StandardInput});
  Plan.Events.push_back(
      stdinEvent(0, 0, 0, 4, {'x'}, ProcessReplayReadOutcome::Data, false));
  FakeExecutor Fake(Plan);
  Fake.Events.push_back(output(ProcessReplayOutputStreamV1::StandardOutput,
                               std::vector<uint8_t>(9, 'o')));
  const auto &Read =
      std::get<ProcessReplayStdinReadEvent>(Plan.Events[0].Payload);
  Fake.Events.push_back(stdinRequest(Read.At, Read.RequestedBytes));

  ProcessReplayExecutionLimitsV1 Limits;
  Limits.MaxProtocolBytes = kProcessReplayBackendEventProtocolBytes +
                            kProcessReplayStdinRequestProtocolBytes +
                            kProcessReplayStdinReplyProtocolBytes + 1 +
                            kProcessReplayBackendEventProtocolBytes +
                            kProcessReplayTargetHitProtocolBytes;
  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, Limits, Fake.operations());
  EXPECT_EQ(Result.Reason,
            ProcessReplayExecutionReason::ProtocolByteLimitExceeded);
  EXPECT_FALSE(Fake.Calls.empty());
  EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "next"), 2);
  EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "reply"), 0);
  EXPECT_TRUE(Fake.Replies.empty());
  EXPECT_EQ(Result.Receipt.MatchedInputEventCount, 0u);
}

TEST(ProcessReplayExecutor, ReauthenticatesTheLoadedObjectBeforeResume) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  Fake.Loaded.ObjectIdentitySHA256 = std::array<uint8_t, 32>{3};
  Fake.Events.push_back(targetHit(Plan.TargetOccurrence));

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Fake.operations());
  EXPECT_EQ(Result.Reason,
            ProcessReplayExecutionReason::LoadedIdentityMismatch);
  EXPECT_FALSE(Result.Receipt.LoadedIdentityMatched);
  EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "resume"), 0);
  EXPECT_EQ(Fake.Calls.back(), "cleanup");
  EXPECT_TRUE(Result.Receipt.Termination.TreeFullyReaped);
}

TEST(ProcessReplayExecutor, RequiresAStaticImageAtBothAuthenticationPoints) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  {
    FakeExecutor Fake(Plan);
    Fake.Source.SelfContainedStaticExecutable = false;
    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, {}, Fake.operations());
    EXPECT_EQ(Result.Reason,
              ProcessReplayExecutionReason::DynamicLoaderUnsupported);
    EXPECT_EQ(Fake.Calls,
              (std::vector<std::string>{"begin", "auth-source", "cleanup"}));
  }

  {
    FakeExecutor Fake(Plan);
    Fake.Loaded.SelfContainedStaticExecutable = false;
    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, {}, Fake.operations());
    EXPECT_EQ(Result.Reason,
              ProcessReplayExecutionReason::DynamicLoaderUnsupported);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "resume"), 0);
    EXPECT_TRUE(Result.Receipt.Termination.TreeFullyReaped);
  }
}

TEST(ProcessReplayExecutor, RechecksHostReadIsolationBeforeResume) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
  Operations.ObserveAppliedIsolation =
      [&Fake]() -> llvm::Expected<ProcessReplayExecutorCapabilitiesV1> {
    Fake.Calls.push_back("observe");
    ProcessReplayExecutorCapabilitiesV1 Applied = Fake.Capabilities;
    Applied.HostFilesystemReadIsolated = false;
    return Applied;
  };

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Operations);
  EXPECT_EQ(Result.Reason,
            ProcessReplayExecutionReason::IsolationReceiptIncomplete);
  EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "auth-loaded"), 0);
  EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "resume"), 0);
  EXPECT_TRUE(Result.Receipt.Termination.TreeFullyReaped);
}

TEST(ProcessReplayExecutor,
     RechecksEveryInputInterpositionBeforeResumeAndFinalizesOnce) {
  enum class MissingInterposition {
    EnvironmentLookup,
    StandardInput,
  };

  for (MissingInterposition Missing : {MissingInterposition::EnvironmentLookup,
                                       MissingInterposition::StandardInput}) {
    SCOPED_TRACE(Missing == MissingInterposition::EnvironmentLookup
                     ? "environment-lookup"
                     : "standard-input");
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    ASSERT_TRUE(Plan.Events.empty());
    FakeExecutor Fake(Plan);
    ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
    Operations.ObserveAppliedIsolation =
        [&Fake,
         Missing]() -> llvm::Expected<ProcessReplayExecutorCapabilitiesV1> {
      Fake.Calls.push_back("observe");
      ProcessReplayExecutorCapabilitiesV1 Applied = Fake.Capabilities;
      if (Missing == MissingInterposition::EnvironmentLookup)
        Applied.EnvironmentLookupInterposition = false;
      else
        Applied.StandardInputInterposition = false;
      return Applied;
    };

    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, {}, Operations);
    EXPECT_EQ(Result.Reason,
              ProcessReplayExecutionReason::IsolationReceiptIncomplete);
    EXPECT_EQ(Result.TeardownReason, ProcessReplayExecutionReason::None);
    EXPECT_FALSE(Result.Receipt.Complete);
    EXPECT_TRUE(Result.Receipt.Termination.TreeFullyReaped);
    EXPECT_EQ(Fake.Calls,
              (std::vector<std::string>{"begin", "auth-source", "prepare",
                                        "launch", "observe", "terminate",
                                        "reap", "drain", "cleanup"}));
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "launch"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "resume"), 0);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "terminate"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "reap"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "drain"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "cleanup"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "retain"), 0);
  }
}

TEST(ProcessReplayExecutor, RecordsExitSignalAndTimeoutBeforeTarget) {
  struct Case {
    ProcessReplayTerminationEventV1 Termination;
    ProcessReplayExecutionReason Reason;
  };
  std::vector<Case> Cases;
  ProcessReplayTerminationEventV1 Exited;
  Exited.Kind = ProcessReplayTerminationKindV1::Exited;
  Exited.ExitCode = 7;
  Cases.push_back({Exited, ProcessReplayExecutionReason::ExitedBeforeTarget});
  ProcessReplayTerminationEventV1 Signaled;
  Signaled.Kind = ProcessReplayTerminationKindV1::Signaled;
  Signaled.Signal = 9;
  Signaled.CoreDumped = true;
  Cases.push_back(
      {Signaled, ProcessReplayExecutionReason::SignaledBeforeTarget});
  ProcessReplayTerminationEventV1 Timeout;
  Timeout.Kind = ProcessReplayTerminationKindV1::WallTimeExceeded;
  Timeout.WallTimeMs = 30'000;
  Cases.push_back({Timeout, ProcessReplayExecutionReason::WallTimeExceeded});

  for (const Case &TestCase : Cases) {
    SCOPED_TRACE(toString(TestCase.Reason));
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    FakeExecutor Fake(Plan);
    ProcessReplayBackendEventV1 Event;
    Event.Payload = TestCase.Termination;
    Fake.Events.push_back(std::move(Event));
    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, {}, Fake.operations());
    EXPECT_EQ(Result.Reason, TestCase.Reason);
    EXPECT_EQ(Result.Receipt.Termination.Kind, TestCase.Termination.Kind);
    EXPECT_EQ(Result.Receipt.Termination.ExitCode,
              TestCase.Termination.ExitCode);
    EXPECT_EQ(Result.Receipt.Termination.Signal, TestCase.Termination.Signal);
    EXPECT_EQ(Result.Receipt.Termination.CoreDumped,
              TestCase.Termination.CoreDumped);
    EXPECT_TRUE(Result.Receipt.Termination.TreeFullyReaped);
  }
}

TEST(ProcessReplayExecutor, DigestsCompleteBoundedOutputAcrossChunkBoundaries) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  Fake.Events.push_back(
      output(ProcessReplayOutputStreamV1::StandardOutput, {'a', 'b'}));
  Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
  Fake.DrainChunks.push_back(
      {ProcessReplayOutputStreamV1::StandardOutput, {'c', 'd'}});
  Fake.DrainChunks.push_back(
      {ProcessReplayOutputStreamV1::StandardError, {'x'}});
  ProcessReplayExecutionLimitsV1 Limits;
  Limits.PreviewBytesPerStream = 3;

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, Limits, Fake.operations());
  ASSERT_TRUE(Result.succeeded()) << Result.Detail << Result.TeardownDetail;
  const ProcessReplayOutputReceiptV1 &Receipt = Result.Receipt.StandardOutput;
  EXPECT_EQ(Receipt.ObservedBytes, 4u);
  EXPECT_EQ(Receipt.DigestedBytes, 4u);
  EXPECT_EQ(Receipt.Preview, (std::vector<uint8_t>{'a', 'b', 'c'}));
  EXPECT_TRUE(Receipt.PreviewTruncated);
  EXPECT_FALSE(Receipt.StreamTruncated);
  EXPECT_EQ(Receipt.DigestScope, ProcessReplayDigestScopeV1::CompleteStream);
  const std::vector<uint8_t> Bytes{'a', 'b', 'c', 'd'};
  EXPECT_EQ(Receipt.SHA256, llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(Bytes)));
  const ProcessReplayOutputReceiptV1 &Stderr = Result.Receipt.StandardError;
  EXPECT_EQ(Stderr.ObservedBytes, 1u);
  EXPECT_EQ(Stderr.Preview, (std::vector<uint8_t>{'x'}));
  EXPECT_FALSE(Stderr.StreamTruncated);
  const std::vector<uint8_t> StderrBytes{'x'};
  EXPECT_EQ(Stderr.SHA256,
            llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(StderrBytes)));
}

TEST(ProcessReplayExecutor, StopsAtTheFirstByteBeyondTheOutputBudget) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  Fake.Events.push_back(output(ProcessReplayOutputStreamV1::StandardOutput,
                               {'a', 'b', 'c', 'd', 'e'}));
  ProcessReplayExecutionLimitsV1 Limits;
  Limits.MaxStdoutBytes = 4;
  Limits.MaxAggregateOutputBytes = 4;
  Limits.PreviewBytesPerStream = 2;

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, Limits, Fake.operations());
  EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::OutputLimitExceeded);
  EXPECT_EQ(Result.Receipt.Termination.Kind,
            ProcessReplayTerminationKindV1::OutputLimitExceeded);
  const ProcessReplayOutputReceiptV1 &Receipt = Result.Receipt.StandardOutput;
  EXPECT_EQ(Receipt.ObservedBytes, 5u);
  EXPECT_EQ(Receipt.DigestedBytes, 4u);
  EXPECT_EQ(Receipt.Preview, (std::vector<uint8_t>{'a', 'b'}));
  EXPECT_TRUE(Receipt.PreviewTruncated);
  EXPECT_TRUE(Receipt.StreamTruncated);
  EXPECT_EQ(Receipt.DigestScope, ProcessReplayDigestScopeV1::ObservedPrefix);
  const std::vector<uint8_t> Prefix{'a', 'b', 'c', 'd'};
  EXPECT_EQ(Receipt.SHA256,
            llvm::SHA256::hash(llvm::ArrayRef<uint8_t>(Prefix)));
}

TEST(ProcessReplayExecutor,
     BackendOutputLimitMakesBothStreamDigestsConservativePrefixes) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  ProcessReplayTerminationEventV1 Termination;
  Termination.Kind = ProcessReplayTerminationKindV1::OutputLimitExceeded;
  ProcessReplayBackendEventV1 Event;
  Event.Payload = Termination;
  Fake.Events.push_back(std::move(Event));

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Fake.operations());
  EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::OutputLimitExceeded);
  EXPECT_EQ(Result.Receipt.Termination.Kind,
            ProcessReplayTerminationKindV1::OutputLimitExceeded);
  for (const ProcessReplayOutputReceiptV1 *Receipt :
       {&Result.Receipt.StandardOutput, &Result.Receipt.StandardError}) {
    EXPECT_TRUE(Receipt->StreamTruncated);
    EXPECT_EQ(Receipt->DigestScope, ProcessReplayDigestScopeV1::ObservedPrefix);
  }
}

TEST(ProcessReplayExecutor,
     RejectedOutputLimitEventsStillMakeBothDigestsConservativePrefixes) {
  struct Case {
    bool Malformed;
    ProcessReplayExecutionReason ExpectedReason;
  };
  const Case Cases[] = {
      {false, ProcessReplayExecutionReason::ProtocolByteLimitExceeded},
      {true, ProcessReplayExecutionReason::BackendProtocolViolation},
  };

  for (const Case &TestCase : Cases) {
    SCOPED_TRACE(TestCase.Malformed ? "malformed" : "protocol-budget");
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    FakeExecutor Fake(Plan);
    ProcessReplayTerminationEventV1 Termination;
    Termination.Kind = ProcessReplayTerminationKindV1::OutputLimitExceeded;
    if (TestCase.Malformed)
      Termination.ExitCode = 0;
    ProcessReplayBackendEventV1 Event;
    Event.Payload = Termination;
    Fake.Events.push_back(std::move(Event));

    ProcessReplayExecutionLimitsV1 Limits;
    if (!TestCase.Malformed)
      Limits.MaxProtocolBytes = kProcessReplayBackendEventProtocolBytes +
                                kProcessReplayTargetHitProtocolBytes;
    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, Limits, Fake.operations());
    EXPECT_EQ(Result.Reason, TestCase.ExpectedReason);
    for (const ProcessReplayOutputReceiptV1 *Receipt :
         {&Result.Receipt.StandardOutput, &Result.Receipt.StandardError}) {
      EXPECT_TRUE(Receipt->StreamTruncated);
      EXPECT_EQ(Receipt->DigestScope,
                ProcessReplayDigestScopeV1::ObservedPrefix);
    }
  }
}

TEST(ProcessReplayExecutor,
     MalformedOutputStreamNeverAuthenticatesCompleteStreams) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  Fake.Events.push_back(
      output(static_cast<ProcessReplayOutputStreamV1>(0xff), {'x'}));

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Fake.operations());
  EXPECT_EQ(Result.Reason,
            ProcessReplayExecutionReason::BackendProtocolViolation);
  EXPECT_FALSE(Result.Receipt.Complete);
  for (const ProcessReplayOutputReceiptV1 *Receipt :
       {&Result.Receipt.StandardOutput, &Result.Receipt.StandardError}) {
    EXPECT_TRUE(Receipt->StreamTruncated);
    EXPECT_EQ(Receipt->DigestScope, ProcessReplayDigestScopeV1::ObservedPrefix);
  }
}

TEST(ProcessReplayExecutor,
     SecurelyDiscardedDrainTerminalNeverAuthenticatesCompleteStreams) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
  Fake.DrainSecurelyDiscarded = true;

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Fake.operations());
  EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::OutputLimitExceeded);
  EXPECT_FALSE(Result.Receipt.Complete);
  for (const ProcessReplayOutputReceiptV1 *Receipt :
       {&Result.Receipt.StandardOutput, &Result.Receipt.StandardError}) {
    EXPECT_TRUE(Receipt->StreamTruncated);
    EXPECT_EQ(Receipt->DigestScope, ProcessReplayDigestScopeV1::ObservedPrefix);
  }
}

TEST(ProcessReplayExecutor, PreservesPrimaryAndTeardownFailuresSeparately) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
  ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
  Operations.TerminateTree = [&Fake] {
    Fake.Calls.push_back("terminate");
    return testError("kill failed");
  };

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Operations);
  EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::None);
  EXPECT_EQ(Result.TeardownReason,
            ProcessReplayExecutionReason::TerminateFailed);
  EXPECT_FALSE(Result.succeeded());
  EXPECT_FALSE(Result.Receipt.Complete);
  EXPECT_TRUE(Result.Receipt.TargetAttested);
  EXPECT_TRUE(Result.Receipt.Termination.TreeFullyReaped);
  EXPECT_EQ(Fake.Calls.back(), "cleanup");
}

TEST(ProcessReplayExecutor, LaunchFailureStillTerminatesReapsAndCleansUp) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
  Operations.LaunchStopped = [&Fake] {
    Fake.Calls.push_back("launch");
    return testError("partial launch");
  };

  const ProcessReplayExecutionResultV1 Result =
      executeProcessReplay(Plan, {}, Operations);
  EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::LaunchFailed);
  EXPECT_EQ(Fake.Calls, (std::vector<std::string>{
                            "begin", "auth-source", "prepare", "launch",
                            "terminate", "reap", "drain", "cleanup"}));
  EXPECT_TRUE(Result.Receipt.Termination.TreeFullyReaped);
}

TEST(ProcessReplayExecutor, RetainsContainmentWheneverTreeCannotBeReaped) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  {
    FakeExecutor Fake(Plan);
    Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
    ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
    Operations.ReapTree = [&Fake] {
      Fake.Calls.push_back("reap");
      return testError("reap failed");
    };
    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, {}, Operations);
    EXPECT_EQ(Result.TeardownReason, ProcessReplayExecutionReason::ReapFailed);
    EXPECT_EQ(Result.ContainmentReason, ProcessReplayExecutionReason::None);
    EXPECT_FALSE(Result.Receipt.Termination.TreeFullyReaped);
    EXPECT_TRUE(Result.Receipt.ContainmentRetained);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "retain"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "cleanup"), 0);
  }

  {
    FakeExecutor Fake(Plan);
    Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
    ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
    Operations.TerminateTree = [&Fake] {
      Fake.Calls.push_back("terminate");
      return testError("terminate failed");
    };
    Operations.ReapTree = [&Fake] {
      Fake.Calls.push_back("reap");
      return testError("reap failed");
    };
    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, {}, Operations);
    EXPECT_EQ(Result.TeardownReason,
              ProcessReplayExecutionReason::TerminateFailed);
    EXPECT_NE(Result.TeardownDetail.find("reap failed"), std::string::npos);
    EXPECT_TRUE(Result.Receipt.ContainmentRetained);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "retain"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "cleanup"), 0);
  }

  {
    FakeExecutor Fake(Plan);
    Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
    ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
    Operations.ReapTree = [&Fake]() -> llvm::Error {
      Fake.Calls.push_back("reap");
      throw std::runtime_error("reap exception");
    };
    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, {}, Operations);
    EXPECT_EQ(Result.TeardownReason, ProcessReplayExecutionReason::ReapFailed);
    EXPECT_TRUE(Result.Receipt.ContainmentRetained);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "retain"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "cleanup"), 0);
  }

  {
    FakeExecutor Fake(Plan);
    Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
    ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
    Operations.TerminateTree = [&Fake]() -> llvm::Error {
      Fake.Calls.push_back("terminate");
      throw std::runtime_error("terminate exception");
    };
    Operations.ReapTree = [&Fake]() -> llvm::Error {
      Fake.Calls.push_back("reap");
      throw std::runtime_error("reap exception");
    };
    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, {}, Operations);
    EXPECT_EQ(Result.TeardownReason,
              ProcessReplayExecutionReason::TerminateFailed);
    EXPECT_NE(Result.TeardownDetail.find("reap callback threw"),
              std::string::npos);
    EXPECT_FALSE(Result.Receipt.Complete);
    EXPECT_FALSE(Result.Receipt.Termination.TreeFullyReaped);
    EXPECT_TRUE(Result.Receipt.ContainmentRetained);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "retain"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "cleanup"), 0);
  }
}

TEST(ProcessReplayExecutor,
     AllocationExceptionsFromEveryTeardownCallbackAreExactlyOnce) {
  enum class Stage {
    TerminateTree,
    ReapTree,
    DrainOutput,
    CleanupAfterReap,
    RetainContainment,
  };
  struct Case {
    Stage At;
    const char *Name;
    AllocationFailureKind Failure;
    ProcessReplayExecutionReason TeardownReason;
  };
  const Case Cases[] = {
      {Stage::TerminateTree, "terminate", AllocationFailureKind::BadAlloc,
       ProcessReplayExecutionReason::TerminateFailed},
      {Stage::ReapTree, "reap", AllocationFailureKind::LengthError,
       ProcessReplayExecutionReason::ReapFailed},
      {Stage::DrainOutput, "drain", AllocationFailureKind::BadAlloc,
       ProcessReplayExecutionReason::OutputDrainFailed},
      {Stage::CleanupAfterReap, "cleanup", AllocationFailureKind::LengthError,
       ProcessReplayExecutionReason::CleanupFailed},
      {Stage::RetainContainment, "retain", AllocationFailureKind::BadAlloc,
       ProcessReplayExecutionReason::ReapFailed},
  };

  for (const Case &TestCase : Cases) {
    SCOPED_TRACE(TestCase.Name);
    ProcessReplayPlanCandidateV1 Plan = minimalPlan();
    FakeExecutor Fake(Plan);
    Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
    ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
    unsigned CallbackCalls = 0;

    switch (TestCase.At) {
    case Stage::TerminateTree:
      Operations.TerminateTree = [&]() -> llvm::Error {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::ReapTree:
      Operations.ReapTree = [&]() -> llvm::Error {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::DrainOutput:
      Operations.DrainOutput = [&](std::chrono::steady_clock::time_point)
          -> llvm::Expected<ProcessReplayOutputDrainResultV1> {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::CleanupAfterReap:
      Operations.CleanupAfterReap = [&]() -> llvm::Error {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    case Stage::RetainContainment:
      Operations.ReapTree = [&Fake] {
        Fake.Calls.push_back("reap");
        return testError("reap failed before retain");
      };
      Operations.RetainContainmentForUnreaped = [&]() -> llvm::Error {
        ++CallbackCalls;
        throwAllocationFailure(TestCase.Failure);
      };
      break;
    }

    ProcessReplayExecutionResultV1 Result;
    EXPECT_NO_THROW(Result = executeProcessReplay(Plan, {}, Operations));
    EXPECT_EQ(Result.Reason, ProcessReplayExecutionReason::None);
    EXPECT_EQ(Result.TeardownReason, TestCase.TeardownReason);
    EXPECT_EQ(CallbackCalls, 1u);

    auto TotalCalls = [&](const char *Name, Stage Replaced) {
      return std::count(Fake.Calls.begin(), Fake.Calls.end(), Name) +
             (TestCase.At == Replaced ? CallbackCalls : 0u);
    };
    EXPECT_EQ(TotalCalls("terminate", Stage::TerminateTree), 1u);
    EXPECT_EQ(TotalCalls("reap", Stage::ReapTree), 1u);

    const bool ReapUncertain = TestCase.At == Stage::ReapTree ||
                               TestCase.At == Stage::RetainContainment;
    EXPECT_EQ(TotalCalls("drain", Stage::DrainOutput), ReapUncertain ? 0u : 1u);
    EXPECT_EQ(TotalCalls("cleanup", Stage::CleanupAfterReap),
              ReapUncertain ? 0u : 1u);
    EXPECT_EQ(TotalCalls("retain", Stage::RetainContainment),
              ReapUncertain ? 1u : 0u);
    EXPECT_EQ(Result.Receipt.Termination.TreeFullyReaped, !ReapUncertain);
    EXPECT_EQ(Result.Receipt.ContainmentRetained,
              TestCase.At == Stage::ReapTree);
    EXPECT_EQ(Result.ContainmentReason,
              TestCase.At == Stage::RetainContainment
                  ? ProcessReplayExecutionReason::ContainmentRetentionFailed
                  : ProcessReplayExecutionReason::None);
  }
}

TEST(ProcessReplayExecutor,
     DiagnosticAllocationFailuresPreserveTypedResultsAndContainment) {
  enum class DiagnosticStage {
    ErrorMaterialization,
    DiagnosticWrite,
  };
  for (DiagnosticStage Stage : {DiagnosticStage::ErrorMaterialization,
                                DiagnosticStage::DiagnosticWrite}) {
    for (AllocationFailureKind Failure : {AllocationFailureKind::BadAlloc,
                                          AllocationFailureKind::LengthError}) {
      SCOPED_TRACE(Stage == DiagnosticStage::ErrorMaterialization
                       ? "error-materialization"
                       : "diagnostic-write");
      SCOPED_TRACE(Failure == AllocationFailureKind::BadAlloc ? "bad-alloc"
                                                              : "length-error");
      ProcessReplayPlanCandidateV1 Plan = minimalPlan();
      FakeExecutor Fake(Plan);
      ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
      if (Stage == DiagnosticStage::ErrorMaterialization) {
        Operations.Begin = [&Fake] {
          Fake.Calls.push_back("begin");
          return testError("begin failed");
        };
      } else {
        Operations.Begin = [&Fake, Failure]() -> llvm::Error {
          Fake.Calls.push_back("begin");
          throwAllocationFailure(Failure);
        };
      }

      detail::ProcessReplayPreBeginHooksForTestingV1 Hooks;
      if (Stage == DiagnosticStage::ErrorMaterialization)
        Hooks.BeforeErrorDetailMaterialization = [Failure] {
          throwAllocationFailure(Failure);
        };
      else
        Hooks.BeforeDiagnosticWrite = [Failure] {
          throwAllocationFailure(Failure);
        };

      ProcessReplayExecutionResultV1 Result;
      EXPECT_NO_THROW(Result = detail::executeProcessReplayForTesting(
                          Plan, {}, Operations, Hooks));
      EXPECT_EQ(Result.Reason,
                Stage == DiagnosticStage::ErrorMaterialization
                    ? ProcessReplayExecutionReason::BeginFailed
                    : ProcessReplayExecutionReason::CallbackException);
      EXPECT_TRUE(Result.Detail.empty());
      EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "cleanup"), 1);
      EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "retain"), 0);
    }
  }

  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  FakeExecutor Fake(Plan);
  Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
  ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
  Operations.ReapTree = [&Fake] {
    Fake.Calls.push_back("reap");
    return testError("reap failed");
  };
  detail::ProcessReplayPreBeginHooksForTestingV1 Hooks;
  Hooks.BeforeErrorDetailMaterialization = [] { throw std::bad_alloc(); };

  ProcessReplayExecutionResultV1 Result;
  EXPECT_NO_THROW(Result = detail::executeProcessReplayForTesting(
                      Plan, {}, Operations, Hooks));
  EXPECT_EQ(Result.TeardownReason, ProcessReplayExecutionReason::ReapFailed);
  EXPECT_TRUE(Result.TeardownDetail.empty());
  EXPECT_TRUE(Result.Receipt.ContainmentRetained);
  EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "retain"), 1);
  EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "cleanup"), 0);
}

TEST(ProcessReplayExecutor, ReportsContainmentRetentionFailureSeparately) {
  ProcessReplayPlanCandidateV1 Plan = minimalPlan();
  for (bool Throw : {false, true}) {
    SCOPED_TRACE(Throw);
    FakeExecutor Fake(Plan);
    Fake.Events.push_back(targetHit(Plan.TargetOccurrence));
    ProcessReplayExecutorOperationsV1 Operations = Fake.operations();
    Operations.ReapTree = [&Fake] {
      Fake.Calls.push_back("reap");
      return testError("reap failed");
    };
    Operations.RetainContainmentForUnreaped = [&Fake, Throw]() -> llvm::Error {
      Fake.Calls.push_back("retain");
      if (Throw)
        throw std::runtime_error("retain exception");
      return testError("retain failed");
    };
    const ProcessReplayExecutionResultV1 Result =
        executeProcessReplay(Plan, {}, Operations);
    EXPECT_EQ(Result.TeardownReason, ProcessReplayExecutionReason::ReapFailed);
    EXPECT_EQ(Result.ContainmentReason,
              ProcessReplayExecutionReason::ContainmentRetentionFailed);
    EXPECT_FALSE(Result.Receipt.ContainmentRetained);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "retain"), 1);
    EXPECT_EQ(std::count(Fake.Calls.begin(), Fake.Calls.end(), "cleanup"), 0);
  }
}

TEST(ProcessReplayExecutor, ReasonStringsAndAdapterAreStable) {
  EXPECT_EQ(kProcessReplayExecutorAdapter,
            std::string_view("process-replay-exec-v1"));
  EXPECT_EQ(kProcessReplayExecutorSchemaVersion, 1u);
  EXPECT_STREQ(toString(ProcessReplayExecutionReason::None), "none");
  EXPECT_STREQ(toString(ProcessReplayExecutionReason::TargetNotPreCall),
               "target_not_precall");
  EXPECT_STREQ(toString(ProcessReplayExecutionReason::OutputLimitExceeded),
               "output_limit_exceeded");
  EXPECT_STREQ(toString(ProcessReplayExecutionReason::DynamicLoaderUnsupported),
               "dynamic_loader_unsupported");
  EXPECT_STREQ(toString(ProcessReplayExecutionReason::SnapshotCopyFailed),
               "snapshot_copy_failed");
  EXPECT_STREQ(
      toString(ProcessReplayExecutionReason::ContainmentRetentionFailed),
      "containment_retention_failed");
  EXPECT_STREQ(
      toString(ProcessReplayExecutionReason::InsufficientExecutionBudget),
      "insufficient_execution_budget");
  EXPECT_STREQ(toString(static_cast<ProcessReplayExecutionReason>(0xffff)),
               "unknown");
}

} // namespace
