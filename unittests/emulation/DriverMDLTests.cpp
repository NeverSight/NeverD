//===- DriverMDLTests.cpp - Driver-owned MDL lifetime semantics -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest execution tests for nonpaged pool aliases and descriptor ownership.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {

std::filesystem::path mdlFixture() {
  return std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "driver_mdl.sys";
}

DriverRequest mdlRequest(DriverRequestKind Kind) {
  DriverRequest Value;
  Value.Kind = Kind;
  return Value;
}

DriverOptions mdlScenario(unsigned Action = 0) {
  DriverOptions Options;
  auto Create = mdlRequest(DriverRequestKind::Create);
  Create.Device = "\\Device\\NeverDMDL";
  Options.Requests.push_back(std::move(Create));
  auto IO = mdlRequest(DriverRequestKind::DeviceControl);
  IO.ControlCode = 0x222000 | (Action << 2) | (Action == 19 ? 2 : 0);
  IO.OutputSize = 2;
  Options.Requests.push_back(std::move(IO));
  Options.Requests.push_back(mdlRequest(DriverRequestKind::Cleanup));
  Options.Requests.push_back(mdlRequest(DriverRequestKind::Close));
  Options.Unload = true;
  return Options;
}

void expectCompleted(const DriverResult &Result) {
  EXPECT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_EQ(Result.NTStatus, 0u);
  EXPECT_TRUE(Result.UnloadCompleted) << Result.Diagnostic;
  for (const auto &Request : Result.Requests) {
    EXPECT_TRUE(Request.Completed);
    EXPECT_EQ(Request.DispatchStatus, 0u);
    EXPECT_EQ(Request.IOStatus, 0u);
  }
}

void expectRejected(unsigned Action, const char *Diagnostic) {
  SCOPED_TRACE(Action);
  auto Result = emulateDriver(mdlFixture(), mdlScenario(Action));
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
  EXPECT_NE(Result->Diagnostic.find(Diagnostic), std::string::npos)
      << Result->Diagnostic;
  EXPECT_FALSE(Result->UnloadCompleted);
  if (Action != 20) {
    ASSERT_EQ(Result->Requests.size(), 2u);
    EXPECT_FALSE(Result->Requests[1].Completed);
  }
}

TEST(DriverMDL, NonPageAlignedPoolAliasesSurviveRequestsAndCompleteUnload) {
  auto Options = mdlScenario();
  Options.Requests.insert(Options.Requests.begin() + 2, Options.Requests[1]);
  auto Result = emulateDriver(mdlFixture(), Options);
  ASSERT_TRUE(static_cast<bool>(Result)) << llvm::toString(Result.takeError());
  expectCompleted(*Result);
  ASSERT_EQ(Result->Requests.size(), 5u);
  EXPECT_EQ(Result->Requests[1].Output, (std::vector<uint8_t>{0x41, 0x42}));
  EXPECT_EQ(Result->Requests[2].Output, (std::vector<uint8_t>{0x42, 0x42}));
  EXPECT_EQ(std::count_if(Result->Calls.begin(), Result->Calls.end(),
                          [](const DriverAPIEvent &Call) {
                            return Call.Name == "MmMapLockedPagesSpecifyCache";
                          }),
            0);
}

TEST(DriverMDL, DescriptorAndBackingAllocationHaveIndependentLifetimes) {
  // Descriptor-first and pool-first teardown are both valid when no dead
  // allocation is subsequently accessed. An unbuilt MDL only owns metadata.
  for (unsigned Action : {1u, 2u, 15u, 21u, 31u}) {
    SCOPED_TRACE(Action);
    auto Result = emulateDriver(mdlFixture(), mdlScenario(Action));
    ASSERT_TRUE(static_cast<bool>(Result))
        << llvm::toString(Result.takeError());
    expectCompleted(*Result);
  }
}

TEST(DriverMDL, NonpagedBuildRejectsPagedStackFreedAndOverrunBuffers) {
  for (unsigned Action : {3u, 4u, 5u, 22u, 23u})
    expectRejected(Action, "nonpaged pool");
}

TEST(DriverMDL, AllocationDoesNotBuildAndBuiltDescriptorsCannotBeRebuilt) {
  expectRejected(6, "built MDL");
  expectRejected(27, "unbuilt");
}

TEST(DriverMDL, NonpagedPoolRejectsAdditionalMappingAndUnmapping) {
  expectRejected(7, "additional system-space mapping");
  expectRejected(8, "existing system-space mapping");
}

TEST(DriverMDL, PublicFieldsAreReadOnlyAndPhysicalStateRemainsOpaque) {
  expectRejected(9, "PFN");
  expectRejected(24, "process");
  expectRejected(10, "read-only");
  expectRejected(28, "read-only");
}

TEST(DriverMDL, DescriptorAndPoolUseAfterFreeStopAtTheFirstAccess) {
  for (unsigned Action : {11u, 12u, 13u, 14u})
    expectRejected(Action, "freed");
  expectRejected(29, "unknown");
}

TEST(DriverMDL, RequestOwnershipAndUnmodeledAllocationContractsAreRejected) {
  for (unsigned Action : {16u, 17u, 18u})
    expectRejected(Action, "standalone");
  expectRejected(19, "request-owned");
}

TEST(DriverMDL, InvalidBufferLengthsFailExplicitly) {
  for (unsigned Action : {25u, 26u, 30u})
    expectRejected(Action, "nonempty, nonoverflowing");
}

TEST(DriverMDL, UnloadDetectsLeakedDescriptorEvenAfterBackingPoolIsFreed) {
  expectRejected(20, "live");
}

} // namespace
} // namespace neverd::emulation
