//===- BackendFaultTests.cpp - CPU fault and isolation contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute small, independent x64 programs against the real CPU adapter. Fault
/// observations must identify the failed access without implying SEH recovery.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"

#include "neverd/emulation/DriverProfile.h"

#include <array>
#include <stdexcept>

namespace neverd::emulation {
namespace {

constexpr uint64_t CodeAddress = 0x1000;
constexpr uint64_t DataAddress = 0x4000;
constexpr uint64_t RunTimeout = 1000000;

class DriverBackendFault : public ::testing::Test {
protected:
  std::unique_ptr<UnicornBackend> CPU;

  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * profile::PageSize);
    ASSERT_TRUE(static_cast<bool>(Backend))
        << llvm::toString(Backend.takeError());
    CPU = std::move(*Backend);
    ASSERT_EQ(llvm::toString(CPU->map(CodeAddress, profile::PageSize,
                                      Read | Write | Execute)),
              "");
  }

  void code(llvm::ArrayRef<uint8_t> Bytes) {
    ASSERT_EQ(llvm::toString(CPU->write(CodeAddress, Bytes)), "");
  }

  uint64_t reg(X64Register Register) {
    auto Value = CPU->reg(Register);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }
};

struct MemoryCase {
  BackendAccessKind Access;
  bool Mapped;
};

class DriverBackendMemoryFault
    : public DriverBackendFault,
      public ::testing::WithParamInterface<MemoryCase> {};

TEST_P(DriverBackendMemoryFault, RecordsFailingInstructionAndAccess) {
  const auto Case = GetParam();
  unsigned Permission = Read | Write;
  switch (Case.Access) {
  case BackendAccessKind::Read:
    code({0x48, 0x8b, 0x08}); // MOV rcx, [rax].
    Permission = Write;
    break;
  case BackendAccessKind::Write:
    code({0x48, 0x89, 0x08}); // MOV [rax], rcx.
    Permission = Read;
    break;
  case BackendAccessKind::Execute:
    break;
  }
  if (Case.Mapped)
    ASSERT_EQ(
        llvm::toString(CPU->map(DataAddress, profile::PageSize, Permission)),
        "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, DataAddress)), "");
  const uint64_t PC =
      Case.Access == BackendAccessKind::Execute ? DataAddress : CodeAddress;
  auto Error = CPU->run(PC, RunTimeout);
  ASSERT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
  const auto Fault = CPU->fault();
  ASSERT_TRUE(Fault.has_value());
  EXPECT_EQ(Fault->Kind, Case.Mapped ? BackendFaultKind::Protection
                                     : BackendFaultKind::UnmappedMemory);
  EXPECT_EQ(Fault->PC, PC);
  EXPECT_EQ(Fault->Address, DataAddress);
  EXPECT_EQ(Fault->Access, Case.Access);
  ASSERT_TRUE(Fault->Size.has_value());
  if (Case.Access == BackendAccessKind::Execute)
    EXPECT_GT(*Fault->Size, 0u);
  else
    EXPECT_EQ(*Fault->Size, 8u);
  EXPECT_FALSE(Fault->Interrupt.has_value());
  EXPECT_TRUE(CPU->hasMemoryFault());
  EXPECT_FALSE(CPU->timedOut());

  // Diagnostics after the stop cannot replace the causal fault. Nor can a
  // later run silently resume a CPU whose post-error state is unspecified.
  auto Read = CPU->readInteger(0, 1);
  ASSERT_FALSE(static_cast<bool>(Read));
  llvm::consumeError(Read.takeError());
  EXPECT_EQ(CPU->fault()->Address, DataAddress);
  EXPECT_NE(llvm::toString(CPU->run(CodeAddress, RunTimeout)).find("resume"),
            std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(
    Accesses, DriverBackendMemoryFault,
    ::testing::Values(MemoryCase{BackendAccessKind::Read, false},
                      MemoryCase{BackendAccessKind::Write, false},
                      MemoryCase{BackendAccessKind::Execute, false},
                      MemoryCase{BackendAccessKind::Read, true},
                      MemoryCase{BackendAccessKind::Write, true},
                      MemoryCase{BackendAccessKind::Execute, true}));

TEST_F(DriverBackendFault, AdmittedUserReadFaultCanEnterGuestHandler) {
  // MOV rcx, [rax]; MOV eax, 42; NOP. The modeled exception transfer enters
  // the handler at the MOV immediate, without reexecuting the failed load.
  code({0x48, 0x8b, 0x08, 0xb8, 42, 0, 0, 0, 0x90});
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, DataAddress)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::CX, 17)), "");
  unsigned Accepted = 0;
  BackendHooks Hooks;
  Hooks.RecoverableFault = [&](const BackendFault &Fault) {
    ++Accepted;
    return Fault.PC == CodeAddress && Fault.Address == DataAddress &&
           Fault.Access == BackendAccessKind::Read;
  };
  Hooks.Instruction = [&](uint64_t PC, uint32_t) {
    if (PC == CodeAddress + 8)
      CPU->stop();
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
  ASSERT_EQ(llvm::toString(CPU->run(CodeAddress, RunTimeout)), "");
  EXPECT_EQ(Accepted, 1u);
  EXPECT_FALSE(CPU->fault());
  EXPECT_EQ(reg(X64Register::CX), 17u);
  EXPECT_NE(llvm::toString(CPU->run(CodeAddress + 3, RunTimeout))
                .find("resume"),
            std::string::npos);
  auto Fault = CPU->takeRecoverableFault();
  ASSERT_TRUE(Fault);
  EXPECT_EQ(Fault->PC, CodeAddress);
  EXPECT_EQ(Fault->Address, DataAddress);
  ASSERT_EQ(llvm::toString(CPU->run(CodeAddress + 3, RunTimeout)), "");
  EXPECT_EQ(reg(X64Register::AX), 42u);
  EXPECT_FALSE(CPU->fault());
}

TEST_F(DriverBackendFault, CheckedWritesValidateTheWholeRangeBeforeMutation) {
  ASSERT_EQ(
      llvm::toString(CPU->map(DataAddress, profile::PageSize, Read | Write)),
      "");
  const uint64_t Address = DataAddress + profile::PageSize - 4;
  ASSERT_EQ(llvm::toString(CPU->writeInteger(Address, 0x12345678, 4)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::PC, CodeAddress + 8)), "");
  auto Error = CPU->writeInteger(Address, UINT64_MAX, 8);
  ASSERT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
  const auto Fault = CPU->fault();
  ASSERT_TRUE(Fault.has_value());
  EXPECT_EQ(Fault->Kind, BackendFaultKind::UnmappedMemory);
  EXPECT_EQ(Fault->PC, CodeAddress + 8);
  EXPECT_EQ(Fault->Address, Address);
  EXPECT_EQ(Fault->Size, 8u);
  EXPECT_EQ(Fault->Access, BackendAccessKind::Write);
  auto Value = CPU->readInteger(Address, 4);
  ASSERT_TRUE(static_cast<bool>(Value)) << llvm::toString(Value.takeError());
  EXPECT_EQ(*Value, 0x12345678u);
}

TEST_F(DriverBackendFault, CheckedReadsReportOverflowWithoutWrapping) {
  std::array<uint8_t, 8> Bytes{};
  auto Error = CPU->read(UINT64_MAX - 3, Bytes);
  ASSERT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
  const auto Fault = CPU->fault();
  ASSERT_TRUE(Fault.has_value());
  EXPECT_EQ(Fault->Kind, BackendFaultKind::InvalidMemoryRange);
  EXPECT_EQ(Fault->Address, UINT64_MAX - 3);
  EXPECT_EQ(Fault->Size, 8u);
  EXPECT_EQ(Fault->Access, BackendAccessKind::Read);
}

TEST_F(DriverBackendFault, InvalidInstructionHasNoInventedMemoryAccess) {
  code({0x0f, 0x0b}); // UD2.
  auto Error = CPU->run(CodeAddress, RunTimeout);
  ASSERT_TRUE(static_cast<bool>(Error));
  llvm::consumeError(std::move(Error));
  const auto Fault = CPU->fault();
  ASSERT_TRUE(Fault.has_value());
  EXPECT_EQ(Fault->Kind, BackendFaultKind::InvalidInstruction);
  EXPECT_EQ(Fault->PC, CodeAddress);
  EXPECT_FALSE(Fault->Address.has_value());
  EXPECT_FALSE(Fault->Size.has_value());
  EXPECT_FALSE(Fault->Access.has_value());
  EXPECT_FALSE(Fault->Interrupt.has_value());
  EXPECT_FALSE(CPU->hasMemoryFault());
}

TEST_F(DriverBackendFault, DivideErrorStopsWithoutAnObserverOrSEHDispatch) {
  // XOR ecx, ecx; DIV ecx; MOV eax, 0x12345678. The last MOV must not execute.
  code({0x31, 0xc9, 0xf7, 0xf1, 0xb8, 0x78, 0x56, 0x34, 0x12});
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, 7)), "");
  ASSERT_EQ(llvm::toString(CPU->run(CodeAddress, RunTimeout)), "");
  const auto Fault = CPU->fault();
  ASSERT_TRUE(Fault.has_value());
  EXPECT_EQ(Fault->Kind, BackendFaultKind::Interrupt);
  EXPECT_EQ(Fault->PC, CodeAddress + 2);
  EXPECT_EQ(Fault->Interrupt, 0u);
  EXPECT_FALSE(Fault->Access.has_value());
  EXPECT_FALSE(CPU->hasMemoryFault());
  EXPECT_FALSE(CPU->timedOut());
  EXPECT_EQ(reg(X64Register::AX), 7u);
}

TEST_F(DriverBackendFault, ReplacingObserversDoesNotMultiplyInstructionEvents) {
  code({0x90, 0x90}); // NOP; NOP, with a stop before the second instruction.
  unsigned ReplacedCalls = 0;
  BackendHooks First;
  First.Instruction = [&](uint64_t, uint32_t) { ++ReplacedCalls; };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(First))), "");
  unsigned CurrentCalls = 0;
  BackendHooks Second;
  Second.Instruction = [&](uint64_t PC, uint32_t) {
    ++CurrentCalls;
    if (PC == CodeAddress + 1)
      CPU->stop();
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Second))), "");
  ASSERT_EQ(llvm::toString(CPU->run(CodeAddress, RunTimeout)), "");
  EXPECT_EQ(ReplacedCalls, 0u);
  EXPECT_EQ(CurrentCalls, 2u);
  EXPECT_FALSE(CPU->fault().has_value());
}

TEST_F(DriverBackendFault, ObserverExceptionsStayInsideTheCppBoundary) {
  code({0xb8, 0x78, 0x56, 0x34, 0x12}); // MOV eax, 0x12345678.
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, 7)), "");
  BackendHooks Hooks;
  Hooks.Instruction = [](uint64_t, uint32_t) {
    throw std::runtime_error("observer failure");
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
  EXPECT_NE(llvm::toString(CPU->run(CodeAddress, RunTimeout))
                .find("exception in emulator hook"),
            std::string::npos);
  EXPECT_EQ(reg(X64Register::AX), 7u);
  EXPECT_FALSE(
      CPU->fault().has_value()); // Host failure, not a guest exception.
}

TEST(DriverBackendIsolation, IndependentEnginesKeepRegistersSIMDAndMemory) {
  auto First = UnicornBackend::create(2 * profile::PageSize);
  auto Second = UnicornBackend::create(2 * profile::PageSize);
  ASSERT_TRUE(static_cast<bool>(First)) << llvm::toString(First.takeError());
  ASSERT_TRUE(static_cast<bool>(Second)) << llvm::toString(Second.takeError());
  // MOVD xmm0, eax; MOVD ecx, xmm0; NOP.
  const std::array<uint8_t, 9> Code = {0x66, 0x0f, 0x6e, 0xc0, 0x66,
                                       0x0f, 0x7e, 0xc1, 0x90};
  unsigned Index = 0;
  for (UnicornBackend *CPU : {First->get(), Second->get()}) {
    ASSERT_EQ(llvm::toString(CPU->map(CodeAddress, profile::PageSize,
                                      Read | Write | Execute)),
              "");
    ASSERT_EQ(
        llvm::toString(CPU->map(DataAddress, profile::PageSize, Read | Write)),
        "");
    ASSERT_EQ(llvm::toString(CPU->write(CodeAddress, Code)), "");
    ASSERT_EQ(llvm::toString(CPU->writeInteger(DataAddress, ++Index, 8)), "");
    ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, Index * 11)), "");
    BackendHooks Hooks;
    Hooks.Instruction = [CPU](uint64_t PC, uint32_t) {
      if (PC == CodeAddress + 8)
        CPU->stop();
    };
    ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
    ASSERT_EQ(llvm::toString(CPU->run(CodeAddress, RunTimeout)), "");
  }
  Index = 0;
  for (UnicornBackend *CPU : {First->get(), Second->get()}) {
    // Read each engine's previously written XMM0 after the other engine ran.
    ASSERT_EQ(llvm::toString(CPU->run(CodeAddress + 4, RunTimeout)), "");
    auto Register = CPU->reg(X64Register::CX);
    ASSERT_TRUE(static_cast<bool>(Register))
        << llvm::toString(Register.takeError());
    EXPECT_EQ(*Register, ++Index * 11u);
    auto Memory = CPU->readInteger(DataAddress, 8);
    ASSERT_TRUE(static_cast<bool>(Memory))
        << llvm::toString(Memory.takeError());
    EXPECT_EQ(*Memory, Index);
    EXPECT_FALSE(CPU->fault().has_value());
  }
  auto BadRead = (*First)->readInteger(0, 1);
  ASSERT_FALSE(static_cast<bool>(BadRead));
  llvm::consumeError(BadRead.takeError());
  EXPECT_TRUE((*First)->hasMemoryFault());
  EXPECT_FALSE((*Second)->fault().has_value());
}

} // namespace
} // namespace neverd::emulation
