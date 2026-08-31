//===- X86_64_AMXLLVMGuardTests.cpp - AMX LLVM safety boundary ----------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/IR/LLVMContext.h"

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
  // Sizes exclude the intrinsic-id input.  These are the exact LowIR/MedIR
  // state shapes a future runtime ABI must preserve; none may be narrowed to a
  // host vector or replaced with a scalar status value.
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
  for (const AMXShape &Shape : architecturalShapes())
    EXPECT_TRUE(isAMXIntrinsic(Shape.Id));

  EXPECT_FALSE(isAMXIntrinsic(Intrinsic::None));
  EXPECT_FALSE(isAMXIntrinsic(Intrinsic::MaskedLoadD));
  EXPECT_FALSE(isAMXIntrinsic(Intrinsic::ApxRaoAdd));
}

TEST(X86AMXLLVMGuard, ExactStateShapesFailBeforeLLVMEmission) {
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
