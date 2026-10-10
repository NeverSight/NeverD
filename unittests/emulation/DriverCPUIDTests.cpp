//===- DriverCPUIDTests.cpp - Declared driver CPU identity ------===//
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

class CPUIDObserver final : public ProcessObserver {
public:
  std::vector<uint64_t> Offsets;
  std::vector<std::array<uint64_t, 7>> States;
  std::vector<ExecutionWatch> Watches;
  llvm::Expected<std::vector<ExecutionWatch>> started(ProcessView &P) override {
    for (uint64_t Offset : Offsets)
      Watches.push_back({P.inputModule()->Entry + Offset, 1});
    return Watches;
  }
  llvm::Expected<std::optional<std::vector<ExecutionWatch>>>
  watched(ProcessView &P, uint64_t) override {
    constexpr X64Register Registers[] = {
        X64Register::AX, X64Register::BX,    X64Register::CX, X64Register::DX,
        X64Register::R8, X64Register::FLAGS, X64Register::SP};
    std::array<uint64_t, 7> Values{};
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

class DriverCPUIDExecution : public testing::TestWithParam<Parameter> {
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
        llvm::sys::fs::createUniqueDirectory("neverd-driver-cpuid", Temporary));
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
    const auto Path = Directory / "cpuid.sys";
    std::ofstream Out(Path, std::ios::binary);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Out.close();
    EXPECT_TRUE(Out);
    return Path;
  }
  std::vector<uint8_t> program(uint32_t Leaf, uint32_t Subleaf) {
    std::vector<uint8_t> Code{0x53}; // Preserve the entry's nonvolatile RBX.
    auto Move = [&](uint8_t Rex, uint8_t Opcode, uint64_t Value) {
      Code.insert(Code.end(), {Rex, Opcode});
      const size_t At = Code.size();
      Code.resize(At + 8);
      llvm::support::endian::write64le(Code.data() + At, Value);
    };
    Move(0x48, 0xb8, 0xa5a5a5a500000000ULL | Leaf);
    Move(0x48, 0xbb, UINT64_MAX);
    Move(0x48, 0xb9, 0x5a5a5a5a00000000ULL | Subleaf);
    Move(0x48, 0xba, UINT64_MAX);
    Move(0x49, 0xb8, OtherValue);
    // STC; CPUID; NOP; XOR EAX,EAX; POP RBX; RET.
    Code.insert(Code.end(), {0xf9, 0x0f, 0xa2, 0x90, 0x31, 0xc0, 0x5b, 0xc3});
    return Code;
  }
};

TEST_P(DriverCPUIDExecution, ReportsDeclaredRegistersAcrossBackends) {
  Options.CPUID = {{0, std::nullopt, {1, 0x756e6547, 0x6c65746e, 0x49656e69}},
                   {7, 3, {UINT32_MAX, 0, 0x87654321, 0x80000000}}};
  for (const auto &[Leaf, Subleaf] :
       {std::pair<uint32_t, uint32_t>{0, 0}, {0, UINT32_MAX}, {7, 3}}) {
    SCOPED_TRACE(Leaf);
    SCOPED_TRACE(Subleaf);
    const auto Path = image(program(Leaf, Subleaf));
    ASSERT_FALSE(HasFailure());
    CPUIDObserver Observer;
    Observer.Offsets = {52, 54};
    auto Run = observeDriver(Path, Options, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    ASSERT_EQ(Run->Stop, DriverStopReason::Returned) << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, 12u);
    ASSERT_EQ(Observer.States.size(), 2u);
    const auto &Expected = Options.CPUID[Leaf == 7 ? 1 : 0].Registers;
    for (unsigned I = 0; I < 4; ++I)
      EXPECT_EQ(Observer.States[1][I], uint64_t(Expected[I]));
    EXPECT_EQ(Observer.States[1][4], OtherValue);
    EXPECT_EQ(Observer.States[1][5], Observer.States[0][5]);
    EXPECT_NE(Observer.States[1][5] & 1, 0u);
    EXPECT_EQ(Observer.States[1][6], Observer.States[0][6]);
  }
}

TEST_P(DriverCPUIDExecution, UndeclaredQueriesFailBeforePublishingResults) {
  Options.CPUID = {{7, 3, {1, 2, 3, 4}}};
  for (const auto &[Leaf, Subleaf] :
       {std::pair<uint32_t, uint32_t>{0, 3}, {7, 4}}) {
    const auto Path = image(program(Leaf, Subleaf));
    ASSERT_FALSE(HasFailure());
    CPUIDObserver Observer;
    Observer.Offsets = {52, 54};
    auto Run = observeDriver(Path, Options, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::UnsupportedInstruction);
    EXPECT_NE(Run->Diagnostic.find("undeclared leaf"), std::string::npos);
    ASSERT_EQ(Observer.States.size(), 1u);
    EXPECT_EQ(Observer.States[0][1], UINT64_MAX);
    EXPECT_EQ(Observer.States[0][3], UINT64_MAX);
  }
}

TEST_P(DriverCPUIDExecution, InstructionBudgetsBoundCPUIDObservations) {
  Options.CPUID = {{0, std::nullopt, {1, 2, 3, 4}}};
  const auto Path = image(program(0, UINT32_MAX));
  ASSERT_FALSE(HasFailure());
  for (uint64_t Limit : {7u, 8u}) {
    Options.InstructionLimit = Limit;
    CPUIDObserver Observer;
    Observer.Offsets = {52, 54};
    auto Run = observeDriver(Path, Options, Observer);
    ASSERT_TRUE(bool(Run)) << llvm::toString(Run.takeError());
    EXPECT_EQ(Run->Stop, DriverStopReason::InstructionLimit) << Run->Diagnostic;
    EXPECT_EQ(Run->Instructions, Limit);
    ASSERT_EQ(Observer.States.size(), Limit == 7 ? 1u : 2u);
    EXPECT_EQ(Observer.States[0][1], UINT64_MAX);
    if (Limit == 8)
      EXPECT_EQ(Observer.States[1][1], 2u);
  }
}

std::string parameterName(const testing::TestParamInfo<Parameter> &P) {
  const auto &[Backend, Contract] = P.param;
  return std::string(executionBackendName(Backend)) +
         (Contract == ExecutionContract::Legacy ? "_driver" : "_checked");
}
INSTANTIATE_TEST_SUITE_P(
    Native, DriverCPUIDExecution,
    testing::Combine(testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP),
                     testing::Values(ExecutionContract::Legacy,
                                     ExecutionContract::CheckedX64)),
    parameterName);
} // namespace
} // namespace neverd::emulation
