//===- AndroidObservationTests.cpp - Unmodified execution observations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare complete workloads with and without read-only execution watches.
#include "gtest/gtest.h"
#include "os/linux/android/AndroidThreads.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/Limits.h"
#include "neverd/emulation/ProcessReport.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x21000000, Code = 0x22000000;
class AndroidObservation : public testing::TestWithParam<
                               std::tuple<const char *, ExecutionBackendKind>> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android cross fixtures unavailable";
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
    auto &A = *Options.Android;
    A.Initialize = false;
    A.EntrySymbol = "observation_call";
    A.Arguments = {Code, Buffer, 0};
    A.TraceLimit = 4096;
    std::vector<uint8_t> Data(64, 0xc7);
    llvm::support::endian::write64le(Data.data() + 16, 42);
    llvm::support::endian::write64le(Data.data() + 48, 84);
    A.Memory.push_back({Buffer, 4096, Data, false});
    // ldr x1,[x0,#16]; str x1,[x0]; add x1,x1,#1;
    // str x1,[x0,#8]; mov x0,x1; ret
    const uint32_t Instructions[] = {0xf9400801, 0xf9000001, 0x91000421,
                                     0xf9000401, 0xaa0103e0, 0xd65f03c0};
    std::vector<uint8_t> Bytes(sizeof(Instructions));
    for (unsigned I = 0; I < 6; ++I)
      llvm::support::endian::write32le(Bytes.data() + 4 * I, Instructions[I]);
    A.Memory.push_back({Code, 4096, Bytes, true});
    A.ReadMemory = {{Buffer, 64}, {Code, sizeof(Instructions)}};
    Options.LinuxTime.emplace();
    Options.LinuxTime->Clocks[1] = {3, 0};
    Options.LinuxTime->NanosecondsPerInstruction = 7;
#endif
  }
  void observe(uint64_t Limit = 6) {
    Options.Android->ObserveExecution = NativeExecutionObservationOptions{
        {Code, Code + 4, Code + 8, Code + 12, Code + 16, Code + 20},
        Limit,
        {{Buffer, 64}, {Code, 24}}};
  }
  ProcessResult run() {
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
  void sameWorkload(const ProcessResult &Before, ProcessResult After) {
    After.ExecutionObservationsEnabled = false;
    After.CPUState.reset();
    After.ThreadCPUStates.reset();
    EXPECT_EQ(processResultJSON(Before), processResultJSON(After));
  }
};
TEST_P(AndroidObservation, TerminalRegistersPreserveOriginalStopsAndEffects) {
  struct Case {
    const char *Entry;
    ProcessStopReason Stop;
    bool Initialize = false;
    uint64_t Instructions = process_defaults::Instructions;
    uint64_t Events = process_defaults::Events;
  };
  for (const auto &C :
       {Case{"observation_call", ProcessStopReason::Returned},
        Case{"unknown_call", ProcessStopReason::UnsupportedService},
        Case{"stdio_content", ProcessStopReason::CPUFailure},
        Case{"bad_free", ProcessStopReason::RuntimeFailure},
        Case{"busy_loop", ProcessStopReason::InstructionLimit, false, 37},
        Case{"tls_slots", ProcessStopReason::EventLimit, true,
             process_defaults::Instructions, 1}}) {
    SCOPED_TRACE(C.Entry);
    Options.Android->EntrySymbol = C.Entry;
    Options.Android->Initialize = C.Initialize;
    Options.Limits.Instructions = C.Instructions;
    Options.Limits.Events = C.Events;
    Options.Android->ObserveCPUState = false;
    Options.Android->ObserveThreadCPUStates = false;
    const auto Before = run();
    ASSERT_EQ(Before.Stop, C.Stop) << Before.Diagnostic;
    EXPECT_FALSE(Before.CPUState);
    Options.Android->ObserveCPUState = true;
    Options.Android->ObserveThreadCPUStates = true;
    const auto After = run();
    sameWorkload(Before, After);
    ASSERT_TRUE(After.CPUState);
    EXPECT_EQ(After.CPUState->ThreadID, 1000u);
    ASSERT_TRUE(After.CPUState->Registers) << After.CPUState->Diagnostic;
    EXPECT_EQ(After.CPUState->Registers->size(), 71u);
    ASSERT_TRUE(After.ThreadCPUStates);
    if (C.Stop == ProcessStopReason::Returned) {
      EXPECT_TRUE(After.ThreadCPUStates->empty());
    } else {
      ASSERT_EQ(After.ThreadCPUStates->size(), 1u);
      const auto &Thread = After.ThreadCPUStates->front();
      EXPECT_TRUE(Thread.Current);
      EXPECT_EQ(Thread.State.ThreadID, After.CPUState->ThreadID);
      EXPECT_EQ(Thread.State.Registers, After.CPUState->Registers);
      EXPECT_EQ(Thread.State.Diagnostic, "");
    }
  }
}
TEST_P(AndroidObservation,
       SavedThreadRegistersPreserveWaitingOwnersAndOriginalExecution) {
  auto &A = *Options.Android;
  A.EntrySymbol = "observation_threads";
  A.Arguments = {Code, Buffer};
  A.ThreadLimit = 3;
  // mov x19,x0; ldr q31,[x0]; b .
  A.Memory[1].Bytes = {0xf3, 0x03, 0x00, 0xaa, 0x1f, 0x00,
                       0xc0, 0x3d, 0x00, 0x00, 0x00, 0x14};
  Options.Limits.Instructions = 10000;
  for (uint64_t Quantum : {1, 7, 1000}) {
    SCOPED_TRACE(Quantum);
    Options.InstructionQuantum = Quantum;
    A.ObserveCPUState = A.ObserveThreadCPUStates = false;
    const auto Before = run();
    ASSERT_EQ(Before.Stop, ProcessStopReason::InstructionLimit);
    A.ObserveCPUState = A.ObserveThreadCPUStates = true;
    const auto After = run();
    sameWorkload(Before, After);
    ASSERT_TRUE(After.CPUState);
    ASSERT_TRUE(After.ThreadCPUStates);
    ASSERT_EQ(After.ThreadCPUStates->size(), 3u);
    unsigned Current = 0;
    for (const auto &Snapshot : *After.ThreadCPUStates) {
      const auto &State = Snapshot.State;
      ASSERT_TRUE(State.Registers) << State.Diagnostic;
      ASSERT_EQ(State.Registers->size(), 71u);
      const auto Thread =
          std::find_if(After.NativeThreads.begin(), After.NativeThreads.end(),
                       [&](const auto &T) { return T.ID == State.ThreadID; });
      ASSERT_NE(Thread, After.NativeThreads.end());
      EXPECT_FALSE(Thread->Finished);
      if (Snapshot.Current) {
        ++Current;
        EXPECT_EQ(State.ThreadID, After.CPUState->ThreadID);
        EXPECT_EQ(State.Registers, After.CPUState->Registers);
      }
      std::map<CPURegister, RegisterValue> Registers;
      for (const auto &R : *State.Registers)
        Registers.emplace(R.Register, R.Value);
      EXPECT_EQ(Registers.at(CPURegister::AArch64TPIDR_EL0)[0], Thread->TLS);
      EXPECT_GE(Registers.at(CPURegister::AArch64SP)[0], Thread->StackBase);
      EXPECT_LE(Registers.at(CPURegister::AArch64SP)[0],
                Thread->StackBase + Thread->StackSize);
      if (State.ThreadID == 1000) {
        EXPECT_TRUE(Thread->Waiting);
        EXPECT_FALSE(Snapshot.Current);
        continue;
      }
      const auto Offset = (State.ThreadID - 1001) * 32;
      EXPECT_EQ(Registers.at(CPURegister::AArch64X19)[0], Buffer + Offset);
      EXPECT_EQ(Registers.at(CPURegister::AArch64PC)[0], Code + 8);
      EXPECT_EQ(Registers.at(CPURegister::AArch64V31),
                (RegisterValue{llvm::support::endian::read64le(
                                   A.Memory[0].Bytes.data() + Offset),
                               llvm::support::endian::read64le(
                                   A.Memory[0].Bytes.data() + Offset + 8)}));
    }
    EXPECT_EQ(Current, 1u);
  }
  // Returning workers are joined and retired; their old contexts are omitted.
  A.Memory[1].Bytes = {0xc0, 0x03, 0x5f, 0xd6};
  const auto Finished = run();
  ASSERT_EQ(Finished.Stop, ProcessStopReason::Returned);
  ASSERT_TRUE(Finished.ThreadCPUStates);
  EXPECT_TRUE(Finished.ThreadCPUStates->empty());
  ASSERT_EQ(Finished.NativeThreads.size(), 3u);
  for (const auto &Thread : Finished.NativeThreads)
    EXPECT_TRUE(Thread.Finished);
}
TEST_P(AndroidObservation, ThreadRegisterBudgetReservesEveryAllowedOwner) {
  auto &A = *Options.Android;
  A.TraceLimit = 0;
  A.ReadMemory.clear();
  A.ThreadLimit = 3;
  A.ObserveCPUState = A.ObserveThreadCPUStates = true;
  const uint64_t RegisterBytes =
      sizeof(uint64_t) +
      architectureRegisters(GuestArchitecture::AArch64).size() *
          sizeof(RegisterValue);
  Options.OutputLimit =
      RegisterBytes +
      A.ThreadLimit * (RegisterBytes + sizeof(bool) +
                       android_model::thread_model::ReportBytes) -
      1;
  auto Rejected =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
  ++Options.OutputLimit;
  const auto Result = run();
  EXPECT_EQ(Result.Stop, ProcessStopReason::Returned);
  ASSERT_TRUE(Result.CPUState);
  ASSERT_TRUE(Result.ThreadCPUStates);
  EXPECT_TRUE(Result.ThreadCPUStates->empty());
}
TEST_P(AndroidObservation, FailureMemoryPreservesOriginalStopsAndRegisters) {
  auto &A = *Options.Android;
  A.ObserveCPUState = true;
  for (const char *Entry : {"stdio_content", "bad_free"}) {
    SCOPED_TRACE(Entry);
    A.EntrySymbol = Entry;
    A.ObserveMemoryOnFailure = false;
    const auto Before = run();
    ASSERT_TRUE(Before.Stop == ProcessStopReason::CPUFailure ||
                Before.Stop == ProcessStopReason::RuntimeFailure);
    EXPECT_TRUE(Before.MemorySnapshots.empty());
    A.ObserveMemoryOnFailure = true;
    auto After = run();
    ASSERT_EQ(After.MemorySnapshots.size(), 2u);
    for (unsigned I = 0; I != 2; ++I) {
      EXPECT_EQ(After.MemorySnapshots[I].Address, A.ReadMemory[I].Address);
      EXPECT_EQ(After.MemorySnapshots[I].Bytes, A.Memory[I].Bytes);
      EXPECT_TRUE(After.MemorySnapshots[I].Diagnostic.empty());
    }
    After.MemorySnapshots.clear();
    EXPECT_EQ(processResultJSON(After), processResultJSON(Before));
  }
}
TEST_P(AndroidObservation,
       FailureMemoryErrorsRemainSeparateAndDoNotEndCapture) {
  auto &A = *Options.Android;
  A.EntrySymbol = "bad_free";
  A.ObserveCPUState = true;
  A.ReadMemory.insert(A.ReadMemory.begin(), {0x23000000, 16, false});
  const auto Before = run();
  ASSERT_EQ(Before.Stop, ProcessStopReason::RuntimeFailure);
  A.ObserveMemoryOnFailure = true;
  auto After = run();
  ASSERT_EQ(After.MemorySnapshots.size(), 3u);
  const auto &Missing = After.MemorySnapshots.front();
  EXPECT_EQ(Missing.Address, 0x23000000u);
  EXPECT_TRUE(Missing.Bytes.empty());
  EXPECT_FALSE(Missing.Diagnostic.empty());
  EXPECT_LE(Missing.Diagnostic.size(),
            ::neverd::limits::emulation::kAndroidFailureMemoryDiagnosticBytes);
  for (unsigned I = 1; I != 3; ++I) {
    EXPECT_EQ(After.MemorySnapshots[I].Bytes, A.Memory[I - 1].Bytes);
    EXPECT_TRUE(After.MemorySnapshots[I].Diagnostic.empty());
  }
  After.MemorySnapshots.clear();
  EXPECT_EQ(processResultJSON(After), processResultJSON(Before));
}
TEST_P(AndroidObservation,
       FailureMemoryReservesItsDiagnosticBudgetBeforeEntry) {
  auto &A = *Options.Android;
  A.EntrySymbol = "bad_free";
  A.TraceLimit = 0;
  A.ObserveMemoryOnFailure = true;
  A.ReadMemory = {{Buffer, 64}};
  Options.OutputLimit =
      64 + ::neverd::limits::emulation::kAndroidFailureMemoryDiagnosticBytes -
      1;
  auto Invalid =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("snapshot"),
            std::string::npos);
  ++Options.OutputLimit;
  const auto Valid = run();
  EXPECT_EQ(Valid.Stop, ProcessStopReason::RuntimeFailure) << Valid.Diagnostic;
  ASSERT_EQ(Valid.MemorySnapshots.size(), 1u);
  EXPECT_EQ(Valid.MemorySnapshots.front().Bytes, A.Memory.front().Bytes);
}
TEST(AndroidObservationOptions, FailureMemoryFlagIsStrictAndOptional) {
  auto Default = processOptionsFromJSON(R"({"android":{}})");
  ASSERT_TRUE(bool(Default)) << llvm::toString(Default.takeError());
  EXPECT_FALSE(Default->Android->ObserveMemoryOnFailure);
  for (const char *Text : {"true", "false"}) {
    auto O = processOptionsFromJSON(
        std::string("{\"android\":{\"observe_memory_on_failure\":") + Text +
        "}}");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    EXPECT_EQ(O->Android->ObserveMemoryOnFailure,
              llvm::StringRef(Text) == "true");
  }
  for (const char *Text : {"null", "0", "\"true\"", "{}", "[]"}) {
    auto O = processOptionsFromJSON(
        std::string("{\"android\":{\"observe_memory_on_failure\":") + Text +
        "}}");
    EXPECT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
}
TEST_P(AndroidObservation, TerminalRegistersRetainEveryIntegerAndVectorLane) {
  auto &A = *Options.Android;
  auto &Data = A.Memory[0].Bytes;
  auto &Bytes = A.Memory[1].Bytes;
  Data.assign(2048, 0);
  Bytes.clear();
  auto Emit = [&](uint32_t Word) {
    const auto Offset = Bytes.size();
    Bytes.resize(Offset + 4);
    llvm::support::endian::write32le(Bytes.data() + Offset, Word);
  };
  constexpr uint64_t Integer = 0xfedcba9876540000ull;
  constexpr uint64_t Low = 0x0123456789abcdefull;
  constexpr uint64_t High = 0xfedcba9876543210ull;
  for (unsigned I = 0; I < 31; ++I)
    llvm::support::endian::write64le(Data.data() + 8 * I, Integer + I);
  for (unsigned I = 0; I < 32; ++I) {
    llvm::support::endian::write64le(Data.data() + 0x100 + 16 * I, Low + I);
    llvm::support::endian::write64le(Data.data() + 0x108 + 16 * I, High - I);
    Emit(0x3dc00000 | ((0x10 + I) << 10) | I); // ldr qI,[x0,#offset]
  }
  const uint32_t Controls[] = {0xa0000000, 0x00400000, 0x0800001f};
  const uint32_t MSR[] = {0xd51b4201, 0xd51b4401, 0xd51b4421};
  for (unsigned I = 0; I < 3; ++I) {
    llvm::support::endian::write64le(Data.data() + 0x600 + I * 8, Controls[I]);
    Emit(0xf9400001 | ((0xc0 + I) << 10)); // ldr x1,[x0,#offset]
    Emit(MSR[I]);                          // msr nzcv/fpcr/fpsr,x1
  }
  for (unsigned I = 1; I < 31; ++I)
    Emit(0xf9400000 | (I << 10) | I); // ldr xI,[x0,#I*8]
  Emit(0xf9400000);                   // ldr x0,[x0]
  const uint64_t StopPC = Code + Bytes.size();
  Emit(0xd4000001); // svc #0: x8 names an unknown, authored syscall
  A.ReadMemory = {{Buffer, Data.size()}, {Code, Bytes.size()}};
  const auto Before = run();
  ASSERT_EQ(Before.Stop, ProcessStopReason::UnsupportedService)
      << Before.Diagnostic;
  A.ObserveCPUState = true;
  const auto After = run();
  sameWorkload(Before, After);
  ASSERT_TRUE(After.CPUState);
  ASSERT_TRUE(After.CPUState->Registers) << After.CPUState->Diagnostic;
  std::map<CPURegister, RegisterValue> Registers;
  for (const auto &R : *After.CPUState->Registers)
    Registers.emplace(R.Register, R.Value);
  ASSERT_EQ(Registers.size(), 71u);
  for (unsigned I = 0; I < 31; ++I)
    EXPECT_EQ(Registers.at(CPURegister(unsigned(CPURegister::AArch64X0) + I)),
              (RegisterValue{Integer + I, 0}));
  for (unsigned I = 0; I < 32; ++I)
    EXPECT_EQ(Registers.at(vectorRegister(GuestArchitecture::AArch64, I)),
              (RegisterValue{Low + I, High - I}));
  EXPECT_EQ(Registers.at(CPURegister::AArch64PC), (RegisterValue{StopPC, 0}));
  EXPECT_EQ(Registers.at(CPURegister::AArch64NZCV),
            (RegisterValue{Controls[0], 0}));
  EXPECT_EQ(Registers.at(CPURegister::AArch64FPCR),
            (RegisterValue{Controls[1], 0}));
  EXPECT_EQ(Registers.at(CPURegister::AArch64FPSR),
            (RegisterValue{Controls[2], 0}));
  ASSERT_FALSE(After.Services.empty());
  EXPECT_FALSE(After.Services.back().Result);
}
TEST_P(AndroidObservation, TerminalRegistersBelongToTheActualStoppedChild) {
  auto &A = *Options.Android;
  A.EntrySymbol = "observation_threads";
  A.Arguments = {Code, Buffer};
  A.ThreadLimit = 3;
  // mov x8,#65535; svc #0
  A.Memory[1].Bytes = {0xe8, 0xff, 0x9f, 0xd2, 1, 0, 0, 0xd4};
  for (uint64_t Quantum : {1, 7, 1000}) {
    SCOPED_TRACE(Quantum);
    Options.InstructionQuantum = Quantum;
    A.ObserveCPUState = false;
    const auto Before = run();
    ASSERT_EQ(Before.Stop, ProcessStopReason::UnsupportedService);
    A.ObserveCPUState = true;
    const auto After = run();
    sameWorkload(Before, After);
    ASSERT_TRUE(After.CPUState);
    ASSERT_TRUE(After.CPUState->Registers);
    EXPECT_NE(After.CPUState->ThreadID, 1000u);
    ASSERT_FALSE(After.Services.empty());
    EXPECT_EQ(After.Services.back().ThreadID, After.CPUState->ThreadID);
    const auto Thread = std::find_if(
        After.NativeThreads.begin(), After.NativeThreads.end(),
        [&](const auto &T) { return T.ID == After.CPUState->ThreadID; });
    ASSERT_NE(Thread, After.NativeThreads.end());
    for (const auto &R : *After.CPUState->Registers)
      if (R.Register == CPURegister::AArch64TPIDR_EL0)
        EXPECT_EQ(R.Value, (RegisterValue{Thread->TLS, 0}));
  }
}
TEST_P(AndroidObservation, TerminalRegisterBudgetIsReservedBeforeExecution) {
  auto &A = *Options.Android;
  A.TraceLimit = 0;
  A.ReadMemory.clear();
  A.ObserveCPUState = true;
  Options.OutputLimit = 8 + 71 * 16 - 1;
  auto Rejected =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
  ++Options.OutputLimit;
  const auto R = run();
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned);
  ASSERT_TRUE(R.CPUState);
  ASSERT_TRUE(R.CPUState->Registers);
}
TEST_P(AndroidObservation, ExactBeforeInstructionStateAndUnchangedWorkload) {
  for (uint64_t Quantum : {1, 2, 3, 1000}) {
    SCOPED_TRACE(Quantum);
    Options.InstructionQuantum = Quantum;
    Options.Android->ObserveExecution.reset();
    const auto Baseline = run();
    ASSERT_EQ(Baseline.Stop, ProcessStopReason::Returned);
    ASSERT_EQ(Baseline.ReturnValue, 43u);
    observe();
    auto R = run();
    ASSERT_EQ(R.ExecutionObservations.size(), 6u);
    EXPECT_FALSE(R.ExecutionObservationsTruncated);
    sameWorkload(Baseline, R);
    for (unsigned I = 0; I < 6; ++I) {
      const auto &O = R.ExecutionObservations[I];
      EXPECT_EQ(O.PC, Code + I * 4);
      EXPECT_EQ(O.Registers[0], I < 5 ? Buffer : 43);
      EXPECT_EQ(O.Registers[1], I == 0 ? 0 : I < 3 ? 42 : 43);
      EXPECT_EQ(O.SP % 16, 0u);
      EXPECT_EQ(O.NZCV & ~uint64_t(0xf0000000), 0u);
      EXPECT_EQ(O.ThreadID, 1000u);
      ASSERT_EQ(O.Memory.size(), 2u);
      auto Expected = Options.Android->Memory[0].Bytes;
      if (I >= 2)
        llvm::support::endian::write64le(Expected.data(), 42);
      if (I >= 4)
        llvm::support::endian::write64le(Expected.data() + 8, 43);
      EXPECT_EQ(O.Memory[0].Bytes, Expected);
      EXPECT_EQ(O.Memory[1].Bytes, Options.Android->Memory[1].Bytes);
      EXPECT_EQ(R.Trace[O.Instructions], O.PC);
    }
    auto JSON = llvm::cantFail(llvm::json::parse(processResultJSON(R)));
    const auto *A = JSON.getAsObject()->getObject("android");
    ASSERT_NE(A, nullptr);
    ASSERT_EQ(A->getArray("execution_observations")->size(), 6u);
    EXPECT_EQ(A->getBoolean("execution_observations_truncated"), false);
  }
}
TEST_P(AndroidObservation, TruncationRequiresAnAdditionalMatch) {
  auto Baseline = run();
  for (uint64_t Limit : {1, 5, 6, 7}) {
    observe(Limit);
    auto R = run();
    EXPECT_EQ(R.ExecutionObservations.size(), std::min(Limit, uint64_t(6)));
    EXPECT_EQ(R.ExecutionObservationsTruncated, Limit < 6);
    sameWorkload(Baseline, R);
  }
}
TEST_P(AndroidObservation, ThreadIdentityAndQuantumOrderArePreserved) {
  Options.Android->EntrySymbol = "observation_threads";
  Options.Android->Arguments = {Code, Buffer};
  Options.Android->ThreadLimit = 3;
  for (uint64_t Quantum : {1, 2, 3, 7}) {
    SCOPED_TRACE(Quantum);
    Options.InstructionQuantum = Quantum;
    Options.Android->ObserveExecution.reset();
    auto Baseline = run();
    ASSERT_EQ(Baseline.ReturnValue, 126u) << Baseline.Diagnostic;
    observe(12);
    auto R = run();
    ASSERT_EQ(R.ExecutionObservations.size(), 12u);
    EXPECT_FALSE(R.ExecutionObservationsTruncated);
    sameWorkload(Baseline, R);
    std::map<uint64_t, std::vector<uint64_t>> PCs;
    for (const auto &O : R.ExecutionObservations) {
      PCs[O.ThreadID].push_back(O.PC);
      EXPECT_EQ(R.Trace[O.Instructions], O.PC);
    }
    ASSERT_EQ(PCs.size(), 2u);
    for (const auto &[ID, Sequence] : PCs) {
      EXPECT_NE(ID, 1000u);
      EXPECT_EQ(Sequence, Options.Android->ObserveExecution->PCs);
    }
  }
}
TEST_P(AndroidObservation, UnknownServiceIsNotSkippedOrGivenAReturn) {
  Options.Android->EntrySymbol = "unknown_call";
  Options.Android->Arguments.clear();
  auto Baseline = run();
  ASSERT_EQ(Baseline.Stop, ProcessStopReason::UnsupportedService);
  ASSERT_FALSE(Baseline.NativeCalls.empty());
  const uint64_t PC = Baseline.NativeCalls.back().PC;
  observe();
  Options.Android->ObserveExecution->PCs = {PC};
  auto R = run();
  ASSERT_EQ(R.ExecutionObservations.size(), 1u);
  EXPECT_EQ(R.ExecutionObservations[0].Registers[0], 19u);
  EXPECT_FALSE(R.NativeCalls.back().Result);
  sameWorkload(Baseline, R);
}
TEST_P(AndroidObservation, ExplicitMemoryMustBeReadableAtItsObservation) {
  observe();
  auto &Reads = Options.Android->ObserveExecution->ReadMemory;
  Reads = {{0x23000000, 8, false}};
  auto R = run();
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_EQ(R.PC, Code);
  EXPECT_TRUE(R.ExecutionObservations.empty());
  EXPECT_EQ(R.Diagnostic.find("observation memory is not readable") !=
                std::string::npos,
            true);
  Reads[0].RequireMappedAtEntry = true;
  auto Rejected =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
}
TEST_P(AndroidObservation, WholeObservationBudgetIsReservedBeforeExecution) {
  observe();
  auto &O = *Options.Android->ObserveExecution;
  O.Limit = UINT64_MAX;
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  O.Limit = 1;
  O.ReadMemory = {{0, Options.OutputLimit}};
  R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
}
TEST_P(AndroidObservation, RegisterReadsFollowEachThreadsCurrentPointer) {
  Options.Android->EntrySymbol = "observation_threads";
  Options.Android->Arguments = {Code, Buffer};
  Options.Android->ThreadLimit = 3;
  for (uint64_t Quantum : {1, 7}) {
    Options.InstructionQuantum = Quantum;
    Options.Android->ObserveExecution.reset();
    const auto Baseline = run();
    ASSERT_EQ(Baseline.ReturnValue, 126u);
    observe(10);
    auto &O = *Options.Android->ObserveExecution;
    O.PCs.pop_back(); // x0 becomes an integer result after the last move.
    O.ReadRegisterMemory = {{0, 16, 8}, {0, 0, 16}};
    auto R = run();
    ASSERT_EQ(R.ExecutionObservations.size(), 10u);
    sameWorkload(Baseline, R);
    std::set<uint64_t> Addresses;
    for (const auto &Record : R.ExecutionObservations) {
      ASSERT_EQ(Record.Memory.size(), 4u);
      uint64_t Address = Record.Registers[0];
      Addresses.insert(Address);
      EXPECT_EQ(Record.Memory[2].Address, Address + 16);
      EXPECT_EQ(llvm::support::endian::read64le(Record.Memory[2].Bytes.data()),
                Address == Buffer ? 42u : 84u);
      EXPECT_EQ(Record.Memory[3].Address, Address);
      const auto Begin = Record.Memory[0].Bytes.begin() + (Address - Buffer);
      EXPECT_EQ(Record.Memory[3].Bytes,
                std::vector<uint8_t>(Begin, Begin + 16));
    }
    EXPECT_EQ(Addresses, (std::set<uint64_t>{Buffer, Buffer + 32}));
  }
}
TEST_P(AndroidObservation, SignedOffsetsAndStackUseStoppedState) {
  // sub sp,sp,#16; str x0,[sp]; add x0,x0,#8; add sp,sp,#16; ret.
  const uint32_t Words[] = {0xd10043ff, 0xf90003e0, 0x91002000, 0x910043ff,
                            0xd65f03c0};
  auto &Bytes = Options.Android->Memory[1].Bytes;
  Bytes.resize(sizeof(Words));
  for (unsigned I = 0; I < 5; ++I)
    llvm::support::endian::write32le(Bytes.data() + I * 4, Words[I]);
  auto Baseline = run();
  ASSERT_EQ(Baseline.ReturnValue, Buffer + 8);
  observe(1);
  auto &O = *Options.Android->ObserveExecution;
  O.PCs = {Code + 12};
  O.ReadRegisterMemory = {{0, -8, 24}, {31, 0, 8}};
  auto R = run();
  sameWorkload(Baseline, R);
  ASSERT_EQ(R.ExecutionObservations.size(), 1u);
  const auto &Record = R.ExecutionObservations.front();
  ASSERT_EQ(Record.Memory.size(), 4u);
  EXPECT_EQ(Record.Memory[2].Address, Buffer);
  EXPECT_EQ(
      Record.Memory[2].Bytes,
      (std::vector<uint8_t>(Options.Android->Memory[0].Bytes.begin(),
                            Options.Android->Memory[0].Bytes.begin() + 24)));
  EXPECT_EQ(Record.Memory[3].Address, Record.SP);
  EXPECT_EQ(llvm::support::endian::read64le(Record.Memory[3].Bytes.data()),
            Buffer);
}
TEST_P(AndroidObservation,
       AllocatedPointerNeedsNoEntryMappingOrRetainedLifetime) {
  Options.Android->EntrySymbol = "observation_allocated";
  Options.Android->Arguments[2] = 42;
  Options.Android->MallocFillByte = 0xa5;
  const auto Baseline = run();
  ASSERT_EQ(Baseline.ReturnValue, 43u) << Baseline.Diagnostic;
  observe(5);
  auto &O = *Options.Android->ObserveExecution;
  O.PCs.pop_back();
  O.ReadRegisterMemory = {{0, 0, 24}};
  auto R = run();
  sameWorkload(Baseline, R);
  ASSERT_EQ(R.ExecutionObservations.size(), 5u);
  auto Allocation =
      std::find_if(R.NativeCalls.begin(), R.NativeCalls.end(),
                   [](const auto &Call) { return Call.Name == "malloc"; });
  ASSERT_NE(Allocation, R.NativeCalls.end());
  ASSERT_TRUE(Allocation->Result);
  for (unsigned I = 0; I < 5; ++I) {
    const auto &Record = R.ExecutionObservations[I];
    ASSERT_EQ(Record.Memory.size(), 3u);
    EXPECT_EQ(Record.Memory[2].Address, *Allocation->Result);
    std::vector<uint8_t> Expected(24, 0xa5);
    llvm::support::endian::write64le(Expected.data() + 16, 42);
    if (I >= 2)
      llvm::support::endian::write64le(Expected.data(), 42);
    if (I >= 4)
      llvm::support::endian::write64le(Expected.data() + 8, 43);
    EXPECT_EQ(Record.Memory[2].Bytes, Expected);
  }
}
TEST_P(AndroidObservation, DynamicAddressFailuresStopBeforeTheInstruction) {
  observe(1);
  auto &O = *Options.Android->ObserveExecution;
  O.PCs = {Code};
  for (auto Read : {NativeRegisterMemoryRead{1, -1, 8},
                    {0, INT64_MIN, 8},
                    {1, 1, 8},
                    {1, 0, 8},
                    {0, 4088, 16},
                    {1, 0, UINT64_MAX}}) {
    Options.Android->Arguments[2] = Read.Offset < 0 ? 0 : UINT64_MAX;
    O.ReadRegisterMemory = {Read};
    if (Read.Size == UINT64_MAX) {
      auto Rejected =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      EXPECT_FALSE(bool(Rejected));
      llvm::consumeError(Rejected.takeError());
      continue;
    }
    auto R = run();
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_EQ(R.PC, Code);
    EXPECT_TRUE(R.ExecutionObservations.empty());
    EXPECT_NE(R.Diagnostic.find("observation memory is not readable"),
              std::string::npos);
  }
  O.ReadRegisterMemory = {{32, 0, 8}};
  auto Rejected =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
  O.ReadRegisterMemory = {{0, 0, Options.OutputLimit}};
  Rejected =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Rejected));
  llvm::consumeError(Rejected.takeError());
}
TEST(AndroidObservationOptions, RegisterMemoryIsExplicitAndStrict) {
  auto Parse = [](const std::string &Reads) {
    return processOptionsFromJSON(
        R"({"android":{"observe_execution":{"pcs":[4096],"limit":2,"read_register_memory":)" +
        Reads + "}}}");
  };
  auto Good = Parse(
      R"([{"register":"x0","offset":-8,"size":8},{"register":"x30","offset":9223372036854775807,"size":"0x10"},{"register":"sp","offset":-9223372036854775808,"size":1}])");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  const auto &Reads = Good->Android->ObserveExecution->ReadRegisterMemory;
  ASSERT_EQ(Reads.size(), 3u);
  EXPECT_EQ(Reads[0].Offset, -8);
  EXPECT_EQ(Reads[1].Register, 30u);
  EXPECT_EQ(Reads[1].Offset, INT64_MAX);
  EXPECT_EQ(Reads[2].Register, 31u);
  EXPECT_EQ(Reads[2].Offset, INT64_MIN);
  for (const char *Bad :
       {"null", "{}", "[null]", "[{}]", R"([{"register":"x31","size":8}])",
        R"([{"register":"x01","size":8}])", R"([{"register":"xzr","size":8}])",
        R"([{"register":1,"size":8}])", R"([{"register":"x0","size":0}])",
        R"([{"register":"x0","size":-1}])",
        R"([{"register":"x0","size":1,"offset":"-1"}])",
        R"([{"register":"x0","size":1,"offset":1.5}])",
        R"([{"register":"x0","size":1,"offset":9223372036854775808}])",
        R"([{"register":"x0","size":1,"offset":9.223372036854776e18}])",
        R"([{"register":"x0","size":1,"offset":-1e20}])",
        R"([{"register":"x0","size":1,"offset":false}])",
        R"([{"register":"x0","size":1,"address":1}])",
        R"([{"register":"x0","size":1,"require_mapped_at_entry":false}])"}) {
    auto R = Parse(Bad);
    EXPECT_FALSE(bool(R)) << Bad;
    llvm::consumeError(R.takeError());
  }
}
TEST(AndroidObservationOptions, StrictShapeAndLosslessAddresses) {
  auto Parse = [](const char *Value) {
    return processOptionsFromJSON(
        std::string("{\"android\":{\"observe_execution\":") + Value + "}}");
  };
  auto Good = Parse(
      R"({"pcs":["0xfffffffffffffffc",4096],"limit":3,"read_memory":[{"address":"0x21000000","size":8,"require_mapped_at_entry":false}]})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->Android->ObserveExecution->PCs[0], UINT64_MAX - 3);
  EXPECT_FALSE(
      Good->Android->ObserveExecution->ReadMemory[0].RequireMappedAtEntry);
  for (
      const char *Bad :
      {"null", "[]", "{}", R"({"pcs":[],"limit":1})",
       R"({"pcs":[4096],"limit":0})", R"({"pcs":[4097],"limit":1})",
       R"({"pcs":[4096,4096],"limit":1})", R"({"pcs":[true],"limit":1})",
       R"({"pcs":[4096],"limit":1,"extra":0})",
       R"({"pcs":[4096],"limit":1,"read_memory":[{"address":1,"size":0}]})",
       R"({"pcs":[4096],"limit":1,"read_memory":[{"address":"0xffffffffffffffff","size":2}]})",
       R"({"pcs":[4096],"limit":1,"read_memory":[{"address":1,"size":8,"bytes_hex":"00"}]})"}) {
    auto R = Parse(Bad);
    EXPECT_FALSE(bool(R)) << Bad;
    llvm::consumeError(R.takeError());
  }
}
INSTANTIATE_TEST_SUITE_P(
    Relocations, AndroidObservation,
    testing::Combine(testing::Values("none", "android", "relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF)));
} // namespace
} // namespace neverd::emulation
