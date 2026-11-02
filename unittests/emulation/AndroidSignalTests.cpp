//===- AndroidSignalTests.cpp - Shared dispositions and distinct ABIs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/android/AndroidInternal.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
constexpr LinuxSignalAction Initial{0x123456789abcdef0, 0x10000004,
                                    0xfedcba9876543210, 0x8000000000000001};

TEST(AndroidSignalState, UnknownMasksAndWaitingRecipientsRemainDistinct) {
  LinuxSignalOptions Options;
  linux_model::LinuxSignals Signals{Options};
  ProcessResult R;
  EXPECT_FALSE(Signals.send(1000, 10, 1000, R));
  EXPECT_NE(R.Diagnostic.find("known thread mask"), std::string::npos);
  EXPECT_FALSE(Signals.mask(1000, 2, 512, true, R));
  ASSERT_TRUE(Signals.mask(1000, 2, 512, false, R));
  Signals.inherit(1000, 1001);
  EXPECT_EQ(Signals.mask(1001, 0, std::nullopt, true, R)->Previous, 512u);
  ASSERT_TRUE(Signals.mask(1001, 2, 0, false, R));
  char ChildState = 'S';
  Signals.setTaskProvider([&] {
    return std::vector<linux_model::ProcTask>{{1000, 'R'}, {1001, ChildState}};
  });
  ASSERT_EQ(Signals.send(1000, 10, 1000, R), 0u);
  EXPECT_TRUE(Signals.ready(1001));
  EXPECT_FALSE(Signals.ready(1000));
  ChildState = 'R';
  ASSERT_EQ(Signals.send(1000, 10, 1000, R), 0u);
  ASSERT_EQ(Signals.send(1000, 10, 1000, R), 0u);
  EXPECT_EQ(Signals.pending(1000, R), 512u);
  EXPECT_EQ(Signals.pending(1001, R), 0u);
  Signals.retire(1001);
  EXPECT_FALSE(Signals.mask(1001, 0, std::nullopt, true, R));
}

class AndroidSignals : public testing::TestWithParam<
                           std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
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
    Options.Android->Memory.push_back(
        {Buffer, 8192, std::vector<uint8_t>(8192, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 8192});
    Options.LinuxSignals.emplace();
    Options.LinuxSignals->Actions = {{11, Initial}, {64, Initial}, {9, {}}};
    Options.InstructionQuantum = 7;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  ProcessResult call(uint64_t Op, uint64_t Signal = 11, uint64_t New = 0,
                     uint64_t Old = 0, uint64_t Size = 8, uint64_t RO = 0) {
    return run("signal_call", {Op, Signal, New, Old, Size, Buffer + 512, RO});
  }
  static void put(std::vector<uint8_t> &Bytes, unsigned At, uint64_t Value) {
    llvm::support::endian::write64le(Bytes.data() + At, Value);
  }
  static void bionic(std::vector<uint8_t> &Bytes, unsigned At,
                     const LinuxSignalAction &Action) {
    llvm::support::endian::write32le(Bytes.data() + At, Action.Flags);
    put(Bytes, At + 8, Action.Handler);
    put(Bytes, At + 16, Action.Mask);
    put(Bytes, At + 24, Action.Restorer);
  }
  static void kernel(std::vector<uint8_t> &Bytes, unsigned At,
                     const LinuxSignalAction &Action) {
    put(Bytes, At, Action.Handler);
    put(Bytes, At + 8, Action.Flags);
    put(Bytes, At + 16, Action.Restorer);
    put(Bytes, At + 24, Action.Mask);
  }
  static void returned(const ProcessResult &R, uint64_t Value,
                       uint32_t Error = 73) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, Value);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(llvm::support::endian::read32le(
                  R.MemorySnapshots[0].Bytes.data() + 512),
              Error);
  }
  std::vector<uint8_t> &input() { return Options.Android->Memory[0].Bytes; }
  ProcessResult sleep(uint64_t Op, uint64_t Mode) {
    input().assign(8192, 0);
    Options.LinuxSignals->InitialMask = 0;
    Options.LinuxSignals->AlternateStackDisabled = true;
    Options.Android->ThreadLimit = 2;
    Options.LinuxTime.emplace();
    Options.LinuxTime->AdvanceOnIdle = true;
    Options.LinuxTime->Clocks[1] = {0, 0};
    return run("signal_sleep", {Op, Mode, Buffer});
  }
  ProcessResult descriptors(uint64_t Op, uint64_t Mode, uint64_t Type = 1) {
    input().assign(8192, 0);
    Options.LinuxSignals->InitialMask = 0;
    Options.LinuxSignals->AlternateStackDisabled = true;
    Options.Android->ThreadLimit = 2;
    Options.Android->TraceLimit = 50000;
    Options.Limits.Instructions = 50000;
    Options.LinuxTime.emplace().AdvanceOnIdle = true;
    Options.LinuxTime->Clocks[1] = {0, 0};
    Options.LinuxFiles.emplace();
    LinuxStreamConnection S;
    S.Peer = {2, 0, 0x23, 0x29, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    S.ReceiveEvents = {{{0x5a}, 0, 0, 5000000}, {{0x6b}, 0, 0, 7000000}};
    Options.LinuxNetwork.emplace().Connections = {S, S};
    Options.LinuxNetwork->Connections[1].ReceiveEvents = {{{0xb6}, 0, 0, 0}};
    LinuxDatagramConnection D;
    D.Peer = S.Peer;
    D.ReceiveEvents = S.ReceiveEvents;
    Options.LinuxNetwork->DatagramConnections = {D};
    return run("signal_descriptors", {Op, Mode, Type, Buffer});
  }
  static bool rawDescriptor(uint64_t Op) {
    return Op == 2 || Op == 6 || Op == 7 || Op == 9 || Op == 12;
  }
  static void descriptorReturned(const ProcessResult &R) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    ASSERT_EQ(R.ReturnValue, 0u);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(word(R, 11), 1u);
    EXPECT_EQ(word(R, 21), 77u);
    // Guarded bytes outside every fixture output must stay untouched.
    const auto &B = R.MemorySnapshots[0].Bytes;
    for (auto [Begin, End] : {std::pair{232, 1024},
                              {1048, 1056},
                              {1072, 1152},
                              {1160, 1184},
                              {1200, 1208},
                              {1216, 1280},
                              {1288, 1360},
                              {1368, 4096},
                              {4128, 6144},
                              {6160, 8192}}) {
      SCOPED_TRACE(Begin);
      EXPECT_TRUE(std::all_of(B.begin() + Begin, B.begin() + End,
                              [](uint8_t V) { return !V; }));
    }
  }
  static uint64_t word(const ProcessResult &R, unsigned Index) {
    return llvm::support::endian::read64le(
        R.MemorySnapshots.at(0).Bytes.data() + Index * 8);
  }
};

TEST_P(AndroidSignals, DescriptorInterruptionKeepsOperationSpecificCopyOut) {
  for (uint64_t Op = 0; Op != 8; ++Op) {
    for (uint64_t Mode : {0, 1}) {
      SCOPED_TRACE(testing::Message() << Op << "/" << Mode);
      const auto R = descriptors(Op, Mode);
      ASSERT_NO_FATAL_FAILURE(descriptorReturned(R));
      EXPECT_EQ(word(R, 0), 1u);
      EXPECT_EQ(word(R, 1), 73u); // errno conversion follows the handler.
      EXPECT_EQ(word(R, 2), uint64_t(-4));
      EXPECT_EQ(word(R, 8), rawDescriptor(Op) ? uint64_t(-4) : UINT64_MAX);
      EXPECT_EQ(word(R, 9), rawDescriptor(Op) ? 55u : 4u);
      EXPECT_EQ(word(R, 7), 1000u);
      EXPECT_EQ(word(R, 10), 1000u);
      const bool RawTimeout = Op == 2 || Op == 3 || Op == 7;
      EXPECT_EQ(word(R, 5), 0u);
      EXPECT_EQ(word(R, 6), RawTimeout ? 19999000u : 20000000u);
      if (Op < 4) {
        EXPECT_EQ(word(R, 4), 0u);
        EXPECT_EQ(word(R, 128), 0x0000000100000003u);
        EXPECT_EQ(word(R, 129), 0x00000001ffffffffu);
      } else if (Op == 7) {
        EXPECT_EQ(word(R, 4), 8u); // pselect6 does not clear the input set.
        EXPECT_EQ(word(R, 130), 8u);
      }
      EXPECT_EQ(word(R, 26), 0xa5a5a5a5u); // epoll output was not touched.
      EXPECT_EQ(word(R, 27), 0xa5a5a5a5a5a5a5a5u);
      EXPECT_EQ(word(R, 12), 1u); // Arrival/one-shot not consumed by EINTR.
      EXPECT_EQ(word(R, 13), Op >= 4 && Op <= 6 ? 0x1234567887654321u : 0x5au);
      if (Op >= 4 && Op <= 6) {
        EXPECT_EQ(word(R, 16), 0u); // First real delivery disables one-shot.
        EXPECT_EQ(word(R, 132), 0xa5a5a5a500000001u); // Preserve ARM padding.
        EXPECT_EQ(word(R, 133), 0x1234567887654321u);
      }
    }
  }
}

TEST_P(AndroidSignals, DescriptorInterruptionOrdersFaultsAndTimeoutCopyOut) {
  for (uint64_t Op = 0; Op != 8; ++Op) {
    for (uint64_t Mode : {12, 13}) {
      SCOPED_TRACE(testing::Message() << Op << "/" << Mode);
      const auto R = descriptors(Op, Mode);
      ASSERT_NO_FATAL_FAILURE(descriptorReturned(R));
      const auto Error = Op < 4 && Mode == 12 ? 14u : 4u;
      EXPECT_EQ(word(R, 2), uint64_t(0) - Error);
      EXPECT_EQ(word(R, 8),
                rawDescriptor(Op) ? uint64_t(0) - Error : UINT64_MAX);
      EXPECT_EQ(word(R, 9), rawDescriptor(Op) ? 55u : Error);
      EXPECT_EQ(word(R, 6), 20000000u); // Unwritable timeout is ignored.
      EXPECT_EQ(word(R, 130), 8u);
      EXPECT_EQ(word(R, 4), Op == 7                 ? 8u
                            : Mode == 12 || Op >= 4 ? 0xa5a5u
                                                    : 0u);
      EXPECT_EQ(word(R, 26), 0xa5a5a5a5u);
      EXPECT_EQ(word(R, 27), 0xa5a5a5a5a5a5a5a5u);
    }
  }
  const auto R = descriptors(0, 18);
  ASSERT_NO_FATAL_FAILURE(descriptorReturned(
      R)); // Infinite poll needs no clock/readiness deadline.
  EXPECT_EQ(word(R, 8), UINT64_MAX);
  EXPECT_EQ(word(R, 9), 4u);
}

TEST_P(AndroidSignals, ReceiveInterruptionPreservesDataAndCapturedTimeout) {
  for (uint64_t Op = 8; Op != 14; ++Op) {
    for (uint64_t Mode : {0, 2, 16}) {
      for (uint64_t Type : {1, 2}) {
        if (Op == 13 && Type == 1)
          continue; // Explicit TCP peer output is outside the contract.
        SCOPED_TRACE(testing::Message() << Op << "/" << Mode << "/" << Type);
        const auto R = descriptors(Op, Mode, Type);
        ASSERT_NO_FATAL_FAILURE(descriptorReturned(R));
        EXPECT_EQ(word(R, 0), 1u);
        EXPECT_EQ(word(R, 1), 73u);
        EXPECT_EQ(word(R, 2), uint64_t(-4));
        EXPECT_EQ(word(R, 8), rawDescriptor(Op) ? uint64_t(-4) : UINT64_MAX);
        EXPECT_EQ(word(R, 9), rawDescriptor(Op) ? 55u : 4u);
        EXPECT_EQ(word(R, 10), 1000u);
        EXPECT_EQ(word(R, 15), 0xa5a5a5a5a5a5a5a5u);
        EXPECT_EQ(word(R, 14), 0xa5a5a5a5a5a5a5a5u);
        EXPECT_EQ(word(R, 148), 0xa5a5a5a5a5a5a5a5u);
        EXPECT_EQ(word(R, 149), 0xa5a5a5a5a5a5a5a5u);
        EXPECT_EQ(word(R, 151), 16u);
        EXPECT_EQ(word(R, 12), 1u);
        EXPECT_EQ(word(R, 13), 0x5au); // First byte is still queued.
      }
    }
  }
}

TEST_P(AndroidSignals, ReceiveRestartReentersOriginalTrapAfterHandlers) {
  for (uint64_t Op = 8; Op != 14; ++Op) {
    for (uint64_t Mode : {1, 10, 11, 15, 19, 22}) {
      SCOPED_TRACE(testing::Message() << Op << "/" << Mode);
      const auto R = descriptors(Op, Mode, Op == 13 ? 2 : 1);
      ASSERT_NO_FATAL_FAILURE(descriptorReturned(R));
      EXPECT_EQ(word(R, 0), Mode == 11 || Mode == 22 ? 2u : 1u);
      EXPECT_EQ(word(R, 1), Mode == 11 || Mode == 22 ? 55u : 73u);
      EXPECT_EQ(word(R, 2), Op == 10 ? 207u : 3u);
      EXPECT_EQ(word(R, 8), 1u);
      EXPECT_EQ(word(R, 9), 55u);
      EXPECT_EQ(word(R, 10), Mode == 19   ? 7000000u
                             : Mode == 10 ? 6001000u
                                          : 5000000u);
      EXPECT_EQ(word(R, 15), 0xa5a5a5a5a5a5a5a5u);
      EXPECT_EQ(word(R, 14),
                Mode == 19 ? 0xa5a5a5a5a5a5a56bu : 0xa5a5a5a5a5a5a55au);
      if (Mode == 19) {
        EXPECT_EQ(word(R, 17), 1u);
        EXPECT_EQ(word(R, 160), 0x5au);
      }
      unsigned Attempts = 0, Completed = 0;
      const char *Name = Op == 8    ? "recv"
                         : Op == 10 ? "syscall"
                         : Op == 11 ? "read"
                                    : "recvfrom";
      auto Check = [&](const auto &Call) {
        if (Call.ThreadID != 1001 || Call.PC != word(R, 3))
          return;
        ++Attempts;
        if (Call.Result) {
          ++Completed;
          EXPECT_EQ(*Call.Result, 1u);
        }
      };
      if (rawDescriptor(Op)) {
        for (const auto &Call : R.Services)
          Check(Call);
      } else {
        for (const auto &Call : R.NativeCalls)
          if (Call.Name == Name)
            Check(Call);
      }
      // A restarted trap executes again. Its first attempt is not a completed
      // EINTR; a nested handler recv has a separate return/caller lifetime.
      const bool SharedImport = Op == 8 && Mode == 19;
      EXPECT_EQ(Attempts, SharedImport || Mode == 22 ? 3u : 2u);
      EXPECT_EQ(Completed, SharedImport ? 2u : 1u);
    }
  }
}

TEST_P(AndroidSignals, ReceiveRestartImportsChangedDescriptorsAndContext) {
  for (uint64_t Op : {8, 9, 10, 11, 12}) {
    for (uint64_t Mode : {3, 4, 8, 20, 21}) {
      if ((Mode == 8 || Mode == 20) && Op == 10)
        continue; // syscall's modeled import arguments include its number.
      SCOPED_TRACE(testing::Message() << Op << "/" << Mode);
      const auto R = descriptors(Op, Mode);
      ASSERT_NO_FATAL_FAILURE(descriptorReturned(R));
      const bool BadFD = Mode == 3 || Mode == 8;
      EXPECT_EQ(word(R, 8),
                BadFD        ? (rawDescriptor(Op) ? uint64_t(-9) : UINT64_MAX)
                : Mode == 21 ? 88u
                             : 1u);
      EXPECT_EQ(word(R, 9), BadFD && !rawDescriptor(Op) ? 9u : 55u);
      EXPECT_EQ(word(R, 10), Mode == 20 ? 5000000u : 1000u);
      EXPECT_EQ(word(R, 14),
                Mode == 4 ? 0xa5a5a5a5a5a5a5b6u : 0xa5a5a5a5a5a5a5a5u);
      if (Mode == 20)
        EXPECT_EQ(word(R, 160), 0x5au);
      if (Mode == 4) {
        EXPECT_EQ(word(R, 17), 0u);
        EXPECT_EQ(word(R, 18), 3u);
        EXPECT_EQ(word(R, 19), 0u);
      }
    }
  }
  const auto R = descriptors(8, 9);
  ASSERT_NO_FATAL_FAILURE(descriptorReturned(R));
  EXPECT_EQ(word(R, 8), 88u);
  EXPECT_EQ(word(R, 9), 55u);
  EXPECT_EQ(word(R, 14), 0xa5a5a5a5a5a5a5a5u);
}

TEST_P(AndroidSignals, DescriptorSignalsRespectDispositionAndReadyData) {
  for (uint64_t Op : {0, 2, 4, 6, 7, 8, 9}) {
    for (uint64_t Mode : {5, 6, 7, 14}) {
      Options.InstructionQuantum = Mode == 14 ? 2048 : 7;
      SCOPED_TRACE(testing::Message() << Op << "/" << Mode);
      const auto R = descriptors(Op, Mode);
      if (Mode == 7) {
        EXPECT_EQ(R.Stop, ProcessStopReason::Signaled) << R.Diagnostic;
        EXPECT_EQ(R.TerminationSignal, 15u);
        EXPECT_EQ(word(R, 0), 0u);
        EXPECT_EQ(word(R, 11), 0u);
        EXPECT_EQ(word(R, 144), 0xa5a5a5a5a5a5a5a5u);
        continue;
      }
      ASSERT_NO_FATAL_FAILURE(descriptorReturned(R));
      EXPECT_EQ(word(R, 0), Mode == 6 ? 0u : 1u);
      EXPECT_EQ(word(R, 8), 1u); // Partial receive returns the available byte.
      EXPECT_EQ(word(R, 9), Mode == 14 ? 55u : 73u);
      EXPECT_EQ(word(R, 10), 5000000u);
    }
  }
}

TEST_P(AndroidSignals,
       SleepInterruptionWritesRemainderBeforeHandlerAndErrnoAfter) {
  for (uint64_t Op : {0, 1, 2, 3}) {
    for (uint64_t Mode : {0, 1, 2, 3}) {
      if (Op == 3 && Mode > 1)
        continue;
      SCOPED_TRACE(Op);
      SCOPED_TRACE(Mode);
      const auto R = sleep(Op, Mode);
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, 0u);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      const uint64_t Error = Mode == 3 ? 14 : 4;
      EXPECT_EQ(word(R, 0), 1u);
      EXPECT_EQ(word(R, 1), 73u);
      EXPECT_EQ(word(R, 2), uint64_t(0) - Error);
      EXPECT_EQ(word(R, 5), 1000u);
      EXPECT_EQ(word(R, 6), 1001u);
      EXPECT_EQ(word(R, 8), Op == 2 ? uint64_t(0) - Error : UINT64_MAX);
      EXPECT_EQ(word(R, 9), Op == 2 ? 55u : Error);
      EXPECT_EQ(word(R, 10), 1000u);
      EXPECT_EQ(word(R, 11), 1u);
      EXPECT_EQ(word(R, 21), 77u);
      if (Mode != 3) {
        EXPECT_EQ(word(R, 3), Op == 3 ? 119u : 2u);
        EXPECT_EQ(word(R, 4), Op == 3 ? 119u : 499999000u);
      }
      // Input is copied once, even if remainder output aliases it.
      EXPECT_EQ(word(R, 32), 2u);
      EXPECT_EQ(word(R, 33), Mode == 2 ? 499999000u : 500000000u);
      EXPECT_EQ(word(R, 34),
                Mode == 0 || Mode == 1 ? (Op == 3 ? 119u : 2u) : 119u);
      if (Op == 2) {
        const auto S =
            std::find_if(R.Services.begin(), R.Services.end(),
                         [](const auto &S) { return S.Number == 101; });
        ASSERT_NE(S, R.Services.end());
        EXPECT_EQ(S->Result, uint64_t(0) - Error);
      }
    }
  }
}

TEST_P(AndroidSignals,
       SleepReturnHonorsMutableContextAndNestedWrapperLifetimes) {
  for (uint64_t Op : {0, 1, 2, 3}) {
    for (uint64_t Mode : {5, 6, 11, 12, 16}) {
      if (Op == 2 && Mode == 12)
        continue; // Native wrapper bypass is a distinct synthetic boundary.
      SCOPED_TRACE(Op);
      SCOPED_TRACE(Mode);
      const auto R = sleep(Op, Mode);
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, 0u);
      EXPECT_EQ(word(R, 0), Mode == 11 || Mode == 16 ? 2u : 1u);
      EXPECT_EQ(word(R, 8), Mode == 5    ? 77u
                            : Mode == 12 ? 88u
                            : Op == 2    ? uint64_t(Mode == 6 ? -22 : -4)
                                         : UINT64_MAX);
      EXPECT_EQ(word(R, 9), Op == 2 || Mode == 5 || Mode == 12 ? 55u
                            : Mode == 6                        ? 22u
                                                               : 4u);
      EXPECT_EQ(word(R, 10), Mode == 11 ? 3000u : 1000u);
      if (Mode == 11) {
        EXPECT_EQ(word(R, 1), 55u);
        EXPECT_EQ(word(R, 7), UINT64_MAX);
        EXPECT_EQ(word(R, 12), 4u);
      }
      if (Mode == 16)
        EXPECT_EQ(word(R, 1), 55u); // No early libc conversion between frames.
      if (Op != 2) {
        const auto Call = std::find_if(
            R.NativeCalls.begin(), R.NativeCalls.end(), [&](const auto &C) {
              return C.ThreadID == 1001 &&
                     (C.Name == "nanosleep" || C.Name == "syscall" ||
                      (C.Name == "usleep" && C.Arguments[0] == 2500000));
            });
        ASSERT_NE(Call, R.NativeCalls.end());
        if (Mode == 12)
          EXPECT_FALSE(Call->Result);
        else
          EXPECT_EQ(Call->Result, word(R, 8));
      }
    }
  }
}

TEST_P(AndroidSignals, IgnoredAndBlockedSignalsDoNotShortenSleep) {
  for (uint64_t Op : {0, 2, 3}) {
    for (uint64_t Mode : {7, 8, 13}) {
      SCOPED_TRACE(Op);
      SCOPED_TRACE(Mode);
      const auto R = sleep(Op, Mode);
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, 0u);
      EXPECT_EQ(word(R, 0), Mode == 7 ? 1u : 0u);
      EXPECT_EQ(word(R, 8), 0u);
      EXPECT_EQ(word(R, 9), 73u);
      EXPECT_EQ(word(R, 10), 2500000000u);
      EXPECT_EQ(word(R, 34), 119u);
      EXPECT_EQ(word(R, 35), 119u);
    }
  }
}

TEST_P(AndroidSignals, SecondSleepRoundsRemainingAfterNestedAndMutableFrames) {
  for (uint64_t Op : {4, 5}) {
    for (uint64_t Mode : {0, 1, 5, 6, 11, 12, 16, 17, 18, 19, 20}) {
      SCOPED_TRACE(testing::Message() << Op << ":" << Mode);
      const auto R = sleep(Op, Mode);
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, 0u);
      const uint64_t Requested = Op == 4 ? 3 : 0xffffffff;
      const uint64_t Expected = Mode == 12 ? 88
                                : Mode == 5 || Mode == 20
                                    ? (Op == 4 ? 0 : 0x80000000)
                                    : Requested - (Mode == 17 || Mode == 19);
      EXPECT_EQ(word(R, 0), Mode == 11 || Mode == 16 ? 2u : 1u);
      EXPECT_EQ(word(R, 1), Mode == 11 || Mode == 16 ? 55u : 73u);
      EXPECT_EQ(word(R, 2), uint64_t(0) - 4);
      EXPECT_EQ(word(R, 8), Expected);
      EXPECT_EQ(word(R, 9), Mode == 5 || Mode == 12 || Mode == 18 || Mode == 20
                                ? 55u
                            : Mode == 6 ? 22u
                                        : 4u);
      EXPECT_EQ(word(R, 10), Mode == 17 || Mode == 18 || Mode == 19
                                 ? 1000000000u
                             : Mode == 11 ? 3000u
                                          : 1000u);
      EXPECT_EQ(word(R, 11), 1u);
      EXPECT_EQ(word(R, 21), 77u);
      EXPECT_EQ(word(R, 34), 119u);
      EXPECT_EQ(word(R, 35), 119u);
      if (Mode == 11) {
        EXPECT_EQ(word(R, 7), 1u);
        EXPECT_EQ(word(R, 12), 4u);
      }
      const auto Call = std::find_if(
          R.NativeCalls.begin(), R.NativeCalls.end(), [&](const auto &C) {
            return C.ThreadID == 1001 && C.Name == "sleep" &&
                   C.Arguments[0] == Requested;
          });
      ASSERT_NE(Call, R.NativeCalls.end());
      if (Mode == 12)
        EXPECT_FALSE(Call->Result);
      else
        EXPECT_EQ(Call->Result, Expected);
    }
  }
}

TEST_P(AndroidSignals, SecondSleepIgnoresBlockedSignalsAndKeepsTermination) {
  for (uint64_t Op : {4, 5}) {
    for (uint64_t Mode : {7, 8, 9, 13}) {
      SCOPED_TRACE(testing::Message() << Op << ":" << Mode);
      const auto R = sleep(Op, Mode);
      ASSERT_EQ(R.Stop, Mode == 9 ? ProcessStopReason::Signaled
                                  : ProcessStopReason::Returned)
          << R.Diagnostic;
      EXPECT_EQ(word(R, 0), Mode == 7 ? 1u : 0u);
      EXPECT_EQ(word(R, 8), Mode == 9 || Op == 4 ? 0u : 0x80000000u);
      EXPECT_EQ(word(R, 9), Mode == 9 ? 0u : 73u);
      EXPECT_EQ(word(R, 10), Mode == 9 ? 0
                             : Op == 4 ? 3000000000ULL
                                       : 2147483647000000000ULL);
      EXPECT_EQ(word(R, 11), Mode == 9 ? 0u : 1u);
      EXPECT_EQ(word(R, 34), 119u);
      EXPECT_EQ(word(R, 35), 119u);
      if (Mode == 9) {
        EXPECT_EQ(R.TerminationSignal, 15u);
        for (const auto &Call : R.NativeCalls)
          if (Call.Name == "sleep")
            EXPECT_FALSE(Call.Result);
      }
    }
  }
}

TEST_P(AndroidSignals, SecondSleepDeadlineWinsBeforePendingHandler) {
  Options.InstructionQuantum = 2048;
  const auto R = sleep(4, 14);
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  EXPECT_EQ(word(R, 0), 1u);
  EXPECT_EQ(word(R, 1), 73u);
  EXPECT_EQ(word(R, 2), 0u);
  EXPECT_EQ(word(R, 8), 0u);
  EXPECT_EQ(word(R, 9), 55u);
  EXPECT_EQ(word(R, 10), 1000000000u);
  EXPECT_EQ(word(R, 34), 119u);
  EXPECT_EQ(word(R, 35), 119u);
}

TEST_P(AndroidSignals, TerminationAndUnsupportedWaitsDoNotFabricateCompletion) {
  for (uint64_t Mode : {4, 9, 10}) {
    SCOPED_TRACE(Mode);
    const auto R = sleep(0, Mode);
    ASSERT_EQ(R.Stop, Mode == 9 ? ProcessStopReason::Signaled
                                : ProcessStopReason::UnsupportedService)
        << R.Diagnostic;
    EXPECT_EQ(word(R, 0), 0u);
    EXPECT_EQ(word(R, 8), 0u);
    EXPECT_EQ(word(R, 11), 0u);
    EXPECT_EQ(word(R, 34), 119u);
    EXPECT_EQ(word(R, 35), 119u);
    EXPECT_EQ(word(R, 511), 0u); // No invented partial remainder stores.
    if (Mode == 9)
      EXPECT_EQ(R.TerminationSignal, 15u);
    else
      EXPECT_NE(R.Diagnostic.find(Mode == 4 ? "partial" : "waiting thread"),
                std::string::npos);
  }
}

TEST_P(AndroidSignals, SleepDeadlineCompletionWinsBeforePendingHandler) {
  // Keep the sender running until join, making both expired sleep and pending
  // signal observable at the receiver's next scheduling boundary.
  Options.InstructionQuantum = 2048;
  for (uint64_t Op : {0, 1, 2, 3}) {
    const auto R = sleep(Op, 14);
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_EQ(word(R, 0), 1u);
    EXPECT_EQ(word(R, 1), 73u);
    EXPECT_EQ(word(R, 2), 0u);
    EXPECT_EQ(word(R, 8), 0u);
    EXPECT_EQ(word(R, 9), 55u);
    EXPECT_EQ(word(R, 10), 1000u);
    EXPECT_EQ(word(R, 34), 119u);
    EXPECT_EQ(word(R, 35), 119u);
  }
}

TEST_P(AndroidSignals, QueryTranslatesBothLayoutsPreservesPaddingAndErrno) {
  for (unsigned Op : {0, 1, 2, 3}) {
    SCOPED_TRACE(Op);
    auto R = call(Op, 0x123456780000000b, 0, Buffer + 8);
    returned(R, 0);
    auto Expected = std::vector<uint8_t>(8192, 0xa5);
    if (Op < 2)
      bionic(Expected, 8, Initial);
    else
      kernel(Expected, 8, Initial);
    llvm::support::endian::write32le(Expected.data() + 512, 73);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  returned(call(0, 64, 0, Buffer), 0);
  returned(call(2, 9, 0, Buffer), 0);
}

TEST_P(AndroidSignals, MissingObservationsNeverBecomeDefaultHandlers) {
  for (bool Enabled : {false, true}) {
    if (Enabled)
      Options.LinuxSignals.emplace();
    else
      Options.LinuxSignals.reset();
    for (unsigned Op : {0, 1, 2, 3}) {
      auto R = call(Op, 11, 0, Buffer);
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(8192, 0xa5));
      returned(call(Op), 0);
    }
  }
}

TEST_P(AndroidSignals, InvalidArgumentsRespectKernelAndBionicBoundaries) {
  for (uint64_t Signal : {uint64_t(0), uint64_t(65), UINT64_MAX}) {
    returned(call(0, Signal), UINT64_MAX, 22);
    returned(call(2, Signal, 0, 1), uint64_t(0) - 22);
    returned(call(2, Signal, 1), uint64_t(0) - 14);
  }
  returned(call(2, 0, 1, 1, 7), uint64_t(0) - 22);
  returned(call(3, 11, 1), UINT64_MAX, 14);
  for (unsigned Op : {0, 1}) {
    auto R = call(Op, 0, 0, Buffer);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("output after a failed"), std::string::npos);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(8192, 0xa5));
    EXPECT_EQ(call(Op, 11, 1).Stop, ProcessStopReason::RuntimeFailure);
  }
}

TEST_P(AndroidSignals, ReplacementsShareStateAndRetainFailedCopySideEffects) {
  auto R = run("signal_sequence", {Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  auto Expected = std::vector<uint8_t>(8192, 0xa5);
  const uint64_t Values[] = {0, 0, uint64_t(0) - 14, 0, 73};
  for (size_t I = 0; I < std::size(Values); ++I)
    put(Expected, I * 8, Values[I]);
  bionic(Expected, 64, Initial);
  const LinuxSignalAction New{0x123456789abc0018, 0x94000004,
                              0xfedcba9876540018, UINT64_MAX};
  bionic(Expected, 128, New);
  kernel(Expected, 192,
         {New.Handler, 0xffffffff94000004, New.Restorer, 0xfffffff87ffbfeff});
  const LinuxSignalAction Last{1, 0x10000001, 0x1122334455667788, UINT64_MAX};
  kernel(Expected, 256, Last);
  bionic(Expected, 320,
         {Last.Handler, Last.Flags, Last.Restorer, 0xfffffffffffbfeff});
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidSignals, OverlappingObjectsCaptureInputBeforeOutput) {
  for (unsigned Op : {0, 2}) {
    const LinuxSignalAction New{1, 0x10000000, 0x1122334455667788, UINT64_MAX};
    input().assign(8192, 0xa5);
    if (Op == 0)
      bionic(input(), 0, New);
    else
      kernel(input(), 0, New);
    auto Expected = input();
    if (Op == 0)
      bionic(Expected, 8, Initial);
    else
      kernel(Expected, 8, Initial);
    llvm::support::endian::write32le(Expected.data() + 512, 73);
    auto R = call(Op, 11, Buffer, Buffer + 8);
    returned(R, 0);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidSignals, KernelOnlyActionsAndUnsupportedFlagsDoNotSucceed) {
  input().assign(8192, 0);
  returned(call(0, 9, Buffer), UINT64_MAX, 22);
  returned(call(2, 19, Buffer), uint64_t(0) - 22);
  for (unsigned Op : {0, 2}) {
    if (Op == 0)
      bionic(input(), 0, {1, 0x400, 0, 0});
    else
      kernel(input(), 0, {1, 0x400, 0, 0});
    auto R = call(Op, 11, Buffer);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("action flags"), std::string::npos);
  }
}

TEST_P(AndroidSignals, OutputFaultsKeepUserStoresDistinctFromKernelCopies) {
  for (unsigned Op : {0, 1, 2, 3}) {
    auto R = call(Op, 11, 0, Buffer + 4096, 8, Buffer + 4096);
    if (Op < 2)
      EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    else
      returned(R, Op == 2 ? uint64_t(0) - 14 : UINT64_MAX, Op == 2 ? 73 : 14);
  }
  auto R = call(2, 11, 0, Buffer + 4080, 8, Buffer + 4096);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("partial Linux signal"), std::string::npos);
}

TEST_P(AndroidSignals, DynamicCallsKeepTheirProviderAndClosedHandleBoundary) {
  Options.Android->Libraries["libsignals-model.so"] = {"sigaction"};
  auto R = run("signal_dynamic", {Buffer, 0});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "sigaction");
  EXPECT_EQ(R.NativeCalls.back().Library, "libsignals-model.so");
  EXPECT_EQ(run("signal_dynamic", {Buffer, 1}).Stop,
            ProcessStopReason::UnsupportedService);
}

TEST_P(AndroidSignals, KillQueryAndInvalidSignalsPreserveWidthErrnoAndMemory) {
  Options.LinuxSignals.reset();
  Options.Android->Libraries["libsignals-model.so"] = {"kill"};
  for (uint64_t Op : {0, 1, 2, 3}) {
    for (uint64_t Signal : {uint64_t(0), uint64_t(65), UINT64_MAX,
                            uint64_t(0x1234567800000000)}) {
      SCOPED_TRACE(Op);
      SCOPED_TRACE(Signal);
      const bool Invalid = Signal == 65 || Signal == UINT64_MAX;
      const uint64_t Value = !Invalid  ? 0
                             : Op == 2 ? uint64_t(-22)
                                       : UINT64_MAX;
      const auto R =
          run("signal_send", {Op, 0x12345678000003e8, Signal, Buffer});
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, Value);
      EXPECT_FALSE(R.TerminationSignal);
      EXPECT_FALSE(R.ExitStatus);
      auto Expected = input();
      put(Expected, 0, 0x11223344);
      put(Expected, 8, Invalid && Op != 2 ? 22 : 73);
      put(Expected, 16, Value);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
    const auto InvalidTarget = run("signal_send", {Op, 0, 65, Buffer});
    EXPECT_EQ(InvalidTarget.Stop, ProcessStopReason::Returned);
    EXPECT_EQ(InvalidTarget.ReturnValue, Op == 2 ? uint64_t(-22) : UINT64_MAX);
    EXPECT_FALSE(InvalidTarget.TerminationSignal);
  }
}

TEST_P(AndroidSignals, DeliveryExecutesHandlersAndRestoresGuestContexts) {
  Options.LinuxSignals->InitialMask = 0;
  Options.LinuxSignals->AlternateStackDisabled = true;
  Options.Android->Libraries["libsignals-model.so"] = {"kill"};
  input().assign(8192, 0);
  for (uint64_t Mode : {0, 1, 2, 3, 5, 6, 7, 11}) {
    for (uint64_t Op : {0, 1, 2, 3}) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Op);
      const auto R = run("signal_delivery", {Op, Mode, Buffer});
      ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
      EXPECT_EQ(R.ReturnValue, 0u);
      auto Expected = input();
      if (Mode != 6) {
        put(Expected, 0, Mode == 2 ? 3 : Mode == 3 ? 2 : 1);
        put(Expected, 8, 10);
        put(Expected, 16, Mode == 2 ? 2048 : 2560);
        put(Expected, 48, Mode == 2 ? 2048 : 0);
        put(Expected, 56, (uint64_t(1000) << 32) | 1000);
        put(Expected, 72, Mode == 2 ? 3 : 1);
        put(Expected, 96, 1);
        put(Expected, 120, 1000);
        EXPECT_TRUE(std::any_of(
            R.Services.begin(), R.Services.end(),
            [](const auto &S) { return S.Number == 139 && !S.Result; }));
      }
      if (Mode == 1 || Mode == 6)
        put(Expected, 80, 512);
      if (Mode == 3)
        put(Expected, 104, 512);
      if (Mode == 5) {
        put(Expected, 40, 4096);
        put(Expected, 64, 0x1234);
      }
      put(Expected, 112, 0x55);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidSignals, DeliveryResetAndDefaultActionsRetainTermination) {
  input().assign(8192, 0);
  Options.LinuxSignals->InitialMask = 0;
  Options.LinuxSignals->AlternateStackDisabled = true;
  auto R = run("signal_delivery", {0, 4, Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Signaled) << R.Diagnostic;
  EXPECT_EQ(R.TerminationSignal, 10u);
  EXPECT_FALSE(R.ReturnValue);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data()),
            1u);
  EXPECT_EQ(
      llvm::support::endian::read64le(R.MemorySnapshots[0].Bytes.data() + 112),
      0u);
  Options.LinuxSignals->Actions[15] = {};
  R = run("signal_send", {2, 1000, 15, Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::Signaled);
  EXPECT_EQ(R.TerminationSignal, 15u);
  for (int Signal : {17, 23, 28}) {
    Options.LinuxSignals->Actions[Signal] = {};
    R = run("signal_send", {2, 1000, uint64_t(Signal), Buffer});
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}

TEST_P(AndroidSignals, DeliveryRejectsUnknownStackAndMalformedReturnFrames) {
  input().assign(8192, 0);
  Options.LinuxSignals->InitialMask = 0;
  auto R = run("signal_delivery", {0, 0, Buffer});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("signal frame"), std::string::npos);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, input());
  Options.LinuxSignals->AlternateStackDisabled = true;
  for (uint64_t Mode : {8, 9, 10}) {
    R = run("signal_delivery", {0, Mode, Buffer});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("signal return frame"), std::string::npos);
    EXPECT_FALSE(R.ReturnValue);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_EQ(R.Services.back().Number, 139u);
    EXPECT_FALSE(R.Services.back().Result);
  }
}

TEST_P(AndroidSignals, DeliveryThreadMasksInheritButPendingRemainsProcessWide) {
  input().assign(8192, 0);
  Options.LinuxSignals->InitialMask = 0;
  Options.LinuxSignals->AlternateStackDisabled = true;
  Options.Android->ThreadLimit = 2;
  const auto R = run("signal_thread_masks", {Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  auto Expected = input();
  for (auto [Index, Value] : {std::pair<unsigned, uint64_t>{0, 1},
                              {1, 10},
                              {2, 2560},
                              {7, (uint64_t(1000) << 32) | 1000},
                              {9, 1},
                              {12, 1},
                              {15, 1001},
                              {16, 512},
                              {17, 77},
                              {18, 512},
                              {20, 1}})
    put(Expected, Index * 8, Value);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[1].Finished);
  EXPECT_EQ(R.NativeThreads[1].ReturnValue, 77u);
}

TEST_P(AndroidSignals, DeliveryMaskABIsPreserveCopyOrderingAndReservedSignals) {
  Options.LinuxSignals->InitialMask = 1ULL << 12;
  for (uint64_t Op : {0, 1, 2, 3}) {
    input().assign(8192, 0);
    const auto Sequence = run("signal_mask_sequence", {Op, Buffer});
    ASSERT_EQ(Sequence.Stop, ProcessStopReason::Returned)
        << Sequence.Diagnostic;
    EXPECT_EQ(Sequence.ReturnValue, 0u);
    auto MaskBytes = input();
    const uint64_t Allowed = UINT64_MAX & ~((1ULL << 8) | (1ULL << 18));
    put(MaskBytes, 0, 4096);
    put(MaskBytes, 8, Op < 2 ? Allowed & ~(0xfULL << 31) : Allowed);
    put(MaskBytes, 16, Allowed & ~(0xfULL << 31) & ~(1ULL << 9));
    put(MaskBytes, 24, 73);
    ASSERT_EQ(Sequence.MemorySnapshots.size(), 1u);
    EXPECT_EQ(Sequence.MemorySnapshots[0].Bytes, MaskBytes);
    input().assign(8192, 0xa5);
    put(input(), 0, UINT64_MAX);
    auto R =
        run("signal_mask_call", {Op, 2, Buffer, Buffer + 8, 8, Buffer + 512});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    auto Expected = input();
    put(Expected, 8, 1ULL << 12);
    llvm::support::endian::write32le(Expected.data() + 512, 73);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    R = run("signal_mask_call", {Op, 99, 0, Buffer + 8, 8, Buffer + 512});
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned);
    EXPECT_EQ(R.ReturnValue, 0u); // how is irrelevant without an input set.
    R = run("signal_mask_call", {Op, 99, Buffer, 0, 8, Buffer + 512});
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned);
    EXPECT_EQ(R.ReturnValue, Op == 2 ? uint64_t(-22) : UINT64_MAX);
    R = run("signal_mask_call", {Op, 2, Buffer, 1, 8, Buffer + 512});
    if (Op == 0)
      EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    else {
      EXPECT_EQ(R.Stop, ProcessStopReason::Returned);
      EXPECT_EQ(R.ReturnValue, Op == 2 ? uint64_t(-14) : UINT64_MAX);
    }
  }
}

TEST_P(AndroidSignals, DeliveryConvenienceFunctionsUseTheSameActionTable) {
  input().assign(8192, 0);
  Options.LinuxSignals->InitialMask = 0;
  Options.LinuxSignals->AlternateStackDisabled = true;
  Options.LinuxSignals->Actions[10] = {};
  const auto R = run("signal_convenience", {Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  auto Expected = input();
  for (auto [Index, Value] : {std::pair<unsigned, uint64_t>{0, 10},
                              {4, 1ULL << 63},
                              {5, 1},
                              {9, UINT64_MAX},
                              {10, UINT64_MAX},
                              {11, 22},
                              {12, UINT64_MAX},
                              {13, 22}})
    put(Expected, Index * 8, Value);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidSignals, DeliveryPreservesIntegerVectorAndFPControlState) {
  input().assign(8192, 0);
  Options.LinuxSignals->InitialMask = 0;
  Options.LinuxSignals->AlternateStackDisabled = true;
  for (uint64_t Mode : {0, 14}) {
    const auto R = run("signal_registers", {Mode, Buffer});
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    auto Expected = input();
    for (auto [Index, Value] : {std::pair<unsigned, uint64_t>{0, 1},
                                {1, 10},
                                {2, 2560},
                                {7, (uint64_t(1000) << 32) | 1000},
                                {9, 1},
                                {12, 1},
                                {15, 1000},
                                {32, Mode == 14 ? 0x987u : 0x988u},
                                {33, 0x654},
                                {34, 0x3c3c3c3c3c3c3c3cULL},
                                {35, 0x3c3c3c3c3c3c3c3cULL},
                                {36, 0x400000},
                                {37, 0x8000000},
                                {38, 0xa0000000}})
      put(Expected, Index * 8, Value);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidSignals, KillTerminatesGuestWithoutAFunctionOrExitReturn) {
  Options.Android->Libraries["libsignals-model.so"] = {"kill"};
  for (bool ExplicitActions : {false, true}) {
    if (ExplicitActions)
      Options.LinuxSignals.emplace();
    else
      Options.LinuxSignals.reset();
    for (uint64_t Op : {0, 1, 2, 3}) {
      SCOPED_TRACE(Op);
      const auto R = run("signal_send",
                         {Op, 0x12345678000003e8, 0x7654321000000009, Buffer});
      ASSERT_EQ(R.Stop, ProcessStopReason::Signaled) << R.Diagnostic;
      EXPECT_EQ(R.TerminationSignal, 9u);
      EXPECT_FALSE(R.ExitStatus);
      EXPECT_FALSE(R.ReturnValue);
      auto Expected = input();
      put(Expected, 0, 0x11223344);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
      if (Op == 2) {
        ASSERT_FALSE(R.Services.empty());
        EXPECT_EQ(R.Services.back().Number, 129u);
        EXPECT_FALSE(R.Services.back().Result);
      } else {
        ASSERT_FALSE(R.NativeCalls.empty());
        EXPECT_EQ(R.NativeCalls.back().Name, Op == 1 ? "syscall" : "kill");
        EXPECT_FALSE(R.NativeCalls.back().Result);
        if (Op == 3)
          EXPECT_EQ(R.NativeCalls.back().Library, "libsignals-model.so");
      }
      auto JSON = llvm::cantFail(llvm::json::parse(processResultJSON(R)));
      EXPECT_EQ(JSON.getAsObject()->getString("stop_reason"), "signaled");
      EXPECT_EQ(JSON.getAsObject()->getInteger("termination_signal"), 9);
      EXPECT_EQ(*JSON.getAsObject()->get("exit_status"),
                llvm::json::Value(nullptr));
    }
  }
}

TEST_P(AndroidSignals, KillDoesNotInventTargetsGroupsMasksOrHandlers) {
  Options.Android->Libraries["libsignals-model.so"] = {"kill"};
  Options.LinuxSignals->Actions[15] = {1, 0, 0, 0};
  for (uint64_t Op : {0, 1, 2, 3}) {
    for (auto [PID, Signal] : {std::pair<uint64_t, uint64_t>{0, 9},
                               {UINT64_MAX, 0},
                               {uint64_t(-1000), 9},
                               {1001, 0},
                               {1001, 9},
                               {1000, 19},
                               {1000, 15},
                               {1000, 64},
                               {1000, 11}}) {
      auto R = run("signal_send", {Op, PID, Signal, Buffer});
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      EXPECT_NE(R.Diagnostic.find(PID == 1000 ? "delivery" : "target"),
                std::string::npos);
      EXPECT_FALSE(R.TerminationSignal);
      EXPECT_FALSE(R.ReturnValue);
      EXPECT_FALSE(R.ExitStatus);
      auto Expected = input();
      put(Expected, 0, 0x11223344);
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
  // Provider lifetime is checked before even invalid syscall arguments.
  const auto Closed = run("signal_send", {4, 1000, 65, Buffer});
  EXPECT_EQ(Closed.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Closed.Diagnostic.find("inactive"), std::string::npos);
  EXPECT_FALSE(Closed.TerminationSignal);
}

TEST_P(AndroidSignals, KillStopsAllLiveThreadsAndKeepsCompletedThreadHistory) {
  Options.LinuxSignals.reset();
  Options.Android->ThreadLimit = 2;
  input().assign(8192, 0);
  for (uint64_t Mode : {0, 1, 2}) {
    for (uint64_t Op : {0, 1, 2}) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Op);
      const auto R = run("signal_threads", {Mode, Op, Buffer});
      ASSERT_EQ(R.Stop, ProcessStopReason::Signaled) << R.Diagnostic;
      EXPECT_EQ(R.TerminationSignal, 9u);
      EXPECT_FALSE(R.ReturnValue);
      EXPECT_FALSE(R.ExitStatus);
      ASSERT_EQ(R.NativeThreads.size(), 2u);
      for (size_t I = 0; I < 2; ++I) {
        const auto &T = R.NativeThreads[I];
        EXPECT_FALSE(T.Waiting);
        if (I == 1 && Mode == 2) {
          EXPECT_TRUE(T.Finished);
          EXPECT_TRUE(T.Retired);
          EXPECT_EQ(T.ReturnValue, 0x7788u);
          EXPECT_FALSE(T.TerminationSignal);
        } else {
          EXPECT_FALSE(T.Finished);
          EXPECT_FALSE(T.ReturnValue);
          EXPECT_EQ(T.TerminationSignal, 9u);
        }
      }
      auto Expected = input();
      put(Expected, 0, 0x11223344);
      put(Expected, 24, 1001);
      if (Mode == 2) {
        put(Expected, 40, 0x5566);
        put(Expected, 48, 0x7788);
      }
      ASSERT_EQ(R.MemorySnapshots.size(), 1u);
      EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
    }
  }
}

TEST_P(AndroidSignals,
       KillAfterEntryReturnPreservesHistoryWithoutDrainingAgain) {
  Options.LinuxSignals.reset();
  Options.Android->ThreadLimit = 2;
  Options.Android->DrainThreads = true;
  Options.LinuxTime.emplace();
  Options.LinuxTime->AdvanceOnIdle = true;
  input().assign(8192, 0);
  const auto R = run("signal_threads", {3, 0, Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Signaled) << R.Diagnostic;
  EXPECT_EQ(R.TerminationSignal, 9u);
  EXPECT_EQ(R.ReturnValue, 42u);
  EXPECT_FALSE(R.ExitStatus);
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[0].Finished);
  EXPECT_EQ(R.NativeThreads[0].ReturnValue, 42u);
  EXPECT_FALSE(R.NativeThreads[0].TerminationSignal);
  EXPECT_FALSE(R.NativeThreads[1].Finished);
  EXPECT_EQ(R.NativeThreads[1].TerminationSignal, 9u);
  auto Expected = input();
  put(Expected, 0, 0x11223344);
  put(Expected, 24, 1001);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST_P(AndroidSignals, KillInsideCallbackDoesNotCompleteSuspendedCaller) {
  Options.LinuxSignals.reset();
  input().assign(8192, 0);
  const auto R = run("signal_callback", {Buffer});
  ASSERT_EQ(R.Stop, ProcessStopReason::Signaled) << R.Diagnostic;
  EXPECT_EQ(R.TerminationSignal, 9u);
  EXPECT_FALSE(R.ReturnValue);
  unsigned Pending = 0;
  for (const auto &Call : R.NativeCalls) {
    if (Call.Name == "pthread_once" || Call.Name == "kill") {
      EXPECT_FALSE(Call.Result);
      ++Pending;
    }
  }
  EXPECT_EQ(Pending, 2u);
  auto Expected = input();
  put(Expected, 0, 0x11223344);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
}

TEST(AndroidSignalMemory,
     EarlierBionicFieldsAndInstalledActionSurviveLaterFault) {
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
  ASSERT_EQ(llvm::toString(CPU.addressSpace()->map(
                Buffer, 4096, Read | Write | UserAccessible)),
            "");
  auto Bytes = std::vector<uint8_t>(4096, 0xa5);
  llvm::support::endian::write32le(Bytes.data(), 4);
  llvm::support::endian::write64le(Bytes.data() + 8, 1);
  llvm::support::endian::write64le(Bytes.data() + 16, 0);
  llvm::support::endian::write64le(Bytes.data() + 24, 0);
  ASSERT_EQ(llvm::toString(CPU.write(Buffer, Bytes)), "");
  ProcessOptions Options;
  Options.Android.emplace();
  Options.LinuxSignals.emplace();
  Options.LinuxSignals->Actions[11] = Initial;
  const linux_model::MemoryLayout Layout{linux_model::UserLimitARM64, 4096};
  ProcessResult Result{ProcessProfile::AndroidNativeAArch64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  linux_model::LinuxServices Kernel(CPU, Layout, Buffer + 4096, Options,
                                    Result);
  const android_model::LinkedImage Linked{};
  auto Budget = llvm::cantFail(ExecutionBudget::create(Options.Limits));
  android_model::Bionic Model(CPU, Kernel, Layout, Options, Result, *Budget,
                              Linked);
  NativeCallEvent Call{};
  Call.Name = "sigaction";
  Call.Arguments = {11, Buffer, Buffer + 4092};
  auto R = Model.invoke(Call);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  llvm::support::endian::write32le(Bytes.data() + 4092, Initial.Flags);
  auto After = std::vector<uint8_t>(4096);
  ASSERT_EQ(llvm::toString(CPU.read(Buffer, After)), "");
  EXPECT_EQ(After, Bytes);
  auto Current = Kernel.signalAction(11, nullptr, true);
  ASSERT_TRUE(Current);
  EXPECT_EQ(Current->Status, 0u);
  EXPECT_EQ(Current->Previous.Handler, 1u);
  EXPECT_EQ(Current->Previous.Flags, 4u);
  EXPECT_EQ(Current->Previous.Mask, 0u);
  EXPECT_EQ(Current->Previous.Restorer, 0u);
}

INSTANTIATE_TEST_SUITE_P(
    Compiled, AndroidSignals,
    testing::Combine(testing::Values("signals-O0-none", "signals-O0-android",
                                     "signals-O0-relr", "signals-O2-none",
                                     "signals-O2-android", "signals-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
