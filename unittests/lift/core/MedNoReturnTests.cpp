//===- MedNoReturnTests.cpp - Internal no-return propagation tests -------===//

#include "gtest/gtest.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedNoReturn.h"

#include <initializer_list>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

MedOp trapOp() {
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(MedVar::makeConst(static_cast<uint64_t>(Intrinsic::Brk), 2));
  return Op;
}

MedOp callOp(va_t Target) {
  MedOp Op;
  Op.Opcode = NdOp::CALL;
  Op.addInput(MedVar::makeConst(Target, 8));
  return Op;
}

MedOp returnOp() {
  MedOp Op;
  Op.Opcode = NdOp::RETURN;
  return Op;
}

MedBlock block(int Id, std::initializer_list<MedOp> Ops,
               std::initializer_list<int> Succs = {}) {
  MedBlock Block;
  Block.Id = Id;
  Block.Ops.assign(Ops.begin(), Ops.end());
  Block.Succs.assign(Succs.begin(), Succs.end());
  return Block;
}

MedFunc function(va_t Entry, std::initializer_list<MedBlock> Blocks) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = "sub_" + std::to_string(Entry);
  Func.Blocks.assign(Blocks.begin(), Blocks.end());
  return Func;
}

} // namespace

TEST(MedNoReturn, PropagatesOnlyFromExplicitTerminatingFacts) {
  constexpr va_t TrapEntry = 0x1000;
  constexpr va_t WrapperEntry = 0x2000;
  constexpr va_t MixedEntry = 0x3000;
  constexpr va_t UnknownEntry = 0x4000;

  std::vector<MedFunc> Funcs;
  // Put the wrapper first so proving it requires a second fixed-point round.
  Funcs.push_back(
      function(WrapperEntry, {block(0, {callOp(TrapEntry), returnOp()})}));
  Funcs.push_back(function(MixedEntry, {block(0, {}, {1, 2}),
                                        block(1, {callOp(TrapEntry)}, {2}),
                                        block(2, {returnOp()})}));
  Funcs.push_back(function(UnknownEntry, {block(0, {MedOp{}})}));
  Funcs.push_back(function(TrapEntry, {block(0, {trapOp()})}));

  propagateInternalNoReturn(Funcs, Arch::AArch64);

  EXPECT_TRUE(Funcs[0].DoesNotReturn);
  EXPECT_FALSE(Funcs[1].DoesNotReturn);
  EXPECT_FALSE(Funcs[2].DoesNotReturn);
  EXPECT_TRUE(Funcs[3].DoesNotReturn);
  EXPECT_TRUE(Funcs[0].Blocks[0].Ops[0].DoesNotReturn);
  EXPECT_TRUE(Funcs[1].Blocks[1].Ops[0].DoesNotReturn);

  // The late pipeline refresh is intentionally idempotent.
  propagateInternalNoReturn(Funcs, Arch::AArch64);
  EXPECT_TRUE(Funcs[0].DoesNotReturn);
  EXPECT_TRUE(Funcs[1].Blocks[1].Ops[0].DoesNotReturn);
}
