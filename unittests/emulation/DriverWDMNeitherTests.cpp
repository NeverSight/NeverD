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

TEST(DriverWDMNeither, LockedUserAliasesRemainUsableInPendingWorker) {
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       })
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Result = emulateDriver(Image, options(0x22201b, Address));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), 4u);
      EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
      EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
      EXPECT_EQ(Result->Requests[1].Output,
                (std::vector<uint8_t>{0x11, 0x12, 0x13, 0x14}));
      EXPECT_TRUE(Result->Requests[1].Completed);
      EXPECT_TRUE(Result->UnloadCompleted);
      EXPECT_FALSE(Result->Fault);
    }
}

TEST(DriverWDMNeither, RevokedCallerAddressesKeepPendingMdlAliasesAlive) {
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       })
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = options(0x22201b, Address);
      Options.Requests[1].UserUnmapAfterDispatch = true;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), 4u);
      EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
      EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
      EXPECT_EQ(Result->Requests[1].Information, 4u);
      EXPECT_TRUE(Result->Requests[1].Output.empty());
      EXPECT_TRUE(Result->Requests[1].Completed);
      EXPECT_TRUE(Result->UnloadCompleted);
      EXPECT_FALSE(Result->Fault);
    }
}

TEST(DriverWDMNeither, ExitedRequestorKeepsLockedWorkerAndCancelAlive) {
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       })
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (uint32_t Code : {0x22201bu, 0x222023u}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Address);
        SCOPED_TRACE(Code);
        auto Options = options(Code, Address);
        for (auto &Request : Options.Requests)
          Request.RequestorProcessID = 0x1010;
        Options.Requests[1].RequestorExitAfterDispatch = true;
        if (Code == 0x222023)
          Options.Requests[1].CancelAfter100ns = 0;
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        EXPECT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), 4u);
        EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
        EXPECT_EQ(Result->Requests[1].IOStatus,
                  Code == 0x222023 ? 0xc0000120u : 0u);
        EXPECT_TRUE(Result->Requests[1].Output.empty());
        EXPECT_TRUE(Result->Requests[1].Completed);
        EXPECT_TRUE(Result->UnloadCompleted);
        EXPECT_FALSE(Result->Fault);
      }
}

TEST(DriverWDMNeither, AnotherRequestorRunsBeforeExitedRequestorsWorker) {
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       })
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = options(0x22201b, Address);
      for (auto &Request : Options.Requests)
        Request.RequestorProcessID = 0x1010;
      auto &FirstIO = Options.Requests[1];
      FirstIO.DeferCallbackDrain = true;
      FirstIO.RequestorExitAfterDispatch = true;
      DriverRequest SecondCreate = Options.Requests[0];
      SecondCreate.File = 1;
      SecondCreate.RequestorProcessID = 0x2020;
      Options.Requests.insert(Options.Requests.begin() + 1, SecondCreate);
      DriverRequest SecondIO = Options.Requests[2];
      SecondIO.File = 1;
      SecondIO.RequestorProcessID = 0x2020;
      SecondIO.ControlCode = 0x222003;
      SecondIO.DeferCallbackDrain = false;
      SecondIO.RequestorExitAfterDispatch = false;
      Options.Requests.insert(Options.Requests.begin() + 3, SecondIO);
      for (size_t I : {4u, 5u}) {
        DriverRequest Request = Options.Requests[I];
        Request.File = 1;
        Request.RequestorProcessID = 0x2020;
        Options.Requests.push_back(Request);
      }
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), 8u);
      EXPECT_EQ(Result->Requests[2].DispatchStatus, 0x103u);
      EXPECT_EQ(Result->Requests[2].IOStatus, 0u);
      EXPECT_EQ(Result->Requests[2].Information, 4u);
      EXPECT_TRUE(Result->Requests[2].Output.empty());
      EXPECT_EQ(Result->Requests[3].IOStatus, 0u);
      EXPECT_EQ(Result->Requests[3].Output,
                (std::vector<uint8_t>{0x5b, 0x58, 0x59, 0x5e}));
      EXPECT_TRUE(Result->UnloadCompleted);
      EXPECT_FALSE(Result->Fault);
    }
}

TEST(DriverWDMNeither, RawCallerAddressInPendingWorkerStopsExplicitly) {
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       })
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = options(0x22201f, Address);
      Options.Requests.resize(2);
      Options.Unload = false;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
      EXPECT_NE(Result->Diagnostic.find("user address access requires the "
                                        "requesting process"),
                std::string::npos);
      ASSERT_EQ(Result->Requests.size(), 2u);
      EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
      EXPECT_FALSE(Result->Requests[1].Completed);
      EXPECT_FALSE(Result->Fault);
    }
}

TEST(DriverWDMNeither, RevokedRawCallerAddressStopsPendingWorker) {
  auto Options = options(0x22201f, 0x180000000ULL);
  Options.Requests[1].UserUnmapAfterDispatch = true;
  Options.Requests.resize(2);
  Options.Unload = false;
  auto Result = emulateDriver(NEVERD_WDM_NEITHER_FIXTURE, Options);
  ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
  EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
  EXPECT_NE(Result->Diagnostic.find("unmapped user virtual address"),
            std::string::npos)
      << Result->Diagnostic;
  ASSERT_EQ(Result->Requests.size(), 2u);
  EXPECT_FALSE(Result->Requests[1].Completed);
  ASSERT_TRUE(Result->Fault);
  EXPECT_EQ(Result->Fault->Kind, "protection");
}

TEST(DriverWDMNeither, CancelRoutineOwnsPendingLockedBufferCompletion) {
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       })
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Address);
      auto Options = options(0x222023, Address);
      Options.Requests[1].CancelAfter100ns = 0;
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), 4u);
      EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
      EXPECT_EQ(Result->Requests[1].IOStatus, 0xc0000120u);
      EXPECT_EQ(Result->Requests[1].Information, 0u);
      EXPECT_TRUE(Result->Requests[1].Output.empty());
      EXPECT_TRUE(Result->Requests[1].CancelRequestedAt100ns);
      EXPECT_TRUE(Result->Requests[1].Completed);
      EXPECT_TRUE(Result->UnloadCompleted);
      EXPECT_FALSE(Result->Fault);
    }
}

TEST(DriverWDMNeither, RevokedCallerAddressesCanStillCancelLockedIRP) {
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    auto Options = options(0x222023, 0x180000000ULL);
    Options.Requests[1].UserUnmapAfterDispatch = true;
    Options.Requests[1].CancelAfter100ns = 0;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->Requests.size(), 4u);
    EXPECT_EQ(Result->Requests[1].IOStatus, 0xc0000120u);
    EXPECT_TRUE(Result->Requests[1].CancelRequestedAt100ns);
    EXPECT_TRUE(Result->Requests[1].Completed);
    EXPECT_TRUE(Result->UnloadCompleted);
    EXPECT_FALSE(Result->Fault);
  }
}

TEST(DriverWDMNeither, CompletionBeforeDeadlineDisarmsCancellation) {
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    auto Options = options(0x222023, 0x180000000ULL);
    Options.Requests[1].CancelAfter100ns = 100;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    ASSERT_EQ(Result->Requests.size(), 4u);
    EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
    EXPECT_EQ(Result->Requests[1].Output,
              (std::vector<uint8_t>{0x11, 0x12, 0x13, 0x14}));
    EXPECT_FALSE(Result->Requests[1].CancelRequestedAt100ns);
    EXPECT_TRUE(Result->UnloadCompleted);
  }
}

TEST(DriverWDMNeither, CancelSpinLockRequiresItsSavedIRQL) {
  for (const auto *Image : {NEVERD_WDM_NEITHER_FIXTURE,
#ifdef NEVERD_WDM_NEITHER_CFG_FIXTURE
                            NEVERD_WDM_NEITHER_CFG_FIXTURE
#endif
       }) {
    auto Options = options(0x222027, 0x180000000ULL);
    Options.Requests[1].CancelAfter100ns = 0;
    Options.Requests.resize(2);
    Options.Unload = false;
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
    EXPECT_NE(Result->Diagnostic.find("restore the saved IRQL"),
              std::string::npos)
        << Result->Diagnostic;
    ASSERT_EQ(Result->Requests.size(), 2u);
    EXPECT_EQ(Result->Requests[1].DispatchStatus, 0x103u);
    EXPECT_FALSE(Result->Requests[1].Completed);
    EXPECT_FALSE(Result->Fault);
  }
}

TEST(DriverWDMNeither, DriverInitiatedCancelReturnsAfterSynchronousCallback) {
  struct Case {
    uint32_t Code;
    uint32_t DispatchStatus;
  };
  const Case Cases[]{{0x22202b, 0x103}, {0x22202f, 0xc0000120u}};
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
        auto Result = emulateDriver(Image, options(Case.Code, Address));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), 4u);
        EXPECT_EQ(Result->Requests[1].DispatchStatus, Case.DispatchStatus);
        EXPECT_EQ(Result->Requests[1].IOStatus, 0xc0000120u);
        EXPECT_EQ(Result->Requests[1].Information, 0u);
        EXPECT_TRUE(Result->Requests[1].CancelRequestedAt100ns);
        EXPECT_TRUE(Result->Requests[1].Completed);
        EXPECT_TRUE(Result->UnloadCompleted);
        EXPECT_FALSE(Result->Fault);
        unsigned CancelCalls = 0;
        for (const auto &Call : Result->Calls)
          if (Call.Name == "IoCancelIrp") {
            ++CancelCalls;
            ASSERT_TRUE(Call.Result);
            EXPECT_EQ(*Call.Result, Case.Code == 0x22202b ? 1u : 0u);
          }
        EXPECT_EQ(CancelCalls, 1u);
      }
}
#endif
} // namespace
} // namespace neverd::emulation
