//===- X86_64_AMXLLVMGuardTests.cpp - AMX LLVM safety boundary ----------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

struct AMXShape {
  Intrinsic Id;
  uint16_t OutputSize;
  std::vector<uint16_t> InputSizes;
};

MedVar temporary(int Id, uint16_t Size) {
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.TheArch = Arch::X64;
  Value.Id = Id;
  Value.Size = Size;
  return Value;
}

MedFunc makeProbe(const AMXShape &Shape) {
  MedFunc Function;
  Function.Entry = 0x1000;
  Function.Name = "amx_llvm_guard_probe";
  Function.CC = CallingConv::SysV_AMD64;

  MedOp Operation;
  Operation.Addr = Function.Entry;
  Operation.Opcode = NdOp::INTRINSIC;
  if (Shape.OutputSize != 0)
    Operation.Output = temporary(1, Shape.OutputSize);
  else
    Operation.Output.Size = 0;
  Operation.addInput(
      MedVar::makeConst(static_cast<uint16_t>(Shape.Id), /*Sz=*/2));
  int NextId = 2;
  for (uint16_t Size : Shape.InputSizes)
    Operation.addInput(temporary(NextId++, Size));

  MedOp Return;
  Return.Addr = Function.Entry + 1;
  Return.Opcode = NdOp::RETURN;

  MedBlock Block;
  Block.Id = 0;
  Block.Ops = {std::move(Operation), std::move(Return)};
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

const std::array<AMXShape, 8> &architecturalShapes() {
  // Legacy value-form state cannot be silently reinterpreted as the native
  // architectural bank. Its explicit tile/configuration values still require
  // a transport ABI; the versioned X86AMXState operation has a different owner.
  static const std::array<AMXShape, 8> Shapes = {{
      {Intrinsic::AMXLoadConfig, 64, {8, 1}},
      {Intrinsic::AMXStoreConfig, 0, {8, 64, 1}},
      {Intrinsic::AMXTileLoad, 1024, {8, 8, 64, 1024, 1}},
      {Intrinsic::AMXTileStore, 64, {8, 8, 64, 1024, 1}},
      {Intrinsic::AMXTileZero, 1024, {64}},
      {Intrinsic::AMXClearStartRow, 64, {64}},
      {Intrinsic::AMXTileCompute, 1024, {1, 64, 1024, 1024, 1024}},
      {Intrinsic::AMXTileRow, 64, {1, 64, 1024, 4}},
  }};
  return Shapes;
}

} // namespace

TEST(X86AMXLLVMGuard, ClassifiesTheCompleteArchitecturalStateSurface) {
  EXPECT_TRUE(isAMXIntrinsic(Intrinsic::X86AMXState));
  for (const AMXShape &Shape : architecturalShapes())
    EXPECT_TRUE(isAMXIntrinsic(Shape.Id));

  EXPECT_FALSE(isAMXIntrinsic(Intrinsic::None));
  EXPECT_FALSE(isAMXIntrinsic(Intrinsic::MaskedLoadD));
  EXPECT_FALSE(isAMXIntrinsic(Intrinsic::ApxRaoAdd));
}

TEST(X86AMXLLVMGuard, LegacyValueShapesFailBeforeLLVMEmission) {
  for (const AMXShape &Shape : architecturalShapes()) {
    SCOPED_TRACE(static_cast<unsigned>(Shape.Id));
    EXPECT_DEATH(
        {
          llvm::LLVMContext Context;
          MedFunc Probe = makeProbe(Shape);
          (void)MedLLVMEmitter().emit({Probe}, Context, "amx-llvm-guard",
                                      Arch::X64);
        },
        "versioned tile-state, restartable-memory, and fault-continuation "
        "runtime ABI");
  }
}

TEST(X86AMXLLVMGuard, NativeBankShapesAreCheckedBeforeEmission) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = makeProbe({Intrinsic::X86AMXState, 0, {8, 4}});
    Function.ReturnsNoValue = true;
    auto &Op = Function.Blocks[0].Ops[0];
    Op.Inputs[1] = MedVar::makeConst(0, 8);
    Op.Inputs[2] =
        MedVar::makeConst(X86AmxStateVersion | unsigned(X86AmxOp::Release), 4);
    switch (Mutation) {
    case 1:
      Op.Inputs[2].ConstVal = unsigned(X86AmxOp::Release);
      break;
    case 2:
      Op.Inputs[2].ConstVal |= 1u << 26;
      break;
    case 3:
      Op.Inputs[2].ConstVal |= X86AmxEvexMemory;
      break;
    case 4:
      Op.Inputs[2].ConstVal |= X86AmxExtendedRow;
      break;
    case 5:
      Op.Inputs[1].ConstVal = 1;
      break;
    case 6:
      Op.Inputs[2].Size = 8;
      break;
    case 7:
      Op.Output = temporary(1, 64);
      break;
    case 8:
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 9:
      Op.NumInputs = 2;
      break;
    case 10:
      Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    default:
      break;
    }
    EXPECT_EQ(x86AmxShapeIsValid(x86AmxMedShape(Op, Arch::X64)), !Mutation);
    EXPECT_EQ(verifyMedFunc(Function, "AMX bank admission"), !Mutation);
    if (!Mutation) {
      llvm::LLVMContext Context;
      auto Module =
          MedLLVMEmitter().emit({Function}, Context, "amx-bank", Arch::X64);
      ASSERT_TRUE(Module);
      EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    } else
      EXPECT_DEATH(
          {
            llvm::LLVMContext Context;
            (void)MedLLVMEmitter().emit({Function}, Context, "amx-bank",
                                        Arch::X64);
          },
          "invalid AMX architectural bank contract");
  }
}
