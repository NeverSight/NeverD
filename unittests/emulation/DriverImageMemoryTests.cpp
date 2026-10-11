//===- DriverImageMemoryTests.cpp - Driver image MDL execution ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include "llvm/Support/JSON.h"

#include <algorithm>
#include <filesystem>
#include <tuple>

namespace neverd::emulation {
namespace {
using Parameter = std::tuple<ExecutionBackendKind, ExecutionContract>;
class DriverImageMemory : public testing::TestWithParam<Parameter> {
protected:
  DriverOptions Options;
  void SetUp() override {
    std::tie(Options.Backend, Options.Contract) = GetParam();
    auto Probe = createExecutionBackend(Options.Backend, Options.Contract,
                                        Options.MemoryLimit);
    if (!Probe) {
      auto E = Probe.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Text = llvm::toString(std::move(E));
      if (Unavailable)
        GTEST_SKIP() << Text;
      FAIL() << Text;
    }
  }
};

TEST_P(DriverImageMemory, ImageMDLAliasesPreserveBytesAndCompleteUnload) {
  const auto Path =
      std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "driver_mdl.sys";
  ASSERT_TRUE(std::filesystem::is_regular_file(Path));
  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Create.Device = "\\Device\\NeverDMDL";
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = 0x222008;
  IO.OutputSize = 2;
  DriverRequest Cleanup;
  Cleanup.Kind = DriverRequestKind::Cleanup;
  DriverRequest Close;
  Close.Kind = DriverRequestKind::Close;
  Options.Requests = {Create, IO, Cleanup, Close};
  Options.Unload = true;
  // The compiled fixture checks both ends of the shared image view. Its
  // independent aliases must retain identical bytes after relocation too.
  for (uint64_t Base : {uint64_t(0), uint64_t(0x190000000)}) {
    SCOPED_TRACE(Base);
    Options.LoadAddress = Base;
    auto Run = emulateDriver(Path, Options);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->NTStatus, 0u);
    EXPECT_TRUE(Run->UnloadCompleted) << Run->Diagnostic;
    ASSERT_EQ(Run->Requests.size(), 4u);
    for (const auto &Request : Run->Requests) {
      EXPECT_TRUE(Request.Completed);
      EXPECT_EQ(Request.DispatchStatus, 0u);
      EXPECT_EQ(Request.IOStatus, 0u);
    }
    for (const auto *Name :
         {"MmProbeAndLockPages", "MmMapLockedPagesSpecifyCache",
          "MmUnmapLockedPages", "MmUnlockPages"})
      EXPECT_EQ(
          std::count_if(Run->Calls.begin(), Run->Calls.end(),
                        [&](const auto &Call) { return Call.Name == Name; }),
          2)
          << Name;
  }
}

TEST_P(DriverImageMemory, WriteTraceSelectionPreservesEffectsFaultsAndBudgets) {
  const std::filesystem::path Fixtures(NEVERD_DRIVER_FIXTURES);
  for (const auto *Name : {"success.sys", "devices.sys", "fault.sys",
                           "readonly.sys", "unknown.sys"}) {
    SCOPED_TRACE(Name);
    auto Traced = emulateDriver(Fixtures / Name, Options);
    ASSERT_TRUE(bool(Traced)) << llvm::toString(Traced.takeError());
    auto QuietOptions = Options;
    QuietOptions.TraceMemoryWrites = false;
    auto Quiet = emulateDriver(Fixtures / Name, QuietOptions);
    ASSERT_TRUE(bool(Quiet)) << llvm::toString(Quiet.takeError());
    EXPECT_TRUE(Quiet->Writes.empty());
    if (std::string(Name) == "success.sys")
      EXPECT_FALSE(Traced->Writes.empty());
    // Apart from the selected trace, every reported effect and stop must agree.
    auto Expected = llvm::json::parse(driverResultJSON(*Traced));
    auto Actual = llvm::json::parse(driverResultJSON(*Quiet));
    ASSERT_TRUE(bool(Expected));
    ASSERT_TRUE(bool(Actual));
    (*Expected->getAsObject())["writes"] = llvm::json::Array{};
    (*Expected->getAsObject()->getObject(
        "configuration"))["trace_memory_writes"] = false;
    EXPECT_EQ(*Actual, *Expected);
  }

  Options.TraceMemoryWrites = false;
  auto Limited = Options;
  Limited.EventLimit = 1;
  auto Events = emulateDriver(Fixtures / "devices.sys", Limited);
  ASSERT_TRUE(bool(Events)) << llvm::toString(Events.takeError());
  EXPECT_EQ(Events->Stop, DriverStopReason::EventLimit) << Events->Diagnostic;
  EXPECT_EQ(Events->Calls.size(), 1u);
  EXPECT_TRUE(Events->Writes.empty());
  Limited = Options;
  Limited.InstructionLimit = 1;
  auto Instructions = emulateDriver(Fixtures / "success.sys", Limited);
  ASSERT_TRUE(bool(Instructions)) << llvm::toString(Instructions.takeError());
  EXPECT_EQ(Instructions->Stop, DriverStopReason::InstructionLimit)
      << Instructions->Diagnostic;
  EXPECT_TRUE(Instructions->Writes.empty());

  DriverRequest Create;
  Create.Kind = DriverRequestKind::Create;
  Create.Device = "\\Device\\NeverDMDL";
  DriverRequest IO;
  IO.Kind = DriverRequestKind::DeviceControl;
  IO.ControlCode = 0x222028;
  IO.OutputSize = 2;
  Options.Requests = {Create, IO};
  // The MDL is in CPU-writable RAM, but its ByteCount is owned by the model.
  // Turning off retention must not bypass that logical write guard.
  auto Guarded = emulateDriver(Fixtures / "driver_mdl.sys", Options);
  ASSERT_TRUE(bool(Guarded)) << llvm::toString(Guarded.takeError());
  EXPECT_EQ(Guarded->Stop, DriverStopReason::ModelError) << Guarded->Diagnostic;
  EXPECT_TRUE(Guarded->Writes.empty());
  EXPECT_EQ(Guarded->Phase, "request:1");
}

std::string parameterName(const testing::TestParamInfo<Parameter> &P) {
  const auto &[Backend, Contract] = P.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "_driver" : "_checked");
}
INSTANTIATE_TEST_SUITE_P(
    Native, DriverImageMemory,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64)),
    parameterName);
} // namespace
} // namespace neverd::emulation
