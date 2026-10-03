//===- InterpreterTransferChainTests.cpp - Proved control sequences
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/interpreter/FrameEntryConstraints.h"
#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <map>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

namespace {
constexpr va_t Entry = 0x800, Middle = 0x840, Dispatch = 0x880, Exit = 0x8c0;

NdVar r(uint64_t Offset) { return NdVar::reg(Offset, 8); }
NdVar c(uint64_t Value) { return NdVar::scalar(Value, 8); }
LowOp op(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp Result;
  Result.Opcode = Code;
  Result.Output = Output;
  for (const auto &Input : Inputs)
    Result.addInput(Input);
  return Result;
}
LowOp jump(va_t Address) {
  return op(NdOp::BRANCH, {}, {NdVar::cst(Address, 8)});
}
LowOp ret() { return op(NdOp::RETURN, {}, {r(0)}); }

class Program : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;
  void add(va_t Address, std::initializer_list<LowOp> Ops,
           va_t Fallthrough = InvalidVA) {
    SpecializationInstruction I;
    I.Ops = Ops;
    I.Origin.Address = Address;
    I.Origin.Size = 1;
    I.Origin.OpCount = Ops.size();
    I.Fallthrough = {Fallthrough == InvalidVA ? Address + 1 : Fallthrough};
    for (size_t N = 0; N != I.Ops.size(); ++N) {
      auto &O = I.Ops[N];
      O.Addr = Address;
      O.Seq = N;
      if (O.Opcode == NdOp::RETURN) {
        I.Origin.Control = LowInstructionControl::Return;
        I.Origin.ControlFlags = LowInstructionControlFlag::Return;
      } else if (O.Opcode == NdOp::BRANCH || O.Opcode == NdOp::COND_BR ||
                 O.Opcode == NdOp::INDIR_BR) {
        I.Origin.Control = LowInstructionControl::Branch;
        I.Origin.ControlFlags = LowInstructionControlFlag::Branch;
        if (O.Opcode == NdOp::INDIR_BR)
          I.Origin.ControlFlags |= LowInstructionControlFlag::Indirect;
        else
          I.Origin.Immediate = O.Inputs[0].Offset;
        if (O.Opcode == NdOp::COND_BR)
          I.Origin.ControlFlags |= LowInstructionControlFlag::Conditional;
      }
    }
    Code.emplace(Address, std::move(I));
  }
  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto At = Code.find(Cursor.Address);
    if (At == Code.end() || At->second.Origin.Mode != Cursor.Mode)
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "missing independently authored block");
    return At->second;
  }
};

std::optional<uint64_t> execute(const LowFunc &F, uint64_t Input,
                                uint64_t Root = 0x10000) {
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, 8, Ctx.mkConst(64, Input));
  State.write(SymSpace::Register, 32, Ctx.mkConst(64, Root));
  va_t Cursor = F.Entry;
  for (unsigned Step = 0; Step != 1000; ++Step) {
    const auto B =
        std::find_if(F.Blocks.begin(), F.Blocks.end(),
                     [&](const auto &B) { return B.StartAddr == Cursor; });
    if (B == F.Blocks.end())
      return std::nullopt;
    SymExec Exec(Ctx, State);
    bool Transferred = false;
    for (const auto &O : B->Ops) {
      const auto Flow = Exec.step(O);
      if (Flow == StepResult::Continue)
        continue;
      if (Flow == StepResult::Return) {
        const auto Value = Ctx.asConst(State.read(SymSpace::Register, 0, 8));
        return Value ? std::optional<uint64_t>(Value->getZExtValue())
                     : std::nullopt;
      }
      if (Flow == StepResult::CondBranch) {
        const auto Condition = Ctx.asConst(Exec.branchCondition());
        if (!Condition || B->Succs.size() != 2)
          return std::nullopt;
        const auto Next = B->Succs[Condition->isZero() ? 1 : 0];
        const auto To =
            std::find_if(F.Blocks.begin(), F.Blocks.end(),
                         [&](const auto &B) { return B.Id == Next; });
        if (To == F.Blocks.end())
          return std::nullopt;
        Cursor = To->StartAddr;
      } else if (Flow == StepResult::Branch) {
        const auto Target = Ctx.asConst(Exec.branchTarget());
        if (!Target)
          return std::nullopt;
        Cursor = Target->getZExtValue();
      } else
        return std::nullopt;
      Transferred = true;
      break;
    }
    if (!Transferred)
      return std::nullopt;
  }
  return std::nullopt;
}

Program correlatedProgram() {
  Program P;
  P.add(Entry, {op(NdOp::INT_XOR, r(16), {r(8), c(90)}), jump(Middle)});
  P.add(Middle, {op(NdOp::INT_XOR, r(16), {r(16), r(8)}), jump(Dispatch)});
  P.add(Dispatch, {op(NdOp::INT_ADD, r(24), {r(16), c(Exit - 90)}),
                   op(NdOp::INDIR_BR, {}, {r(24)})});
  P.add(Exit, {op(NdOp::INT_ADD, r(0), {r(8), c(17)}), ret()});
  return P;
}

Program countedLoop(bool Nested) {
  Program P;
  const auto Flag = NdVar::reg(48, 1);
  P.add(Entry, {op(NdOp::COPY, r(0), {r(8)}), op(NdOp::COPY, r(16), {c(32)}),
                op(NdOp::COPY, r(24), {c(4)}), jump(Middle)});
  if (Nested)
    P.add(Middle, {op(NdOp::COPY, r(16), {c(7)}), jump(Dispatch)});
  P.add(Nested ? Dispatch : Middle,
        {op(NdOp::INT_ADD, r(0), {r(0), c(3)}),
         op(NdOp::INT_SUB, r(16), {r(16), c(1)}),
         op(NdOp::INT_NOTEQUAL, Flag, {r(16), c(0)}),
         op(NdOp::COND_BR, {},
            {NdVar::cst(Nested ? Dispatch : Middle, 8), Flag})},
        Nested ? 0x8a0 : Exit);
  if (Nested)
    P.add(0x8a0,
          {op(NdOp::INT_SUB, r(24), {r(24), c(1)}),
           op(NdOp::INT_NOTEQUAL, Flag, {r(24), c(0)}),
           op(NdOp::COND_BR, {}, {NdVar::cst(Middle, 8), Flag})},
          Exit);
  P.add(Exit, {ret()});
  return P;
}

TEST(InterpreterTransferChain, RepeatedDestinationPreservesRuntimeLoops) {
  for (bool Nested : {false, true}) {
    SCOPED_TRACE(Nested);
    auto P = countedLoop(Nested);
    SpecializationOptions O;
    O.MaxChainedTransfers = 128;
    O.MaxOperations = 96;
    const auto Unrolled = specializeInterpreter(P, {Entry}, O);
    EXPECT_EQ(Unrolled.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Unrolled.Residual.Blocks.empty());
    EXPECT_TRUE(Unrolled.Origins.empty());
    EXPECT_TRUE(Unrolled.Reads.empty());
    O.StopChainingAtRepeatedDestination = true;
    const auto R = specializeInterpreter(P, {Entry}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_LE(R.EvaluatedOperations, O.MaxOperations);
    for (const auto &[Address, Instruction] : P.Code)
      EXPECT_TRUE(std::any_of(
          R.Origins.begin(), R.Origins.end(), [&](const auto &Origin) {
            return Origin.NativeInstruction.Address == Address;
          }));
    for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{37}, UINT64_MAX})
      EXPECT_EQ(execute(R.Residual, Input), Input + (Nested ? 84 : 96));
    P.Code.erase(Exit);
    const auto Missing = specializeInterpreter(P, {Entry}, O);
    EXPECT_FALSE(Missing.complete());
    EXPECT_TRUE(Missing.Residual.Blocks.empty());
    EXPECT_TRUE(Missing.Origins.empty());
    EXPECT_TRUE(Missing.Reads.empty());
    O.MaxOperations = 1;
    const auto Short = specializeInterpreter(P, {Entry}, O);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Short.Residual.Blocks.empty());
    EXPECT_TRUE(Short.Origins.empty());
    EXPECT_TRUE(Short.Reads.empty());
  }
}

TEST(InterpreterTransferChain, RepeatedDestinationOptInCanLoseCorrelation) {
  Program P;
  const auto Flag = NdVar::reg(48, 1);
  P.add(Entry, {op(NdOp::INT_XOR, r(16), {r(8), c(90)}),
                op(NdOp::COPY, r(24), {c(2)}), jump(Middle)});
  P.add(Middle,
        {op(NdOp::INT_XOR, r(16), {r(16), c(1)}),
         op(NdOp::INT_SUB, r(24), {r(24), c(1)}),
         op(NdOp::INT_NOTEQUAL, Flag, {r(24), c(0)}),
         op(NdOp::COND_BR, {}, {NdVar::cst(Middle, 8), Flag})},
        Dispatch);
  P.add(Dispatch, {op(NdOp::INT_XOR, r(16), {r(16), r(8)}),
                   op(NdOp::INT_ADD, r(24), {r(16), c(Exit - 90)}),
                   op(NdOp::INDIR_BR, {}, {r(24)})});
  P.add(Exit, {op(NdOp::INT_ADD, r(0), {r(8), c(17)}), ret()});
  SpecializationOptions O;
  O.MaxChainedTransfers = 128;
  O.DiscoverControlState = true;
  const auto Complete = specializeInterpreter(P, {Entry}, O);
  ASSERT_TRUE(Complete.complete()) << Complete.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{37}, UINT64_MAX})
    EXPECT_EQ(execute(Complete.Residual, Input), Input + 17);
  O.StopChainingAtRepeatedDestination = true;
  const auto Cut = specializeInterpreter(P, {Entry}, O);
  EXPECT_EQ(Cut.Status, SpecializationStatus::UnresolvedControl)
      << Cut.Diagnostic;
  EXPECT_TRUE(Cut.Residual.Blocks.empty());
  EXPECT_TRUE(Cut.Origins.empty());
  EXPECT_TRUE(Cut.Reads.empty());
  O.MaxChainedVisitsPerDestination = 4;
  EXPECT_EQ(specializeInterpreter(P, {Entry}, O).Status,
            SpecializationStatus::UnresolvedControl);
  O.StopChainingAtRepeatedDestination = false;
  O.MaxChainedVisitsPerDestination = 1;
  EXPECT_EQ(specializeInterpreter(P, {Entry}, O).Status,
            SpecializationStatus::UnresolvedControl);
  for (uint32_t Visits : {0u, 2u, 4u, UINT32_MAX}) {
    SCOPED_TRACE(Visits);
    O.MaxChainedVisitsPerDestination = Visits;
    const auto Kept = specializeInterpreter(P, {Entry}, O);
    ASSERT_TRUE(Kept.complete()) << Kept.Diagnostic;
    for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{37}, UINT64_MAX})
      EXPECT_EQ(execute(Kept.Residual, Input), Input + 17);
  }
}

TEST(InterpreterTransferChain, VisitCapCountsTransferredDestinations) {
  Program P;
  const auto Flag = NdVar::reg(48, 1);
  P.add(Entry, {op(NdOp::COPY, r(0), {r(8)}), op(NdOp::COPY, r(16), {c(2)})});
  P.add(Entry + 1, {op(NdOp::INT_ADD, r(0), {r(0), c(3)}),
                    op(NdOp::INT_SUB, r(16), {r(16), c(1)}), jump(Middle)});
  P.add(Middle,
        {op(NdOp::INT_NOTEQUAL, Flag, {r(16), c(0)}),
         op(NdOp::COND_BR, {}, {NdVar::cst(Entry + 1, 8), Flag})},
        Exit);
  P.add(Exit, {ret()});
  for (bool Legacy : {false, true}) {
    SCOPED_TRACE(Legacy);
    SpecializationOptions O;
    O.MaxChainedTransfers = 128;
    O.StopChainingAtRepeatedDestination = Legacy;
    O.MaxChainedVisitsPerDestination = Legacy ? 4 : 1;
    const auto R = specializeInterpreter(P, {Entry}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_GE(R.Residual.Blocks.size(), 2u);
    // Entry+1 was first reached by fallthrough. Its first transfer visit is
    // still in the entry chain; the next transfer to Middle ends that chain.
    EXPECT_EQ(R.Residual.Blocks[1].InstructionBoundaries.size(), 4u);
    for (uint64_t Input : {uint64_t{0}, uint64_t{37}, UINT64_MAX})
      EXPECT_EQ(execute(R.Residual, Input), Input + 6);
  }
}

TEST(InterpreterTransferChain, VisitCapRetainsNestedLoopsAndWorkLimits) {
  for (bool Nested : {false, true})
    for (uint32_t Visits : {1u, 2u, 4u, UINT32_MAX}) {
      SCOPED_TRACE(Nested);
      SCOPED_TRACE(Visits);
      auto P = countedLoop(Nested);
      SpecializationOptions O;
      O.MaxChainedTransfers = 128;
      O.MaxChainedVisitsPerDestination = Visits;
      const auto R = specializeInterpreter(P, {Entry}, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      for (uint64_t Input : {uint64_t{0}, uint64_t{37}, UINT64_MAX})
        EXPECT_EQ(execute(R.Residual, Input), Input + (Nested ? 84 : 96));
      O.MaxOperations = R.EvaluatedOperations;
      EXPECT_TRUE(specializeInterpreter(P, {Entry}, O).complete());
      --O.MaxOperations;
      const auto Short = specializeInterpreter(P, {Entry}, O);
      EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
      EXPECT_TRUE(Short.Residual.Blocks.empty());
      EXPECT_TRUE(Short.Origins.empty());
      EXPECT_TRUE(Short.Reads.empty());
    }
}

TEST(InterpreterTransferChain, RepeatedDestinationIncludesDecodeMode) {
  class ModeProgram : public SpecializationProvider {
  public:
    Program Arm, Thumb;
    llvm::Expected<SpecializationInstruction>
    instruction(SpecializationCursor Cursor) override {
      return (Cursor.Mode == InstructionMode::ARM ? Arm : Thumb)
          .instruction(Cursor);
    }
  } P;
  P.Arm.add(Entry, {op(NdOp::INT_XOR, r(16), {r(8), c(90)}), jump(Entry)});
  P.Arm.Code.at(Entry).Origin.TargetMode = LowInstructionTargetMode::Thumb;
  P.Thumb.add(Entry, {op(NdOp::INT_XOR, r(16), {r(16), r(8)}),
                      op(NdOp::INT_ADD, r(24), {r(16), c(Exit - 90)}),
                      op(NdOp::INDIR_BR, {}, {r(24)})});
  P.Thumb.add(Exit, {op(NdOp::INT_ADD, r(0), {r(8), c(17)}), ret()});
  for (auto [Code, Mode] : {std::pair{&P.Arm, InstructionMode::ARM},
                            std::pair{&P.Thumb, InstructionMode::Thumb}})
    for (auto &[Address, I] : Code->Code) {
      I.Origin.Mode = Mode;
      I.Fallthrough.Mode = Mode;
    }
  SpecializationOptions O;
  O.MaxChainedTransfers = 3;
  O.MaxChainedVisitsPerDestination = 1;
  const auto R = specializeInterpreter(P, {Entry, InstructionMode::ARM}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(R.Origins.size(), 3U);
  EXPECT_EQ(R.Origins[0].NativeInstruction.Mode, InstructionMode::ARM);
  EXPECT_EQ(R.Origins[1].NativeInstruction.Mode, InstructionMode::Thumb);
  EXPECT_EQ(R.Origins[0].NativeInstruction.Address,
            R.Origins[1].NativeInstruction.Address);
  for (uint64_t Input : {uint64_t{0}, uint64_t{37}, UINT64_MAX})
    EXPECT_EQ(execute(R.Residual, Input), Input + 17);
  O.StopChainingAtRepeatedDestination = true;
  O.MaxChainedVisitsPerDestination = 4;
  const auto Legacy =
      specializeInterpreter(P, {Entry, InstructionMode::ARM}, O);
  ASSERT_TRUE(Legacy.complete()) << Legacy.Diagnostic;
  EXPECT_EQ(Legacy.Origins.size(), R.Origins.size());
  EXPECT_EQ(execute(Legacy.Residual, UINT64_MAX), UINT64_MAX + 17);
}

TEST(InterpreterTransferChain, RepeatOptionHasNoEffectWithoutChaining) {
  for (bool Nested : {false, true}) {
    auto P = countedLoop(Nested);
    SpecializationOptions O;
    const auto Default = specializeInterpreter(P, {Entry}, O);
    ASSERT_TRUE(Default.complete()) << Default.Diagnostic;
    O.StopChainingAtRepeatedDestination = true;
    O.MaxChainedVisitsPerDestination = 4;
    const auto Enabled = specializeInterpreter(P, {Entry}, O);
    ASSERT_TRUE(Enabled.complete()) << Enabled.Diagnostic;
    EXPECT_EQ(Default.EvaluatedOperations, Enabled.EvaluatedOperations);
    EXPECT_EQ(Default.SolverQueries, Enabled.SolverQueries);
    EXPECT_EQ(Default.Residual.Blocks.size(), Enabled.Residual.Blocks.size());
    EXPECT_EQ(Default.Origins.size(), Enabled.Origins.size());
    EXPECT_EQ(execute(Enabled.Residual, UINT64_MAX),
              UINT64_MAX + (Nested ? 84 : 96));
  }
}

TEST(InterpreterTransferChain, KeepsCorrelatedRuntimeValuesAndNativeOrigins) {
  auto P = correlatedProgram();
  EXPECT_FALSE(specializeInterpreter(P, {Entry}).complete());
  SpecializationOptions O;
  O.MaxChainedTransfers = 3;
  const auto R = specializeInterpreter(P, {Entry}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(R.Origins.size(), 4U);
  for (size_t I = 0; I != 4; ++I)
    EXPECT_EQ(R.Origins[I].NativeInstruction.Address, Entry + 0x40 * I);
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{57}, UINT64_MAX})
    EXPECT_EQ(execute(R.Residual, Input), Input + 17);
  EXPECT_EQ(R.Residual.Blocks.size(), 2U);
}

TEST(InterpreterTransferChain,
     LimitUsesOrdinaryEdgesAndBudgetNeverPublishesAPrefix) {
  auto P = correlatedProgram();
  SpecializationOptions O;
  O.MaxChainedTransfers = 1;
  const auto ShortChain = specializeInterpreter(P, {Entry}, O);
  ASSERT_TRUE(ShortChain.complete()) << ShortChain.Diagnostic;
  EXPECT_EQ(execute(ShortChain.Residual, 41), 58U);
  O.MaxChainedTransfers = 3;
  const auto Complete = specializeInterpreter(P, {Entry}, O);
  ASSERT_TRUE(Complete.complete()) << Complete.Diagnostic;
  EXPECT_GT(ShortChain.Residual.Blocks.size(), Complete.Residual.Blocks.size());
  O.MaxOperations = Complete.EvaluatedOperations;
  EXPECT_TRUE(specializeInterpreter(P, {Entry}, O).complete());
  --O.MaxOperations;
  const auto Short = specializeInterpreter(P, {Entry}, O);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
  EXPECT_TRUE(Short.Reads.empty());
}

TEST(InterpreterTransferChain, ProvesTrueAndFalseAndKeepsBothDynamicArms) {
  for (unsigned Condition = 0; Condition != 3; ++Condition) {
    Program P;
    const auto Flag = NdVar::reg(48, 1);
    P.add(Entry,
          {op(NdOp::INT_EQUAL, Flag, {r(8), Condition == 2 ? c(0) : r(8)}),
           op(Condition == 0 ? NdOp::BOOL_NOT : NdOp::COPY, Flag, {Flag}),
           op(NdOp::COND_BR, {}, {NdVar::cst(Exit, 8), Flag})},
          Middle);
    P.add(Middle, {op(NdOp::COPY, r(0), {c(11)}), ret()});
    P.add(Exit, {op(NdOp::COPY, r(0), {c(29)}), ret()});
    SpecializationOptions O;
    O.MaxChainedTransfers = 4;
    const auto R = specializeInterpreter(P, {Entry}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(execute(R.Residual, 0), Condition == 0 ? 11U : 29U);
    EXPECT_EQ(execute(R.Residual, 1), Condition == 1 ? 29U : 11U);
  }
}

TEST(InterpreterTransferChain, ChainedDestinationsKeepReservedLabelAdmission) {
  Program P;
  P.add(Entry, {jump(uint64_t{1} << 63)});
  SpecializationOptions O;
  O.MaxChainedTransfers = 2;
  const auto R = specializeInterpreter(P, {Entry}, O);
  EXPECT_EQ(R.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_NE(R.Diagnostic.find("reserved residual labels"), std::string::npos);
  EXPECT_TRUE(R.Residual.Blocks.empty());
}

TEST(InterpreterTransferChain, LaterPredecessorRebuildsAPreviouslyUniqueChain) {
  for (bool Stop : {false, true}) {
    for (bool Missing : {false, true}) {
      Program P;
      const auto Flag = NdVar::reg(48, 1);
      P.add(Entry,
            {op(NdOp::INT_EQUAL, Flag, {r(8), c(0)}),
             op(NdOp::COND_BR, {}, {NdVar::cst(0x900, 8), Flag})},
            0xa00);
      P.add(0x900, {jump(0x940)});
      P.add(0x940, {op(NdOp::COPY, r(16), {c(0xc00)}), jump(0xb00)});
      P.add(0xa00, {jump(0xa40)});
      P.add(0xa40, {jump(0xa80)});
      P.add(0xa80, {jump(0xac0)});
      P.add(0xac0, {op(NdOp::COPY, r(16), {c(0xc40)}), jump(0xb00)});
      P.add(0xb00, {op(NdOp::INDIR_BR, {}, {r(16)})});
      P.add(0xc00, {op(NdOp::COPY, r(0), {c(11)}), ret()});
      if (!Missing)
        P.add(0xc40, {op(NdOp::COPY, r(0), {c(29)}), ret()});
      SpecializationOptions O;
      O.StopChainingAtRepeatedDestination = Stop;
      O.MaxChainedTransfers = 1;
      O.DiscoverControlState = true;
      const auto R = specializeInterpreter(P, {Entry}, O);
      if (Missing) {
        EXPECT_FALSE(R.complete());
        EXPECT_TRUE(R.Residual.Blocks.empty());
        EXPECT_TRUE(R.Origins.empty());
        EXPECT_TRUE(R.Reads.empty());
      } else {
        ASSERT_TRUE(R.complete()) << R.Diagnostic;
        EXPECT_GT(R.ControlRefinements, 0U);
        EXPECT_EQ(execute(R.Residual, 0), 11U);
        EXPECT_EQ(execute(R.Residual, 1), 29U);
      }
    }
  }
}

TEST(InterpreterTransferChain, EntryFrameBoundsAreExplicitAndUsePhysicalRoot) {
  Program P;
  const auto Flag = NdVar::reg(48, 1);
  P.add(Entry, {op(NdOp::INT_SUB, r(64), {r(32), c(16)}),
                op(NdOp::INT_SUB, r(72), {r(32), c(48)}),
                op(NdOp::INT_SUB, r(32), {r(32), c(80)}), jump(Middle)});
  P.add(Middle, {op(NdOp::INT_LESS, Flag, {r(72), r(64)}),
                 op(NdOp::COND_BR, {}, {NdVar::cst(Exit, 8), Flag})});
  P.add(Exit, {op(NdOp::INT_ADD, r(0), {r(8), c(17)}), ret()});
  SpecializationOptions O;
  O.FrameBaseRegister = SymRegisterRange{32, 8};
  O.MaxChainedTransfers = 4;
  const auto Unbounded = specializeInterpreter(P, {Entry}, O);
  EXPECT_FALSE(Unbounded.complete());
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-96, 8};
  const auto R = specializeInterpreter(P, {Entry}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Root : {uint64_t{96}, uint64_t{1} << 63, UINT64_MAX - 7})
    EXPECT_EQ(execute(R.Residual, 41, Root), 58U);
  O.FrameBaseRegister.reset();
  EXPECT_EQ(specializeInterpreter(P, {Entry}, O).Status,
            SpecializationStatus::InvalidInput);
  O.FrameBaseRegister = SymRegisterRange{32, 8};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{8, 8};
  EXPECT_EQ(specializeInterpreter(P, {Entry}, O).Status,
            SpecializationStatus::InvalidInput);
}

TEST(InterpreterTransferChain, EntryFrameBoundsDoNotAuthorizeUnknownStores) {
  Program P;
  P.add(Entry, {op(NdOp::INT_MULT, r(64), {r(32), c(2)}),
                op(NdOp::STORE, {}, {r(64), r(8)}), ret()});
  SpecializationOptions O;
  O.FrameBaseRegister = SymRegisterRange{32, 8};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-96, 8};
  O.RequireRestoredFrameAtReturn = true;
  O.MaxChainedTransfers = 4;
  const auto R = specializeInterpreter(P, {Entry}, O);
  EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(R.Residual.Blocks.empty());
}

TEST(InterpreterTransferChain, FrameBoundsStayOnEntryRootAfterStackAdjustment) {
  Program P;
  const auto Flag = NdVar::reg(48, 1);
  P.add(Entry, {op(NdOp::COPY, r(64), {r(32)}),
                op(NdOp::INT_SUB, r(32), {r(32), c(80)}), jump(Middle)});
  P.add(Middle,
        {op(NdOp::INT_LESS, Flag, {r(64), c(160)}),
         op(NdOp::COND_BR, {}, {NdVar::cst(Exit, 8), Flag})},
        Dispatch);
  P.add(Exit, {op(NdOp::COPY, r(0), {c(11)}), ret()});
  P.add(Dispatch, {op(NdOp::COPY, r(0), {c(29)}), ret()});
  SpecializationOptions O;
  O.FrameBaseRegister = SymRegisterRange{32, 8};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-96, 8};
  const auto R = specializeInterpreter(P, {Entry}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Root :
       {uint64_t{96}, uint64_t{159}, uint64_t{160}, UINT64_MAX - 7})
    EXPECT_EQ(execute(R.Residual, 0, Root), Root < 160 ? 11U : 29U);
}

TEST(InterpreterTransferChain,
     NonwrappingBoundsIncludeLastAddressAndSignedMin) {
  SymContext Ctx;
  const auto Accepts = [&](uint64_t Root, int64_t Begin, int64_t End) {
    return Ctx.isConstOnes(detail::nonwrappingFramePredicate(
        Ctx, Ctx.mkConst(64, Root), Begin, End));
  };
  EXPECT_FALSE(Accepts(95, -96, 8));
  EXPECT_TRUE(Accepts(96, -96, 8));
  EXPECT_TRUE(Accepts(UINT64_MAX - 7, -96, 8));
  EXPECT_FALSE(Accepts(UINT64_MAX - 6, -96, 8));
  EXPECT_TRUE(Accepts(UINT64_MAX, -96, 1));
  EXPECT_FALSE(Accepts((uint64_t{1} << 63) - 1, INT64_MIN, 1));
  EXPECT_TRUE(Accepts(uint64_t{1} << 63, INT64_MIN, 1));
}
} // namespace
