//===- DriverFileObjectTests.cpp - Retained file execution ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute deferred file CLOSE callbacks across contracts and backends.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include <chrono>
#include <iostream>

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"

#include <filesystem>
#include <map>
#include <tuple>

namespace neverd::emulation {
namespace {
class KernelFileObjectExecution
    : public testing::TestWithParam<
          std::tuple<ExecutionBackendKind, ExecutionContract, bool>> {};

void exerciseFileQueries(const std::filesystem::path &Image,
                         ExecutionBackendKind Backend,
                         ExecutionContract Contract, bool Rebase,
                         bool Native = false) {
  for (char Mode : {'S', 'P', 'W', 'E', 'M', 'C', 'G', 'L', 'B', 'U'})
    for (bool Async : {false, true}) {
      if ((Async || Native) && Mode == 'C')
        continue; // Last asynchronous query references require deferred delete.
      for (bool Scheduled : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Mode << ' ' << Async << ' ' << Scheduled);
        DriverOptions Options;
        Options.Backend = Backend;
        Options.Contract = Contract;
        Options.Unload = true;
        Options.ServiceName =
            std::string("FileQueries") + (Native ? "N" : "") + Mode;
        if (Scheduled)
          Options.Scheduling = DriverScheduling{17, 10};
        if (Rebase)
          Options.LoadAddress = 0x1c0000000;
        for (uint32_t PID : {0x1000, 0x2000})
          for (auto Kind :
               {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
                DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
            if (PID == 0x1000 && Kind == DriverRequestKind::DeviceControl)
              continue;
            if (Native && PID == 0x1000 &&
                (Kind == DriverRequestKind::Cleanup ||
                 Kind == DriverRequestKind::Close))
              continue;
            DriverRequest Input;
            Input.Kind = Kind;
            Input.File = PID;
            Input.RequestorProcessID = PID;
            if (Kind == DriverRequestKind::DeviceControl && Mode == 'U') {
              Input.ControlCode = 3;
              Input.OutputSize = 64;
            }
            if (PID == 0x1000 && Kind == DriverRequestKind::Create)
              Input.AsynchronousFile = Async;
            Options.Requests.push_back(std::move(Input));
          }
        if (Native)
          for (auto Kind :
               {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
            DriverRequest Input;
            Input.Kind = Kind;
            Input.File = Input.RequestorProcessID = 0x1000;
            Options.Requests.push_back(std::move(Input));
          }
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_TRUE(Result->UnloadCompleted) << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), Options.Requests.size() + 3 + Native)
            << driverResultJSON(*Result);
        const uint32_t Status = Mode == 'W' ? 0x80000005
                                : Mode == 'E' || Mode == 'M' || Mode == 'C'
                                    ? 0xc0000001
                                    : 0;
        unsigned Queries = 0, HandleCloses = 0;
        for (const auto &Request : Result->Requests) {
          EXPECT_TRUE(Request.Completed);
          const bool Query =
              Request.Kind == DriverRequestKind::QueryInformation;
          EXPECT_EQ(Request.IOStatus,
                    Query                                          ? Status
                    : Request.Kind == DriverRequestKind::UnlockAll ? 0xc0000010u
                                                                   : 0u);
          if (Request.Origin == DriverRequestOrigin::FileHandleClose) {
            EXPECT_EQ(Request.Kind, DriverRequestKind::UnlockAll);
            EXPECT_EQ(Request.File, 0x1000u);
            EXPECT_EQ(Request.RequestorProcessID, 4u);
            ++HandleCloses;
          }
          if (!Query)
            continue;
          EXPECT_EQ(Request.Origin,
                    Native ? DriverRequestOrigin::FileHandleQuery
                           : DriverRequestOrigin::FileInformationQuery);
          EXPECT_EQ(Request.RequestorProcessID, 0x2000u);
          EXPECT_EQ(Request.File, 0x1000u);
          EXPECT_EQ(Request.FileInformationClass, Queries == 1 ? 75u
                                                  : Queries == 2
                                                      ? (Native ? 6u : 14u)
                                                      : 4u);
          EXPECT_EQ(Request.Information, 7u);
          EXPECT_EQ(Request.Output.size(), Status >> 30 == 3 ? 0u : 7u);
          ++Queries;
        }
        EXPECT_EQ(Queries, 3u);
        EXPECT_EQ(HandleCloses, unsigned(Native));
        EXPECT_TRUE(Result->Devices.empty());
        unsigned Returned = 0;
        for (const auto &Call : Result->Calls)
          if (Call.Name ==
              (Native ? "ZwQueryInformationFile" : "IoQueryFileInformation")) {
            EXPECT_EQ(Call.Result, Mode == 'M' ? 0 : Status);
            ++Returned;
          }
        EXPECT_EQ(Returned, 3u);
        unsigned BaseQueries = 0, RelatedQueries = 0, Lookups = 0;
        uint64_t BaseDevice = 0;
        for (const auto &Call : Result->Calls) {
          if (Call.Name == "IoGetRelatedDeviceObject") {
            ASSERT_TRUE(Call.Result);
            BaseDevice = *Call.Result;
            ++RelatedQueries;
          } else if (Call.Name == "IoGetBaseFileSystemDeviceObject") {
            EXPECT_NE(BaseDevice, 0u);
            EXPECT_EQ(Call.Result, BaseDevice);
            ++BaseQueries;
          } else if (Call.Name == "MmGetSystemRoutineAddress") {
            ASSERT_TRUE(Call.Result);
            EXPECT_NE(Call.Result, 0u);
            ++Lookups;
          }
        }
        EXPECT_EQ(BaseQueries, 2u);
        EXPECT_EQ(RelatedQueries, 1u);
        EXPECT_EQ(Lookups, 1u);
      }
    }
}

TEST_P(KernelFileObjectExecution,
       OriginalDriverQueriesBorrowedFileInformation) {
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  exerciseFileQueries(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                          "driver_file_queries.sys",
                      Backend, Contract, Rebase);
}

TEST_P(KernelFileObjectExecution, GenuineWdkQueriesBorrowedFileInformation) {
#if !defined(NEVERD_WDM_FILE_QUERIES_FIXTURE) ||                               \
    !defined(NEVERD_WDM_FILE_QUERIES_CFG_FIXTURE)
  GTEST_SKIP() << "NEVERD_WDM_FILE_QUERIES_FIXTURE requires genuine WDK input";
#else
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  exerciseFileQueries(NEVERD_WDM_FILE_QUERIES_FIXTURE, Backend, Contract,
                      Rebase);
#ifdef NEVERD_WDM_FILE_QUERIES_CFG_FIXTURE
  exerciseFileQueries(NEVERD_WDM_FILE_QUERIES_CFG_FIXTURE, Backend, Contract,
                      Rebase);
#endif
#endif
}

TEST_P(KernelFileObjectExecution,
       OriginalDriverQueriesBufferedFileInformation) {
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  exerciseFileQueries(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                          "driver_file_queries.sys",
                      Backend, Contract, Rebase, true);
}

TEST_P(KernelFileObjectExecution, GenuineWdkQueriesBufferedFileInformation) {
#if !defined(NEVERD_WDM_FILE_QUERIES_FIXTURE) ||                               \
    !defined(NEVERD_WDM_FILE_QUERIES_CFG_FIXTURE)
  GTEST_SKIP()
      << "genuine WDK file-query normal and CFG fixtures were not supplied";
#else
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  exerciseFileQueries(NEVERD_WDM_FILE_QUERIES_FIXTURE, Backend, Contract,
                      Rebase, true);
  if (HasFatalFailure() || IsSkipped())
    return;
  exerciseFileQueries(NEVERD_WDM_FILE_QUERIES_CFG_FIXTURE, Backend, Contract,
                      Rebase, true);
#endif
}

void exerciseFileObjects(const std::filesystem::path &Image,
                         ExecutionBackendKind Backend,
                         ExecutionContract Contract, bool Rebase) {
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  for (char Mode : {'S', 'P', 'W', 'F', 'L'}) {
    for (bool Preemptive : {false, true}) {
      SCOPED_TRACE(std::string(1, Mode) +
                   (Preemptive ? " scheduled" : " cooperative"));
      DriverOptions Options;
      Options.Backend = Backend;
      Options.Contract = Contract;
      Options.Unload = true;
      Options.ServiceName = std::string("FileObjects") + Mode;
      if (Preemptive)
        Options.Scheduling = DriverScheduling{17, 10};
      if (Rebase)
        Options.LoadAddress = 0x1c0000000;
      auto Add = [&](DriverRequestKind Kind, uint32_t PID) {
        DriverRequest Request;
        Request.Kind = Kind;
        Request.File = 1;
        Request.RequestorProcessID = PID;
        Options.Requests.push_back(std::move(Request));
      };
      Add(DriverRequestKind::Create, 0x1000);
      if (Mode != 'F') {
        Add(DriverRequestKind::Cleanup, 0x1000);
        Add(DriverRequestKind::Close, 0x1000);
      }
      Add(DriverRequestKind::Create, 0x2000);
      Add(DriverRequestKind::DeviceControl, 0x2000);
      Add(DriverRequestKind::Cleanup, 0x2000);
      Add(DriverRequestKind::Close, 0x2000);
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->NTStatus, 0u) << Result->Diagnostic;
      EXPECT_EQ(Result->Stop, Mode == 'L' ? DriverStopReason::ModelError
                                          : DriverStopReason::Returned)
          << Result->Diagnostic;
      EXPECT_EQ(Result->UnloadCompleted, Mode != 'L');
      ASSERT_EQ(Result->Requests.size(), Options.Requests.size());
      for (size_t I = 0; I < Result->Requests.size(); ++I) {
        const auto &Request = Result->Requests[I];
        if (Mode == 'L' && I == 2) {
          EXPECT_EQ(Request.IRP, 0u);
          EXPECT_FALSE(Request.Completed);
          EXPECT_FALSE(Request.DispatchStatus);
          continue;
        }
        EXPECT_TRUE(Request.Completed) << I;
        EXPECT_EQ(Request.IOStatus, Mode == 'F' && !I ? 0xc0000001u : 0u) << I;
      }
      if (Mode == 'P') {
        EXPECT_EQ(Result->Requests[2].DispatchStatus, 0x103u);
        EXPECT_EQ(Result->Requests[2].RequestorProcessID, 0x2000u);
      }
      if (Mode == 'W')
        EXPECT_EQ(Result->Requests[2].RequestorProcessID, 4u);
      if (Mode == 'L')
        EXPECT_NE(Result->Diagnostic.find("file closed"), std::string::npos);
      else
        EXPECT_TRUE(Result->Devices.empty());
    }
  }
}

void exerciseFileHandles(const std::filesystem::path &Image,
                         ExecutionBackendKind Backend,
                         ExecutionContract Contract, bool Rebase) {
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  for (char Mode : {'S', 'P', 'C', 'B', 'W', 'L'}) {
    for (bool Preemptive : {false, true}) {
      SCOPED_TRACE(std::string(1, Mode) +
                   (Preemptive ? " scheduled" : " cooperative"));
      DriverOptions Options;
      Options.Backend = Backend;
      Options.Contract = Contract;
      Options.Unload = true;
      Options.ServiceName = std::string("FileHandles") + Mode;
      if (Preemptive)
        Options.Scheduling = DriverScheduling{17, 10};
      if (Rebase)
        Options.LoadAddress = 0x1c0000000;
      for (uint32_t PID : {0x1000, 0x2000}) {
        for (auto Kind :
             {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
              DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
          DriverRequest Input;
          Input.Kind = Kind;
          Input.File = 1;
          Input.RequestorProcessID = PID;
          Options.Requests.push_back(std::move(Input));
        }
      }
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, Mode == 'L' ? DriverStopReason::ModelError
                                          : DriverStopReason::Returned)
          << Result->Diagnostic;
      EXPECT_EQ(Result->UnloadCompleted, Mode != 'L') << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), Options.Requests.size() + 1);
      size_t Scenario = 0;
      for (const auto &Request : Result->Requests) {
        if (Request.Origin == DriverRequestOrigin::FileHandleClose) {
          EXPECT_EQ(Request.Kind, DriverRequestKind::UnlockAll);
          EXPECT_EQ(Request.RequestorProcessID, 0x1000u);
          EXPECT_TRUE(Request.Completed);
          EXPECT_EQ(Request.IOStatus, 0u);
          continue;
        }
        const size_t Index = Scenario++;
        if (Mode == 'L' && (Index == 2 || Index == 3)) {
          EXPECT_FALSE(Request.Completed);
          EXPECT_EQ(Request.IRP, 0u);
          EXPECT_FALSE(Request.DispatchStatus);
          continue;
        }
        EXPECT_TRUE(Request.Completed) << Index;
        EXPECT_EQ(Request.IOStatus, 0u) << Index;
        if (Index == 2) {
          EXPECT_EQ(Request.RequestorProcessID, 4u);
          EXPECT_EQ(Request.DispatchStatus,
                    Mode == 'P' || Mode == 'B' ? 0x103u : 0u);
        }
        if (Index == 3) {
          EXPECT_EQ(Request.RequestorProcessID, Mode == 'W' ? 4u : 0x2000u);
          EXPECT_EQ(Request.DispatchStatus,
                    Mode == 'C' || Mode == 'B' ? 0x103u : 0u);
        }
      }
      if (Mode != 'L')
        EXPECT_TRUE(Result->Devices.empty());
    }
  }
}

TEST_P(KernelFileObjectExecution, OriginalDriverDefersCloseUntilLastReference) {
  const auto [Backend, Contract, Rebase] = GetParam();
  exerciseFileObjects(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                          "driver_file_objects.sys",
                      Backend, Contract, Rebase);
}

TEST_P(KernelFileObjectExecution, GenuineWdkDefersCloseUntilLastReference) {
#if !defined(NEVERD_WDM_FILE_OBJECTS_FIXTURE) ||                               \
    !defined(NEVERD_WDM_FILE_OBJECTS_CFG_FIXTURE)
  GTEST_SKIP() << "genuine WDK file-lifetime fixtures were not supplied";
#else
  const auto [Backend, Contract, Rebase] = GetParam();
  exerciseFileObjects(NEVERD_WDM_FILE_OBJECTS_FIXTURE, Backend, Contract,
                      Rebase);
  if (HasFatalFailure() || IsSkipped())
    return;
  exerciseFileObjects(NEVERD_WDM_FILE_OBJECTS_CFG_FIXTURE, Backend, Contract,
                      Rebase);
#endif
}

TEST_P(KernelFileObjectExecution, OriginalDriverClosesSharedFileHandles) {
  const auto [Backend, Contract, Rebase] = GetParam();
  exerciseFileHandles(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                          "driver_file_handles.sys",
                      Backend, Contract, Rebase);
}

TEST_P(KernelFileObjectExecution, GenuineWdkClosesSharedFileHandles) {
#if !defined(NEVERD_WDM_FILE_HANDLES_FIXTURE) ||                               \
    !defined(NEVERD_WDM_FILE_HANDLES_CFG_FIXTURE)
  GTEST_SKIP() << "genuine WDK file-lifetime fixtures were not supplied";
#else
  const auto [Backend, Contract, Rebase] = GetParam();
  exerciseFileHandles(NEVERD_WDM_FILE_HANDLES_FIXTURE, Backend, Contract,
                      Rebase);
  if (HasFatalFailure() || IsSkipped())
    return;
  exerciseFileHandles(NEVERD_WDM_FILE_HANDLES_CFG_FIXTURE, Backend, Contract,
                      Rebase);
#endif
}

TEST_P(KernelFileObjectExecution, GenuineKmdfRestoresFileHandleCaller) {
#if !defined(NEVERD_KMDF_FILE_HANDLES_FIXTURE) ||                              \
    !defined(NEVERD_KMDF_FILE_HANDLES_CFG_FIXTURE)
  GTEST_SKIP() << "NEVERD_KMDF_FILE_HANDLES_FIXTURE requires genuine WDK input";
#else
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  std::vector<const char *> Images{NEVERD_KMDF_FILE_HANDLES_FIXTURE};
#ifdef NEVERD_KMDF_FILE_HANDLES_CFG_FIXTURE
  Images.push_back(NEVERD_KMDF_FILE_HANDLES_CFG_FIXTURE);
#endif
  for (const char *Image : Images) {
    SCOPED_TRACE(Image);
    for (char Mode : {'S', 'N', 'F'}) {
      for (bool Scheduled : {false, true}) {
        SCOPED_TRACE(std::string(1, Mode) +
                     (Scheduled ? " scheduled" : " cooperative"));
        DriverOptions Options;
        Options.Backend = Backend;
        Options.Contract = Contract;
        Options.Unload = true;
        Options.ServiceName = std::string("OriginalKmdfFileHandles") + Mode;
        if (Scheduled)
          Options.Scheduling = DriverScheduling{17, 10};
        if (Rebase)
          Options.LoadAddress = 0x1c0000000;
        for (uint32_t PID : {0x1000, 0x2000}) {
          for (auto Kind :
               {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
                DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
            DriverRequest Input;
            Input.Kind = Kind;
            Input.File = 1;
            Input.RequestorProcessID = PID;
            if (Kind == DriverRequestKind::DeviceControl)
              Input.ControlCode = 0x222000;
            Options.Requests.push_back(std::move(Input));
          }
        }
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        EXPECT_TRUE(Result->UnloadCompleted) << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), 9u);
        for (const auto &Request : Result->Requests) {
          EXPECT_TRUE(Request.Completed);
          EXPECT_EQ(Request.IOStatus,
                    Request.Kind == DriverRequestKind::UnlockAll ? 0xc0000010u
                                                                 : 0u);
        }
        EXPECT_EQ(Result->Requests[2].RequestorProcessID, 4u);
        EXPECT_EQ(Result->Requests[4].RequestorProcessID, 0x2000u);
        EXPECT_TRUE(Result->Devices.empty());
      }
    }
  }
#endif
}

void exerciseFileTransfers(const std::filesystem::path &Image,
                           ExecutionBackendKind Backend,
                           ExecutionContract Contract, bool Rebase) {
  for (char Mode : {'S', 'P', 'W', 'E', 'R', 'M', 'G', 'L', 'U'})
    for (bool Scheduled : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Mode << ' ' << Scheduled);
      DriverOptions Options;
      Options.Backend = Backend;
      Options.Contract = Contract;
      Options.Unload = true;
      Options.ServiceName = std::string("FileTransfers") + Mode;
      if (Scheduled)
        Options.Scheduling = DriverScheduling{17, 10};
      if (Rebase)
        Options.LoadAddress = 0x1c0000000;
      for (auto [Kind, PID] :
           {std::pair{DriverRequestKind::Create, 0x1000u},
            std::pair{DriverRequestKind::Create, 0x2000u},
            std::pair{DriverRequestKind::DeviceControl, 0x2000u},
            std::pair{DriverRequestKind::Cleanup, 0x2000u},
            std::pair{DriverRequestKind::Close, 0x2000u},
            std::pair{DriverRequestKind::Cleanup, 0x1000u},
            std::pair{DriverRequestKind::Close, 0x1000u}}) {
        DriverRequest Input;
        Input.Kind = Kind;
        Input.File = Input.RequestorProcessID = PID;
        if (Mode == 'U' && Kind == DriverRequestKind::DeviceControl) {
          Input.ControlCode = 3;
          Input.OutputSize = 64;
        }
        Options.Requests.push_back(std::move(Input));
      }
      std::cerr << "DIAG begin mode=" << Mode << " scheduled=" << Scheduled << std::endl;
        const auto DiagStart = std::chrono::steady_clock::now();
        auto Result = emulateDriver(Image, Options);
        std::cerr << "DIAG end mode=" << Mode << " scheduled=" << Scheduled
                  << " seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now()-DiagStart).count();
        if (Result) std::cerr << " stop=" << unsigned(Result->Stop) << " instructions=" << Result->Instructions << " diagnostic=" << Result->Diagnostic;
        std::cerr << std::endl;
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_TRUE(Result->UnloadCompleted) << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), Options.Requests.size() + 17)
          << driverResultJSON(*Result);
      const uint32_t Status = Mode == 'W' ? 0x80000005u
                              : Mode == 'E' || Mode == 'R' || Mode == 'M'
                                  ? 0xc0000001u
                                  : 0u;
      unsigned Index = 0, Flushes = 0, Controls = 0;
      for (const auto &Request : Result->Requests) {
        EXPECT_TRUE(Request.Completed);
        if (Request.Origin == DriverRequestOrigin::FileHandleDeviceControl ||
            Request.Origin == DriverRequestOrigin::FileHandleSystemControl) {
          const bool FS = Controls >= 4;
          const unsigned Shape = Controls % 4;
          EXPECT_EQ(Request.Kind, FS ? DriverRequestKind::FileSystemControl
                                     : DriverRequestKind::DeviceControl);
          EXPECT_EQ(Request.Origin,
                    FS ? DriverRequestOrigin::FileHandleSystemControl
                       : DriverRequestOrigin::FileHandleDeviceControl);
          EXPECT_EQ(Request.ControlCode, 0x22e000u + 4 * Shape);
          EXPECT_EQ(Request.IOStatus, Status);
          EXPECT_EQ(Request.Information, Shape < 2 ? 7u : 0x100000007ull);
          EXPECT_EQ(Request.Output,
                    Shape < 2 && Status >> 30 != 3
                        ? std::vector<uint8_t>(
                              {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26})
                        : std::vector<uint8_t>{});
          EXPECT_FALSE(Request.FileInformationClass);
          EXPECT_EQ(Request.File, 0x1000u);
          EXPECT_EQ(Request.RequestorProcessID, 0x2000u);
          ++Controls;
          continue;
        }
        if (Request.Kind == DriverRequestKind::FlushBuffers) {
          EXPECT_EQ(Request.Origin, DriverRequestOrigin::FileHandleFlush);
          EXPECT_EQ(Request.IOStatus, Status);
          EXPECT_EQ(Request.Information, 0x100000007ull);
          EXPECT_TRUE(Request.Output.empty());
          EXPECT_FALSE(Request.FileInformationClass);
          EXPECT_EQ(Request.File, 0x1000u);
          EXPECT_EQ(Request.RequestorProcessID, 0x2000u);
          ++Flushes;
          continue;
        }
        if (Request.Kind != DriverRequestKind::Read &&
            Request.Kind != DriverRequestKind::Write) {
          EXPECT_EQ(Request.IOStatus, 0u);
          continue;
        }
        const bool Write = Index & 1;
        const uint64_t Count = Index == 2 || Index == 3 ? 0 : 7;
        EXPECT_EQ(Request.Origin, Write ? DriverRequestOrigin::FileHandleWrite
                                        : DriverRequestOrigin::FileHandleRead);
        EXPECT_EQ(Request.IOStatus, Status);
        EXPECT_EQ(Request.Information, Count);
        EXPECT_EQ(Request.Output,
                  !Write && Count && Status >> 30 != 3
                      ? std::vector<uint8_t>(
                            {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26})
                      : std::vector<uint8_t>{});
        EXPECT_EQ(Request.ByteOffset, Index < 2 ? 0x100000025ull + Index
                                      : Index == 3 || Index == 5
                                          ? UINT64_MAX
                                          : 0x123456789ull);
        EXPECT_EQ(Request.File, 0x1000u);
        EXPECT_EQ(Request.RequestorProcessID, 0x2000u);
        EXPECT_FALSE(Request.FileInformationClass);
        ++Index;
      }
      EXPECT_EQ(Index, 6u);
      EXPECT_EQ(Flushes, 2u);
      EXPECT_EQ(Controls, 8u);
      EXPECT_TRUE(Result->Devices.empty());
      for (auto [Name, Expected] : {std::pair{"ZwReadFile", 3u},
                                    {"ZwWriteFile", 3u},
                                    {"ZwFlushBuffersFile", 2u},
                                    {"ZwDeviceIoControlFile", 4u},
                                    {"ZwFsControlFile", 4u}}) {
        unsigned Calls = 0;
        for (const auto &Call : Result->Calls)
          if (Call.Name == Name) {
            EXPECT_EQ(Call.Result, Mode == 'M' ? 0 : Status);
            ++Calls;
          }
        EXPECT_EQ(Calls, Expected);
      }
    }
}

TEST_P(KernelFileObjectExecution, OriginalBufferedNativeFileTransfers) {
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  exerciseFileTransfers(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                            "driver_file_transfers.sys",
                        Backend, Contract, Rebase);
}

TEST_P(KernelFileObjectExecution, GenuineWdkBufferedNativeFileTransfers) {
#if !defined(NEVERD_WDM_FILE_TRANSFERS_FIXTURE) ||                             \
    !defined(NEVERD_WDM_FILE_TRANSFERS_CFG_FIXTURE)
  GTEST_SKIP() << "genuine WDK file-transfer fixtures are unavailable";
#else
  const auto [Backend, Contract, Rebase] = GetParam();
  auto Probe = createExecutionBackend(Backend, Contract, 64 * 1024 * 1024);
  if (!Probe) {
    auto E = Probe.takeError();
    const bool Unavailable = E.isA<BackendUnavailableError>();
    const auto Reason = llvm::toString(std::move(E));
    if (Unavailable)
      GTEST_SKIP() << Reason;
    FAIL() << Reason;
  }
  Probe->CPU.reset();
  for (const auto *Image : {NEVERD_WDM_FILE_TRANSFERS_FIXTURE,
                            NEVERD_WDM_FILE_TRANSFERS_CFG_FIXTURE})
    exerciseFileTransfers(Image, Backend, Contract, Rebase);
#endif
}

std::string fileObjectParameterName(
    const testing::TestParamInfo<KernelFileObjectExecution::ParamType> &Info) {
  const auto [Backend, Contract, Rebase] = Info.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "Legacy" : "Checked") +
         (Rebase ? "Rebased" : "Preferred");
}

INSTANTIATE_TEST_SUITE_P(
    Backends, KernelFileObjectExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP,
                                     ExecutionBackendKind::HVF),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64),
                     testing::Bool()),
    fileObjectParameterName);
} // namespace
} // namespace neverd::emulation
