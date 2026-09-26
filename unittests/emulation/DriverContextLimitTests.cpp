//===- DriverContextLimitTests.cpp - Callback execution limits ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify IRQL, resident-memory and isolated-stack checks in compiled drivers.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#define NEVERD_CONTEXT_LIMIT_FAILURE(Name, Code, Diagnostic, API)              \
  constexpr uint32_t Name = Code;
#define NEVERD_CONTEXT_LIMIT_SUCCESS(Name, Code) constexpr uint32_t Name = Code;
#define NEVERD_CONTEXT_LIMIT_STACK(Name, Code) constexpr uint32_t Name = Code;
#define NEVERD_CONTEXT_LIMIT_STORAGE(Name, Code) constexpr uint32_t Name = Code;
#define NEVERD_CONTEXT_LIMIT_ENTRY(Name, Marker, Service)                      \
  constexpr char Name[] = Service;
#include "fixtures/DriverContextLimitCases.def"
#undef NEVERD_CONTEXT_LIMIT_ENTRY
#undef NEVERD_CONTEXT_LIMIT_STORAGE
#undef NEVERD_CONTEXT_LIMIT_STACK
#undef NEVERD_CONTEXT_LIMIT_SUCCESS
#undef NEVERD_CONTEXT_LIMIT_FAILURE

DriverOptions scenario(uint32_t Code) {
  DriverOptions Options;
  Options.Registry = std::vector<DriverRegistryKey>{
      {"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\NeverDDriver",
       {}}};
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Options.Requests.push_back(Create);
  DriverRequest IO;
  IO.ControlCode = Code;
  IO.OutputSize = 4;
  Options.Requests.push_back(IO);
  DriverRequest Cleanup;
  Cleanup.Kind = DriverRequestKind::Cleanup;
  Options.Requests.push_back(Cleanup);
  DriverRequest Close;
  Close.Kind = DriverRequestKind::Close;
  Options.Requests.push_back(Close);
  Options.Unload = true;
  return Options;
}

bool observedDPC(const DriverResult &Result) {
  return std::any_of(
      Result.Calls.begin(), Result.Calls.end(), [](const auto &Call) {
        return Call.Name == "KeGetCurrentIrql" &&
               Call.Phase.starts_with("callback:") && Call.Result == 2u;
      });
}

struct FailureCase {
  uint32_t Code;
  const char *Diagnostic;
  const char *API;
};

constexpr FailureCase FailureCases[] = {
#define NEVERD_CONTEXT_LIMIT_FAILURE(Name, Code, Diagnostic, API)              \
  {Name, Diagnostic, API},
#define NEVERD_CONTEXT_LIMIT_SUCCESS(Name, Code)
#define NEVERD_CONTEXT_LIMIT_STACK(Name, Code)
#define NEVERD_CONTEXT_LIMIT_STORAGE(Name, Code)
#define NEVERD_CONTEXT_LIMIT_ENTRY(Name, Marker, Service)
#include "fixtures/DriverContextLimitCases.def"
#undef NEVERD_CONTEXT_LIMIT_ENTRY
#undef NEVERD_CONTEXT_LIMIT_STORAGE
#undef NEVERD_CONTEXT_LIMIT_STACK
#undef NEVERD_CONTEXT_LIMIT_SUCCESS
#undef NEVERD_CONTEXT_LIMIT_FAILURE
};

class DriverContextFailure : public ::testing::TestWithParam<FailureCase> {};

TEST_P(DriverContextFailure, DPCRejectsContextViolationBeforeCompletion) {
  const auto Case = GetParam();
  auto Result = emulateDriver(
      NEVERD_DRIVER_FIXTURES "/driver_context_limits.sys", scenario(Case.Code));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE(observedDPC(*Result));
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find(Case.Diagnostic), std::string::npos)
      << Result->Diagnostic;
  EXPECT_TRUE(Result->Phase.starts_with("callback:"));
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_FALSE(Result->UnloadCompleted);
  if (*Case.API) {
    const auto FailedCall = std::find_if(
        Result->Calls.begin(), Result->Calls.end(), [&](const auto &Call) {
          return Call.Name == Case.API && Call.Phase.starts_with("callback:");
        });
    ASSERT_NE(FailedCall, Result->Calls.end());
    EXPECT_FALSE(FailedCall->Result);
  }
  if (Case.Code == RegistryAtDpc) {
    EXPECT_TRUE(
        std::none_of(Result->Calls.begin(), Result->Calls.end(),
                     [](const auto &Call) { return Call.Name == "ZwClose"; }));
    ASSERT_TRUE(Result->Registry);
    const auto Key = std::find_if(
        Result->Registry->begin(), Result->Registry->end(), [](const auto &K) {
          return K.Path.ends_with("\\Services\\NeverDDriver");
        });
    ASSERT_NE(Key, Result->Registry->end());
    EXPECT_TRUE(Key->Values.empty());
  }
  if (Case.Code == UnicodePrintAtDpc)
    EXPECT_TRUE(Result->Messages.empty());
}

INSTANTIATE_TEST_SUITE_P(CompiledGuest, DriverContextFailure,
                         ::testing::ValuesIn(FailureCases));

TEST(DriverContextLimits, NonpagedMemoryAndAnsiDebugRemainUsableAtDpc) {
  for (uint32_t Code : {NonpagedAtDpc, AnsiPrintAtDpc}) {
    SCOPED_TRACE(Code);
    auto Result = emulateDriver(
        NEVERD_DRIVER_FIXTURES "/driver_context_limits.sys", scenario(Code));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_TRUE(observedDPC(*Result));
    ASSERT_EQ(Result->Requests.size(), 4u);
    EXPECT_TRUE(Result->Requests[1].Completed);
    EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
    EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[1].Output,
              (std::vector<uint8_t>{2, 'O', 'K', 0xa5}));
    EXPECT_TRUE(Result->UnloadCompleted);
    EXPECT_TRUE(Result->Devices.empty());
    if (Code == NonpagedAtDpc)
      EXPECT_EQ(std::count_if(Result->Calls.begin(), Result->Calls.end(),
                              [](const auto &Call) {
                                return Call.Name == "ExFreePoolWithTag" &&
                                       Call.Phase.starts_with("callback:");
                              }),
                2);
    else
      EXPECT_EQ(Result->Messages,
                (std::vector<std::string>{"context ANSI IRQL=2\n"}));
  }
}

TEST(DriverContextLimits, StackLeapStopsBeforeTouchingSuspendedWorker) {
  auto Result =
      emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_context_limits.sys",
                    scenario(WorkerStackEscape));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_TRUE(Result->Stop == DriverStopReason::ModelError ||
              Result->Stop == DriverStopReason::MemoryFault)
      << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find("stack"), std::string::npos)
      << Result->Diagnostic;
  EXPECT_TRUE(Result->Phase.starts_with("callback:"));
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
  EXPECT_TRUE(std::any_of(
      Result->Calls.begin(), Result->Calls.end(), [](const auto &Call) {
        return Call.Name == "KeWaitForSingleObject" &&
               Call.Phase.starts_with("callback:") && !Call.Result;
      }));
  EXPECT_TRUE(std::none_of(
      Result->Calls.begin(), Result->Calls.end(), [](const auto &Call) {
        return Call.Name == "KeSetEvent" || Call.Name == "IoFreeWorkItem";
      }));
  EXPECT_TRUE(Result->Messages.empty());
  EXPECT_FALSE(Result->UnloadCompleted);
}

TEST(DriverContextLimits, DeviceExtensionTimerPreventsImmediateDeviceDeletion) {
  DriverOptions Options;
  Options.ServiceName = DeleteDeviceArmedTimer;
  auto Result = emulateDriver(
      NEVERD_DRIVER_FIXTURES "/driver_context_limits.sys", Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find("timer"), std::string::npos)
      << Result->Diagnostic;
  EXPECT_TRUE(Result->Requests.empty());
  EXPECT_FALSE(Result->UnloadCompleted);
  ASSERT_EQ(Result->Devices.size(), 1u);
  EXPECT_NE(Result->Devices.front().Extension, 0u);
  ASSERT_FALSE(Result->Calls.empty());
  EXPECT_EQ(Result->Calls.back().Name, "IoDeleteDevice");
  EXPECT_FALSE(Result->Calls.back().Result);
  EXPECT_TRUE(std::any_of(
      Result->Calls.begin(), Result->Calls.end(), [](const auto &Call) {
        return Call.Name == "KeSetTimer" && Call.Result == 0u;
      }));
}

TEST(DriverContextLimits, QueuedDpcPreventsDirectMdlUnmapping) {
  auto Options = scenario(MdlUnmapQueuedDpc);
  Options.Requests[1].OutputSize = 128;
  auto Result = emulateDriver(
      NEVERD_DRIVER_FIXTURES "/driver_context_limits.sys", Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find("queued"), std::string::npos)
      << Result->Diagnostic;
  EXPECT_EQ(Result->Phase, "request:1");
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_FALSE(Result->Requests[1].DispatchStatus);
  ASSERT_FALSE(Result->Calls.empty());
  EXPECT_EQ(Result->Calls.back().Name, "MmUnmapLockedPages");
  EXPECT_FALSE(Result->Calls.back().Result);
  const auto Mapping = std::find_if(
      Result->Calls.begin(), Result->Calls.end(), [](const auto &Call) {
        return Call.Name == "MmMapLockedPagesSpecifyCache";
      });
  ASSERT_NE(Mapping, Result->Calls.end());
  ASSERT_TRUE(Mapping->Result);
  ASSERT_EQ(Result->Calls.back().Arguments.size(), 2u);
  EXPECT_EQ(Result->Calls.back().Arguments[0], *Mapping->Result);
  EXPECT_TRUE(std::any_of(
      Result->Calls.begin(), Result->Calls.end(), [&](const auto &Call) {
        return Call.Name == "KeInsertQueueDpc" && Call.Result == 1u &&
               Call.Arguments[0] == *Mapping->Result;
      }));
  EXPECT_TRUE(Result->Messages.empty());
  EXPECT_FALSE(Result->UnloadCompleted);
}

TEST(DriverContextLimits, QueuedDpcPreventsBorrowedRegistryPathRetirement) {
  DriverOptions Options;
  Options.ServiceName = RegistryPathQueuedDpc;
  auto Result = emulateDriver(
      NEVERD_DRIVER_FIXTURES "/driver_context_limits.sys", Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find("queued"), std::string::npos)
      << Result->Diagnostic;
  EXPECT_EQ(Result->NTStatus, 0u);
  EXPECT_EQ(Result->Phase, "driver_entry");
  EXPECT_TRUE(Result->Requests.empty());
  ASSERT_EQ(Result->Calls.size(), 2u);
  EXPECT_EQ(Result->Calls[0].Name, "KeInitializeDpc");
  EXPECT_EQ(Result->Calls[0].Result, 0u);
  EXPECT_EQ(Result->Calls[1].Name, "KeInsertQueueDpc");
  EXPECT_EQ(Result->Calls[1].Result, 1u);
  EXPECT_TRUE(Result->Messages.empty());
  EXPECT_FALSE(Result->UnloadCompleted);
}

TEST(DriverContextLimits, DpcInitializationCannotReplaceWaitedEvent) {
  auto Result =
      emulateDriver(NEVERD_DRIVER_FIXTURES "/driver_context_limits.sys",
                    scenario(ReplaceWaitedEvent));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find("outstanding waits"), std::string::npos)
      << Result->Diagnostic;
  EXPECT_TRUE(Result->Phase.starts_with("callback:"));
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
  const auto Wait = std::find_if(
      Result->Calls.begin(), Result->Calls.end(),
      [](const auto &Call) { return Call.Name == "KeWaitForSingleObject"; });
  ASSERT_NE(Wait, Result->Calls.end());
  EXPECT_FALSE(Wait->Result);
  ASSERT_FALSE(Result->Calls.empty());
  EXPECT_EQ(Result->Calls.back().Name, "KeInitializeDpc");
  EXPECT_FALSE(Result->Calls.back().Result);
  EXPECT_NE(Result->Calls.back().Detail.find("outstanding waits"),
            std::string::npos);
  EXPECT_EQ(Wait->Arguments[0], Result->Calls.back().Arguments[0]);
  EXPECT_TRUE(Result->Messages.empty());
  EXPECT_FALSE(Result->UnloadCompleted);
}
} // namespace
} // namespace neverd::emulation
