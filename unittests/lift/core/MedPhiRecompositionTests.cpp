//===- MedPhiRecompositionTests.cpp - Correlated word joins --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check complete edge correlation, bitvector behavior and retained effects.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedPhiRecomposition.h"

#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

using namespace neverd;
namespace {
MedVar value(int Id, uint16_t Size, MedVar::VarKind Kind = MedVar::Temp) {
  MedVar V;
  V.Id = Id;
  V.Size = Size;
  V.SSAVer = Kind == MedVar::Param ? 0 : 1;
  V.Kind = Kind;
  V.TheArch = Arch::X64;
  return V;
}
MedOp operation(NdOp Opcode, MedVar Output, std::vector<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (auto Input : Inputs)
    Op.addInput(Input);
  return Op;
}
void slices(MedBlock &Block, MedVar Whole, MedVar Low, MedVar High,
            bool Shifted) {
  const auto Zero = MedVar::makeConst(0, 1);
  Block.Ops.push_back(operation(NdOp::SUBBYTES, Low, {Whole, Zero}));
  if (Shifted) {
    const auto Shift = value(High.Id + 1000, Whole.Size);
    Block.Ops.push_back(
        operation(NdOp::INT_RIGHT, Shift,
                  {Whole, MedVar::makeConst(Low.Size * 8u, Whole.Size)}));
    Block.Ops.push_back(operation(NdOp::SUBBYTES, High, {Shift, Zero}));
  } else {
    Block.Ops.push_back(operation(NdOp::SUBBYTES, High,
                                  {Whole, MedVar::makeConst(Low.Size, 1)}));
  }
}
MedFunc diamond(uint16_t LowSize, uint16_t HighSize, bool Concat,
                bool Shifted) {
  const uint16_t Whole = LowSize + HighSize;
  MedFunc F;
  F.Params = {value(0, Whole, MedVar::Param), value(1, Whole, MedVar::Param)};
  F.Blocks.resize(4);
  for (int I = 0; I != 4; ++I)
    F.Blocks[I].Id = I;
  F.Blocks[0].Succs = {1, 2};
  for (int I : {1, 2}) {
    auto &B = F.Blocks[I];
    B.Preds = {0};
    B.Succs = {3};
    slices(B, F.Params[I - 1], value(200 + 10 * I, LowSize),
           value(201 + 10 * I, HighSize), Shifted);
  }
  auto &Join = F.Blocks[3];
  Join.Preds = {1, 2};
  const auto Low = value(100, LowSize), High = value(101, HighSize);
  Join.Phis.push_back(
      {Low, {{1, value(210, LowSize)}, {2, value(220, LowSize)}}});
  Join.Phis.push_back(
      {High, {{2, value(221, HighSize)}, {1, value(211, HighSize)}}});
  if (Concat) {
    Join.Ops.push_back(operation(NdOp::CONCAT, value(400, Whole), {High, Low}));
  } else {
    const auto L = value(300, Whole), H = value(301, Whole);
    Join.Ops.push_back(operation(NdOp::INT_ZEXT, L, {Low}));
    Join.Ops.push_back(operation(NdOp::INT_ZEXT, H, {High}));
    Join.Ops.push_back(operation(NdOp::INT_LEFT, value(302, Whole),
                                 {H, MedVar::makeConst(LowSize * 8u, Whole)}));
    Join.Ops.push_back(
        operation(NdOp::INT_OR, value(400, Whole), {value(302, Whole), L}));
  }
  Join.Ops.push_back(operation(NdOp::RETURN, {}, {value(400, Whole)}));
  return F;
}

using Key = std::tuple<int, int, int, uint16_t>;
Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer, V.Size}; }

// Evaluate the selected CFG route using ordinary bitvector operation semantics.
// This checks the old and new programs independently of the matching rule.
llvm::APInt execute(const MedFunc &F, const std::vector<int> &Route,
                    const llvm::APInt &A, const llvm::APInt &B) {
  std::map<Key, llvm::APInt> Values{{key(F.Params[0]), A},
                                    {key(F.Params[1]), B}};
  auto Read = [&](const MedVar &V) {
    return V.isConst() ? llvm::APInt(V.Size * 8u, V.ConstVal)
                       : Values.at(key(V));
  };
  int Predecessor = -1;
  for (int Id : Route) {
    const auto &Block =
        *std::find_if(F.Blocks.begin(), F.Blocks.end(),
                      [&](const auto &B) { return B.Id == Id; });
    std::vector<std::pair<Key, llvm::APInt>> Incoming;
    for (const auto &Phi : Block.Phis) {
      const auto Edge =
          std::find_if(Phi.Args.begin(), Phi.Args.end(),
                       [&](const auto &A) { return A.first == Predecessor; });
      Incoming.emplace_back(key(Phi.Output), Read(Edge->second));
    }
    for (auto &[K, V] : Incoming)
      Values.insert_or_assign(K, V);
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::RETURN)
        return Read(Op.Inputs[0]);
      const auto Input = Read(Op.Inputs[0]);
      const unsigned Bits = Op.Output.Size * 8u;
      const auto Result = [&]() -> llvm::APInt {
        switch (Op.Opcode) {
        case NdOp::COPY:
          return Input;
        case NdOp::SUBBYTES:
          return Input.lshr(Op.Inputs[1].ConstVal * 8u).trunc(Bits);
        case NdOp::INT_ZEXT:
          return Input.zext(Bits);
        case NdOp::INT_RIGHT:
          return Input.lshr(Read(Op.Inputs[1]).getZExtValue());
        case NdOp::INT_LEFT:
          return Input.shl(Read(Op.Inputs[1]).getZExtValue());
        case NdOp::INT_OR:
          return Input | Read(Op.Inputs[1]);
        case NdOp::INT_ADD:
          return Input + Read(Op.Inputs[1]);
        case NdOp::CONCAT: {
          const auto Low = Read(Op.Inputs[1]);
          return Input.zext(Bits).shl(Low.getBitWidth()) | Low.zext(Bits);
        }
        default:
          throw std::runtime_error("unexpected operation in pure test program");
        }
      }();
      Values.insert_or_assign(key(Op.Output), Result);
    }
    Predecessor = Id;
  }
  throw std::runtime_error("test route never returned");
}

TEST(MedPhiRecomposition, CorrelatedEdgesKeepAllBitsAndIgnoreTraversalOrder) {
  for (auto [Low, High] : {std::pair{1, 1}, {1, 3}, {2, 2}, {4, 4}, {8, 8}})
    for (bool Concat : {false, true})
      for (bool Shifted : {false, true})
        for (bool Reversed : {false, true}) {
          auto F = diamond(Low, High, Concat, Shifted);
          const auto Before = F;
          if (Reversed)
            std::reverse(F.Blocks.begin(), F.Blocks.end());
          ASSERT_TRUE(recomposeSplitPhis(F));
          EXPECT_TRUE(verifyMedFunc(F, "correlated PHI words"));
          EXPECT_FALSE(recomposeSplitPhis(F));
          for (uint64_t Seed : std::array<uint64_t, 5>{
                   0, 1, 0x80000000, 0xFEDCBA9876543210ULL, UINT64_MAX}) {
            const unsigned Bits = (Low + High) * 8u;
            const auto A = llvm::APInt(Bits, Seed);
            const auto B = ~A;
            for (int Arm : {1, 2}) {
              const std::vector<int> Route{0, Arm, 3};
              const auto Expected = execute(Before, Route, A, B);
              EXPECT_EQ(execute(F, Route, A, B), Expected);
              EXPECT_EQ(Expected, Arm == 1 ? A : B);
            }
          }
        }
}

TEST(MedPhiRecomposition, RejectsUncorrelatedIncompleteAndExceptionalJoins) {
  for (int Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = diamond(4, 4, false, false);
    auto &Join = F.Blocks[3];
    switch (Mutation) {
    case 0: // High bytes on one edge belong to another value.
      F.Blocks[1].Ops.back().Inputs[0] = F.Params[1];
      break;
    case 1:
      Join.Phis[1].Args.pop_back();
      break;
    case 2:
      Join.Phis[1].Args[0].first = Join.Phis[1].Args[1].first;
      break;
    case 3:
      Join.Phis[1].ExceptionalEntry = MedVar::makeUnspecified(4, Arch::X64);
      break;
    case 4:
      Join.Ops[0].Opcode = NdOp::INT_SEXT;
      break;
    case 5:
      Join.Ops[2].Inputs[1].ConstVal = 31;
      break;
    case 6:
      F.Blocks[1].Ops.back().Inputs[1].ConstVal = 3;
      break;
    case 7:
      F.Blocks[0].Succs.push_back(3); // An additional, unrepresented entry.
      break;
    case 8:
      F.SkippedSSA = true;
      break;
    case 9:
      for (auto &Op : F.Blocks[1].Ops)
        Op.Inputs[0] = MedVar::makeUnspecified(8, Arch::X64);
      break;
    }
    EXPECT_FALSE(recomposeSplitPhis(F));
    EXPECT_EQ(Join.Phis.size(), 2u);
    EXPECT_EQ(Join.Ops[3].Opcode, NdOp::INT_OR);
  }
}

TEST(MedPhiRecomposition, ReusesTheWholePhiAndKeepsLivePartialReads) {
  auto F = diamond(4, 4, true, false);
  auto &Join = F.Blocks[3];
  const auto Whole = value(9000, 8);
  Join.Phis.push_back({Whole, {{2, F.Params[1]}, {1, F.Params[0]}}});
  Join.Ops.insert(Join.Ops.begin(),
                  operation(NdOp::INT_ADD, value(9001, 4),
                            {Join.Phis[0].Output, MedVar::makeConst(1, 4)}));
  ASSERT_TRUE(recomposeSplitPhis(F));
  EXPECT_EQ(Join.Phis.size(), 3u);
  EXPECT_EQ(Join.Ops[1].Inputs[0], Whole);
  EXPECT_EQ(Join.Ops[0].Inputs[0], Join.Phis[0].Output);
  EXPECT_TRUE(verifyMedFunc(F, "retained partial PHI use"));
}
TEST(MedPhiRecomposition, FreshIdsIncludeDeclaredLocalsAndKeepTaggedZeroUses) {
  auto F = diamond(4, 4, false, true);
  F.Locals.push_back(value(10000, 8));
  auto Query = operation(NdOp::INTRINSIC, value(9000, 4),
                         {MedVar::makeConst(unsigned(Intrinsic::Cpuid), 4),
                          MedVar::makeConst(0, 4), MedVar::makeConst(0, 4)});
  Query.IntrinsicOutputs = {value(10001, 4), value(10002, 4), value(10003, 4)};
  F.Blocks.front().Ops.insert(F.Blocks.front().Ops.begin(), Query);
  ASSERT_TRUE(recomposeSplitPhis(F));
  EXPECT_GT(F.Blocks[3].Phis.back().Output.Id, 10003);
  EXPECT_TRUE(verifyMedFunc(F, "fresh identity after auxiliary outputs"));
  for (bool Address : {false, true}) {
    auto G = diamond(4, 4, false, true);
    auto &Ops = G.Blocks[3].Ops;
    const auto Wrapped = value(500, 8);
    auto Zero = MedVar::makeConst(0, 8);
    if (Address)
      Zero.Provenance = ConstantAddressProvenance::DataAddress;
    Ops.insert(Ops.begin() + 3,
               operation(NdOp::INT_OR, Wrapped, {value(300, 8), Zero}));
    Ops[4].Inputs[1] = Wrapped;
    EXPECT_EQ(recomposeSplitPhis(G), !Address);
  }
}

TEST(MedPhiRecomposition, LoopCarriedSlicesKeepCarriesAndWholeValueIdentity) {
  auto F = diamond(4, 4, true, true);
  auto &Entry = F.Blocks[0];
  auto &Header = F.Blocks[3];
  auto &Body = F.Blocks[2];
  Entry.Succs = {1};
  F.Blocks[1].Succs = {3};
  Header.Succs = {2};
  Body.Preds = {3};
  const auto Next = value(500, 8);
  Body.Ops.clear();
  Body.Ops.push_back(
      operation(NdOp::INT_ADD, Next, {value(400, 8), MedVar::makeConst(1, 8)}));
  slices(Body, Next, value(220, 4), value(221, 4), true);
  Header.Ops.pop_back(); // The selected route exits on the final iteration.
  const auto Before = F;
  ASSERT_TRUE(recomposeSplitPhis(F));
  const auto &Whole = Header.Phis.back();
  ASSERT_EQ(Whole.Args.size(), 2u);
  EXPECT_EQ(Whole.Args[1].second, Next);
  EXPECT_EQ(Body.Ops.front().Opcode, NdOp::INT_ADD);
  EXPECT_EQ(Body.Ops.front().Inputs[0], value(400, 8));
  EXPECT_TRUE(verifyMedFunc(F, "loop whole-value PHI"));

  // Returning the recomposed value after repeated wraparound must agree with
  // the original simultaneous PHIs, including carries from the low word.
  auto Old = Before, New = F;
  Old.Blocks[3].Ops.push_back(operation(NdOp::RETURN, {}, {value(400, 8)}));
  New.Blocks[3].Ops.push_back(operation(NdOp::RETURN, {}, {value(400, 8)}));
  // Intermediate header visits continue rather than return.
  auto Evaluate = [&](MedFunc Program, uint64_t Seed) {
    const auto Result = Program.Blocks[3].Ops.back();
    Program.Blocks[3].Ops.pop_back();
    auto Exit = Program.Blocks[3];
    Exit.Id = 4;
    Exit.Ops.push_back(Result);
    Program.Blocks.push_back(std::move(Exit));
    std::vector<int> Route{0, 1, 3};
    for (unsigned I = 0; I != 7; ++I) {
      Route.push_back(2);
      Route.push_back(I == 6 ? 4 : 3);
    }
    return execute(Program, Route, llvm::APInt(64, Seed), llvm::APInt(64, 0));
  };
  for (uint64_t Seed : std::array<uint64_t, 2>{0xFFFFFFFC, UINT64_MAX - 3}) {
    EXPECT_EQ(Evaluate(Old, Seed), llvm::APInt(64, Seed + 7));
    EXPECT_EQ(Evaluate(New, Seed), Evaluate(Old, Seed));
  }
}
TEST(MedPhiRecomposition, DoesNotMoveOrCloneCallsAndMemoryReads) {
  auto F = diamond(4, 4, false, true);
  for (int I : {1, 2}) {
    auto &B = F.Blocks[I];
    const auto Parent = value(500 + I, 8);
    B.Ops.clear();
    auto Producer =
        operation(I == 1 ? NdOp::LOAD : NdOp::CALL, Parent, {F.Params[I - 1]});
    Producer.Addr = 0x1000 + I;
    if (I == 2)
      Producer.CallSiteId = 7;
    B.Ops.push_back(Producer);
    slices(B, Parent, value(200 + 10 * I, 4), value(201 + 10 * I, 4), true);
  }
  const auto Before = F;
  ASSERT_TRUE(recomposeSplitPhis(F));
  for (int I : {1, 2}) {
    const auto &Old = Before.Blocks[I].Ops, &New = F.Blocks[I].Ops;
    ASSERT_EQ(Old.size(), New.size());
    for (size_t J = 0; J < Old.size(); ++J) {
      EXPECT_EQ(Old[J].Opcode, New[J].Opcode);
      EXPECT_EQ(Old[J].Addr, New[J].Addr);
      EXPECT_EQ(Old[J].CallSiteId, New[J].CallSiteId);
      EXPECT_EQ(Old[J].Output, New[J].Output);
      ASSERT_EQ(Old[J].NumInputs, New[J].NumInputs);
      for (unsigned K = 0; K < Old[J].NumInputs; ++K)
        EXPECT_EQ(Old[J].Inputs[K], New[J].Inputs[K]);
    }
  }
  EXPECT_TRUE(verifyMedFunc(F, "unchanged call and load producers"));
}
TEST(MedPhiRecomposition, DefaultLowConversionRecoversTheWholeValue) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto Arguments = TRI.integerArgumentLayout(Format).Registers;
      ASSERT_GE(Arguments.size(), 3u);
      LowFunc Low;
      Low.Entry = 0x1000;
      Low.Name = "split_word_join";
      Low.Blocks.resize(4);
      for (int I = 0; I < 4; ++I) {
        auto &B = Low.Blocks[I];
        B.Id = I;
        B.StartAddr = Low.Entry + 0x10 * I;
        B.EndAddr = B.StartAddr + 0x10;
      }
      Low.Blocks[0].Succs = {1, 2};
      Low.Blocks[1].Preds = Low.Blocks[2].Preds = {0};
      Low.Blocks[1].Succs = Low.Blocks[2].Succs = {3};
      Low.Blocks[3].Preds = {1, 2};
      auto Add = [&](int B, NdOp Opcode, NdVar Output,
                     std::initializer_list<NdVar> Inputs) {
        LowOp Op;
        Op.Opcode = Opcode;
        Op.Output = Output;
        Op.Addr = Low.Blocks[B].StartAddr;
        for (auto Input : Inputs)
          Op.addInput(Input);
        Low.Blocks[B].Ops.push_back(Op);
      };
      const auto L = NdVar::tmp(10, 4), H = NdVar::tmp(11, 4);
      const auto Shift = NdVar::tmp(14, 8), C = NdVar::tmp(15, 1);
      Add(0, NdOp::INT_NOTEQUAL, C,
          {NdVar::reg(Arguments[2], 8), NdVar::cst(0, 8)});
      Add(0, NdOp::COND_BR, {}, {NdVar::cst(0x1020, 8), C});
      for (int B : {1, 2}) {
        const auto Input = NdVar::reg(Arguments[B - 1], 8);
        Add(B, NdOp::SUBBYTES, L, {Input, NdVar::cst(0, 1)});
        Add(B, NdOp::INT_RIGHT, Shift, {Input, NdVar::cst(32, 8)});
        Add(B, NdOp::SUBBYTES, H, {Shift, NdVar::cst(0, 1)});
        Add(B, NdOp::BRANCH, {}, {NdVar::cst(0x1030, 8)});
      }
      const auto WideL = NdVar::tmp(12, 8), WideH = NdVar::tmp(13, 8);
      const auto Result = NdVar::reg(TRI.IntReturnReg, 8);
      Add(3, NdOp::INT_ZEXT, WideL, {L});
      Add(3, NdOp::INT_ZEXT, WideH, {H});
      Add(3, NdOp::INT_LEFT, Shift, {WideH, NdVar::cst(32, 8)});
      Add(3, NdOp::INT_OR, Result, {WideL, Shift});
      Add(3, NdOp::RETURN, {}, {Result});
      auto Med = LowToMedConverter().convert(Low, Architecture, Format);
      ASSERT_FALSE(Med.SkippedSSA);
      ASSERT_TRUE(verifyMedFunc(Med, "default split word conversion"));
      const auto &Join = Med.Blocks.back();
      const auto &Return = Join.Ops.back();
      ASSERT_EQ(Return.Opcode, NdOp::RETURN);
      EXPECT_TRUE(
          std::any_of(Join.Phis.begin(), Join.Phis.end(), [&](const auto &Phi) {
            return Phi.Output.Size == 8 && Phi.Output == Return.Inputs[0];
          }));
    }
}
} // namespace
