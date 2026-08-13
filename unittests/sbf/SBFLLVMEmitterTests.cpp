//===- SBFLLVMEmitterTests.cpp - Solana SBF LLVM backend tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sbf/emit/SBFLLVMEmitter.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace neverd::sbf {
namespace {

SBFProgram makeProgram(Version TheVersion,
                       std::initializer_list<MedInstruction> Instructions,
                       size_t EntrySlot = 0) {
  SBFProgram Program;
  Program.Low.TheVersion = TheVersion;
  Program.Low.TextAddress = kBytecodeStart;
  Program.Low.EntrySlot = EntrySlot;
  size_t InstructionCount = 0;
  for (const MedInstruction &Instruction : Instructions)
    InstructionCount = std::max(InstructionCount, Instruction.Slot + 1);
  std::vector<uint8_t> Text(InstructionCount * kInstructionSize);
  Program.ExecutableImage =
      llvm::cantFail(createProgramImage(Text, kBytecodeStart));
  for (const MedInstruction &Instruction : Instructions) {
    LowInstruction Low;
    Low.Slot = Instruction.Slot;
    Low.Address = Instruction.Address;
    Low.Info = getOpcodeInfo(Instruction.SourceOpcode);
    if (Program.Low.Instructions.size() <= Instruction.Slot)
      Program.Low.Instructions.resize(Instruction.Slot + 1);
    Program.Low.Instructions[Instruction.Slot] = Low;
    MedInstruction Normalized = Instruction;
    EXPECT_NE(Low.Info, nullptr);
    if (Low.Info)
      Normalized.Semantics = semanticTraits(*Low.Info, TheVersion);
    Program.Med.Instructions.push_back(std::move(Normalized));
  }
  std::set<size_t> Leaders{0, EntrySlot};
  for (const MedInstruction &Instruction : Program.Med.Instructions) {
    if (Instruction.BranchTarget)
      Leaders.insert(*Instruction.BranchTarget);
    if (Instruction.CallTarget)
      Leaders.insert(*Instruction.CallTarget);
    if (Instruction.Semantics.Terminator != TerminatorKind::None &&
        Instruction.Slot + 1 < InstructionCount)
      Leaders.insert(Instruction.Slot + 1);
  }
  std::vector<size_t> Ordered(Leaders.begin(), Leaders.end());
  for (size_t I = 0; I < Ordered.size(); ++I) {
    BasicBlock Block;
    Block.ID = I;
    Block.StartSlot = Ordered[I];
    Block.EndSlot = I + 1 < Ordered.size() ? Ordered[I + 1] : InstructionCount;
    Program.Low.Blocks.push_back(Block);
  }
  return Program;
}

MedInstruction instruction(size_t Slot, Opcode SourceOpcode, Operation Op,
                           OperandForm Form = OperandForm::None,
                           unsigned Width = 64) {
  MedInstruction Instruction;
  Instruction.Slot = Slot;
  Instruction.Address = kBytecodeStart + Slot * kInstructionSize;
  Instruction.SourceOpcode = SourceOpcode;
  Instruction.Op = Op;
  Instruction.Form = Form;
  Instruction.Width = Width;
  return Instruction;
}

TEST(SBFLLVMEmitter, ProducesVerifiedVersionedModuleWithRodata) {
  MedInstruction Move =
      instruction(0, Opcode::MOV64_IMM, Operation::Mov, OperandForm::DstImm);
  Move.Dst = kReturnRegister;
  Move.Immediate = 7;
  MedInstruction Exit =
      instruction(1, Opcode::EXIT, Operation::Exit, OperandForm::None);
  SBFProgram Program = makeProgram(Version::V3, {Move, Exit});
  const std::array<uint8_t, 4> Rodata{0x41, 0x42, 0x43, 0};
  Program.ExecutableImage = llvm::cantFail(createProgramImage(
      Program.text(), kBytecodeStart, Rodata, kRodataStartV3));

  llvm::LLVMContext Context;
  auto Module = emitLLVM(Program, Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());
  std::string Error;
  llvm::raw_string_ostream ErrorStream(Error);
  EXPECT_FALSE(llvm::verifyModule(**Module, &ErrorStream)) << Error;

  const std::string IR = emitLLVMText(**Module);
  EXPECT_NE(IR.find("define i64 @neverd_sbf_program"), std::string::npos);
  EXPECT_NE(IR.find("@sbf.rodata"), std::string::npos);
  EXPECT_NE(IR.find("!neverd.sbf.version"), std::string::npos);
  EXPECT_NE(IR.find("!\"v3\""), std::string::npos);
  EXPECT_NE(IR.find("sbf.bb.0.pc.0"), std::string::npos);
  EXPECT_EQ(IR.find("pc.1:"), std::string::npos);
}

TEST(SBFLLVMEmitter, LowersV2WideMathAndDivisionFaultGuards) {
  MedInstruction Move =
      instruction(0, Opcode::MOV64_IMM, Operation::Mov, OperandForm::DstImm);
  Move.Dst = 1;
  Move.Immediate = 0x1234;

  MedInstruction HighMultiply = instruction(
      1, Opcode::UHMUL64_IMM, Operation::UHighMul, OperandForm::DstImm);
  HighMultiply.Dst = 1;
  HighMultiply.Immediate = 0x5678;

  MedInstruction SignedDivide =
      instruction(2, Opcode::SDIV64_IMM, Operation::SDiv, OperandForm::DstImm);
  SignedDivide.Dst = 1;
  SignedDivide.Immediate = 3;

  MedInstruction Exit =
      instruction(3, Opcode::EXIT, Operation::Exit, OperandForm::None);
  llvm::LLVMContext Context;
  auto Module = emitLLVM(
      makeProgram(Version::V2, {Move, HighMultiply, SignedDivide, Exit}),
      Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());

  const std::string IR = emitLLVMText(**Module);
  EXPECT_NE(IR.find("mul i128"), std::string::npos);
  EXPECT_NE(IR.find("sdiv i64"), std::string::npos);
  EXPECT_NE(IR.find("divide.zero.fault"), std::string::npos);
  EXPECT_NE(IR.find("divide.overflow.fault"), std::string::npos);
}

TEST(SBFLLVMEmitter, MatchesCurrentCallFrameAndCallXSemantics) {
  MedInstruction Call =
      instruction(0, Opcode::CALL_REG, Operation::CallX, OperandForm::CallReg);
  Call.Call = CallKind::Indirect;
  Call.CallRegister = 1;
  MedInstruction CallerExit =
      instruction(1, Opcode::EXIT, Operation::Exit, OperandForm::None);
  MedInstruction CalleeExit =
      instruction(2, Opcode::EXIT, Operation::Exit, OperandForm::None);

  llvm::LLVMContext Context;
  auto Module = emitLLVM(
      makeProgram(Version::V2, {Call, CallerExit, CalleeExit}), Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());

  const std::string IR = emitLLVMText(**Module);
  EXPECT_NE(IR.find("store i64 8590196736"), std::string::npos);
  EXPECT_NE(IR.find("icmp ult i32"), std::string::npos);
  EXPECT_NE(IR.find(", 63"), std::string::npos);
  EXPECT_EQ(IR.find("and i64 %"), std::string::npos);
  EXPECT_NE(IR.find("udiv i64"), std::string::npos);
}

TEST(SBFLLVMEmitter, UsesCheckedRuntimeCallsForMemoryAndSyscalls) {
  MedInstruction Load =
      instruction(0, Opcode::LD_DW_REG, Operation::Load, OperandForm::Load);
  Load.Dst = 0;
  Load.Src = 1;
  MedInstruction Store = instruction(1, Opcode::ST_DW_REG, Operation::Store,
                                     OperandForm::StoreReg);
  Store.Dst = 1;
  Store.Src = 0;
  MedInstruction Syscall =
      instruction(2, Opcode::CALL_IMM, Operation::Call, OperandForm::CallImm);
  Syscall.Call = CallKind::Syscall;
  Syscall.SyscallHash = hashSymbolName("sol_log_64_");
  MedInstruction Exit =
      instruction(3, Opcode::EXIT, Operation::Exit, OperandForm::None);

  llvm::LLVMContext Context;
  auto Module =
      emitLLVM(makeProgram(Version::V3, {Load, Store, Syscall, Exit}), Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());

  const llvm::Function *LoadRuntime = (*Module)->getFunction(kRuntimeLoadName);
  const llvm::Function *StoreRuntime =
      (*Module)->getFunction(kRuntimeStoreName);
  const llvm::Function *SyscallRuntime =
      (*Module)->getFunction(kRuntimeSyscallName);
  const llvm::Function *FaultRuntime =
      (*Module)->getFunction(kRuntimeFaultName);
  const llvm::Function *Entry = (*Module)->getFunction(kEntryFunctionName);
  ASSERT_NE(LoadRuntime, nullptr);
  ASSERT_NE(StoreRuntime, nullptr);
  ASSERT_NE(SyscallRuntime, nullptr);
  ASSERT_NE(FaultRuntime, nullptr);
  ASSERT_NE(Entry, nullptr);
  EXPECT_TRUE(LoadRuntime->getReturnType()->isIntegerTy(32));
  EXPECT_TRUE(StoreRuntime->getReturnType()->isIntegerTy(32));
  EXPECT_TRUE(SyscallRuntime->getReturnType()->isIntegerTy(32));
  EXPECT_EQ(LoadRuntime->arg_size(), kRuntimeLoadArgumentCount);
  EXPECT_EQ(StoreRuntime->arg_size(), kRuntimeStoreArgumentCount);
  EXPECT_EQ(SyscallRuntime->arg_size(), kRuntimeSyscallArgumentCount);
  EXPECT_EQ(FaultRuntime->arg_size(), kRuntimeFaultArgumentCount);
  EXPECT_TRUE(LoadRuntime->hasFnAttribute(llvm::Attribute::NoUnwind));
  EXPECT_TRUE(StoreRuntime->hasFnAttribute(llvm::Attribute::NoUnwind));
  EXPECT_TRUE(SyscallRuntime->hasFnAttribute(llvm::Attribute::NoUnwind));
  EXPECT_TRUE(FaultRuntime->hasFnAttribute(llvm::Attribute::NoUnwind));
  EXPECT_TRUE(FaultRuntime->hasFnAttribute(llvm::Attribute::Cold));
  EXPECT_TRUE(Entry->hasFnAttribute(llvm::Attribute::NoUnwind));
  EXPECT_TRUE(
      LoadRuntime->getArg(kRuntimeLoadOutputParameter)->hasNoCaptureAttr());
  EXPECT_TRUE(LoadRuntime->hasParamAttribute(kRuntimeLoadOutputParameter,
                                             llvm::Attribute::WriteOnly));
  EXPECT_TRUE(SyscallRuntime->getArg(kRuntimeSyscallOutputParameter)
                  ->hasNoCaptureAttr());
  EXPECT_TRUE(SyscallRuntime->hasParamAttribute(kRuntimeSyscallOutputParameter,
                                                llvm::Attribute::WriteOnly));

  const std::string IR = emitLLVMText(**Module);
  EXPECT_NE(IR.find("memory.load.fault"), std::string::npos);
  EXPECT_NE(IR.find("memory.store.fault"), std::string::npos);
  EXPECT_NE(IR.find("syscall.fault"), std::string::npos);
}

} // namespace
} // namespace neverd::sbf
