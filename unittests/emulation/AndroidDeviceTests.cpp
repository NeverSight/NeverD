//===- AndroidDeviceTests.cpp - Shared explicit device transactions -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Executes independent callers and checks refused transactions before effects.
//===----------------------------------------------------------------------===//
#include "LinuxDeviceTestData.h"
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxServices.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
class AndroidDevices : public testing::TestWithParam<
                           std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  static constexpr uint64_t Buffer = 0x21000000;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->Libraries["libdevice.so"] = {"ioctl", "mmap", "dup",
                                                  "dup3"};
    Options.LinuxFiles.emplace().Devices["/fixture/control"] =
        deviceTestInput();
    Options.Limits.Instructions = 200000;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Arguments = {}) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Arguments);
    auto Result =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (Result)
      return std::move(*Result);
    ProcessResult Failure{ProcessProfile::AndroidNativeAArch64,
                          GuestArchitecture::AArch64,
                          Options.Backend,
                          {}};
    Failure.Stop = ProcessStopReason::RuntimeFailure;
    Failure.Diagnostic = llvm::toString(Result.takeError());
    ADD_FAILURE() << Failure.Diagnostic;
    return Failure;
  }
};

TEST_P(AndroidDevices, DuplicatesShareCursorsTransactionsAndOutputTargets) {
  Options.LinuxFiles->Files["/fixture/bytes"] = {'a', 'b', 'c'};
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_duplicates", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_EQ(R.StandardOutput, "out");
    EXPECT_EQ(R.StandardError, "err");
  }
}

TEST_P(AndroidDevices, TimedDeviceWritesSurviveAliasesRetirementAndRemapping) {
  Options.LinuxFiles->Devices.at("/fixture/control") =
      updatingDeviceTestInput();
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  auto R = run("devices_updates");
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}

TEST_P(AndroidDevices, ExplicitInstructionTimeAdvancesAPureGuestPollingLoop) {
  auto &D = Options.LinuxFiles->Devices.at("/fixture/control");
  D.ReadOnlyMappings[0].Updates = {{1000000, 0, {0x9a}}};
  Options.LinuxTime.emplace().NanosecondsPerInstruction = 100;
  Options.InstructionQuantum = 1024;
  auto R = run("devices_busy_poll");
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  EXPECT_GE(R.Instructions, 10000u);
  EXPECT_LT(R.Instructions, 13000u);
  Options.LinuxTime->NanosecondsPerInstruction = 0;
  Options.Limits.Instructions = 20000;
  R = run("devices_busy_poll");
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
}

TEST_P(AndroidDevices, RunnableChildAndSleepingParentShareProducerTime) {
  Options.LinuxFiles->Devices.at("/fixture/control")
      .ReadOnlyMappings[0]
      .Updates = {{1000000, 0, {0x9a}}};
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  Options.LinuxTime->NanosecondsPerInstruction = 100;
  Options.InstructionQuantum = 1024;
  Options.Android->ThreadLimit = 2;
  Options.Android->DrainThreads = true;
  auto R = run("devices_thread_updates");
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  EXPECT_GE(R.Instructions, 10000u);
}

TEST_P(AndroidDevices, ContextOwnershipAndTimestampQueriesShareMappedBytes) {
  Options.LinuxFiles->Devices.at("/fixture/control") = contextDeviceTestInput();
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_contexts", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}
TEST_P(AndroidDevices, GPUCommandsWritePinnedRAMAndRetireOwnedTimestamps) {
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->SynchronousA3xx = true;
  Options.LinuxFiles->Devices.at("/fixture/control") = D;
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_gpu_commands", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}
TEST_P(AndroidDevices, SustainedGPUCommandsPreserveTimeAndContextOwnership) {
  // 1024 submissions and sleeps exercise many hardware-backend exits at O0.
  Options.Limits.TimeoutMicroseconds = 30000000;
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->SynchronousA3xx = true;
  Options.LinuxFiles->Devices.at("/fixture/control") = D;
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  Options.LinuxTime->Clocks[1] = {20, 999500000};
  Options.Limits.Instructions = 2000000;
  auto &Trace = Options.LinuxFiles->IoctlTrace.emplace();
  Trace.IncludeUnlisted = false;
  Trace.EventLimit = 4;
  Trace.ByteLimit = 96;
  Trace.Requests[0xc00c0916] = {{0, 12, {}}};
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_gpu_stream", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_FALSE(R.IoctlTraceTruncated);
    ASSERT_EQ(R.IoctlTrace.size(), 4u);
    for (unsigned I = 0; I != 4; ++I) {
      const auto &E = R.IoctlTrace[I];
      EXPECT_EQ(E.Request, 0xc00c0916u);
      EXPECT_EQ(E.Result, I ? 0u : uint64_t(-22));
      ASSERT_EQ(E.Memory.size(), 1u);
      EXPECT_EQ(E.Memory[0].BeforeStatus, "complete");
      EXPECT_EQ(E.Memory[0].AfterStatus, "complete");
      ASSERT_EQ(E.Memory[0].After.size(), 12u);
      EXPECT_EQ(llvm::support::endian::read32le(E.Memory[0].After.data() + 8),
                I ? 1024u : 0xdeadbeefu);
    }
  }
}

TEST_P(AndroidDevices, GPUObjectsShareMemoryAndRetireOnlyTheirFinalOwner) {
  // O0 checks every word of the retained pages. Match the raw Linux case's
  // wall budget without changing guest instruction or event limits.
  Options.Limits.TimeoutMicroseconds = 30000000;
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->MemoryObjects = true;
  for (bool Commands : {false, true}) {
    D.KGSL->SynchronousA3xx = Commands;
    Options.LinuxFiles->Devices.at("/fixture/control") = D;
    for (uint64_t Route = 0; Route != 4; ++Route) {
      SCOPED_TRACE(testing::Message() << Route << ':' << Commands);
      auto R = run("devices_gpu_objects", {Route, Commands});
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      ASSERT_EQ(R.ReturnValue, 0u);
    }
  }
}

TEST_P(AndroidDevices, LegacyGPUObjectsShareModernFlagsMemoryAndLifetime) {
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->MemoryObjects = true;
  // Force a collision at an otherwise aligned starting address.
  D.ReadOnlyMappings.push_back({0x10000000, std::vector<uint8_t>(4096), {}});
  for (bool Commands : {false, true}) {
    D.KGSL->SynchronousA3xx = Commands;
    Options.LinuxFiles->Devices.at("/fixture/control") = D;
    for (uint64_t Route = 0; Route != 4; ++Route) {
      SCOPED_TRACE(testing::Message() << Route << ':' << Commands);
      auto R = run("devices_gpu_legacy", {Route, Commands});
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      ASSERT_EQ(R.ReturnValue, 0u);
    }
  }
}

TEST_P(AndroidDevices, LegacyGPURefusalsCheckCompleteRecordsBeforeEffects) {
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->MemoryObjects = true;
  D.KGSL->SynchronousA3xx = true;
  Options.LinuxFiles->Devices.at("/fixture/control") = D;
  for (uint64_t Mode = 0; Mode != 5; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Memory = {
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false}};
    Options.Android->ReadMemory = {{Buffer, 8192}};
    if (Mode >= 2)
      Options.Android->ReadMemory.push_back({0x23000000, 4096, false});
    auto R = run("devices_gpu_legacy_rejected", {Mode, Buffer});
    ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_FALSE(R.Services.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), Mode >= 2 ? 2u : 1u);
    auto Expected = std::vector<uint8_t>(8192, 0xa5);
    auto Put32 = [&](unsigned At, uint32_t Value) {
      llvm::support::endian::write32le(Expected.data() + At, Value);
    };
    auto Put64 = [&](unsigned At, uint64_t Value) {
      llvm::support::endian::write64le(Expected.data() + At, Value);
    };
    if (Mode < 2) {
      const unsigned At = Mode ? 4064 : 0;
      Put32(At, 0xdeadbeef);
      Put32(At + 4, Mode ? 0 : 8);
      Put64(At + 8, 4096);
      Put64(At + 16, 0xabcdef);
      Put64(At + 24, 0x11223344);
      Put64(At + 32, 0x1234);
      Put64(At + 40, 0x5678);
    } else if (Mode == 2) {
      Put64(4056, 0x10000001);
      Put32(4064, 2);
    } else if (Mode == 3)
      Put32(4092, 2);
    else {
      Put64(0, 0x10002000);
      Put32(8, 0);
    }
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    if (Mode >= 2) {
      auto Bytes = std::vector<uint8_t>(4096, 0);
      llvm::support::endian::write32le(Bytes.data(), 0x76543210);
      EXPECT_EQ(R.MemorySnapshots[1].Bytes, Bytes);
    }
  }
}

TEST_P(AndroidDevices, GPUObjectAddressesExcludeLegacyDeviceRanges) {
  Options.Limits.TimeoutMicroseconds = 30000000;
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.ReadOnlyMappings.push_back(
      {0x10000000, std::vector<uint8_t>(4096, 0xc7), {}});
  D.KGSL->MemoryObjects = true;
  D.KGSL->SynchronousA3xx = true;
  Options.LinuxFiles->Devices.at("/fixture/control") = D;
  auto R = run("devices_gpu_objects", {0, 1});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}

TEST_P(AndroidDevices, GPUObjectWritesRequireFullProgramAdmission) {
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->MemoryObjects = true;
  D.KGSL->SynchronousA3xx = true;
  Options.LinuxFiles->Devices.at("/fixture/control") = D;
  for (uint64_t Mode : {0u, 1u}) {
    Options.Android->Memory = {{Buffer, 4096, {}, false}};
    Options.Android->ReadMemory = {{0x23000000, 4096, false},
                                   {Buffer + 20, 4},
                                   {0x22000000 + 40, 12, false}};
    auto R = run("devices_gpu_object_write_rejected", {Mode, Buffer});
    ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_EQ(R.MemorySnapshots.size(), 3u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(4096));
    EXPECT_EQ(R.MemorySnapshots[1].Bytes,
              (std::vector<uint8_t>{0xef, 0xbe, 0xad, 0xde}));
    EXPECT_EQ(
        R.MemorySnapshots[2].Bytes,
        (std::vector<uint8_t>{0, 0, 0, 0, 0xa5, 0xa5, 0xa5, 0xa5, 0, 0, 0, 0}));
    EXPECT_FALSE(R.Services.back().Result);
  }
}

TEST_P(AndroidDevices, GPUObjectRefusalsPreserveRecordsAndMappings) {
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->MemoryObjects = true;
  Options.LinuxFiles->Devices.at("/fixture/control") = D;
  for (uint64_t Mode = 0; Mode != 8; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Memory = {
        {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false}};
    Options.Android->ReadMemory = {{Buffer, 4096}};
    if (Mode >= 3 && Mode <= 5)
      Options.Android->ReadMemory.push_back({0x22000000, 8192, false});
    auto R = run("devices_gpu_objects_rejected", {Mode, Buffer});
    ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_FALSE(R.Services.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), Mode >= 3 && Mode <= 5 ? 2u : 1u);
    auto Expected = std::vector<uint8_t>(4096, 0xa5);
    std::fill_n(Expected.begin(), 48, 0);
    auto Put64 = [&](unsigned At, uint64_t Value) {
      llvm::support::endian::write64le(Expected.data() + At, Value);
    };
    auto Put32 = [&](unsigned At, uint32_t Value) {
      llvm::support::endian::write32le(Expected.data() + At, Value);
    };
    Put64(0, 8192);
    Put64(8, Mode == 0 ? 8 : Mode < 3 ? 0 : 0x000c0000);
    Put64(24, Mode < 3 ? 0xabcdef : 12288);
    Put32(32, Mode < 3 ? 0xdeadbeef : 2);
    Put32(36, Mode == 1 ? 1 : 0);
    if (Mode == 5) {
      std::fill_n(Expected.begin() + 64, 32, 0);
      Put64(64, 1);
      Put32(80, 2);
      Put32(84, 1);
    }
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    if (R.MemorySnapshots.size() == 2) {
      auto Memory = std::vector<uint8_t>(8192, 0);
      llvm::support::endian::write32le(Memory.data(), 0x76543210);
      EXPECT_EQ(R.MemorySnapshots[1].Bytes, Memory);
    }
  }
}

TEST_P(AndroidDevices, RejectedGPUProgramsCannotPublishEarlierWrites) {
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->SynchronousA3xx = true;
  Options.LinuxFiles->Devices.at("/fixture/control") = D;
  for (uint64_t Mode = 0; Mode != 4; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Memory = {
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false}};
    Options.Android->ReadMemory = {{Buffer, 4096},
                                   {Buffer + 4096 + 20, 4},
                                   {0x22000000 + 40, 4, false},
                                   {0x22000000 + 48, 4, false}};
    auto R = run("devices_gpu_rejected", {Mode, Buffer});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_EQ(R.MemorySnapshots.size(), 4u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(4096, 0xa5));
    EXPECT_EQ(R.MemorySnapshots[1].Bytes,
              (std::vector<uint8_t>{0xef, 0xbe, 0xad, 0xde}));
    EXPECT_EQ(R.MemorySnapshots[2].Bytes, std::vector<uint8_t>(4, 0));
    EXPECT_EQ(R.MemorySnapshots[3].Bytes, std::vector<uint8_t>(4, 0));
    EXPECT_FALSE(R.Services.back().Result);
  }
}

TEST_P(AndroidDevices, RejectedIndirectGPUProgramsPreserveAllFetchedRAM) {
  // O0 initializes and compares complete pinned pages in each case. Keep the
  // instruction/event budgets while allowing the same wall time as objects.
  Options.Limits.TimeoutMicroseconds = 30000000;
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->SynchronousA3xx = true;
  Options.LinuxFiles->Devices.at("/fixture/control") = D;
  const char *Reasons[] = {"opcode",         "extent",         "aligned",
                           "registered RAM", "nonempty",       "recursive",
                           "depth budget",   "self-modifying", "self-modifying",
                           "aggregate",      "writable RAM",   "self-modifying",
                           "opcode",         "opcode",         "predicate"};
  for (unsigned Mode = 0; Mode != std::size(Reasons); ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Memory = {
        {Buffer, 16384, std::vector<uint8_t>(16384, 0xa5), false}};
    Options.Android->ReadMemory = {{Buffer, 4096},
                                   {Buffer + 4096 + 20, 4},
                                   {0x22000000 + 40, 4, false},
                                   {0x22000000 + 48, 4, false},
                                   {0x23000000, 4096, false},
                                   {0x24000000, 4096, false},
                                   {Buffer + 8192, 4096},
                                   {Buffer + 12288, 4096}};
    auto R = run("devices_gpu_indirect_rejected", {Mode, Buffer});
    ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("GPU PM4 buffer 0x"), std::string::npos);
    EXPECT_NE(R.Diagnostic.find(" word "), std::string::npos);
    EXPECT_NE(R.Diagnostic.find(" header 0x"), std::string::npos);
    EXPECT_NE(R.Diagnostic.find(Reasons[Mode]), std::string::npos)
        << R.Diagnostic;
    ASSERT_EQ(R.MemorySnapshots.size(), 8u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(4096, 0xa5));
    EXPECT_EQ(R.MemorySnapshots[1].Bytes,
              (std::vector<uint8_t>{0xef, 0xbe, 0xad, 0xde}));
    EXPECT_EQ(R.MemorySnapshots[2].Bytes, std::vector<uint8_t>(4, 0));
    EXPECT_EQ(R.MemorySnapshots[3].Bytes, std::vector<uint8_t>(4, 0));
    EXPECT_EQ(R.MemorySnapshots[4].Bytes, R.MemorySnapshots[6].Bytes);
    EXPECT_EQ(R.MemorySnapshots[5].Bytes, R.MemorySnapshots[7].Bytes);
    EXPECT_FALSE(R.Services.back().Result);
  }
}

TEST_P(AndroidDevices, ContextRefusalsDoNotInventOutputs) {
  Options.LinuxFiles->Devices.at("/fixture/control") = contextDeviceTestInput();
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  for (uint64_t Mode = 0; Mode != 7; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Memory = {
        {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false}};
    Options.Android->ReadMemory = {{Buffer, 4096}};
    auto R = run("devices_context_unsupported", {Mode, Buffer});
    ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    auto Expected = std::vector<uint8_t>(4096, 0xa5);
    llvm::support::endian::write32le(Expected.data(), Mode >= 3   ? 0x8052
                                                      : Mode == 0 ? 0x20012
                                                      : Mode == 1 ? 0x12
                                                                  : 1);
    llvm::support::endian::write32le(Expected.data() + 4, Mode >= 3 ? 1
                                                          : Mode == 2
                                                              ? 3
                                                              : 0xffffffffU);
    if (Mode == 2)
      llvm::support::endian::write32le(Expected.data() + 8, 0xdeadbeefU);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    EXPECT_FALSE(R.Services.back().Result);
  }
}

TEST_P(AndroidDevices, RawNamedVariadicAndDynamicCallsShareTransactions) {
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_sequence", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}

TEST_P(AndroidDevices, IoctlTracePreservesCallsAndCapturesTransientPayloads) {
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    Options.LinuxFiles->IoctlTrace.reset();
    const auto Before = run("devices_sequence", {Route});
    auto &T = Options.LinuxFiles->IoctlTrace.emplace();
    T.Requests[0xa1000001] = {{0, 24, {}}, {0, 8, 8}};
    T.Requests[0xa1000002] = {
        {0, 4, {}}, {0, 4, 0}, {0x8000000000000000ULL, 4, {}}};
    const auto R = run("devices_sequence", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_EQ(R.Instructions, Before.Instructions);
    EXPECT_EQ(R.Events, Before.Events);
    EXPECT_EQ(R.StandardOutput, Before.StandardOutput);
    EXPECT_TRUE(R.IoctlTraceEnabled);
    EXPECT_FALSE(R.IoctlTraceTruncated);
    ASSERT_EQ(R.IoctlTrace.size(), 5u);
    for (unsigned I : {0u, 2u, 4u}) {
      const auto &C = R.IoctlTrace[I];
      EXPECT_EQ(C.Request, 0xa1000001u);
      EXPECT_EQ(C.Result, 37u);
      EXPECT_NE(C.PC, 0u);
      EXPECT_EQ(C.ThreadID, 1000u);
      EXPECT_EQ(C.ReturnAddress.has_value(), Route != 0);
      ASSERT_EQ(C.Memory.size(), 2u);
      auto Packet = C.Memory[0].Before;
      ASSERT_EQ(Packet.size(), 24u);
      EXPECT_EQ(Packet[0], 0x31);
      EXPECT_EQ(Packet[1], 0x32);
      Packet[0] = 0x52;
      Packet[1] = 0x53;
      EXPECT_EQ(C.Memory[0].After, Packet);
      EXPECT_EQ(C.Memory[1].Address,
                llvm::support::endian::read64le(Packet.data() + 8));
      EXPECT_EQ(C.Memory[1].Before,
                (std::vector<uint8_t>{0xc7, 0x55, 0x56, 0xc7, 0xc7, 0xc7, 0xc7,
                                      0xc7}));
      EXPECT_EQ(C.Memory[1].After,
                (std::vector<uint8_t>{0xc7, 0x90, 0x91, 0xc7, 0xc7, 0xc7, 0xc7,
                                      0xc7}));
      for (const auto &M : C.Memory) {
        EXPECT_EQ(M.BeforeStatus, "complete");
        EXPECT_EQ(M.AfterStatus, "complete");
      }
    }
    const auto &Bad = R.IoctlTrace[1];
    EXPECT_EQ(Bad.Result, uint64_t(-13));
    EXPECT_EQ(Bad.Memory[0].BeforeStatus, "unreadable");
    EXPECT_EQ(Bad.Memory[0].AfterStatus, "unreadable");
    EXPECT_EQ(Bad.Memory[1].BeforeStatus, "unreadable");
    EXPECT_EQ(Bad.Memory[1].AfterStatus, "unresolved_entry_address");
    EXPECT_EQ(Bad.Memory[2].BeforeStatus, "address_overflow");
    EXPECT_EQ(R.IoctlTrace[3].Result, uint64_t(-9));
    EXPECT_TRUE(R.IoctlTrace[3].Memory.empty());
  }
}

TEST_P(AndroidDevices, IoctlTraceSelectionPreservesWholeReports) {
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    Options.LinuxFiles->IoctlTrace.reset();
    const auto Before = run("devices_sequence", {Route});
    auto &T = Options.LinuxFiles->IoctlTrace.emplace();
    T.IncludeUnlisted = false;
    T.EventLimit = 3;
    T.ByteLimit = 216;
    T.Requests[0xa1000001] = {{0, 24, {}}, {0, 8, 8}};
    const auto R = run("devices_sequence", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    ASSERT_EQ(R.IoctlTrace.size(), 3u);
    EXPECT_FALSE(R.IoctlTraceTruncated);
    for (const auto &E : R.IoctlTrace) {
      EXPECT_EQ(E.Request, 0xa1000001u);
      EXPECT_EQ(E.Result, 37u);
      ASSERT_EQ(E.Memory.size(), 2u);
      EXPECT_EQ(E.Memory[1].AfterStatus, "complete");
    }
    auto Observed = llvm::cantFail(llvm::json::parse(processResultJSON(R)));
    Observed.getAsObject()->erase("ioctl_trace");
    Observed.getAsObject()->erase("ioctl_trace_truncated");
    EXPECT_EQ(Observed,
              llvm::cantFail(llvm::json::parse(processResultJSON(Before))));
  }
}

TEST_P(AndroidDevices, IoctlTraceExhaustionDoesNotStopTheGuest) {
  auto &T = Options.LinuxFiles->IoctlTrace.emplace();
  T.EventLimit = 2;
  T.ByteLimit = 24;
  T.Requests[0xa1000001] = {{0, 24, {}}, {0, 8, 8}};
  auto R = run("devices_sequence", {1});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  EXPECT_TRUE(R.IoctlTraceTruncated);
  ASSERT_EQ(R.IoctlTrace.size(), 2u);
  const auto &M = R.IoctlTrace[0].Memory;
  ASSERT_EQ(M.size(), 2u);
  EXPECT_EQ(M[0].Before.size(), 24u);
  EXPECT_TRUE(M[0].After.empty());
  EXPECT_EQ(M[0].AfterStatus, "byte_limit");
  EXPECT_EQ(M[1].BeforeStatus, "byte_limit");
  EXPECT_EQ(M[1].AfterStatus, "unresolved_entry_address");
}

TEST_P(AndroidDevices, DeviceMappingsOutliveDescriptorsAcrossEveryCallRoute) {
  for (uint64_t Route = 0; Route != 4; ++Route) {
    SCOPED_TRACE(Route);
    auto R = run("devices_mapping", {Route});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}

TEST_P(AndroidDevices, DeviceMappingAccessFaultsReachTheGuest) {
  for (uint64_t Mode = 0; Mode != 3; ++Mode) {
    SCOPED_TRACE(Mode);
    auto R = run("devices_mapping_fault", {Mode});
    EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
    ASSERT_TRUE(R.LastCPUExit && R.LastCPUExit->Fault);
    const auto &Fault = *R.LastCPUExit->Fault;
    EXPECT_EQ(Fault.Kind, Mode == 2 ? BackendFaultKind::UnmappedMemory
                                    : BackendFaultKind::Protection);
    EXPECT_EQ(Fault.Access,
              Mode == 1 ? BackendAccessKind::Write : BackendAccessKind::Read);
    auto Map = std::find_if(R.Services.begin(), R.Services.end(),
                            [](const auto &S) { return S.Number == 222; });
    ASSERT_NE(Map, R.Services.end());
    ASSERT_TRUE(Map->Result);
    EXPECT_LT(*Map->Result, uint64_t(-4095));
    EXPECT_EQ(Fault.Address, Map->Result);
  }
}

TEST_P(AndroidDevices, ExplicitOpenErrorsAndInactiveProvidersRetainBoundaries) {
  Options.LinuxFiles->Devices.at("/fixture/control").OpenErrno = 13;
  auto R = run("devices_open_error");
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  Options.LinuxFiles->Devices.at("/fixture/control").OpenErrno = 0;
  R = run("devices_sequence", {4});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  Options.LinuxFiles->Devices.at("/fixture/control").AccessMode = 1;
  R = run("devices_sequence", {0});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("open access mode"), std::string::npos);
}

TEST_P(AndroidDevices, CompletePreflightPreservesEveryByteOnRefusal) {
  for (unsigned Mode = 0; Mode != 7; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Memory = {
        {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false}};
    Options.Android->ReadMemory = {{Buffer, 4096}};
    auto &D = Options.LinuxFiles->Devices.at("/fixture/control");
    D.Ioctls = {{0xa1000001, 0, {}, {{0, {}, {0xa5}, {0x42}}}}};
    if (Mode == 0)
      D.Ioctls.clear();
    if (Mode == 1)
      D.Ioctls[0].Request ^= 1;
    if (Mode == 2)
      D.Ioctls[0].Memory[0].Expected = {0xa4};
    if (Mode == 3)
      D.Ioctls[0].Memory.push_back({4096, {}, {}, {0x99}});
    if (Mode == 4)
      D.Ioctls[0].Memory.push_back({0, 8, {}, {0x99}});
    if (Mode == 5)
      D.Ioctls[0].Memory.push_back({0, {}, {0x42}, {0x99}});
    auto R = run(Mode == 6 ? "devices_pending_readonly" : "devices_pending",
                 {0, Buffer});
    ASSERT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
    ASSERT_FALSE(R.Services.empty());
    EXPECT_FALSE(R.Services.back().Result);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(4096, 0xa5));
  }
}

TEST_P(AndroidDevices, PointersAndAliasedInputsAreResolvedBeforeOutputs) {
  std::vector<uint8_t> Bytes(4096, 0xa5);
  llvm::support::endian::write64le(Bytes.data() + 8, Buffer + 64);
  Options.Android->Memory = {{Buffer, 4096, Bytes, false}};
  Options.Android->ReadMemory = {{Buffer, 4096}};
  auto &D = Options.LinuxFiles->Devices.at("/fixture/control");
  D.Ioctls = {{0xa1000001,
               37,
               {},
               {{8, {}, {}, std::vector<uint8_t>(8, 0)},
                {0, 8, {0xa5}, {0x19}},
                {64, {}, {0xa5}, {0x27}}}}};
  auto R = run("devices_pending", {1, Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 37u);
  std::fill(Bytes.begin() + 8, Bytes.begin() + 16, 0);
  Bytes[64] = 0x27;
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Bytes);
}

TEST(AndroidDeviceMemory, RefusedTransactionsDoNotAdvanceTheOpenDescription) {
  ExecutionConfiguration C;
  C.Backend = ExecutionBackendKind::Unicorn;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(C);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Backend = createExecutionBackend(C, 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  constexpr uint64_t Buffer = 0x21000000;
  ASSERT_EQ(llvm::toString(CPU.addressSpace()->map(
                Buffer, 4096, Read | Write | UserAccessible)),
            "");
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, {'/', 'd', 0})), "");
  ProcessOptions Options;
  auto &Files = Options.LinuxFiles.emplace();
  Files.DescriptorLimit = 4;
  Files.Devices["/d"] = {
      2, 0, {{0x87654321, -13, Buffer + 64, {{0, {}, {0xa5}, {0x42}}}}}};
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 4096, Options,
                                    Result);
  auto Open = [&] {
    return llvm::cantFail(
        Kernel.handle(linux_model::ServiceKind::OpenAt,
                      {0, 56, {uint64_t(-100), Buffer, 2, 0, 0, 0}, {}}));
  };
  auto Ioctl = [&](uint64_t Request, uint64_t Argument) {
    return llvm::cantFail(
        Kernel.handle(linux_model::ServiceKind::Ioctl,
                      {0, 29, {3, Request, Argument, 0, 0, 0}, {}}));
  };
  EXPECT_EQ(Open(), 3u);
  EXPECT_EQ(Open(), uint64_t(-24));
  EXPECT_EQ(llvm::cantFail(Kernel.handle(linux_model::ServiceKind::Dup,
                                         {0, 23, {3, 0, 0, 0, 0, 0}, {}})),
            uint64_t(-24));
  EXPECT_FALSE(Ioctl(0x87654320, Buffer + 64));
  EXPECT_FALSE(Ioctl(0x87654321, Buffer + 65));
  EXPECT_FALSE(Ioctl(0x87654321, Buffer + 64));
  ASSERT_EQ(llvm::toString(CPU.write(Buffer + 64, {0xa5})), "");
  EXPECT_EQ(Ioctl(0x1234567887654321ULL, Buffer + 64), uint64_t(-13));
  EXPECT_EQ(llvm::cantFail(CPU.readInteger(Buffer + 64, 1)), 0x42u);
  EXPECT_FALSE(Ioctl(0x87654321, Buffer + 64));
  EXPECT_EQ(llvm::cantFail(CPU.readInteger(Buffer + 64, 1)), 0x42u);
  for (auto Kind :
       {linux_model::ServiceKind::Read, linux_model::ServiceKind::Lseek,
        linux_model::ServiceKind::Write}) {
    EXPECT_FALSE(llvm::cantFail(
        Kernel.handle(Kind, {0, 0, {3, Buffer + 64, 1, 0, 0, 0}, {}})));
    EXPECT_EQ(Result.Diagnostic, linux_model::DeviceOperation);
  }
}

TEST(AndroidDeviceMemory, LegacyGPURefusedCopyoutKeepsAllocationAndFreeState) {
  ExecutionConfiguration C;
  C.Backend = ExecutionBackendKind::Unicorn;
  C.Architecture = GuestArchitecture::AArch64;
  C.Contract = ExecutionContract::CheckedUserAArch64;
  auto Probe = probeExecutionBackend(C);
  ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
  if (Probe->Availability != BackendAvailability::Available)
    GTEST_SKIP() << Probe->Reason;
  auto Backend = createExecutionBackend(C, 4 * 1024 * 1024);
  ASSERT_TRUE(bool(Backend)) << llvm::toString(Backend.takeError());
  auto &CPU = *Backend->CPU;
  auto &Space = *CPU.addressSpace();
  constexpr uint64_t Buffer = 0x21000000;
  ASSERT_EQ(
      llvm::toString(Space.map(Buffer, 8192, Read | Write | UserAccessible)),
      "");
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, {'/', 'd', 0})), "");
  ProcessOptions Options;
  auto D = contextDeviceTestInput();
  D.ReadOnlyMappings[0].Updates.clear();
  D.KGSL->MemoryObjects = true;
  Options.LinuxFiles.emplace().Devices["/d"] = D;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 8192, Options,
                                    Result);
  auto Call = [&](linux_model::ServiceKind Kind, std::array<uint64_t, 6> Args) {
    return llvm::cantFail(Kernel.handle(Kind, {0, 0, Args, {}}));
  };
  auto Ioctl = [&](uint64_t Request, uint64_t Argument) {
    return Call(linux_model::ServiceKind::Ioctl,
                {3, Request, Argument, 0, 0, 0});
  };
  ASSERT_EQ(Call(linux_model::ServiceKind::OpenAt,
                 {uint64_t(-100), Buffer, 2, 0, 0, 0}),
            3u);
  const uint64_t Before = Space.physicalMemory()->allocatedBytes();
  uint8_t Allocation[48]{};
  llvm::support::endian::write64le(Allocation + 8, 4096);
  ASSERT_EQ(llvm::toString(CPU.write(Buffer + 4064, Allocation)), "");
  ASSERT_EQ(
      llvm::toString(Space.protect(Buffer + 4096, 4096, Read | UserAccessible)),
      "");
  ASSERT_FALSE(Ioctl(0xc0300934, Buffer + 4064));
  EXPECT_EQ(Space.physicalMemory()->allocatedBytes(), Before);
  // Inspect the same kernel owner after refusal; this is not process
  // resumption.
  ASSERT_EQ(llvm::toString(Space.protect(Buffer + 4096, 4096,
                                         Read | Write | UserAccessible)),
            "");
  ASSERT_EQ(Ioctl(0xc0300934, Buffer + 4064), 0u);
  ASSERT_EQ(llvm::toString(CPU.read(Buffer + 4064, Allocation)), "");
  EXPECT_EQ(llvm::support::endian::read32le(Allocation), 2u);
  EXPECT_EQ(llvm::support::endian::read64le(Allocation + 24), 0x10000000u);
  EXPECT_EQ(Space.physicalMemory()->allocatedBytes(), Before + 4096);
  auto Mapping = Call(linux_model::ServiceKind::Mmap, {0, 4096, 3, 1, 3, 8192});
  ASSERT_TRUE(Mapping);
  ASSERT_LT(*Mapping, uint64_t(-4095));
  const uint8_t Release[8] = {2, 0, 0, 0, 0x78, 0x56, 0x34, 0x12};
  ASSERT_EQ(llvm::toString(CPU.write(Buffer + 4092, Release)), "");
  ASSERT_EQ(
      llvm::toString(Space.protect(Buffer + 4096, 4096, Read | UserAccessible)),
      "");
  ASSERT_FALSE(Ioctl(0xc0080935, Buffer + 4092));
  ASSERT_EQ(
      Call(linux_model::ServiceKind::Munmap, {*Mapping, 4096, 0, 0, 0, 0}), 0u);
  uint8_t Query[72]{};
  llvm::support::endian::write32le(Query + 8, 2);
  ASSERT_EQ(llvm::toString(CPU.write(Buffer + 64, Query)), "");
  // A refused free must not mark pending: unmapping leaves this ID alive.
  EXPECT_EQ(Ioctl(0xc0480936, Buffer + 64), 0u);
  EXPECT_EQ(Space.physicalMemory()->allocatedBytes(), Before + 4096);
  ASSERT_EQ(llvm::toString(Space.protect(Buffer + 4096, 4096,
                                         Read | Write | UserAccessible)),
            "");
  EXPECT_EQ(Ioctl(0xc0080935, Buffer + 4092), 0u);
  EXPECT_EQ(Ioctl(0xc0080935, Buffer + 4092), uint64_t(-22));
  EXPECT_EQ(Space.physicalMemory()->allocatedBytes(), Before);
}

INSTANTIATE_TEST_SUITE_P(
    Compiled, AndroidDevices,
    testing::Combine(testing::Values("devices-O0-none", "devices-O0-android",
                                     "devices-O0-relr", "devices-O2-none",
                                     "devices-O2-android", "devices-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
