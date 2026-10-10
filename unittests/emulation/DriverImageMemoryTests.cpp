//===- DriverImageMemoryTests.cpp - Driver image MDL execution ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <filesystem>
#include <tuple>

namespace neverd::emulation {
namespace {
using Parameter = std::tuple<ExecutionBackendKind, ExecutionContract>;
class DriverImageMemory : public testing::TestWithParam<Parameter> {};

TEST_P(DriverImageMemory, ImageMDLAliasesPreserveBytesAndCompleteUnload) {
  DriverOptions Options;
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
  Probe->CPU.reset();
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
          1)
          << Name;
  }
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
