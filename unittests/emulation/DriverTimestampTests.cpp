//===- DriverTimestampTests.cpp - Driver clock instruction semantics ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/CPU.h"
#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ProcessObserver.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <tuple>

namespace neverd::emulation {
namespace {
using Parameter = std::tuple<ExecutionBackendKind, ExecutionContract>;
constexpr uint64_t OtherValue = 0x123456789abcdef0;

class TimestampObserver final : public ProcessObserver {
public:
  std::vector<uint64_t> Offsets;
  std::vector<std::array<uint64_t, 6>> States;
  std::vector<ExecutionWatch> Watches;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    for (uint64_t Offset : Offsets)
      Watches.push_back({P.inputModule()->Entry + Offset, 1});
    return Watches;
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &P, uint64_t) override {
    constexpr X64Register Registers[] = {X64Register::AX,    X64Register::DX,
                                         X64Register::CX,    X64Register::R8,
                                         X64Register::FLAGS, X64Register::SP};
    std::array<uint64_t, 6> Values{};
    for (unsigned I = 0; I < Values.size(); ++I) {
      auto V = P.readRegister(cpuRegister(Registers[I]));
      if (!V)
        return V.takeError();
      Values[I] = (*V)[0];
    }
    States.push_back(Values);
    return Watches;
  }
};

class DriverTimestamp : public testing::TestWithParam<Parameter> {
protected:
  std::filesystem::path Directory;
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
    llvm::SmallString<128> Temporary;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-driver-clock", Temporary));
    Directory = Temporary.str().str();
  }
  void TearDown() override {
    if (!Directory.empty()) {
      std::error_code Ignored;
      std::filesystem::remove_all(Directory, Ignored);
    }
  }
  std::filesystem::path image(llvm::ArrayRef<uint8_t> Code) {
    const auto Input =
        std::filesystem::path(NEVERD_DRIVER_FIXTURES) / "success.sys";
    auto Buffer = llvm::MemoryBuffer::getFile(Input.string());
    if (!Buffer) {
      ADD_FAILURE() << Buffer.getError().message();
      return {};
    }
    auto Object =
        llvm::object::COFFObjectFile::create((*Buffer)->getMemBufferRef());
    if (!Object) {
      ADD_FAILURE() << llvm::toString(Object.takeError());
      return {};
    }
    uintptr_t Entry = 0;
    if (auto E = (*Object)->getRvaPtr(
            (*Object)->getPE32PlusHeader()->AddressOfEntryPoint, Entry)) {
      ADD_FAILURE() << llvm::toString(std::move(E));
      return {};
    }
    const auto Data = (*Buffer)->getBuffer();
    const auto Offset = Entry - reinterpret_cast<uintptr_t>(Data.data());
    if (Offset > Data.size() || Code.size() > Data.size() - Offset) {
      ADD_FAILURE() << "fixture entry has insufficient file bytes";
      return {};
    }
    std::vector<uint8_t> Bytes(Data.bytes_begin(), Data.bytes_end());
    std::copy(Code.begin(), Code.end(), Bytes.begin() + Offset);
    const auto Path = Directory / "clock.sys";
    std::ofstream Out(Path, std::ios::binary);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Out.close();
    EXPECT_TRUE(Out);
    return Path;
  }
  std::vector<uint8_t> program() {
    std::vector<uint8_t> Code;
    auto Move = [&](uint8_t Rex, uint8_t Opcode, uint64_t Value) {
      Code.insert(Code.end(), {Rex, Opcode});
      const size_t At = Code.size();
      Code.resize(At + 8);
      llvm::support::endian::write64le(Code.data() + At, Value);
    };
    Move(0x48, 0xb8, UINT64_MAX); // RAX.
    Move(0x48, 0xba, UINT64_MAX); // RDX.
    Move(0x48, 0xb9, UINT64_MAX); // RCX.
    Move(0x49, 0xb8, OtherValue); // R8.
    // STC; RDTSC; NOP; RDTSCP; NOP; XOR EAX,EAX; RET.
    Code.insert(Code.end(), {0xf9, 0x0f, 0x31, 0x90, 0x0f, 0x01, 0xf9, 0x90,
                             0x31, 0xc0, 0xc3});
    return Code;
  }
};

TEST_P(DriverTimestamp, ReadsOneSchedulerClockAndPreservesOtherState) {
  const auto Path = image(program());
  ASSERT_FALSE(HasFailure());
  for (uint64_t Quantum : {0u, 1u, 3u, 64u})
    for (uint64_t Unit : {uint64_t(7), uint64_t(0x100000001)}) {
      SCOPED_TRACE(Quantum);
      SCOPED_TRACE(Unit);
      Options.Scheduling.reset();
      if (Quantum)
        Options.Scheduling = DriverScheduling{Quantum, Unit};
      TimestampObserver Observer;
      Observer.Offsets = {41, 43, 47};
      auto Run = observeDriver(Path, Options, Observer);
      ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
      ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
      EXPECT_EQ(Run->NTStatus, 0u);
      EXPECT_EQ(Run->Instructions, 11u);
      ASSERT_EQ(Observer.States.size(), 3u);
      EXPECT_EQ(Observer.States[0][0], UINT64_MAX);
      EXPECT_EQ(Observer.States[0][1], UINT64_MAX);
      for (unsigned I = 1; I != 3; ++I) {
        const uint64_t Expected = Quantum ? (I == 1 ? 6 : 8) * Unit : 0;
        EXPECT_EQ(Observer.States[I][0], uint32_t(Expected));
        EXPECT_EQ(Observer.States[I][1], Expected >> 32);
        EXPECT_EQ(Observer.States[I][2], I == 1 ? UINT64_MAX : 0);
        EXPECT_EQ(Observer.States[I][3], OtherValue);
        EXPECT_EQ(Observer.States[I][4], Observer.States[0][4]);
        EXPECT_EQ(Observer.States[I][5], Observer.States[0][5]);
      }
      EXPECT_NE(Observer.States[0][4] & 1, 0u);
    }
}

TEST_P(DriverTimestamp, BudgetStopsBeforeUnadmittedReads) {
  const auto Path = image(program());
  ASSERT_FALSE(HasFailure());
  for (uint64_t Limit : {5u, 6u, 8u}) {
    SCOPED_TRACE(Limit);
    Options.InstructionLimit = Limit;
    Options.Scheduling = DriverScheduling{1, 7};
    TimestampObserver Observer;
    Observer.Offsets = {41, 43, 47};
    auto Run = observeDriver(Path, Options, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::InstructionLimit) << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, Limit);
    ASSERT_EQ(Observer.States.size(), Limit == 5 ? 1u : Limit == 6 ? 2u : 3u);
    EXPECT_EQ(Observer.States[0][0], UINT64_MAX);
    EXPECT_EQ(Observer.States[0][1], UINT64_MAX);
    if (Limit >= 6)
      EXPECT_EQ(Observer.States[1][0], 42u);
    if (Limit >= 8) {
      EXPECT_EQ(Observer.States[2][0], 56u);
      EXPECT_EQ(Observer.States[2][2], 0u);
    }
  }
}

TEST_P(DriverTimestamp, ClockOverflowRemainsAnExplicitFailure) {
  const auto Path = image({0x0f, 0x31, 0x0f, 0x01, 0xf9, 0xc3});
  ASSERT_FALSE(HasFailure());
  Options.Scheduling = DriverScheduling{1, UINT64_MAX};
  TimestampObserver Observer;
  Observer.Offsets = {2, 5};
  auto Run = observeDriver(Path, Options, Observer);
  ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
  EXPECT_EQ(Run->Stop, DriverStopReason::ModelError) << Run->Diagnostic;
  EXPECT_EQ(Run->Diagnostic, "execution virtual time overflows");
  EXPECT_EQ(Run->Instructions, 2u);
  ASSERT_EQ(Observer.States.size(), 1u);
  EXPECT_EQ(Observer.States[0][0], UINT32_MAX);
  EXPECT_EQ(Observer.States[0][1], UINT32_MAX);
}

std::string parameterName(const testing::TestParamInfo<Parameter> &P) {
  const auto &[Backend, Contract] = P.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "_driver" : "_checked");
}
INSTANTIATE_TEST_SUITE_P(
    Native, DriverTimestamp,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64)),
    parameterName);
} // namespace
} // namespace neverd::emulation
