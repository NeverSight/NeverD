//===- MedConstantPropagationTests.cpp - Stable MedIR value propagation ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file Checks SSA value identity, widths, and bounded propagation.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedConstantPropagation.h"
#include "neverd/ir/med/MedIR.h"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>
#include <vector>

using namespace neverd;
namespace {
MedVar variable(int Id, uint16_t Size = 8) {
  MedVar Value;
  Value.Id = Id;
  Value.SSAVer = 1;
  Value.Size = Size;
  return Value;
}
MedOp operation(NdOp Opcode, MedVar Output, std::vector<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

TEST(MedConstantPropagation, SelfComparisonNeedsOneStableObservation) {
  for (auto Kind : {MedVar::Reg, MedVar::Temp, MedVar::Param, MedVar::Const})
    for (uint16_t Width : {1, 2, 4, 8}) {
      auto A = Kind == MedVar::Const ? MedVar::makeConst(19, Width)
                                     : variable(7, Width);
      A.Kind = Kind;
      for (NdOp Opcode :
           {NdOp::INT_EQUAL, NdOp::INT_LESSEQUAL, NdOp::INT_SLESSEQUAL})
        EXPECT_EQ(selfComparisonConstant(Opcode, A, A), true);
      for (NdOp Opcode : {NdOp::INT_NOTEQUAL, NdOp::INT_LESS, NdOp::INT_SLESS,
                          NdOp::INT_SBOR})
        EXPECT_EQ(selfComparisonConstant(Opcode, A, A), false);
      for (NdOp Opcode :
           {NdOp::INT_CARRY, NdOp::INT_SOVF, NdOp::INT_AND, NdOp::FLOAT_EQUAL})
        EXPECT_FALSE(selfComparisonConstant(Opcode, A, A));
    }

  const auto A = variable(7);
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    auto Left = A;
    auto Right = A;
    switch (Mutation) {
    case 0:
      Left.Kind = Right.Kind = MedVar::Unspecified;
      break;
    case 1:
      Left.Id = Right.Id = -1;
      break;
    case 2:
      ++Right.SSAVer;
      break;
    case 3:
      Right.Kind = MedVar::Reg;
      Left.Kind = MedVar::Temp;
      break;
    case 4:
      Right.Size = 4;
      break;
    case 5:
      Left.Size = Right.Size = 0;
      break;
    }
    EXPECT_FALSE(selfComparisonConstant(NdOp::INT_EQUAL, Left, Right));
    EXPECT_FALSE(selfComparisonConstant(NdOp::INT_SBOR, Left, Right));
  }
}

MedFunc loop(MedVar Seed) {
  MedFunc Func;
  Func.Blocks.resize(4);
  for (int I = 0; I < 4; ++I)
    Func.Blocks[I].Id = I;
  auto &Entry = Func.Blocks[0];
  Entry.Succs = {1};
  Entry.Ops.push_back(operation(NdOp::COPY, variable(1, Seed.Size), {Seed}));
  auto &Header = Func.Blocks[1];
  Header.Preds = {0, 2};
  Header.Succs = {2, 3};
  Header.Phis.push_back(
      {variable(2, Seed.Size),
       {{0, variable(1, Seed.Size)}, {2, variable(3, Seed.Size)}}});
  auto &Backedge = Func.Blocks[2];
  Backedge.Preds = {1};
  Backedge.Succs = {1};
  Backedge.Ops.push_back(
      operation(NdOp::COPY, variable(3, Seed.Size), {variable(2, Seed.Size)}));
  auto &Exit = Func.Blocks[3];
  Exit.Preds = {1};
  Exit.Ops.push_back(operation(NdOp::RETURN, {}, {variable(2, Seed.Size)}));
  return Func;
}
MedBlock &block(MedFunc &Func, int Id) {
  return *std::find_if(Func.Blocks.begin(), Func.Blocks.end(),
                       [&](const auto &Block) { return Block.Id == Id; });
}
const MedVar &result(MedFunc &Func) {
  return block(Func, 3).Ops.back().Inputs[0];
}

TEST(MedConstantPropagation,
     SeededCyclesConvergeIndependentlyOfTraversalOrder) {
  for (uint16_t Size : {1, 2, 4, 8})
    for (bool ReverseBlocks : {false, true})
      for (bool ReverseArguments : {false, true}) {
        const auto Seed = MedVar::makeConst(73, Size);
        auto Func = loop(Seed);
        if (ReverseBlocks)
          std::reverse(Func.Blocks.begin(), Func.Blocks.end());
        auto &Arguments = block(Func, 1).Phis.front().Args;
        if (ReverseArguments)
          std::reverse(Arguments.begin(), Arguments.end());
        ASSERT_TRUE(propagateInvariantConstants(Func));
        EXPECT_EQ(result(Func), Seed);
        EXPECT_FALSE(propagateInvariantConstants(Func));
      }
}

TEST(MedConstantPropagation, UnknownAndUnseededArmsInvalidateProvisionalFacts) {
  for (bool ReverseBlocks : {false, true}) {
    auto Func = loop(MedVar::makeConst(73, 8));
    auto &Backedge = block(Func, 2);
    // A separate, unseeded cycle feeds an otherwise seeded merge. Pending
    // values cannot be treated as irrelevant alternatives to the known seed.
    Backedge.Ops = {operation(NdOp::COPY, variable(3), {variable(4)}),
                    operation(NdOp::COPY, variable(4), {variable(3)})};
    if (ReverseBlocks)
      std::reverse(Func.Blocks.begin(), Func.Blocks.end());
    propagateInvariantConstants(Func);
    EXPECT_FALSE(result(Func).isConst());
  }
  for (auto Opcode : {NdOp::COPY, NdOp::LOAD, NdOp::CALL, NdOp::INT_ADD}) {
    auto Func = loop(variable(9));
    auto &Seed = block(Func, 0).Ops.front();
    Seed.Opcode = Opcode;
    if (Opcode != NdOp::COPY)
      Seed.Inputs[0] = MedVar::makeConst(73, 8);
    if (Opcode == NdOp::INT_ADD)
      Seed.addInput(MedVar::makeConst(0, 8));
    propagateInvariantConstants(Func);
    EXPECT_FALSE(result(Func).isConst());
    EXPECT_EQ(Seed.Opcode, Opcode);
  }
}

TEST(MedConstantPropagation, EqualBitsRetainWidthProvenanceAndOwnerIdentity) {
  const auto Seed = MedVar::makeConst(
      0x123000, 8, ConstantAddressProvenance::DataAddress, 0x120000);
  auto Control = loop(Seed);
  ASSERT_TRUE(propagateInvariantConstants(Control));
  EXPECT_EQ(result(Control), Seed);
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    auto Func = loop(Seed);
    auto Alternative = Seed;
    if (Mutation == 0)
      Alternative.ConstVal += 8;
    else if (Mutation == 1)
      Alternative.Provenance = ConstantAddressProvenance::Scalar;
    else if (Mutation == 2)
      Alternative.AddressOwnerVA += 8;
    else
      Alternative.Size = 4;
    block(Func, 2).Ops.front().Inputs[0] = Alternative;
    propagateInvariantConstants(Func);
    EXPECT_FALSE(result(Func).isConst()) << Mutation;
  }
}

TEST(MedConstantPropagation,
     IncompleteEdgesAndDuplicateDefinitionsSupplyNoFacts) {
  for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
    auto Func = loop(MedVar::makeConst(73, 8));
    auto &Header = block(Func, 1);
    if (Mutation == 0)
      Header.Phis.front().Args.pop_back();
    else if (Mutation == 1)
      Header.Phis.front().Args.back().first = 0;
    else if (Mutation == 2)
      Header.Preds.pop_back();
    else if (Mutation == 3)
      block(Func, 0).Ops.push_back(
          operation(NdOp::COPY, variable(1), {MedVar::makeConst(91, 8)}));
    else
      block(Func, 2).Ops.front().MemoryOrdering = NdMemoryOrdering::Acquire;
    propagateInvariantConstants(Func);
    EXPECT_FALSE(result(Func).isConst()) << Mutation;
  }
  auto Func = loop(MedVar::makeConst(73, 8));
  block(Func, 0).Succs.push_back(99);
  EXPECT_FALSE(propagateInvariantConstants(Func));
  EXPECT_FALSE(result(Func).isConst());
}

TEST(MedConstantPropagation, PhiInputsRequireOneArgumentPerIncomingEdge) {
  for (int Count : {1, 2, 4, 5, 17})
    for (bool Reverse : {false, true})
      for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
        SCOPED_TRACE(Count);
        SCOPED_TRACE(Reverse);
        SCOPED_TRACE(Mutation);
        MedBlock Block;
        PhiNode Phi;
        Phi.Output = variable(1);
        std::set<int> Incoming;
        for (int I = 0; I != Count; ++I) {
          const int Pred = std::numeric_limits<int>::max() - I;
          Block.Preds.push_back(Pred);
          Phi.Args.emplace_back(Pred, MedVar::makeConst(73, 8));
          Incoming.insert(Pred);
        }
        if (Reverse)
          std::reverse(Phi.Args.begin(), Phi.Args.end());
        switch (Mutation) {
        case 0:
          break;
        case 1:
          Block.Preds.back() = Count == 1 ? -1 : Block.Preds.front();
          break;
        case 2:
          Phi.Args.back().first = Count == 1 ? -1 : Phi.Args.front().first;
          break;
        case 3:
          Phi.Args.pop_back();
          break;
        case 4:
          Phi.Args.push_back(Phi.Args.front());
          break;
        case 5:
          Phi.Args.back().second.Size = 4;
          break;
        case 6:
          Phi.Output.Size = 0;
          break;
        case 7:
          Phi.ExceptionalEntry = variable(2);
          break;
        case 8:
          Block.Preds.clear();
          break;
        case 9:
          Incoming.erase(Incoming.begin());
          Incoming.insert(-1);
          break;
        }
        EXPECT_EQ(hasCompleteOrdinaryPhiInputs(Block, Phi, Incoming),
                  Mutation == 0);
      }
}

TEST(MedConstantPropagation, CompletePredecessorsDoNotCompleteAnotherPhi) {
  for (bool Reverse : {false, true}) {
    auto Func = loop(MedVar::makeConst(73, 8));
    auto &Header = block(Func, 1);
    auto Incomplete = Header.Phis.front();
    Incomplete.Output = variable(10);
    Incomplete.Args.pop_back();
    Header.Phis.push_back(Incomplete);
    if (Reverse)
      std::reverse(Header.Phis.begin(), Header.Phis.end());
    auto &Exit = block(Func, 3);
    Exit.Ops.insert(Exit.Ops.begin(), operation(NdOp::INT_ADD, variable(11),
                                                {variable(10), variable(2)}));
    ASSERT_TRUE(propagateInvariantConstants(Func));
    EXPECT_EQ(Exit.Ops.front().Inputs[0], variable(10));
    EXPECT_EQ(Exit.Ops.front().Inputs[1].ConstVal, 73U);
    EXPECT_TRUE(Exit.Ops.front().Inputs[1].isConst());
    EXPECT_EQ(result(Func), MedVar::makeConst(73, 8));
  }
}

TEST(MedConstantPropagation, EffectsAndOperandRolesSurviveConstantTransport) {
  auto Func = loop(MedVar::makeConst(73, 8));
  auto &Exit = block(Func, 3);
  Exit.Ops.insert(Exit.Ops.begin(),
                  operation(NdOp::STORE, {}, {variable(2), variable(9)}));
  Exit.Ops.insert(Exit.Ops.begin(),
                  operation(NdOp::CALL, variable(5), {variable(2)}));
  Exit.Ops.insert(Exit.Ops.begin(),
                  operation(NdOp::INT_ADD, variable(6),
                            {variable(2), MedVar::makeConst(1, 8)}));
  ASSERT_TRUE(propagateInvariantConstants(Func));
  ASSERT_EQ(Exit.Ops.size(), 4U);
  EXPECT_EQ(Exit.Ops[0].Inputs[0].Provenance,
            ConstantAddressProvenance::Scalar);
  EXPECT_EQ(Exit.Ops[1].Opcode, NdOp::CALL);
  EXPECT_EQ(Exit.Ops[1].Inputs[0].Provenance,
            ConstantAddressProvenance::Unknown);
  EXPECT_EQ(Exit.Ops[2].Opcode, NdOp::STORE);
  EXPECT_EQ(Exit.Ops[2].Inputs[0].Provenance,
            ConstantAddressProvenance::Unknown);
  EXPECT_EQ(Exit.Ops[2].Inputs[1], variable(9));
}

TEST(MedConstantPropagation, BudgetExhaustionLeavesAllOperandsUnchanged) {
  auto Func = loop(MedVar::makeConst(73, 8));
  for (int I = 10; I < 65546; ++I)
    block(Func, 0).Ops.push_back(
        operation(NdOp::COPY, variable(I), {variable(1)}));
  EXPECT_FALSE(propagateInvariantConstants(Func));
  EXPECT_EQ(result(Func), variable(2));
  EXPECT_EQ(block(Func, 0).Ops.back().Inputs[0], variable(1));
}

TEST(MedConstantPropagation,
     FullSsaIdentityKeepsMaximumIdsAndVersionsDistinct) {
  MedFunc Func;
  Func.Blocks.resize(1);
  auto &Block = Func.Blocks.front();
  Block.Id = 0;
  auto A = variable(std::numeric_limits<int>::max());
  A.Kind = MedVar::Temp;
  A.SSAVer = std::numeric_limits<int>::max();
  auto B = A;
  B.Kind = MedVar::Reg;
  auto C = B;
  --C.SSAVer;
  auto Unknown = variable(-1);
  Unknown.Kind = MedVar::Temp;
  Unknown.SSAVer = -1;
  Block.Ops = {operation(NdOp::COPY, A, {MedVar::makeConst(7, 8)}),
               operation(NdOp::COPY, B, {MedVar::makeConst(11, 8)}),
               operation(NdOp::COPY, C, {MedVar::makeConst(13, 8)}),
               operation(NdOp::INT_ADD, variable(10), {A, B}),
               operation(NdOp::INT_ADD, variable(11), {Unknown, C}),
               operation(NdOp::RETURN, {}, {C})};
  ASSERT_TRUE(propagateInvariantConstants(Func));
  EXPECT_EQ(Block.Ops[3].Inputs[0],
            MedVar::makeConst(7, 8, ConstantAddressProvenance::Scalar));
  EXPECT_EQ(Block.Ops[3].Inputs[1],
            MedVar::makeConst(11, 8, ConstantAddressProvenance::Scalar));
  EXPECT_EQ(Block.Ops[4].Inputs[0], Unknown);
  EXPECT_EQ(Block.Ops[4].Inputs[1],
            MedVar::makeConst(13, 8, ConstantAddressProvenance::Scalar));
  EXPECT_EQ(Block.Ops.back().Inputs[0], MedVar::makeConst(13, 8));
}

TEST(MedConstantPropagation, ReverseWidthChainsFitTheLinearWorkBudget) {
  for (const int Count : {600, 2000}) {
    SCOPED_TRACE(Count);
    MedFunc Func;
    Func.Blocks.resize(1);
    auto &Block = Func.Blocks.front();
    Block.Id = 0;
    for (int Id = Count; Id >= 0; --Id)
      Block.Ops.push_back(operation(
          Id == Count ? NdOp::INT_ZEXT : NdOp::COPY, variable(Id, 4),
          {Id == Count ? MedVar::makeConst(7, 1) : variable(Id + 1, 4)}));
    Block.Ops.push_back(operation(NdOp::RETURN, {}, {variable(0, 4)}));
    const auto Original = Block.Ops;
    ASSERT_TRUE(propagateInvariantConstants(Func));
    EXPECT_EQ(Block.Ops.back().Inputs[0], MedVar::makeConst(7, 4));
    // The return can use the constant, but each copy keeps its width origin.
    for (size_t I = 0; I + 1 < Block.Ops.size(); ++I)
      EXPECT_EQ(Block.Ops[I].Inputs[0], Original[I].Inputs[0]);
  }
}

TEST(MedConstantPropagation, WidthOriginsSurviveCyclesAndStayLocalToTheirUses) {
  for (auto View : {NdOp::INT_ZEXT, NdOp::INT_SEXT, NdOp::SUBBYTES})
    for (bool ReverseBlocks : {false, true})
      for (bool ReverseArguments : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << unsigned(View) << ':' << ReverseBlocks << ':'
                     << ReverseArguments);
        const auto Value = MedVar::makeConst(7, 4);
        auto Func = loop(Value);
        auto &Seed = block(Func, 0).Ops.front();
        Seed =
            operation(View, variable(1, 4),
                      {MedVar::makeConst(7, View == NdOp::SUBBYTES ? 8 : 1)});
        if (View == NdOp::SUBBYTES)
          Seed.addInput(MedVar::makeConst(0, 1));
        auto &Exit = block(Func, 3);
        Exit.Ops = {
            operation(NdOp::COPY, variable(10, 4), {Value}),
            operation(NdOp::COPY, variable(11, 4), {variable(2, 4)}),
            operation(NdOp::CALL, {}, {variable(11, 4), variable(10, 4)}),
            operation(NdOp::INT_ADD, variable(12, 4), {variable(11, 4), Value}),
            operation(NdOp::STORE, {}, {variable(99), variable(11, 4)}),
            operation(NdOp::RETURN, {}, {variable(11, 4)})};
        auto &Arguments = block(Func, 1).Phis.front().Args;
        if (ReverseArguments)
          std::reverse(Arguments.begin(), Arguments.end());
        if (ReverseBlocks)
          std::reverse(Func.Blocks.begin(), Func.Blocks.end());
        ASSERT_TRUE(propagateInvariantConstants(Func));
        const auto &Ops = block(Func, 3).Ops;
        EXPECT_EQ(Ops[1].Inputs[0], variable(2, 4));
        EXPECT_EQ(Ops[2].Inputs[0], variable(11, 4));
        EXPECT_EQ(Ops[2].Inputs[1], Value);
        EXPECT_EQ(Ops[3].Inputs[0],
                  MedVar::makeConst(7, 4, ConstantAddressProvenance::Scalar));
        EXPECT_EQ(Ops[4].Inputs[0], variable(99));
        EXPECT_EQ(Ops[4].Inputs[1], Value);
        EXPECT_EQ(Ops[5].Inputs[0], Value);
      }
}

TEST(MedConstantPropagation,
     WidthOriginWorkExhaustionLeavesEveryOperandIntact) {
  // All PHIs are complete and agree on the seed. The larger join fits the
  // definition cap and value solver, but its width-origin use edges exhaust
  // the remaining shared work budget before any operands may be replaced.
  for (int Fanout : {8, 512}) {
    SCOPED_TRACE(Fanout);
    MedFunc Func;
    Func.Blocks.resize(Fanout + 2);
    for (int Id = 0; Id < Fanout + 2; ++Id)
      Func.Blocks[Id].Id = Id;
    auto &Entry = Func.Blocks.front();
    Entry.Ops.push_back(
        operation(NdOp::INT_ZEXT, variable(0, 4), {MedVar::makeConst(7, 1)}));
    auto &Join = Func.Blocks.back();
    for (int Id = 1; Id <= Fanout; ++Id) {
      Entry.Succs.push_back(Id);
      Func.Blocks[Id].Preds = {0};
      Func.Blocks[Id].Succs = {Join.Id};
      Join.Preds.push_back(Id);
    }
    for (int Id = 1; Id <= Fanout; ++Id) {
      PhiNode Phi;
      Phi.Output = variable(Id, 4);
      for (int Pred : Join.Preds)
        Phi.Args.emplace_back(Pred, variable(0, 4));
      Join.Phis.push_back(std::move(Phi));
    }
    Join.Ops.push_back(operation(NdOp::RETURN, {}, {variable(1, 4)}));
    const auto Original = Func;
    const bool Complete = Fanout == 8;
    EXPECT_EQ(propagateInvariantConstants(Func), Complete);
    EXPECT_EQ(Join.Ops.back().Inputs[0],
              Complete ? MedVar::makeConst(7, 4) : variable(1, 4));
    if (!Complete)
      for (size_t B = 0; B < Func.Blocks.size(); ++B) {
        const auto &Before = Original.Blocks[B];
        const auto &After = Func.Blocks[B];
        for (size_t I = 0; I < After.Phis.size(); ++I)
          EXPECT_EQ(After.Phis[I].Args, Before.Phis[I].Args);
        for (size_t I = 0; I < After.Ops.size(); ++I)
          EXPECT_EQ(After.Ops[I].Inputs, Before.Ops[I].Inputs);
      }
  }
}

TEST(MedConstantPropagation,
     LowConversionPreservesOneValueAcrossLoopDefinitions) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto Held = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
      const auto Anchor = NdVar::reg(TRI.IntParamRegs[1], TRI.PointerSize);
      const auto Count = NdVar::reg(TRI.IntParamRegs[2], TRI.PointerSize);
      const auto Condition = NdVar::tmp(0, 1);
      LowFunc Low;
      Low.Entry = 0x1000;
      Low.Name = "invariant_loop_return";
      Low.Blocks.resize(4);
      for (int I = 0; I < 4; ++I) {
        Low.Blocks[I].Id = I;
        Low.Blocks[I].StartAddr = 0x1000 + 4 * I;
        Low.Blocks[I].EndAddr = Low.Blocks[I].StartAddr + 4;
      }
      Low.Blocks[0].Succs = {1};
      Low.Blocks[1].Preds = {0, 2};
      Low.Blocks[1].Succs = {2, 3};
      Low.Blocks[2].Preds = {1};
      Low.Blocks[2].Succs = {1};
      Low.Blocks[3].Preds = {1};
      auto Add = [&](int Block, NdOp Opcode, NdVar Output,
                     std::initializer_list<NdVar> Inputs) {
        LowOp Op;
        Op.Opcode = Opcode;
        Op.Output = Output;
        Op.Addr = Low.Blocks[Block].StartAddr;
        for (const auto &Input : Inputs)
          Op.addInput(Input);
        Low.Blocks[Block].Ops.push_back(Op);
      };
      Add(0, NdOp::COPY, Held, {NdVar::cst(73, TRI.PointerSize)});
      Add(0, NdOp::COPY, Anchor, {Held});
      Add(0, NdOp::BRANCH, {}, {NdVar::cst(0x1004, TRI.PointerSize)});
      Add(1, NdOp::INT_EQUAL, Condition,
          {Count, NdVar::cst(0, TRI.PointerSize)});
      Add(1, NdOp::COND_BR, {},
          {NdVar::cst(0x100c, TRI.PointerSize), Condition});
      Add(2, NdOp::INT_SUB, Count, {Count, NdVar::cst(1, TRI.PointerSize)});
      Add(2, NdOp::COPY, Held, {Anchor});
      Add(2, NdOp::BRANCH, {}, {NdVar::cst(0x1004, TRI.PointerSize)});
      Add(3, NdOp::RETURN, {}, {Held});
      auto Med = LowToMedConverter().convert(Low, Architecture, Format);
      ASSERT_TRUE(verifyMedFunc(Med, "invariant-loop-return"));
      const auto &Return = Med.Blocks.back().Ops.back();
      ASSERT_EQ(Return.Opcode, NdOp::RETURN);
      ASSERT_EQ(Return.NumInputs, 1U);
      EXPECT_EQ(Return.Inputs[0], MedVar::makeConst(73, TRI.PointerSize));
    }
}

TEST(MedConstantPropagation,
     DefaultWordAbsorptionRequiresTheSameSSAValueAndWidth) {
  for (auto Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF})
      for (auto Outer : {NdOp::INT_AND, NdOp::INT_OR})
        for (bool Reverse : {false, true})
          for (unsigned Mutation = 0; Mutation < 3; ++Mutation) {
            SCOPED_TRACE(testing::Message()
                         << unsigned(Architecture) << ':' << unsigned(Format)
                         << ':' << unsigned(Outer) << ':' << Reverse << ':'
                         << Mutation);
            const auto &TRI = getTargetRegInfo(Architecture);
            const auto Width = TRI.PointerSize;
            const auto Arguments = TRI.integerArgumentLayout(Format).Registers;
            const auto Input = NdVar::reg(Arguments.empty() ? TRI.IntReturnReg
                                                            : Arguments.front(),
                                          Width);
            const auto Result = NdVar::reg(TRI.IntReturnReg, Width);
            LowFunc Low;
            Low.Entry = 0x1000;
            Low.Name = "word_absorption";
            Low.Blocks.resize(1);
            auto &Block = Low.Blocks.front();
            Block.Id = 0;
            Block.StartAddr = Low.Entry;
            Block.EndAddr = Low.Entry + 4;
            const auto Add = [&](NdOp Opcode, NdVar Output,
                                 std::initializer_list<NdVar> Inputs) {
              LowOp Op;
              Op.Opcode = Opcode;
              Op.Output = Output;
              Op.Addr = Low.Entry;
              for (const auto &Value : Inputs)
                Op.addInput(Value);
              Block.Ops.push_back(Op);
            };
            const auto Other = NdVar::tmp(0, Width);
            const auto Nested = NdVar::tmp(1, Width);
            Add(NdOp::INT_ADD, Other, {Input, Input});
            Add(Outer == NdOp::INT_AND ? NdOp::INT_OR : NdOp::INT_AND, Nested,
                {Reverse ? Other : Input, Reverse ? Input : Other});
            auto NestedOperand = Nested;
            if (Mutation == 1)
              Add(NdOp::INT_XOR, Input, {Input, NdVar::cst(0x55, Width)});
            if (Mutation == 2) {
              const auto Narrow = NdVar::tmp(2, Width / 2);
              NestedOperand = NdVar::tmp(3, Width);
              Add(NdOp::SUBBYTES, Narrow, {Nested, NdVar::cst(0, Width)});
              Add(NdOp::INT_ZEXT, NestedOperand, {Narrow});
            }
            Add(Outer, Result,
                {Reverse ? Input : NestedOperand,
                 Reverse ? NestedOperand : Input});
            Add(NdOp::RETURN, {}, {Result});
            const auto Med =
                LowToMedConverter().convert(Low, Architecture, Format);
            ASSERT_TRUE(verifyMedFunc(Med, "default-word-absorption"));
            bool RetainsOuter = false;
            for (const auto &B : Med.Blocks)
              for (const auto &Op : B.Ops)
                if (!Op.Dead)
                  RetainsOuter |= Op.Opcode == Outer;
            EXPECT_EQ(RetainsOuter, Mutation != 0);
          }
}
} // namespace
