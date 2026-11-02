//===- ContextRegisterSnapshotTests.cpp - Saved register observations -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Observe saved CPU registers without restoring state or clearing faults.
///
//===----------------------------------------------------------------------===//

#include "HvfTestPolicy.h"
#include "gtest/gtest.h"

#include "neverd/emulation/AddressSpace.h"
#include "neverd/emulation/CPU.h"

#include <array>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace neverd::emulation {
namespace {
using Configuration =
    std::tuple<GuestArchitecture, ExecutionBackendKind, ExecutionContract>;
std::vector<Configuration> configurations() {
  std::vector<Configuration> Out;
  for (auto ISA : {GuestArchitecture::AArch64, GuestArchitecture::X64}) {
    Out.emplace_back(ISA, ExecutionBackendKind::Unicorn,
                     ExecutionContract::Software);
    for (auto Backend :
         {ExecutionBackendKind::Unicorn, ExecutionBackendKind::HVF,
          ExecutionBackendKind::KVM, ExecutionBackendKind::WHP})
      Out.emplace_back(ISA, Backend,
                       ISA == GuestArchitecture::AArch64
                           ? ExecutionContract::CheckedUserAArch64
                           : ExecutionContract::CheckedUserX64);
  }
  return Out;
}
class ContextRegisterSnapshot : public testing::TestWithParam<Configuration> {
protected:
  static constexpr uint64_t Limit = 1024 * 1024, Data = 0x21000000;
  std::unique_ptr<ExecutionBackend> CPU;
  llvm::Expected<BackendSelection> create() {
    const auto [ISA, Backend, Contract] = GetParam();
    return createExecutionBackend(Backend, Contract, Limit, ISA);
  }
  void SetUp() override {
    const auto [ISA, Backend, Contract] = GetParam();
    auto B = create();
    if (!B) {
      auto E = B.takeError();
      const bool Unavailable = E.isA<BackendUnavailableError>();
      const auto Message = llvm::toString(std::move(E));
      if (Unavailable && !requireHvf(Backend, ISA))
        GTEST_SKIP() << Message;
      FAIL() << Message;
    }
    CPU = std::move(B->CPU);
    ASSERT_EQ(
        llvm::toString(CPU->map(Data, 4096,
                                Read | Write |
                                    (Contract == ExecutionContract::Software
                                         ? 0u
                                         : unsigned(UserAccessible)))),
        "");
  }
};
TEST_P(ContextRegisterSnapshot, FullValuesRemainSeparateFromCPUAndRAM) {
  const auto ISA = CPU->architecture();
  const auto Scalar = ISA == GuestArchitecture::AArch64 ? CPURegister::AArch64X0
                                                        : CPURegister::X64AX;
  for (CPURegister R : architectureRegisters(ISA)) {
    if (registerWidth(R) == 128 || registerWidth(R) == 80) {
      const RegisterValue Value{0xfedcba9876543210ull + unsigned(R),
                                registerWidth(R) == 80
                                    ? 0x4000u
                                    : 0x8123456789abcdefull - unsigned(R)};
      ASSERT_EQ(llvm::toString(CPU->writeRegister(R, Value)), "");
    }
  }
  ASSERT_EQ(llvm::toString(CPU->writeRegister(Scalar, {UINT64_MAX, 0})), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Data, 19, 8)), "");
  const auto SavedRegisters = llvm::cantFail(CPU->readRegisters());
  auto Saved = llvm::cantFail(CPU->saveContext());
  ASSERT_EQ(llvm::toString(CPU->writeRegister(Scalar, {23, 0})), "");
  ASSERT_EQ(
      llvm::toString(CPU->writeRegister(vectorRegister(ISA, 0), {29, 31})), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Data, 37, 8)), "");
  const auto CurrentRegisters = llvm::cantFail(CPU->readRegisters());
  ASSERT_NE(SavedRegisters, CurrentRegisters);
  for (unsigned I = 0; I != 3; ++I) {
    EXPECT_EQ(llvm::cantFail(CPU->readRegisters(*Saved)), SavedRegisters);
    EXPECT_EQ(llvm::cantFail(CPU->readRegisters()), CurrentRegisters);
    EXPECT_EQ(llvm::cantFail(CPU->readInteger(Data, 8)), 37u);
  }
  ASSERT_EQ(llvm::toString(CPU->saveContext(*Saved)), "");
  EXPECT_EQ(llvm::cantFail(CPU->readRegisters(*Saved)), CurrentRegisters);
}
TEST_P(ContextRegisterSnapshot,
       OwnershipMovesExpiryAndAddressSpacesStayStrict) {
  auto Saved = llvm::cantFail(CPU->saveContext());
  const auto Registers = llvm::cantFail(CPU->readRegisters(*Saved));
  auto Other = llvm::cantFail(create()).CPU;
  auto Foreign = Other->readRegisters(*Saved);
  ASSERT_FALSE(bool(Foreign));
  EXPECT_NE(llvm::toString(Foreign.takeError()).find("another backend"),
            std::string::npos);
  BackendContext Moved(std::move(*Saved));
  auto Empty = CPU->readRegisters(*Saved);
  ASSERT_FALSE(bool(Empty));
  EXPECT_NE(llvm::toString(Empty.takeError()).find("expired"),
            std::string::npos);
  EXPECT_EQ(llvm::cantFail(CPU->readRegisters(Moved)), Registers);
  const auto Original = CPU->addressSpace();
  auto Sibling =
      llvm::cantFail(AddressSpace::create(Original->physicalMemory(), Limit));
  ASSERT_EQ(llvm::toString(CPU->bindAddressSpace(Sibling)), "");
  auto Rebound = CPU->readRegisters(Moved);
  ASSERT_FALSE(bool(Rebound));
  EXPECT_NE(llvm::toString(Rebound.takeError()).find("different address space"),
            std::string::npos);
  ASSERT_EQ(llvm::toString(CPU->bindAddressSpace(Original)), "");
  EXPECT_EQ(llvm::cantFail(CPU->readRegisters(Moved)), Registers);
  CPU.reset();
  auto Expired = Other->readRegisters(Moved);
  ASSERT_FALSE(bool(Expired));
  EXPECT_NE(llvm::toString(Expired.takeError()).find("expired"),
            std::string::npos);
}
TEST_P(ContextRegisterSnapshot, FaultedCPUCanObserveSavedStateWithoutRecovery) {
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Data, 43, 8)), "");
  const auto Registers = llvm::cantFail(CPU->readRegisters());
  auto Saved = llvm::cantFail(CPU->saveContext());
  EXPECT_NE(llvm::toString(CPU->run(0x23000000, 1000000)), "");
  const auto Fault = CPU->fault();
  ASSERT_TRUE(Fault);
  const auto Current = llvm::cantFail(CPU->readRegisters());
  EXPECT_EQ(llvm::cantFail(CPU->readRegisters(*Saved)), Registers);
  EXPECT_EQ(llvm::cantFail(CPU->readRegisters()), Current);
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->Kind, Fault->Kind);
  EXPECT_EQ(CPU->fault()->PC, Fault->PC);
  EXPECT_EQ(CPU->fault()->Address, Fault->Address);
  EXPECT_NE(llvm::toString(CPU->restoreContext(*Saved)), "");
  std::array<uint8_t, 8> Bytes{};
  ASSERT_EQ(llvm::toString(CPU->snapshotBacking(Data, Bytes)), "");
  EXPECT_EQ(Bytes, (std::array<uint8_t, 8>{43}));
}
INSTANTIATE_TEST_SUITE_P(Backends, ContextRegisterSnapshot,
                         testing::ValuesIn(configurations()));
} // namespace
} // namespace neverd::emulation
