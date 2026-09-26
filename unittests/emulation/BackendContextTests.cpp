//===- BackendContextTests.cpp - Complete CPU scheduling contexts
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute independent x64 instruction sequences to verify that CPU contexts
/// preserve architectural state without rolling back the shared address space.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"

#include "neverd/emulation/DriverProfile.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace neverd::emulation {
namespace {

constexpr uint64_t CodeAddress = 0x1000;
constexpr uint64_t DataAddress = 0x4000;
constexpr uint64_t StackAddress = 0x8000;
constexpr uint64_t RunTimeout = 1000000;

class DriverBackendContext : public ::testing::Test {
protected:
  std::unique_ptr<UnicornBackend> CPU;
  uint64_t NextCodeOffset = 0;

  void SetUp() override {
    auto Backend = UnicornBackend::create(4 * profile::PageSize);
    ASSERT_TRUE(static_cast<bool>(Backend))
        << llvm::toString(Backend.takeError());
    CPU = std::move(*Backend);
    ASSERT_EQ(llvm::toString(CPU->map(CodeAddress, profile::PageSize,
                                      Read | Write | Execute)),
              "");
    for (uint64_t Address : {DataAddress, StackAddress})
      ASSERT_EQ(
          llvm::toString(CPU->map(Address, profile::PageSize, Read | Write)),
          "");
    ASSERT_EQ(
        llvm::toString(CPU->setReg(X64Register::SP, StackAddress + 0x800)), "");
  }

  void execute(llvm::ArrayRef<uint8_t> Code) {
    ASSERT_FALSE(Code.empty());
    // Keep instruction sequences at distinct addresses so these tests do not
    // depend on Unicorn's treatment of host edits to translated guest code.
    const uint64_t Entry = CodeAddress + NextCodeOffset;
    NextCodeOffset += 64;
    ASSERT_LT(NextCodeOffset, profile::PageSize);
    ASSERT_EQ(llvm::toString(CPU->write(Entry, Code)), "");
    BackendHooks Hooks;
    Hooks.Instruction = [&, StopPC = Entry + Code.size() - 1](uint64_t PC,
                                                              uint32_t) {
      if (PC == StopPC)
        CPU->stop();
    };
    ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
    ASSERT_EQ(llvm::toString(CPU->run(Entry, RunTimeout)), "");
    ASSERT_FALSE(CPU->fault().has_value());
  }

  uint64_t reg(X64Register Register) {
    auto Value = CPU->reg(Register);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }

  uint64_t memory(uint64_t Address, unsigned Width) {
    auto Value = CPU->readInteger(Address, Width);
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return 0;
    }
    return *Value;
  }
};

TEST_F(DriverBackendContext, RestoresGeneralRegistersFlagsStackAndInstruction) {
  const std::array<X64Register, 5> Registers = {
      X64Register::AX, X64Register::CX, X64Register::DX, X64Register::R8,
      X64Register::R9};
  for (size_t I = 0; I < Registers.size(); ++I)
    ASSERT_EQ(llvm::toString(CPU->setReg(Registers[I], 0x1122334455667700 + I)),
              "");
  // STC; STD; MOV rbx, rcx; NOP. Include a callee-saved register outside the
  // backend's small public register inventory.
  execute({0xf9, 0xfd, 0x48, 0x89, 0xcb, 0x90});
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(Saved)) << llvm::toString(Saved.takeError());
  // CLC; CLD; XOR ebx, ebx; NOP.
  execute({0xf8, 0xfc, 0x31, 0xdb, 0x90});
  for (auto Register : Registers)
    ASSERT_EQ(llvm::toString(CPU->setReg(Register, 0)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::SP, StackAddress + 0x400)),
            "");
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**Saved)), "");
  for (size_t I = 0; I < Registers.size(); ++I)
    EXPECT_EQ(reg(Registers[I]), 0x1122334455667700 + I);
  EXPECT_EQ(reg(X64Register::SP), StackAddress + 0x800);
  EXPECT_EQ(reg(X64Register::PC), CodeAddress + 5);
  // PUSHFQ; POP rax; MOV rcx, rbx; CLD; NOP.
  execute({0x9c, 0x58, 0x48, 0x89, 0xd9, 0xfc, 0x90});
  EXPECT_EQ(reg(X64Register::AX) & 0x401, 0x401u); // CF and DF.
  EXPECT_EQ(reg(X64Register::CX), 0x1122334455667701u);
  EXPECT_EQ(reg(X64Register::SP), StackAddress + 0x800);
}

TEST_F(DriverBackendContext,
       RestoresFullSIMDRegistersIncludingHighRegisterBank) {
  std::array<uint8_t, 32> Values{};
  for (size_t I = 0; I < Values.size(); ++I)
    Values[I] = static_cast<uint8_t>(0x83 + 3 * I);
  ASSERT_EQ(llvm::toString(CPU->write(DataAddress, Values)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::R8, DataAddress)), "");
  // MOVDQU xmm0, [r8]; MOVDQU xmm15, [r8 + 16]; NOP.
  execute(
      {0xf3, 0x41, 0x0f, 0x6f, 0x00, 0xf3, 0x45, 0x0f, 0x6f, 0x78, 0x10, 0x90});
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(Saved)) << llvm::toString(Saved.takeError());
  // PXOR xmm0, xmm0; PXOR xmm15, xmm15; NOP.
  execute({0x66, 0x0f, 0xef, 0xc0, 0x66, 0x45, 0x0f, 0xef, 0xff, 0x90});
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**Saved)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::R9, DataAddress + 64)), "");
  // MOVDQU [r9], xmm0; MOVDQU [r9 + 16], xmm15; NOP.
  execute(
      {0xf3, 0x41, 0x0f, 0x7f, 0x01, 0xf3, 0x45, 0x0f, 0x7f, 0x79, 0x10, 0x90});
  std::array<uint8_t, 32> Restored{};
  ASSERT_EQ(llvm::toString(CPU->read(DataAddress + 64, Restored)), "");
  EXPECT_EQ(Restored, Values);
}

TEST_F(DriverBackendContext, RestoresFPUStackAndFloatingPointControlState) {
  constexpr uint64_t DoubleBits = 0x400921fb54442d18;
  constexpr uint64_t ControlWord = 0x077f;
  constexpr uint64_t MXCSR = 0x3f80;
  ASSERT_EQ(llvm::toString(CPU->writeInteger(DataAddress, DoubleBits, 8)), "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(DataAddress + 8, ControlWord, 2)),
            "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(DataAddress + 12, MXCSR, 4)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::R8, DataAddress)), "");
  // FNINIT; FLD qword [r8]; FLDCW [r8 + 8]; LDMXCSR [r8 + 12]; NOP.
  execute({0xdb, 0xe3, 0x41, 0xdd, 0x00, 0x41, 0xd9, 0x68, 0x08, 0x41, 0x0f,
           0xae, 0x50, 0x0c, 0x90});
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(Saved)) << llvm::toString(Saved.takeError());
  ASSERT_EQ(llvm::toString(CPU->writeInteger(DataAddress + 12, 0x1f80, 4)), "");
  // FNINIT; LDMXCSR [r8 + 12]; NOP.
  execute({0xdb, 0xe3, 0x41, 0x0f, 0xae, 0x50, 0x0c, 0x90});
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**Saved)), "");
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::R9, DataAddress + 64)), "");
  // FSTP qword [r9]; FNSTCW [r9 + 8]; STMXCSR [r9 + 12]; NOP.
  execute({0x41, 0xdd, 0x19, 0x41, 0xd9, 0x79, 0x08, 0x41, 0x0f, 0xae, 0x59,
           0x0c, 0x90});
  EXPECT_EQ(memory(DataAddress + 64, 8), DoubleBits);
  EXPECT_EQ(memory(DataAddress + 72, 2), ControlWord);
  EXPECT_EQ(memory(DataAddress + 76, 4), MXCSR);
}

TEST_F(DriverBackendContext, ModelControlledIRQLIsPartOfTheCPUContext) {
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::CR8, 2)), "");
  EXPECT_EQ(reg(X64Register::CR8), 2u);
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(Saved)) << llvm::toString(Saved.takeError());
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::CR8, 0)), "");
  EXPECT_EQ(reg(X64Register::CR8), 0u);
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**Saved)), "");
  EXPECT_EQ(reg(X64Register::CR8), 2u);
}

TEST_F(DriverBackendContext, SnapshotsAreIndependentAndCanBeRefreshed) {
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, 11)), "");
  auto First = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(First)) << llvm::toString(First.takeError());
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, 22)), "");
  auto Second = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(Second)) << llvm::toString(Second.takeError());
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**First)), "");
  EXPECT_EQ(reg(X64Register::AX), 11u);
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, 33)), "");
  ASSERT_EQ(llvm::toString(CPU->saveContext(**First)), "");
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**Second)), "");
  EXPECT_EQ(reg(X64Register::AX), 22u);
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**First)), "");
  EXPECT_EQ(reg(X64Register::AX), 33u);
}

TEST_F(DriverBackendContext, MemoryMappingsContentsAndPermissionsStayShared) {
  ASSERT_EQ(llvm::toString(CPU->writeInteger(DataAddress, 11, 8)), "");
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(Saved)) << llvm::toString(Saved.takeError());
  ASSERT_EQ(llvm::toString(CPU->writeInteger(DataAddress, 22, 8)), "");
  constexpr uint64_t NewAddress = 0xc000;
  ASSERT_EQ(
      llvm::toString(CPU->map(NewAddress, profile::PageSize, Read | Write)),
      "");
  ASSERT_EQ(llvm::toString(CPU->writeInteger(NewAddress, 33, 8)), "");
  ASSERT_EQ(llvm::toString(CPU->protect(DataAddress, profile::PageSize, Read)),
            "");
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**Saved)), "");
  EXPECT_EQ(memory(DataAddress, 8), 22u);
  EXPECT_EQ(memory(NewAddress, 8), 33u);
  EXPECT_NE(llvm::toString(CPU->writeInteger(DataAddress, 44, 8)).find("fault"),
            std::string::npos);
  ASSERT_TRUE(CPU->fault().has_value());
  EXPECT_EQ(CPU->fault()->Kind, BackendFaultKind::Protection);
}

TEST_F(DriverBackendContext, RejectsCrossEngineUseWithoutChangingEitherEngine) {
  ASSERT_EQ(llvm::toString(CPU->setReg(X64Register::AX, 11)), "");
  auto Saved = CPU->saveContext();
  auto Other = UnicornBackend::create(profile::PageSize);
  ASSERT_TRUE(static_cast<bool>(Saved)) << llvm::toString(Saved.takeError());
  ASSERT_TRUE(static_cast<bool>(Other)) << llvm::toString(Other.takeError());
  ASSERT_EQ(llvm::toString((*Other)->setReg(X64Register::AX, 22)), "");
  EXPECT_NE(llvm::toString((*Other)->restoreContext(**Saved)).find("another"),
            std::string::npos);
  EXPECT_NE(llvm::toString((*Other)->saveContext(**Saved)).find("another"),
            std::string::npos);
  auto OtherAX = (*Other)->reg(X64Register::AX);
  ASSERT_TRUE(static_cast<bool>(OtherAX))
      << llvm::toString(OtherAX.takeError());
  EXPECT_EQ(*OtherAX, 22u);
  ASSERT_EQ(llvm::toString(CPU->restoreContext(**Saved)), "");
  EXPECT_EQ(reg(X64Register::AX), 11u);
  EXPECT_FALSE(CPU->fault());
  EXPECT_FALSE((*Other)->fault());
}

TEST_F(DriverBackendContext, ContextCanOutliveBackendAndRejectsExpiredOwner) {
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(Saved)) << llvm::toString(Saved.takeError());
  CPU.reset();
  auto Replacement = UnicornBackend::create(profile::PageSize);
  ASSERT_TRUE(static_cast<bool>(Replacement))
      << llvm::toString(Replacement.takeError());
  EXPECT_NE(
      llvm::toString((*Replacement)->restoreContext(**Saved)).find("expired"),
      std::string::npos);
  EXPECT_NE(
      llvm::toString((*Replacement)->saveContext(**Saved)).find("expired"),
      std::string::npos);
  Saved->reset();
  EXPECT_FALSE((*Replacement)->fault());
}

TEST_F(DriverBackendContext,
       MovedContextTransfersOwnershipAndRejectsEmptySource) {
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(Saved)) << llvm::toString(Saved.takeError());
  BackendContext Moved(std::move(**Saved));
  EXPECT_NE(llvm::toString(CPU->restoreContext(**Saved)).find("expired"),
            std::string::npos);
  EXPECT_NE(llvm::toString(CPU->saveContext(**Saved)).find("expired"),
            std::string::npos);
  ASSERT_EQ(llvm::toString(CPU->restoreContext(Moved)), "");
}

TEST_F(DriverBackendContext, ContextCannotResurrectFaultedEngine) {
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(Saved)) << llvm::toString(Saved.takeError());
  ASSERT_EQ(llvm::toString(CPU->write(CodeAddress, {0x0f, 0x0b})), ""); // UD2.
  EXPECT_FALSE(llvm::toString(CPU->run(CodeAddress, RunTimeout)).empty());
  EXPECT_NE(llvm::toString(CPU->restoreContext(**Saved)).find("faulted"),
            std::string::npos);
  EXPECT_NE(llvm::toString(CPU->saveContext(**Saved)).find("faulted"),
            std::string::npos);
  auto AfterFault = CPU->saveContext();
  ASSERT_FALSE(static_cast<bool>(AfterFault));
  EXPECT_NE(llvm::toString(AfterFault.takeError()).find("faulted"),
            std::string::npos);
  ASSERT_TRUE(CPU->fault());
  EXPECT_EQ(CPU->fault()->Kind, BackendFaultKind::InvalidInstruction);
  EXPECT_NE(llvm::toString(CPU->run(CodeAddress, RunTimeout)).find("faulted"),
            std::string::npos);
}

TEST_F(DriverBackendContext, ContextCannotResumeAfterHostCallbackFailure) {
  auto Saved = CPU->saveContext();
  ASSERT_TRUE(static_cast<bool>(Saved)) << llvm::toString(Saved.takeError());
  ASSERT_EQ(llvm::toString(CPU->write(CodeAddress, {0x90})), "");
  BackendHooks Hooks;
  Hooks.Instruction = [](uint64_t, uint32_t) {
    throw std::runtime_error("test observer failure");
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
  EXPECT_NE(llvm::toString(CPU->run(CodeAddress, RunTimeout)).find("exception"),
            std::string::npos);
  EXPECT_NE(llvm::toString(CPU->restoreContext(**Saved)).find("faulted"),
            std::string::npos);
  EXPECT_FALSE(CPU->fault());
}

TEST_F(DriverBackendContext, HookCanSaveButMustReturnBeforeRestoringContext) {
  ASSERT_EQ(llvm::toString(CPU->write(CodeAddress, {0x90})), "");
  std::unique_ptr<BackendContext> Saved;
  std::string RestoreError;
  std::string RunError;
  BackendHooks Hooks;
  Hooks.Instruction = [&](uint64_t, uint32_t) {
    auto Context = CPU->saveContext();
    ASSERT_TRUE(static_cast<bool>(Context))
        << llvm::toString(Context.takeError());
    Saved = std::move(*Context);
    RestoreError = llvm::toString(CPU->restoreContext(*Saved));
    RunError = llvm::toString(CPU->run(CodeAddress, RunTimeout));
    CPU->stop();
  };
  ASSERT_EQ(llvm::toString(CPU->installHooks(std::move(Hooks))), "");
  ASSERT_EQ(llvm::toString(CPU->run(CodeAddress, RunTimeout)), "");
  ASSERT_NE(Saved, nullptr);
  EXPECT_NE(RestoreError.find("during guest execution"), std::string::npos);
  EXPECT_NE(RunError.find("recursively"), std::string::npos);
  ASSERT_EQ(llvm::toString(CPU->restoreContext(*Saved)), "");
  EXPECT_EQ(reg(X64Register::PC), CodeAddress);
  EXPECT_FALSE(CPU->fault());
}

} // namespace
} // namespace neverd::emulation
