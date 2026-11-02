//===- AArch64MachineProbeTests.cpp - ARM64 startup tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "arch/aarch64/AArch64Machine.h"
#include "core/ExecutionDiagnostics.h"
#include "gtest/gtest.h"

#include "llvm/ADT/ScopeExit.h"

namespace neverd::emulation {
namespace {
#define NEVERD_AARCH64_PROBE_TEST_VALUE(Name, Value)                           \
  constexpr uint64_t Name = Value;
#include "AArch64ProbeCases.def"
#undef NEVERD_AARCH64_PROBE_TEST_VALUE

class CorruptingTransport final : public AArch64Machine {
public:
  unsigned Scalar = 0;
  unsigned Vector = 0;
  unsigned Word = 0;
  bool CorruptVector = false;
  bool CorruptMode = false;
  bool NopOnly = false;
  bool FailTransfer = false;
  unsigned Entries = 0;
  llvm::Error step(AArch64MachineState &State, MachineRunControl) override {
    ++Entries;
    if (FailTransfer)
      return diagnostic::error(diagnostic::KvmState);
    State.reg(AArch64Register::PC) += aarch64::InstructionBytes;
    if (CorruptMode)
      State.UserMode = !State.UserMode;
    else if (NopOnly)
      return llvm::Error::success();
    else if (CorruptVector)
      State.Vectors[Vector][Word] ^= CorruptBit;
    else
      State.Registers[Scalar] ^= CorruptBit;
    return llvm::Error::success();
  }
};
TEST(AArch64MachineProbe, EveryScalarCorruptionRejectsNativeInitialization) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  AArch64MachineState Inventory;
  for (unsigned Index = 0; Index < Inventory.Registers.size(); ++Index) {
    SCOPED_TRACE(Index);
    CorruptingTransport Machine;
    Machine.Scalar = Index;
    EXPECT_EQ(llvm::toString(verifyAArch64Machine(Machine, *Memory)),
              diagnostic::ArmState);
    EXPECT_EQ(Machine.Entries, 1u);
  }
}
TEST(AArch64MachineProbe, EveryVectorCorruptionRejectsNativeInitialization) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  AArch64MachineState Inventory;
  for (unsigned Index = 0; Index < Inventory.Vectors.size(); ++Index) {
    SCOPED_TRACE(Index);
    for (unsigned Word = 0; Word < RegisterValue{}.size(); ++Word) {
      SCOPED_TRACE(Word);
      CorruptingTransport Machine;
      Machine.CorruptVector = true;
      Machine.Vector = Index;
      Machine.Word = Word;
      EXPECT_EQ(llvm::toString(verifyAArch64Machine(Machine, *Memory)),
                diagnostic::ArmState);
      EXPECT_EQ(Machine.Entries, 1u);
    }
  }
}
TEST(AArch64MachineProbe, PrivilegeCorruptionRejectsNativeInitialization) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  CorruptingTransport Machine;
  Machine.CorruptMode = true;
  EXPECT_EQ(llvm::toString(verifyAArch64Machine(Machine, *Memory)),
            diagnostic::ArmState);
  EXPECT_EQ(Machine.Entries, 1u);
}
TEST(AArch64MachineProbe, NopSupportDoesNotProveFloatingPointExecution) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  CorruptingTransport Machine;
  Machine.NopOnly = true;
  EXPECT_EQ(llvm::toString(verifyAArch64Machine(Machine, *Memory)),
            diagnostic::ArmState);
  EXPECT_EQ(Machine.Entries, 2u);
}
TEST(AArch64MachineProbe, GenuineTransferFailureRetainsItsDiagnostic) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  CorruptingTransport Machine;
  Machine.FailTransfer = true;
  EXPECT_EQ(llvm::toString(verifyAArch64Machine(Machine, *Memory)),
            diagnostic::KvmState);
  EXPECT_EQ(Machine.Entries, 1u);
}
TEST(AArch64MachineProbe,
     ActiveExecutionLeaseRejectsInitializationBeforeEntry) {
  auto Memory = llvm::cantFail(MemoryProjection::create(Limit));
  llvm::cantFail(Memory->beginRun());
  auto Release = llvm::scope_exit([&] { Memory->endRun(); });
  CorruptingTransport Machine;
  EXPECT_EQ(llvm::toString(verifyAArch64Machine(Machine, *Memory)),
            diagnostic::Running);
  EXPECT_EQ(Machine.Entries, 0u);
  EXPECT_EQ(llvm::toString(Memory->mutableMemory()), diagnostic::Running);
}
} // namespace
} // namespace neverd::emulation
