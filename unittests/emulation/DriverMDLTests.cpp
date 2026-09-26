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

#include "fixtures/driver_mdl_actions.h"
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
  const bool Direct =
      Action == 19 || Action == MdlAppendDirect || Action == MdlReplaceDirect;
  IO.ControlCode = 0x222000 | (Action << 2) | (Direct ? 2 : 0);
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

TEST(DriverMDL, BuiltPhysicalPagesAreReadableAndOpaqueFieldsRemainReadOnly) {
  auto Result = emulateDriver(mdlFixture(), mdlScenario(9));
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  expectCompleted(*Result);
  expectRejected(24, "process");
  expectRejected(10, "read-only");
}

TEST(DriverMDL, DescriptorAndPoolUseAfterFreeStopAtTheFirstAccess) {
  for (unsigned Action : {11u, 12u, 13u, 14u})
    expectRejected(Action, "freed");
  expectRejected(29, "unknown");
}

TEST(DriverMDL, RequestOwnershipAndUnmodeledAllocationContractsAreRejected) {
  expectRejected(17, "associated IRP");
  expectRejected(18, "ChargeQuota");
  expectRejected(19, "request-owned");
}

TEST(DriverMDL, GuestChainLinksSupportAssociationReplacementAndUnlinking) {
  for (unsigned Action :
       {MdlAttachPrimary, MdlAppendChain, MdlAppendFirst, MdlReplacePrimary,
        MdlUnlinkMiddle, MdlAppendDirect}) {
    SCOPED_TRACE(Action);
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      auto Options = mdlScenario(Action);
      Options.LoadAddress = Address;
      auto Result = emulateDriver(mdlFixture(), Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      expectCompleted(*Result);
      if (Action == MdlAppendDirect) {
        ASSERT_GE(Result->Requests.size(), 2u);
        EXPECT_EQ(Result->Requests[1].Output,
                  (std::vector<uint8_t>{0x62, 0x73}));
      }
    }
  }
}

TEST(DriverMDL, MalformedChainsAndOriginalDirectReplacementFailExplicitly) {
  expectRejected(MdlCyclicChain, "cycle");
  expectRejected(MdlFreeAttached, "freed descriptor");
  expectRejected(MdlReplaceDirect, "original direct-I/O");
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
