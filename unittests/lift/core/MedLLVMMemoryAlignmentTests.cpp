//===- MedLLVMMemoryAlignmentTests.cpp - Machine access alignment
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"

namespace {

using namespace neverd;

void checkAlignment(unsigned Bytes, NdMemoryOrdering Ordering,
                    unsigned ExpectedAlignment) {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "copy_memory";
  Func.CC = CallingConv::Win64;
  Func.ReturnType = NdType::makeVoid();
  for (uint64_t Reg : {x86reg::RCX, x86reg::RDX}) {
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.Id = static_cast<int>(Func.Params.size());
    Param.Size = 8;
    Param.RegOff = Reg;
    Param.TheArch = Arch::X64;
    Func.Params.push_back(Param);
    Func.TypedParams.push_back(
        {"arg" + std::to_string(Param.Id), NdType::makePtr()});
  }

  Func.Blocks.resize(1);
  auto &Block = Func.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.EndAddr = Func.Entry + 3;
  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Addr = Func.Entry;
  Load.Output.Kind = MedVar::Temp;
  Load.Output.Id = 2;
  Load.Output.Size = Bytes;
  Load.MemoryOrdering = Ordering;
  Load.addInput(Func.Params[1]);
  Block.Ops.push_back(Load);
  MedOp Store;
  Store.Opcode = NdOp::STORE;
  Store.Addr = Func.Entry + 1;
  Store.MemoryOrdering = Ordering;
  Store.addInput(Func.Params[0]);
  Store.addInput(Load.Output);
  Block.Ops.push_back(Store);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry + 2;
  Block.Ops.push_back(Return);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::X64);
  ASSERT_NE(Module, nullptr);
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  unsigned Accesses = 0;
  for (const auto &BB : *Module->getFunction(Func.Name)) {
    for (const auto &Inst : BB) {
      if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&Inst);
          LI && !llvm::isa<llvm::AllocaInst>(
                    LI->getPointerOperand()->stripPointerCasts())) {
        EXPECT_EQ(LI->getAlign().value(), ExpectedAlignment);
        ++Accesses;
      }
      if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&Inst);
          SI && !llvm::isa<llvm::AllocaInst>(
                    SI->getPointerOperand()->stripPointerCasts())) {
        EXPECT_EQ(SI->getAlign().value(), ExpectedAlignment);
        ++Accesses;
      }
    }
  }
  EXPECT_EQ(Accesses, 2u);
}

TEST(MedLLVMMemoryAlignment, OrdinaryAccessDoesNotInventAddressAlignment) {
  for (unsigned Bytes : {2u, 4u, 8u, 16u}) {
    SCOPED_TRACE(Bytes);
    checkAlignment(Bytes, NdMemoryOrdering::None, 1);
  }
}

TEST(MedLLVMMemoryAlignment, AtomicAccessKeepsItsExplicitAlignment) {
  checkAlignment(8, NdMemoryOrdering::SequentiallyConsistent, 8);
}

} // namespace
