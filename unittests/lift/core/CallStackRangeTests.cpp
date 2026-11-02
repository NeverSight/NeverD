//===- CallStackRangeTests.cpp - Bounded incoming stack reads ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Distinguish indexed private loads from incoming argument observations.
///
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/LowScalarTargets.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

const auto SP = NdVar::reg(x86reg::RSP, 8);
const auto Index = NdVar::reg(x86reg::RDX, 8);
const auto Address = NdVar::tmp(0, 8);

BinaryImage image() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  return Image;
}

void append(LowBlock &Block, NdOp Opcode, NdVar Output,
            std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Addr = Block.StartAddr;
  Op.Seq = Block.Ops.size();
  Op.Output = Output;
  for (auto Input : Inputs)
    Op.addInput(Input);
  Block.Ops.push_back(Op);
}

LowFunc indexedLoad(int64_t Displacement) {
  LowFunc Function;
  Function.Entry = 0x1000;
  Function.Blocks.resize(1);
  auto &Block = Function.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  Block.EndAddr = Function.Entry + 1;
  append(Block, NdOp::INT_ADD, Address, {SP, NdVar::cst(8, 8)});
  append(Block, NdOp::STORE, {}, {Address, NdVar::reg(x86reg::RCX, 8)});
  append(Block, NdOp::INT_SUB, SP, {SP, NdVar::cst(0x28, 8)});
  append(Block, NdOp::INT_AND, Index, {Index, NdVar::cst(1, 8)});
  append(Block, NdOp::INT_MULT, Index, {Index, NdVar::cst(4, 8)});
  append(Block, NdOp::INT_ADD, Address, {SP, Index});
  append(Block, NdOp::INT_ADD, Address, {Address, NdVar::cst(Displacement, 8)});
  append(Block, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4), {Address});
  append(Block, NdOp::INT_ADD, SP, {SP, NdVar::cst(0x28, 8)});
  append(Block, NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 8)});
  return Function;
}

TEST(CallStackRange, IndexedLocalLoadsDoNotReadTheSpilledHomeArgument) {
  const auto Function = indexedLoad(0x10);
  const auto Effect = localRegisterEffect(image(), Function);
  EXPECT_FALSE(Effect.UnknownStackReads);
  EXPECT_EQ(Effect.StackArgs, 0);
  const auto Solved =
      solveCallRegisterEffects({{Function.Entry, Effect}}, 0xF07, 0x306);
  EXPECT_EQ(Solved.EntryReads.at(Function.Entry)[x86reg::RCX / 8], 0u);
}

TEST(CallStackRange, IndexedReadsRetainHomeAndStackArgumentsTheyReach) {
  for (const int64_t Offset : {0x30, 0x50, 0x58}) {
    SCOPED_TRACE(Offset);
    const auto Function = indexedLoad(Offset);
    const auto Effect = localRegisterEffect(image(), Function);
    EXPECT_FALSE(Effect.UnknownStackReads);
    EXPECT_EQ(Effect.StackArgs, Offset == 0x30 ? 0 : Offset == 0x50 ? 5 : 6);
    const auto Solved =
        solveCallRegisterEffects({{Function.Entry, Effect}}, 0xF07, 0x306);
    EXPECT_EQ(Solved.EntryReads.at(Function.Entry)[x86reg::RCX / 8],
              Offset == 0x30 ? 8u : 0u);
  }
}

TEST(CallStackRange, SystemVReadsUseItsFirstStackSlot) {
  for (const auto Format : {BinaryFormat::ELF, BinaryFormat::MachO})
    for (const int64_t Offset : {0x10, 0x30, 0x38}) {
      SCOPED_TRACE(::testing::Message() << unsigned(Format) << ':' << Offset);
      auto Image = image();
      Image.Format = Format;
      const auto Effect = localRegisterEffect(Image, indexedLoad(Offset));
      EXPECT_FALSE(Effect.UnknownStackReads);
      EXPECT_EQ(Effect.StackArgs, Offset == 0x10 ? 0 : Offset == 0x30 ? 7 : 8);
    }
}

TEST(CallStackRange, MissingAndInvalidatedOffsetsRemainUnknown) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = indexedLoad(0x10);
    auto &Ops = Function.Blocks.front().Ops;
    if (Mutation == 0)
      Ops[3].Opcode = NdOp::INT_OR;
    else if (Mutation == 1)
      Ops[3].Inputs[1] = NdVar::cst(0xFF, 8); // Beyond the complete cover cap.
    else if (Mutation == 2) {
      Ops[4].Opcode = NdOp::LOAD;
      Ops[4].NumInputs = 1;
    } else if (Mutation == 3) {
      Ops[4].Opcode = NdOp::CALL;
      Ops[4].Output = {};
      Ops[4].Inputs[0] = NdVar::cst(0x2000, 8);
      Ops[4].NumInputs = 1;
    } else if (Mutation == 4) {
      Ops[4].Opcode = NdOp::COPY;
      Ops[4].Output = NdVar::reg(x86reg::RDX + 1, 1);
      Ops[4].Inputs[0] = NdVar::reg(x86reg::RCX, 1);
      Ops[4].NumInputs = 1;
    } else if (Mutation == 5)
      Function.ModuleAnalysisRoots.insert(0x1001);
    else if (Mutation == 6)
      Ops[4].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    else if (Mutation == 7)
      Function.UnsafeIndirectBranchAddresses.insert(0x1000);
    else
      Ops[5].Inputs[1].Size = 4;
    EXPECT_TRUE(localRegisterEffect(image(), Function).UnknownStackReads);
  }
}

TEST(CallStackRange, FiniteNegativeOffsetsAndSubtractionStayBelowEntry) {
  for (bool Subtract : {false, true}) {
    auto Function = indexedLoad(0x10);
    auto &Ops = Function.Blocks.front().Ops;
    Ops[5].Opcode = Subtract ? NdOp::INT_SUB : NdOp::INT_ADD;
    Ops[4].Inputs[1] = NdVar::cst(Subtract ? 4 : uint64_t(-4), 8);
    const auto Effect = localRegisterEffect(image(), Function);
    EXPECT_FALSE(Effect.UnknownStackReads);
    EXPECT_EQ(Effect.StackArgs, 0);
  }
}

TEST(CallStackRange, OverflowAndUnrepresentableArgumentCountsFailClosed) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = indexedLoad(std::numeric_limits<int64_t>::max());
    auto &Ops = Function.Blocks.front().Ops;
    if (Mutation == 0)
      Ops[6].Inputs[1] = NdVar::cst(uint64_t{1} << 63, 8);
    else if (Mutation == 1) {
      Ops[5].Opcode = NdOp::INT_SUB;
      Ops[4].Opcode = NdOp::COPY;
      Ops[4].Inputs[0] = NdVar::cst(uint64_t{1} << 63, 8);
      Ops[4].NumInputs = 1;
    } else if (Mutation == 2)
      Ops[6].Inputs[1] = NdVar::cst(uint64_t{1} << 40, 8);
    else if (Mutation == 3)
      Ops[6].Inputs[1] = NdVar::cst(uint64_t{1} << 63 | 0x10, 8);
    else {
      // Address fits, but the final byte of its load does not.
      Ops[2].Inputs[1] = NdVar::cst(0, 8);
      Ops[3].Inputs[1] = NdVar::cst(0, 8);
    }
    EXPECT_TRUE(localRegisterEffect(image(), Function).UnknownStackReads);
  }
}

TEST(CallStackRange, EveryJoinPathMustSupplyTheIndexCover) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = indexedLoad(0x10);
    auto Prefix = Function.Blocks.front();
    auto Suffix = Prefix;
    Prefix.Ops.resize(3);
    Prefix.Succs = {1, 2};
    Suffix.Id = 3;
    Suffix.StartAddr = 0x1300;
    Suffix.Preds = {1, 2};
    Suffix.Ops.erase(Suffix.Ops.begin(), Suffix.Ops.begin() + 4);
    Function.Blocks = {Prefix};
    for (int Id : {1, 2}) {
      LowBlock Arm;
      Arm.Id = Id;
      Arm.StartAddr = 0x1000 + 0x100 * Id;
      Arm.Preds = {0};
      Arm.Succs = {3};
      append(Arm, NdOp::COPY, Index, {NdVar::cst(Id - 1, 8)});
      if (Id == 2 && Mutation == 1)
        Arm.Ops.clear();
      else if (Id == 2 && Mutation == 2)
        Arm.Ops[0].Inputs[0] = NdVar::reg(x86reg::RCX, 8);
      Function.Blocks.push_back(Arm);
    }
    Function.Blocks.push_back(Suffix);
    if (Mutation == 3)
      std::swap(Function.Blocks[1], Function.Blocks[2]);
    const auto Effect = localRegisterEffect(image(), Function);
    EXPECT_EQ(Effect.UnknownStackReads, Mutation == 1 || Mutation == 2);
    EXPECT_EQ(Effect.StackArgs, 0);
  }
}

TEST(CallStackRange, PartialTemporaryWritesCannotKeepAnOldWholeOffset) {
  auto Function = indexedLoad(0x10);
  auto &Ops = Function.Blocks.front().Ops;
  const auto Offset = NdVar::tmp(16, 8);
  Ops[3].Output = Offset;
  Ops[4].Opcode = NdOp::COPY;
  Ops[4].Output = NdVar::tmp(17, 1);
  Ops[4].Inputs[0] = NdVar::reg(x86reg::RCX, 1);
  Ops[4].NumInputs = 1;
  Ops[5].Inputs[1] = Offset;
  EXPECT_TRUE(localRegisterEffect(image(), Function).UnknownStackReads);
}

TEST(CallStackRange, TemporaryOffsetsDoNotCrossMachineInstructions) {
  auto Function = indexedLoad(0x10);
  auto &Ops = Function.Blocks.front().Ops;
  Ops[4].Output = NdVar::tmp(16, 8);
  Ops[5].Inputs[1] = Ops[4].Output;
  for (size_t I = 5; I < Ops.size(); ++I)
    ++Ops[I].Addr;
  EXPECT_TRUE(localRegisterEffect(image(), Function).UnknownStackReads);
}

TEST(CallStackRange, LoopBackedgesRetainAllPossibleIndexValues) {
  for (bool Mask : {false, true}) {
    SCOPED_TRACE(Mask);
    auto Function = indexedLoad(0x10);
    auto Prefix = Function.Blocks.front();
    auto Loop = Prefix;
    auto Suffix = Prefix;
    Prefix.Ops.resize(3);
    append(Prefix, NdOp::COPY, Index, {NdVar::cst(0, 8)});
    Prefix.Succs = {1};
    Loop.Id = 1;
    Loop.StartAddr = 0x1100;
    Loop.Preds = {0, 1};
    Loop.Succs = {1, 2};
    Loop.Ops.erase(Loop.Ops.begin() + 8, Loop.Ops.end());
    Loop.Ops.erase(Loop.Ops.begin(), Loop.Ops.begin() + (Mask ? 3 : 4));
    append(Loop, NdOp::INT_ADD, Index, {Index, NdVar::cst(1, 8)});
    Suffix.Id = 2;
    Suffix.StartAddr = 0x1200;
    Suffix.Preds = {1};
    Suffix.Ops.erase(Suffix.Ops.begin(), Suffix.Ops.begin() + 8);
    Function.Blocks = {Prefix, Loop, Suffix};
    const auto Effect = localRegisterEffect(image(), Function);
    EXPECT_EQ(Effect.UnknownStackReads, !Mask);
    EXPECT_EQ(Effect.StackArgs, 0);
  }
}

TEST(CallStackRange, AFiniteIncomingPointerStillEscapesThroughACall) {
  auto Function = indexedLoad(0x30);
  auto &Ops = Function.Blocks.front().Ops;
  Ops[7].Opcode = NdOp::COPY;
  Ops[7].Output = NdVar::reg(x86reg::RCX, 8);
  Ops[8].Opcode = NdOp::CALL;
  Ops[8].Output = {};
  Ops[8].NumInputs = 1;
  Ops[8].Inputs[0] = NdVar::cst(0x2000, 8);
  EXPECT_TRUE(localRegisterEffect(image(), Function).UnknownStackReads);
}

TEST(CallStackRange, ExhaustedScalarWorkPublishesNoPartialCover) {
  const auto Function = indexedLoad(0x10);
  const auto *Query = &Function.Blocks.front().Ops[5];
  bool Proved = false, Refused = false;
  for (size_t Budget = 0; Budget != 512; ++Budget) {
    size_t Remaining = Budget;
    const auto Values = finiteScalarOperandValues(
        CallRegisterBodyView(Function), {{Query, {1}}}, Remaining);
    EXPECT_LE(Remaining, Budget);
    if (!Values) {
      Refused = true;
      continue;
    }
    Proved = true;
    ASSERT_TRUE(Values->count({Query, 1}));
    EXPECT_EQ(Values->at({Query, 1}), (std::vector<uint64_t>{0, 4}));
  }
  EXPECT_TRUE(Proved);
  EXPECT_TRUE(Refused);
}
} // namespace
