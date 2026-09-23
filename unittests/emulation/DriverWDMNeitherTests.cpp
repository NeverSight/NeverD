//===- DriverWDMNeitherTests.cpp - Genuine WDK user-buffer execution -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute actual WDK packet, probe, CPU exception and user MDL paths.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "neverd/emulation/DriverSession.h"

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_NEITHER_FIXTURE
DriverOptions options(uint32_t Code, uint64_t Address) {
  DriverOptions Options;
  Options.LoadAddress = Address;
  Options.Unload = true;
  auto Request = [&](DriverRequestKind Kind) {
    DriverRequest Item;
    Item.Kind = Kind;
    Item.Device = "\\Device\\NeverDNeither";
    Options.Requests.push_back(std::move(Item));
  };
  Request(DriverRequestKind::Create);
  Request(DriverRequestKind::DeviceControl);
  auto &IO = Options.Requests.back();
  IO.ControlCode = Code;
  IO.Input = {1, 2, 3, 4};
  IO.OutputSize = 4;
  Request(DriverRequestKind::Cleanup);
  Request(DriverRequestKind::Close);
  return Options;
}

TEST(DriverWDMNeither, OriginalWDKHandlersObserveRealUserBytesAndFaultSites) {
  const std::pair<uint32_t, std::vector<uint8_t>> Cases[]{
      {0x222003, {0x5b, 0x58, 0x59, 0x5e}},
      {0x222007, {0xa1}},
      {0x22200b, {4}},
      {0x22200f, {2}},
      {0x222013, {2, 3, 4, 5}},
      {0x222017, {0xa6}}};
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       })
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (const auto &[Code, Output] : Cases) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Address);
        SCOPED_TRACE(Code);
        auto Result = emulateDriver(Image, options(Code, Address));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), 4u);
        EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
        EXPECT_EQ(Result->Requests[1].Output, Output);
        EXPECT_TRUE(Result->Requests[1].Completed);
        EXPECT_TRUE(Result->UnloadCompleted);
        EXPECT_FALSE(Result->Fault);
      }
}

TEST(DriverWDMNeither, UserPageFactsReachActualProbeLockAndCpuFaultPaths) {
  struct Case {
    uint32_t Code;
    bool Input;
    DriverUserPageAccess Access;
  };
  const Case Cases[]{
      {0x222003, true, DriverUserPageAccess::NoAccess},
      {0x222003, false, DriverUserPageAccess::ReadOnly},
      {0x222013, true, DriverUserPageAccess::NoAccess},
      {0x222013, false, DriverUserPageAccess::ReadOnly},
  };
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       })
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (const Case &Case : Cases) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Address);
        SCOPED_TRACE(Case.Code);
        auto Options = options(Case.Code, Address);
        auto &Request = Options.Requests[1];
        (Case.Input ? Request.UserInputAccess : Request.UserOutputAccess) =
            Case.Access;
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        EXPECT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_GE(Result->Requests.size(), 2u);
        EXPECT_EQ(Result->Requests[1].IOStatus, 0xc0000005u);
        EXPECT_TRUE(Result->Requests[1].Completed);
        EXPECT_FALSE(Result->Fault);
      }
}

TEST(DriverWDMNeither, NeitherReadWriteUsesRawCallerBufferAndPageRights) {
  struct Case {
    DriverRequestKind Kind;
    std::optional<DriverUserPageAccess> Access;
    uint32_t Status;
    std::vector<uint8_t> Output;
  };
  const Case Cases[]{
      {DriverRequestKind::Read, std::nullopt, 0, {0x70, 0x71, 0x72, 0x73}},
      {DriverRequestKind::Write, std::nullopt, 0, {}},
      {DriverRequestKind::Read,
       DriverUserPageAccess::ReadOnly,
       0xc0000005u,
       {}},
      {DriverRequestKind::Write,
       DriverUserPageAccess::NoAccess,
       0xc0000005u,
       {}},
  };
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       })
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (const Case &Case : Cases) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Address);
        SCOPED_TRACE(static_cast<unsigned>(Case.Kind));
        auto Options = options(0x222003, Address);
        auto &Request = Options.Requests[1];
        Request.Kind = Case.Kind;
        Request.ControlCode = 0;
        if (Case.Kind == DriverRequestKind::Read) {
          Request.Input.clear();
          Request.UserOutputAccess = Case.Access;
        } else {
          Request.OutputSize = 0;
          Request.UserInputAccess = Case.Access;
        }
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), 4u);
        EXPECT_EQ(Result->Requests[1].IOStatus, Case.Status);
        EXPECT_EQ(Result->Requests[1].Output, Case.Output);
        EXPECT_EQ(Result->Requests[1].Information, Case.Status ? 0u : 4u);
        EXPECT_TRUE(Result->Requests[1].Completed);
        EXPECT_TRUE(Result->UnloadCompleted);
        EXPECT_FALSE(Result->Fault);
      }
}
#endif
} // namespace
} // namespace neverd::emulation
