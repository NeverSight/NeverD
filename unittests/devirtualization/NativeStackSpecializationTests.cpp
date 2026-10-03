//===- NativeStackSpecializationTests.cpp - Physical stack control --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"

#include <map>
#include <optional>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

namespace {

NdVar r(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar c(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp operation(NdOp Opcode, NdVar Output,
                std::initializer_list<NdVar> Inputs) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (NdVar Input : Inputs)
    Result.addInput(Input);
  return Result;
}
LowOp branch(va_t Target) {
  return operation(NdOp::BRANCH, {}, {NdVar::cst(Target, 8)});
}
LowOp call(va_t Target) {
  return operation(NdOp::CALL, r(0), {NdVar::cst(Target, 8)});
}
LowOp ret() { return operation(NdOp::RETURN, {}, {r(0)}); }

class StackProvider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;

  void add(va_t Address, std::initializer_list<LowOp> Ops,
           SpecializationNativeStackControl Native =
               SpecializationNativeStackControl::None) {
    SpecializationInstruction Insn;
    Insn.Ops = Ops;
    Insn.Origin.Address = Address;
    Insn.Origin.Size = 1;
    Insn.Origin.OpCount = Ops.size();
    Insn.Fallthrough = {Address + 1};
    Insn.NativeStackControl = Native;
    for (size_t I = 0; I < Insn.Ops.size(); ++I) {
      LowOp &Op = Insn.Ops[I];
      Op.Addr = Address;
      Op.Seq = static_cast<int>(I);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        Insn.Origin.Control = LowInstructionControl::Call;
        Insn.Origin.ControlFlags = LowInstructionControlFlag::Call;
        if (Op.Opcode == NdOp::INDIR_CALL)
          Insn.Origin.ControlFlags |= LowInstructionControlFlag::Indirect;
        else
          Insn.Origin.Immediate = Op.Inputs[0].Offset;
      } else if (Op.Opcode == NdOp::RETURN) {
        Insn.Origin.Control = LowInstructionControl::Return;
        Insn.Origin.ControlFlags = LowInstructionControlFlag::Return;
      } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
                 Op.Opcode == NdOp::INDIR_BR) {
        Insn.Origin.Control = LowInstructionControl::Branch;
        Insn.Origin.ControlFlags = LowInstructionControlFlag::Branch;
        if (Op.Opcode == NdOp::COND_BR)
          Insn.Origin.ControlFlags |= LowInstructionControlFlag::Conditional;
        if (Op.Opcode == NdOp::INDIR_BR)
          Insn.Origin.ControlFlags |= LowInstructionControlFlag::Indirect;
        else
          Insn.Origin.Immediate = Op.Inputs[0].Offset;
      }
    }
    Code[Address] = std::move(Insn);
  }

  void nativeCall(va_t Address, va_t Target) {
    add(Address, {call(Target)}, SpecializationNativeStackControl::Call);
  }
  void nativeIndirectCall(va_t Address, NdVar Target) {
    add(Address, {operation(NdOp::INDIR_CALL, r(0), {Target})},
        SpecializationNativeStackControl::Call);
  }
  void nativeReturn(va_t Address) {
    add(Address, {ret()}, SpecializationNativeStackControl::Return);
  }
  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto At = Code.find(Cursor.Address);
    if (At == Code.end())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "unreachable continuation was visited");
    return At->second;
  }
};

SpecializationOptions stackOptions() {
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.RequireRestoredFrameAtReturn = true;
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  return Options;
}

struct Execution {
  uint64_t Value;
  uint64_t Stack;
  std::vector<uint8_t> Memory;
};

std::optional<Execution>
execute(const LowFunc &Function, uint64_t Input = 0, uint64_t Flag = 0,
        llvm::endianness Order = llvm::endianness::little,
        uint64_t EntryStack = 0x10000,
        llvm::ArrayRef<int64_t> ObserveFrame = {}) {
  SymContext Ctx;
  SymState State(Ctx, Order);
  State.write(SymSpace::Register, 0, Ctx.mkConst(64, 0));
  State.write(SymSpace::Register, 8, Ctx.mkConst(64, Input));
  State.write(SymSpace::Register, 32, Ctx.mkConst(64, EntryStack));
  State.write(SymSpace::Register, x86reg::CF, Ctx.mkConst(8, Flag));
  for (size_t I = 0; I < ObserveFrame.size(); ++I)
    State.store(Ctx.mkConst(64, EntryStack + ObserveFrame[I]),
                Ctx.mkConst(8, 65 + I));
  va_t Address = Function.Entry;
  for (unsigned Step = 0; Step < 1000; ++Step) {
    const LowBlock *Block = nullptr;
    for (const LowBlock &Candidate : Function.Blocks)
      if (Candidate.StartAddr == Address)
        Block = &Candidate;
    if (!Block)
      return std::nullopt;
    SymExec Exec(Ctx, State);
    bool Transferred = false;
    for (const LowOp &Op : Block->Ops) {
      const StepResult Flow = Exec.step(Op);
      if (Flow == StepResult::Continue)
        continue;
      if (Flow == StepResult::Return) {
        const auto Value = Ctx.asConst(State.read(SymSpace::Register, 0, 8));
        const auto Stack = Ctx.asConst(State.read(SymSpace::Register, 32, 8));
        if (!Value || !Stack)
          return std::nullopt;
        Execution Result{Value->getZExtValue(), Stack->getZExtValue(), {}};
        for (int64_t Offset : ObserveFrame) {
          const auto Byte =
              Ctx.asConst(State.load(Ctx.mkConst(64, EntryStack + Offset), 1));
          if (!Byte)
            return std::nullopt;
          Result.Memory.push_back(Byte->getZExtValue());
        }
        return Result;
      }
      if (Flow == StepResult::CondBranch) {
        const auto Condition = Ctx.asConst(Exec.branchCondition());
        if (!Condition || Block->Succs.size() != 2)
          return std::nullopt;
        const int Next = Block->Succs[Condition->isZero() ? 1 : 0];
        const auto Target = std::find_if(
            Function.Blocks.begin(), Function.Blocks.end(),
            [&](const LowBlock &Candidate) { return Candidate.Id == Next; });
        if (Target == Function.Blocks.end())
          return std::nullopt;
        Address = Target->StartAddr;
        Transferred = true;
        break;
      }
      if (Flow != StepResult::Branch)
        return std::nullopt;
      const auto Target = Ctx.asConst(Exec.branchTarget());
      if (!Target)
        return std::nullopt;
      Address = Target->getZExtValue();
      Transferred = true;
      break;
    }
    if (!Transferred)
      return std::nullopt;
  }
  return std::nullopt;
}

StackProvider selectedFrameOffset() {
  StackProvider P;
  P.add(0x100,
        {operation(NdOp::INT_AND, r(24), {r(8), c(1)}),
         operation(NdOp::SELECT, r(16), {r(24), c(16), c(32)}), branch(0x200)});
  P.add(0x200, {operation(NdOp::INT_SUB, r(40), {r(32), r(16)}),
                operation(NdOp::STORE, {}, {r(40), c(0x5a)}),
                operation(NdOp::LOAD, r(0), {r(40)})});
  P.nativeReturn(0x201);
  return P;
}

SpecializationOptions registerCaseOptions() {
  auto Options = stackOptions();
  Options.ControlRegisters = {{16, 8}};
  Options.DiscoverControlState = true;
  return Options;
}

TEST(NativeStackSpecialization, RegisterCasesSeparateFiniteFrameOffsets) {
  auto P = selectedFrameOffset();
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    auto Options = registerCaseOptions();
    Options.ByteOrder = Order;
    auto Result = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_GT(Result.ControlRefinements, 0u);
    for (uint64_t Base : {uint64_t{0x10000}, uint64_t{0x123456781000}})
      for (uint64_t Input : {0, 1, 7, 18}) {
        // Observe the low-valued byte in either guest byte order.
        const int64_t Lane = Order == llvm::endianness::little ? 0 : 7;
        const auto Run = execute(Result.Residual, Input, 0, Order, Base,
                                 {-16 + Lane, -32 + Lane});
        ASSERT_TRUE(Run);
        EXPECT_EQ(Run->Value, 0x5au);
        EXPECT_EQ(Run->Stack, Base);
        EXPECT_EQ(Run->Memory, (Input & 1 ? std::vector<uint8_t>{0x5a, 66}
                                          : std::vector<uint8_t>{65, 0x5a}));
      }
    // Only the original transfer and the two original case bodies have
    // native origins. A case comparison is a synthetic routing operation.
    unsigned EntryOrigins = 0;
    for (const auto &Origin : Result.Origins)
      EntryOrigins += Origin.NativeInstruction.Address == 0x100;
    EXPECT_EQ(EntryOrigins, 1u);
  }
}

TEST(NativeStackSpecialization,
     RegisterCasesRequireDiscoveryAndCompleteDomain) {
  auto P = selectedFrameOffset();
  for (bool Discovery : {false, true}) {
    auto Options = registerCaseOptions();
    Options.DiscoverControlState = Discovery;
    if (Discovery)
      Options.MaxControlTuples = 1;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_FALSE(Result.complete());
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Origins.empty());
  }
}

StackProvider lateRegisterCases(bool Unbounded = false) {
  auto P = selectedFrameOffset();
  P.add(0x100, {operation(NdOp::INT_AND, r(24), {r(8), c(1)}),
                operation(NdOp::SELECT, r(16), {r(24), c(16), c(32)}),
                operation(NdOp::INT_AND, r(24), {r(8), c(4)}),
                operation(NdOp::COND_BR, {}, {NdVar::cst(0x200, 8), r(24)})});
  if (Unbounded)
    P.add(0x101, {operation(NdOp::COPY, r(16), {r(8)}), branch(0x200)});
  else
    P.add(0x101, {operation(NdOp::INT_AND, r(24), {r(8), c(1)}),
                  operation(NdOp::SELECT, r(16), {r(24), c(48), c(64)}),
                  branch(0x200)});
  return P;
}

TEST(NativeStackSpecialization, RegisterCasesReproveLateOverwrittenControls) {
  for (uint32_t Chaining : {0, 64}) {
    auto P = lateRegisterCases();
    auto Options = registerCaseOptions();
    Options.MaxChainedTransfers = Chaining;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    for (uint64_t Input = 0; Input != 8; ++Input) {
      const auto Run =
          execute(Result.Residual, Input, 0, llvm::endianness::little, 0x10000,
                  {-16, -32, -48, -64});
      ASSERT_TRUE(Run);
      std::vector<uint8_t> Expected{65, 66, 67, 68};
      Expected[(Input & 4 ? 0 : 2) + (Input & 1 ? 0 : 1)] = 0x5a;
      EXPECT_EQ(Run->Memory, Expected);
      EXPECT_EQ(Run->Value, 0x5au);
    }
  }
}

TEST(NativeStackSpecialization, RegisterCasesNeverDropAnIncompleteLaterEdge) {
  auto P = lateRegisterCases(true);
  const auto Result = specializeInterpreter(P, {0x100}, registerCaseOptions());
  EXPECT_FALSE(Result.complete());
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
}

TEST(NativeStackSpecialization, RegisterCasesRetainNativeCallAndReturnEffects) {
  auto P = selectedFrameOffset();
  P.Code[0x300] = P.Code[0x200];
  P.Code[0x300].Origin.Address = 0x300;
  P.Code[0x300].Fallthrough = {0x301};
  for (auto &Op : P.Code[0x300].Ops)
    Op.Addr = 0x300;
  P.nativeReturn(0x301);
  P.nativeCall(0x200, 0x300);
  const auto Result = specializeInterpreter(P, {0x100}, registerCaseOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {0, 1}) {
    const auto Run = execute(Result.Residual, Input, 0,
                             llvm::endianness::little, 0x10000, {-24, -40});
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, 0x5au);
    EXPECT_EQ(Run->Stack, 0x10000u);
    EXPECT_EQ(Run->Memory, (Input & 1 ? std::vector<uint8_t>{0x5a, 66}
                                      : std::vector<uint8_t>{65, 0x5a}));
  }
}

TEST(NativeStackSpecialization, RegisterCasesShareRecoveryBudgets) {
  auto P = selectedFrameOffset();
  const auto Good = specializeInterpreter(P, {0x100}, registerCaseOptions());
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 6; ++Kind) {
    auto Options = registerCaseOptions();
    if (Kind == 0)
      Options.MaxOperations = Good.EvaluatedOperations;
    if (Kind == 1)
      Options.MaxNodeEvaluations = Good.NodeEvaluations;
    if (Kind == 2)
      Options.MaxSolverQueries = Good.SolverQueries;
    if (Kind == 3)
      Options.MaxControlRefinements = Good.ControlRefinements;
    if (Kind == 4)
      Options.MaxContextsPerAddress = 2;
    if (Kind == 5)
      Options.MaxNodes = Good.Contexts + 1; // One synthetic case comparison.
    const auto Exact = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Exact.complete()) << Kind << ": " << Exact.Diagnostic;
    if (Kind == 0)
      --Options.MaxOperations;
    if (Kind == 1)
      --Options.MaxNodeEvaluations;
    if (Kind == 2)
      --Options.MaxSolverQueries;
    if (Kind == 3)
      --Options.MaxControlRefinements;
    if (Kind == 4)
      --Options.MaxContextsPerAddress;
    if (Kind == 5)
      --Options.MaxNodes;
    const auto Short = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded)
        << Kind << ": " << Short.Diagnostic;
    EXPECT_TRUE(Short.Residual.Blocks.empty());
  }
}

TEST(NativeStackSpecialization,
     RegisterCasesRebuildWidenedPredecessorDispatch) {
  for (bool Corrupt : {false, true}) {
    auto P = selectedFrameOffset();
    P.add(0x100, {operation(NdOp::INT_AND, r(24), {r(8), c(4)}),
                  operation(NdOp::COND_BR, {}, {NdVar::cst(0x300, 8), r(24)})});
    P.add(0x300, {operation(NdOp::INT_AND, r(24), {r(8), c(1)}),
                  operation(NdOp::SELECT, r(16), {r(24), c(16), c(32)}),
                  branch(0x400)});
    P.add(0x101, {branch(0x102)});
    P.add(0x102,
          {operation(NdOp::INT_AND, r(24), {r(8), c(1)}),
           operation(NdOp::SELECT, r(16), {r(24), c(Corrupt ? 0 : 48), c(64)}),
           branch(0x400)});
    P.add(0x400, {branch(0x200)});
    const auto Result =
        specializeInterpreter(P, {0x100}, registerCaseOptions());
    if (Corrupt) {
      EXPECT_FALSE(Result.complete());
      EXPECT_TRUE(Result.Residual.Blocks.empty());
      EXPECT_TRUE(Result.Origins.empty());
      continue;
    }
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    for (uint64_t Input = 0; Input != 8; ++Input) {
      const auto Run =
          execute(Result.Residual, Input, 0, llvm::endianness::little, 0x10000,
                  {-16, -32, -48, -64});
      ASSERT_TRUE(Run);
      std::vector<uint8_t> Expected{65, 66, 67, 68};
      Expected[(Input & 4 ? 0 : 2) + (Input & 1 ? 0 : 1)] = 0x5a;
      EXPECT_EQ(Run->Memory, Expected);
    }
  }
}

TEST(NativeStackSpecialization, RegisterCasesDispatchAfterInternalReturnPop) {
  auto P = selectedFrameOffset();
  P.Code[0x101] = P.Code[0x200];
  P.Code[0x101].Origin.Address = 0x101;
  P.Code[0x101].Fallthrough = {0x102};
  for (auto &Op : P.Code[0x101].Ops)
    Op.Addr = 0x101;
  P.nativeReturn(0x102);
  P.nativeCall(0x100, 0x300);
  P.add(0x300, {operation(NdOp::INT_AND, r(24), {r(8), c(1)}),
                operation(NdOp::SELECT, r(16), {r(24), c(16), c(32)})});
  P.nativeReturn(0x301);
  const auto Result = specializeInterpreter(P, {0x100}, registerCaseOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {0, 1}) {
    const auto Run = execute(Result.Residual, Input, 0,
                             llvm::endianness::little, 0x10000, {-16, -32});
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Stack, 0x10000u);
    EXPECT_EQ(Run->Memory, (Input & 1 ? std::vector<uint8_t>{0x5a, 66}
                                      : std::vector<uint8_t>{65, 0x5a}));
  }
}

TEST(NativeStackSpecialization,
     RegisterCasesKeepUnboundedOverlappingHighBytes) {
  auto P = selectedFrameOffset();
  P.add(0x100, {operation(NdOp::COPY, r(16), {r(8)}),
                operation(NdOp::INT_AND, r(24), {r(8), c(1)}),
                operation(NdOp::SELECT, r(16, 1), {r(24), c(16, 1), c(32, 1)}),
                branch(0x200)});
  P.add(0x200, {operation(NdOp::INT_ZEXT, r(48), {r(16, 1)}),
                operation(NdOp::INT_SUB, r(40), {r(32), r(48)}),
                operation(NdOp::STORE, {}, {r(40), c(0x5a)}),
                operation(NdOp::COPY, r(0), {r(16)})});
  auto Options = registerCaseOptions();
  Options.ControlRegisters = {{16, 1}, {16, 8}};
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input :
       {UINT64_C(0), UINT64_C(1), UINT64_C(0x1234567809), UINT64_MAX - 1}) {
    const auto Run = execute(Result.Residual, Input, 0,
                             llvm::endianness::little, 0x10000, {-16, -32});
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, (Input & ~UINT64_C(255)) | (Input & 1 ? 16 : 32));
    EXPECT_EQ(Run->Memory, (Input & 1 ? std::vector<uint8_t>{0x5a, 66}
                                      : std::vector<uint8_t>{65, 0x5a}));
  }
}

StackProvider guardedAlignedSpill(bool Corrupt = false) {
  StackProvider P;
  P.add(0x100,
        {operation(NdOp::COPY, r(48), {r(32)}),
         operation(NdOp::INT_AND, r(40), {r(32), c(15)}),
         operation(NdOp::INT_SUB, r(32), {r(32), c(64)}),
         operation(NdOp::INT_AND, r(32), {r(32), c(uint64_t{0} - 16)}),
         operation(NdOp::INT_EQUAL, r(64, 1), {r(40), c(3)}),
         operation(NdOp::COND_BR, {}, {NdVar::cst(0x200, 8), r(64, 1)})});
  P.add(0x101, {operation(NdOp::COPY, r(32), {r(48)}),
                operation(NdOp::COPY, r(0), {c(9)})});
  P.nativeReturn(0x102);
  P.add(0x200, {operation(NdOp::INT_ADD, r(56), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(56), r(48)}),
                operation(NdOp::COPY, r(48), {c(0)})});
  if (Corrupt)
    P.add(0x201, {operation(NdOp::INT_ADD, r(72), {r(56), c(1)}),
                  operation(NdOp::STORE, {}, {r(72), c(0, 1)})});
  else
    P.add(0x201, {operation(NdOp::NOP, {}, {})});
  P.nativeCall(0x202, 0x300);
  P.add(0x203, {operation(NdOp::LOAD, r(48), {r(56)}),
                operation(NdOp::COPY, r(32), {r(48)})});
  P.nativeReturn(0x204);
  P.add(0x300, {operation(NdOp::COPY, r(0), {c(7)})});
  P.nativeReturn(0x301);
  return P;
}

TEST(NativeStackSpecialization,
     GuardedAlignedStackKeepsSpilledRootAcrossNativeCall) {
  auto P = guardedAlignedSpill();
  const auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Base : {uint64_t{0x10000}, uint64_t{0x123456781000}})
    for (uint64_t Low = 0; Low != 16; ++Low) {
      const auto Run =
          execute(Result.Residual, 0, 0, llvm::endianness::little, Base + Low);
      ASSERT_TRUE(Run);
      EXPECT_EQ(Run->Value, Low == 3 ? 7u : 9u);
      EXPECT_EQ(Run->Stack, Base + Low);
    }
}

TEST(NativeStackSpecialization, PartialAliasWriteInvalidatesAlignedRootSpill) {
  auto P = guardedAlignedSpill(true);
  const auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_FALSE(Result.complete());
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

StackProvider alignedSpillLoop(uint64_t Alignment, bool Corrupt = false) {
  auto P = guardedAlignedSpill(Corrupt);
  // Remove the input guard entirely. Recovery must cover every root residue,
  // including a countdown loop whose business input remains unconstrained.
  P.add(0x100,
        {operation(NdOp::COPY, r(48), {r(32)}),
         operation(NdOp::INT_SUB, r(32), {r(32), c(64)}),
         operation(NdOp::INT_AND, r(32), {r(32), c(uint64_t{0} - Alignment)}),
         operation(NdOp::INT_SUB, r(88), {r(32), r(48)}),
         operation(NdOp::INT_SUB, r(88), {c(0), r(88)}), branch(0x200)});
  P.add(0x203, {operation(NdOp::INT_ADD, r(0), {r(0), r(88)}),
                operation(NdOp::LOAD, r(48), {r(56)}),
                operation(NdOp::COPY, r(32), {r(48)})});
  P.add(0x300, {operation(NdOp::COPY, r(0), {c(7)}),
                operation(NdOp::COPY, r(80), {r(8)}), branch(0x301)});
  P.add(0x301,
        {operation(NdOp::INT_EQUAL, r(64, 1), {r(80), c(0)}),
         operation(NdOp::COND_BR, {}, {NdVar::cst(0x304, 8), r(64, 1)})});
  P.add(0x302, {operation(NdOp::INT_SUB, r(80), {r(80), c(1)}),
                operation(NdOp::INT_ADD, r(0), {r(0), c(2)}), branch(0x301)});
  P.nativeReturn(0x304);
  return P;
}

TEST(NativeStackSpecialization,
     AutomaticAlignmentPartitionCoversSpillsCallsAndCountdownLoops) {
  for (uint64_t Alignment : {2, 4, 8, 16, 32}) {
    auto P = alignedSpillLoop(Alignment);
    const auto Result = specializeInterpreter(P, {0x100}, stackOptions());
    ASSERT_TRUE(Result.complete()) << Alignment << ": " << Result.Diagnostic;
    EXPECT_EQ(Result.ControlRefinements, 1u);
    for (uint64_t Base : {uint64_t{0x10000}, uint64_t{0x123456781000}})
      for (uint64_t Low = 0; Low != Alignment; ++Low)
        for (uint64_t Input : {0, 1, 13}) {
          const auto Run = execute(Result.Residual, Input, 0,
                                   llvm::endianness::little, Base + Low);
          ASSERT_TRUE(Run) << Alignment << ": " << Low << ": " << Input;
          EXPECT_EQ(Run->Value, 7 + 2 * Input + 64 + Low);
          EXPECT_EQ(Run->Stack, Base + Low);
        }
  }
}

TEST(NativeStackSpecialization, OptionalWideMaskDoesNotForceStackPartition) {
  auto P = alignedSpillLoop(16);
  P.add(0xff, {operation(NdOp::INT_AND, r(0), {r(32), c(uint64_t{0} - 65536)}),
               branch(0x100)});
  const auto Result = specializeInterpreter(P, {0xff}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.ControlRefinements, 1u);
  const auto Run =
      execute(Result.Residual, 3, 0, llvm::endianness::little, 0x10007);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 7 + 2 * 3 + 64 + 7);
}

TEST(NativeStackSpecialization, ExplicitAlignmentIntersectsFinerPartitions) {
  auto P = alignedSpillLoop(32);
  for (uint32_t Residue : {0, 1, 3}) {
    auto O = stackOptions();
    O.ExplicitMachineState = true;
    O.EntryFrameAlignment = InterpreterEntryAlignment{4, Residue};
    O.MaxContextsPerAddress = 8;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    for (uint64_t Base : {uint64_t{0x10000}, uint64_t{0x123456781000}})
      for (uint64_t Low = Residue; Low < 32; Low += 4)
        for (uint64_t Input : {0, 1, 13}) {
          const auto Run = execute(R.Residual, Input, 0,
                                   llvm::endianness::little, Base + Low);
          ASSERT_TRUE(Run);
          EXPECT_EQ(Run->Value, 7 + 2 * Input + 64 + Low);
          EXPECT_EQ(Run->Stack, Base + Low);
        }
    O.MaxOperations = R.EvaluatedOperations;
    O.MaxSolverQueries = R.SolverQueries;
    EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
    --O.MaxOperations;
    const auto Short = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Short.Residual.Blocks.empty());
    O = stackOptions();
    O.MaxContextsPerAddress = 8;
    EXPECT_FALSE(specializeInterpreter(P, {0x100}, O).complete());
  }
}

TEST(NativeStackSpecialization, EntryAlignmentDoesNotDropAllowedFailures) {
  for (uint32_t Bad : {0, 15}) {
    auto P = alignedSpillLoop(16);
    P.add(0x300,
          {operation(NdOp::COPY, r(0), {c(7)}),
           operation(NdOp::COPY, r(80), {r(8)}),
           operation(NdOp::INT_EQUAL, r(96, 1), {r(88), c(64 + Bad)}),
           operation(NdOp::COND_BR, {}, {NdVar::cst(0xdead, 8), r(96, 1)})});
    auto O = stackOptions();
    O.ExplicitMachineState = true;
    O.EntryFrameAlignment = InterpreterEntryAlignment{2, 1 - (Bad & 1)};
    ASSERT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
    O.EntryFrameAlignment->Residue = Bad & 1;
    const auto BadDomain = specializeInterpreter(P, {0x100}, O);
    EXPECT_FALSE(BadDomain.complete());
    EXPECT_TRUE(BadDomain.Residual.Blocks.empty());
    EXPECT_TRUE(BadDomain.Origins.empty());
    EXPECT_TRUE(BadDomain.Reads.empty());
  }
}

TEST(NativeStackSpecialization, EntryAlignmentRequiresAnExplicitValidDomain) {
  StackProvider P;
  P.nativeReturn(0x100);
  for (auto Domain :
       {InterpreterEntryAlignment{0, 0}, {3, 0}, {8, 8}, {8, 9}}) {
    auto O = stackOptions();
    O.ExplicitMachineState = true;
    O.EntryFrameAlignment = Domain;
    EXPECT_EQ(specializeInterpreter(P, {0x100}, O).Status,
              SpecializationStatus::InvalidInput);
  }
  auto O = stackOptions();
  O.EntryFrameAlignment = InterpreterEntryAlignment{16, 3};
  EXPECT_EQ(specializeInterpreter(P, {0x100}, O).Status,
            SpecializationStatus::InvalidInput);
  O.ExplicitMachineState = true;
  O.FrameBaseRegister = SymRegisterRange{x86reg::RBP, 8};
  EXPECT_EQ(specializeInterpreter(P, {0x100}, O).Status,
            SpecializationStatus::InvalidInput);
  O.FrameBaseRegister.reset();
  EXPECT_EQ(specializeInterpreter(P, {0x100}, O).Status,
            SpecializationStatus::InvalidInput);
}

TEST(NativeStackSpecialization, EveryAlignmentCaseMustFinishBeforePublication) {
  for (uint64_t BadResidue : {0, 15}) {
    auto P = alignedSpillLoop(16);
    P.add(0x300,
          {operation(NdOp::COPY, r(0), {c(7)}),
           operation(NdOp::COPY, r(80), {r(8)}),
           operation(NdOp::INT_EQUAL, r(96, 1), {r(88), c(64 + BadResidue)}),
           operation(NdOp::COND_BR, {}, {NdVar::cst(0xdead, 8), r(96, 1)})});
    const auto Result = specializeInterpreter(P, {0x100}, stackOptions());
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Origins.empty());
    EXPECT_TRUE(Result.Reads.empty());
  }
}

TEST(NativeStackSpecialization, NecessaryPartitionUpgradeRetainsEarlierWork) {
  auto P = alignedSpillLoop(32);
  P.add(0x100, {operation(NdOp::COPY, r(48), {r(32)}),
                operation(NdOp::INT_SUB, r(32), {r(32), c(64)}),
                operation(NdOp::INT_AND, r(32), {r(32), c(uint64_t{0} - 16)}),
                operation(NdOp::INT_AND, r(32), {r(32), c(uint64_t{0} - 32)}),
                operation(NdOp::INT_SUB, r(88), {r(32), r(48)}),
                operation(NdOp::INT_SUB, r(88), {c(0), r(88)}), branch(0x200)});
  const auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.ControlRefinements, 2u);
  for (uint64_t Low = 0; Low != 32; ++Low) {
    const auto Run =
        execute(Result.Residual, 1, 0, llvm::endianness::little, 0x10000 + Low);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, 7 + 2 + 64 + Low);
  }
  auto Short = stackOptions();
  Short.MaxControlRefinements = 1;
  const auto Failed = specializeInterpreter(P, {0x100}, Short);
  EXPECT_EQ(Failed.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Failed.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, OptionalRootArithmeticRetainsItsOriginalCode) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_MULT, r(0), {r(32), r(8)})});
  P.nativeReturn(0x101);
  auto Options = stackOptions();
  Options.MaxSolverGates = 1;
  Options.MaxSolverQueries = 1;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.SolverQueries, 0u);
  const auto Run = execute(Result.Residual, 17);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, uint64_t{0x10000} * 17);
}

TEST(NativeStackSpecialization, AlignmentOracleDetectsWrongEntryDispatch) {
  auto P = alignedSpillLoop(8);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  bool Changed = false;
  for (auto &B : Result.Residual.Blocks)
    for (auto &Op : B.Ops)
      if (Op.Opcode == NdOp::INT_AND && Op.Output.isTemp() &&
          Op.Inputs[0] == r(32)) {
        Op.Inputs[1] = c(0);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  const auto Run =
      execute(Result.Residual, 1, 0, llvm::endianness::little, 0x10003);
  ASSERT_TRUE(Run);
  EXPECT_NE(Run->Value, 7 + 2 + 64 + 3);
}

TEST(NativeStackSpecialization, AlignmentCannotRepairAnAliasedSavedRoot) {
  auto P = alignedSpillLoop(16, true);
  const auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_FALSE(Result.complete());
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, AlignmentPartitionsShareAllRecoveryBudgets) {
  auto P = alignedSpillLoop(8);
  const auto Good = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    auto Exact = stackOptions();
    if (Kind == 0)
      Exact.MaxOperations = Good.EvaluatedOperations;
    if (Kind == 1)
      Exact.MaxNodeEvaluations = Good.NodeEvaluations;
    if (Kind == 2)
      Exact.MaxSolverQueries = Good.SolverQueries;
    if (Kind == 3)
      Exact.MaxContextsPerAddress = 8;
    const auto Replay = specializeInterpreter(P, {0x100}, Exact);
    ASSERT_TRUE(Replay.complete()) << Kind << ": " << Replay.Diagnostic;
    if (Kind == 0)
      --Exact.MaxOperations;
    if (Kind == 1)
      --Exact.MaxNodeEvaluations;
    if (Kind == 2)
      --Exact.MaxSolverQueries;
    if (Kind == 3)
      --Exact.MaxContextsPerAddress;
    const auto Short = specializeInterpreter(P, {0x100}, Exact);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded)
        << Kind << ": " << Short.Diagnostic;
    EXPECT_TRUE(Short.Residual.Blocks.empty());
  }
}

TEST(NativeStackSpecialization, DirectCalleeRetainsPhysicalRegisterEffects) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::COPY, r(0), {c(17)})});
  P.nativeCall(0x101, 0x200);
  P.nativeReturn(0x102);
  P.add(0x200, {operation(NdOp::INT_ADD, r(0), {r(0), r(8)})});
  P.nativeReturn(0x201);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{19}, UINT64_MAX}) {
    auto Run = execute(Result.Residual, Input);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input + 17);
    EXPECT_EQ(Run->Stack, 0x10000u);
  }
}

TEST(NativeStackSpecialization, SharedCalleeKeepsDistinctReturnContexts) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::COPY, r(0), {c(1)})});
  P.nativeCall(0x101, 0x200);
  P.nativeCall(0x102, 0x200);
  P.nativeReturn(0x103);
  P.nativeCall(0x200, 0x300);
  P.nativeReturn(0x201);
  P.add(0x300, {operation(NdOp::INT_ADD, r(0), {r(0), c(3)})});
  P.nativeReturn(0x301);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 7u);
  EXPECT_EQ(Run->Stack, 0x10000u);
}

TEST(NativeStackSpecialization,
     TransferChainsRetainRepeatedCallsAndReturnSlots) {
  for (bool Stop : {false, true}) {
    StackProvider P;
    P.add(0x100, {operation(NdOp::COPY, r(0), {r(8)})});
    P.nativeCall(0x101, 0x200);
    P.nativeCall(0x102, 0x200);
    P.nativeReturn(0x103);
    P.nativeCall(0x200, 0x300);
    P.nativeReturn(0x201);
    P.add(0x300, {operation(NdOp::INT_ADD, r(0), {r(0), c(3)})});
    P.nativeReturn(0x301);
    std::vector<int64_t> Bytes;
    for (int64_t Offset = -16; Offset != 8; ++Offset)
      Bytes.push_back(Offset);
    for (auto Order : {llvm::endianness::little, llvm::endianness::big})
      for (uint32_t Bound : {0, 1, 16}) {
        auto Options = stackOptions();
        Options.StopChainingAtRepeatedDestination = Stop;
        Options.ByteOrder = Order;
        Options.MaxChainedTransfers = Bound;
        const auto R = specializeInterpreter(P, {0x100}, Options);
        ASSERT_TRUE(R.complete()) << R.Diagnostic;
        EXPECT_EQ(std::count_if(R.Origins.begin(), R.Origins.end(),
                                [](const auto &O) {
                                  return O.NativeInstruction.Address == 0x301;
                                }),
                  2);
        for (uint64_t Input : {uint64_t{0}, uint64_t{23}, UINT64_MAX}) {
          const auto Run = execute(R.Residual, Input, 0, Order, 0x10000, Bytes);
          ASSERT_TRUE(Run);
          EXPECT_EQ(Run->Value, Input + 6);
          EXPECT_EQ(Run->Stack, 0x10000U);
          ASSERT_EQ(Run->Memory.size(), Bytes.size());
          for (unsigned I = 0; I != 8; ++I) {
            const auto Shift =
                8 * (Order == llvm::endianness::little ? I : 7 - I);
            EXPECT_EQ(Run->Memory[I], uint8_t(uint64_t{0x201} >> Shift));
            EXPECT_EQ(Run->Memory[8 + I], uint8_t(uint64_t{0x103} >> Shift));
            EXPECT_EQ(Run->Memory[16 + I], 81 + I);
          }
        }
      }
  }
}

StackProvider finiteCalls() {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_AND, r(24), {r(8), c(1)}),
                operation(NdOp::SELECT, r(16), {r(24), c(0x200), c(0x300)})});
  P.nativeIndirectCall(0x101, r(16));
  P.nativeReturn(0x102);
  P.add(0x200, {operation(NdOp::INT_ADD, r(0), {r(8), c(17)})});
  P.nativeReturn(0x201);
  P.add(0x300, {operation(NdOp::INT_XOR, r(0), {r(8), c(0x1234)})});
  P.nativeReturn(0x301);
  return P;
}

TEST(NativeStackSpecialization, FiniteRegisterCallsRetainTargetsAndGuestStack) {
  auto P = finiteCalls();
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{42}, UINT64_MAX}) {
    auto Run = execute(Result.Residual, Input);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input & 1 ? Input + 17 : Input ^ 0x1234);
    EXPECT_EQ(Run->Stack, 0x10000u);
  }
  for (const auto &Block : Result.Residual.Blocks)
    for (const auto &Op : Block.Ops) {
      EXPECT_NE(Op.Opcode, NdOp::CALL);
      EXPECT_NE(Op.Opcode, NdOp::INDIR_CALL);
      EXPECT_NE(Op.Opcode, NdOp::INDIR_BR);
    }
}

TEST(NativeStackSpecialization,
     FiniteRegisterCallTargetBudgetRefusesPublication) {
  auto P = finiteCalls();
  auto Options = stackOptions();
  Options.MaxIndirectTargets = 1;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_FALSE(Result.complete());
  EXPECT_TRUE(Result.Status == SpecializationStatus::UnresolvedControl ||
              Result.Status == SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, UnknownRegisterCallDoesNotInventACallee) {
  StackProvider P;
  P.nativeIndirectCall(0x100, r(8));
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, FiniteInternalReturnsConsumeActualSlotOnce) {
  StackProvider P;
  P.nativeCall(0x100, 0x200);
  // Neither selected destination is the original call continuation.
  P.add(0x200, {operation(NdOp::INT_AND, r(24), {r(8), c(1)}),
                operation(NdOp::SELECT, r(16), {r(24), c(0x300), c(0x400)}),
                operation(NdOp::STORE, {}, {r(32), r(16)})});
  P.nativeReturn(0x201);
  P.add(0x300, {operation(NdOp::INT_ADD, r(0), {r(8), c(17)})});
  P.nativeReturn(0x301);
  P.add(0x400, {operation(NdOp::INT_XOR, r(0), {r(8), c(0x1234)})});
  P.nativeReturn(0x401);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{42}, UINT64_MAX}) {
    auto Run = execute(Result.Residual, Input);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input & 1 ? Input + 17 : Input ^ 0x1234);
    EXPECT_EQ(Run->Stack, 0x10000u);
  }
  auto Options = stackOptions();
  Options.MaxIndirectTargets = 1;
  auto Limited = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Limited.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Limited.Residual.Blocks.empty());
}

StackProvider frameCursor(bool Spilled) {
  StackProvider P;
  if (Spilled)
    P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(40)}),
                  operation(NdOp::INT_SUB, r(48), {r(32), c(32)}),
                  operation(NdOp::STORE, {}, {r(40), r(48)}), branch(0x200)});
  else
    P.add(0x100,
          {operation(NdOp::INT_SUB, r(48), {r(32), c(32)}), branch(0x200)});
  P.add(0x200, {});
  if (Spilled)
    P.add(0x200, {operation(NdOp::LOAD, r(48), {r(40)})});
  P.add(0x201, {operation(NdOp::STORE, {}, {r(48), r(8)}),
                operation(NdOp::INT_ADD, r(48), {r(48), c(8)})});
  if (Spilled)
    P.add(0x202, {operation(NdOp::STORE, {}, {r(40), r(48)})});
  else
    P.add(0x202, {});
  P.add(0x203,
        {operation(NdOp::INT_SUB, r(56), {r(32), c(16)}),
         operation(NdOp::INT_NOTEQUAL, r(24, 1), {r(48), r(56)}),
         operation(NdOp::COND_BR, {}, {NdVar::cst(0x200, 8), r(24, 1)})});
  P.add(0x204, {operation(NdOp::INT_SUB, r(56), {r(32), c(24)}),
                operation(NdOp::LOAD, r(0), {r(56)})});
  P.nativeReturn(0x205);
  return P;
}

TEST(NativeStackSpecialization, AutomaticContextsPreserveRelativeFrameCursors) {
  for (bool Spilled : {false, true}) {
    SCOPED_TRACE(Spilled);
    auto P = frameCursor(Spilled);
    auto Options = stackOptions();
    Options.DiscoverControlState = true;
    auto Result = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_GT(Result.DiscoveredContextFields, 0u);
    for (uint64_t Input : {uint64_t{0}, uint64_t{71}, UINT64_MAX}) {
      auto Run = execute(Result.Residual, Input);
      ASSERT_TRUE(Run);
      EXPECT_EQ(Run->Value, Input);
      EXPECT_EQ(Run->Stack, 0x10000u);
    }
    Options.MaxContextsPerAddress = 1;
    auto Limited = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Limited.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Limited.Residual.Blocks.empty());
  }
}

TEST(NativeStackSpecialization,
     AddressRefinementPrecedesUnrelatedFramePartitions) {
  for (bool Spilled : {false, true})
    for (uint64_t Alignment : {256, 65536}) {
      SCOPED_TRACE(Spilled);
      SCOPED_TRACE(Alignment);
      auto P = frameCursor(Spilled);
      P.add(0xff, {operation(NdOp::INT_AND, r(112),
                             {r(32), c(uint64_t{0} - Alignment)}),
                   branch(0x100)});
      auto Options = stackOptions();
      Options.DiscoverControlState = true;
      // The smaller incidental mask fits, the larger one does not. Neither
      // supplies the missing relation between successive cursor values.
      Options.MaxContextsPerAddress = 512;
      const auto Baseline = specializeInterpreter(P, {0x100}, Options);
      ASSERT_TRUE(Baseline.complete()) << Baseline.Diagnostic;
      const auto Result = specializeInterpreter(P, {0xff}, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_EQ(Result.ControlRefinements, Baseline.ControlRefinements);
      for (uint64_t Base : {uint64_t{0x10007}, uint64_t{0x123456781003}})
        for (uint64_t Input : {uint64_t{0}, uint64_t{71}, UINT64_MAX}) {
          const auto Run = execute(Result.Residual, Input, 0,
                                   llvm::endianness::little, Base);
          ASSERT_TRUE(Run);
          EXPECT_EQ(Run->Value, Input);
          EXPECT_EQ(Run->Stack, Base);
        }
    }
}

TEST(NativeStackSpecialization,
     ControlRefinementRetainsNecessaryFrameFallback) {
  auto P = frameCursor(true);
  P.add(0xff, {operation(NdOp::COPY, r(104), {r(32)}),
               operation(NdOp::INT_AND, r(32), {r(32), c(uint64_t{0} - 16)}),
               operation(NdOp::INT_SUB, r(32), {r(32), c(64)}), branch(0x100)});
  P.add(0x205, {operation(NdOp::COPY, r(32), {r(104)})});
  P.nativeReturn(0x206);
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  Options.MaxControlFields = 64;
  Options.MaxControlRefinements = 64;
  Options.MaxDiscoveryVisits = 1048576;
  Options.MaxSolverQueries = 65536;
  const auto Good = specializeInterpreter(P, {0xff}, Options);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  EXPECT_GT(Good.DiscoveredControlFields, 0u);
  EXPECT_GT(Good.ControlRefinements, 1u);
  for (uint64_t Base : {uint64_t{0x10000}, uint64_t{0x123456781000}})
    for (uint64_t Low = 0; Low != 16; ++Low)
      for (uint64_t Input : {uint64_t{0}, uint64_t{71}, UINT64_MAX}) {
        const auto Run = execute(Good.Residual, Input, 0,
                                 llvm::endianness::little, Base + Low);
        ASSERT_TRUE(Run);
        EXPECT_EQ(Run->Value, Input);
        EXPECT_EQ(Run->Stack, Base + Low);
      }
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    auto Exact = Options;
    if (Kind == 0)
      Exact.MaxOperations = Good.EvaluatedOperations;
    if (Kind == 1)
      Exact.MaxSolverQueries = Good.SolverQueries;
    if (Kind == 2)
      Exact.MaxControlRefinements = Good.ControlRefinements;
    ASSERT_TRUE(specializeInterpreter(P, {0xff}, Exact).complete()) << Kind;
    if (Kind == 0)
      --Exact.MaxOperations;
    if (Kind == 1)
      --Exact.MaxSolverQueries;
    if (Kind == 2)
      --Exact.MaxControlRefinements;
    const auto Failed = specializeInterpreter(P, {0xff}, Exact);
    EXPECT_EQ(Failed.Status, SpecializationStatus::BudgetExceeded) << Kind;
    EXPECT_TRUE(Failed.Residual.Blocks.empty());
    EXPECT_TRUE(Failed.Origins.empty());
    EXPECT_TRUE(Failed.Reads.empty());
  }
}

StackProvider guardedPointerJoin(bool Spilled) {
  StackProvider P;
  P.add(0x100,
        {operation(NdOp::COPY, r(80), {r(32)}),
         operation(NdOp::INT_SUB, r(40), {r(32), c(96)}),
         operation(NdOp::INT_NOTEQUAL, r(96, 1), {r(8), c(0)}),
         operation(NdOp::COND_BR, {}, {NdVar::cst(0x200, 8), r(96, 1)})});
  P.Code[0x100].Fallthrough = {0x300};
  for (auto [Address, Displacement] :
       {std::pair<va_t, uint64_t>{0x200, 32}, {0x300, 40}}) {
    P.add(Address, {operation(NdOp::INT_SUB, r(72), {r(32), c(Displacement)}),
                    operation(NdOp::INT_SUB, r(56), {r(32), c(48)})});
    if (Spilled)
      P.add(Address + 1, {operation(NdOp::STORE, {}, {r(40), r(72)})});
    else
      P.add(Address + 1, {});
    P.add(Address + 2,
          {operation(NdOp::INT_LESSEQUAL, r(96, 1), {r(72), r(56)}),
           operation(NdOp::COND_BR, {}, {NdVar::cst(0x400, 8), r(96, 1)})});
    P.Code[Address + 2].Fallthrough = {0x500};
  }
  if (Spilled)
    P.add(0x400, {operation(NdOp::LOAD, r(72), {r(40)})});
  else
    P.add(0x400, {});
  P.add(0x401, {operation(NdOp::INT_SUB, r(32), {r(72), c(16)}),
                operation(NdOp::STORE, {}, {r(32), c(5, 1)}),
                operation(NdOp::LOAD, r(24, 1), {r(32)}),
                operation(NdOp::INT_ZEXT, r(0), {r(24, 1)}),
                operation(NdOp::COPY, r(32), {r(80)}), ret()});
  P.add(0x500, {operation(NdOp::COPY, r(0), {c(31)}), ret()});
  return P;
}

TEST(NativeStackSpecialization, NarrowAddressDemandRetainsCompletePointer) {
  for (bool Spilled : {false, true})
    for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      SCOPED_TRACE(Spilled);
      SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
      auto P = guardedPointerJoin(Spilled);
      auto Options = stackOptions();
      Options.DiscoverControlState = true;
      Options.ByteOrder = Order;
      const auto Result = specializeInterpreter(P, {0x100}, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_GT(Result.DiscoveredContextFields, 0u);
      std::vector<int64_t> Observed;
      for (int64_t Offset = -112; Offset != 8; ++Offset)
        Observed.push_back(Offset);
      std::vector<uint64_t> Roots;
      for (uint64_t Root = 0; Root != 65; ++Root)
        Roots.push_back(Root);
      for (uint64_t Root : {uint64_t{0x10000}, uint64_t{0x123456781000},
                            UINT64_MAX - 64, UINT64_MAX})
        Roots.push_back(Root);
      for (uint64_t Root : Roots)
        for (uint64_t Input : {uint64_t{0}, uint64_t{1}, UINT64_MAX}) {
          SCOPED_TRACE(Root);
          SCOPED_TRACE(Input);
          const uint64_t Pointer = Root - (Input ? 32 : 40);
          const bool Taken = Pointer <= Root - 48;
          auto Run = execute(Result.Residual, Input, 0, Order, Root, Observed);
          ASSERT_TRUE(Run);
          EXPECT_EQ(Run->Value, Taken ? 5u : 31u);
          EXPECT_EQ(Run->Stack, Root);
          for (size_t I = 0; I < Observed.size(); ++I) {
            uint8_t Expected = 65 + I;
            const int64_t Offset = Observed[I];
            if (Spilled && Offset >= -96 && Offset < -88) {
              const auto Byte = static_cast<unsigned>(Offset + 96);
              const unsigned Shift =
                  8 * (Order == llvm::endianness::little ? Byte : 7 - Byte);
              Expected = Pointer >> Shift;
            }
            if (Taken && Root + Offset == Pointer - 16)
              Expected = 5;
            EXPECT_EQ(Run->Memory[I], Expected) << Offset;
          }
        }
    }
}

TEST(NativeStackSpecialization, CompleteAddressContextsShareRefinementBudgets) {
  for (bool Spilled : {false, true}) {
    SCOPED_TRACE(Spilled);
    auto P = guardedPointerJoin(Spilled);
    auto Options = stackOptions();
    Options.DiscoverControlState = true;
    const auto Good = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
    for (unsigned Kind = 0; Kind != 4; ++Kind) {
      auto Exact = Options;
      if (Kind == 0)
        Exact.MaxOperations = Good.EvaluatedOperations;
      if (Kind == 1)
        Exact.MaxNodeEvaluations = Good.NodeEvaluations;
      if (Kind == 2)
        Exact.MaxSolverQueries = Good.SolverQueries;
      if (Kind == 3)
        Exact.MaxControlRefinements = Good.ControlRefinements;
      const auto Replay = specializeInterpreter(P, {0x100}, Exact);
      ASSERT_TRUE(Replay.complete()) << Kind << ": " << Replay.Diagnostic;
      if (Kind == 0)
        --Exact.MaxOperations;
      if (Kind == 1)
        --Exact.MaxNodeEvaluations;
      if (Kind == 2)
        --Exact.MaxSolverQueries;
      if (Kind == 3)
        --Exact.MaxControlRefinements;
      const auto Short = specializeInterpreter(P, {0x100}, Exact);
      EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded)
          << Kind << ": " << Short.Diagnostic;
      EXPECT_TRUE(Short.Residual.Blocks.empty());
      EXPECT_TRUE(Short.Reads.empty());
    }
    Options.MaxContextsPerAddress = 1;
    const auto Limited = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Limited.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Limited.Residual.Blocks.empty());
    Options.MaxContextsPerAddress = stackOptions().MaxContextsPerAddress;
    Options.MaxDiscoveryVisits = 1;
    const auto NoDiscovery = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(NoDiscovery.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_LE(NoDiscovery.DiscoveryVisits, Options.MaxDiscoveryVisits);
    EXPECT_TRUE(NoDiscovery.Residual.Blocks.empty());
  }
}

TEST(NativeStackSpecialization, CompleteAddressContextsRejectCorruptedPointer) {
  for (bool Spilled : {false, true}) {
    SCOPED_TRACE(Spilled);
    auto P = guardedPointerJoin(Spilled);
    if (Spilled)
      P.add(0x400, {operation(NdOp::STORE, {}, {r(40), r(88, 1)}),
                    operation(NdOp::LOAD, r(72), {r(40)})});
    else
      P.add(0x400, {operation(NdOp::COPY, r(72, 1), {r(88, 1)})});
    auto Options = stackOptions();
    Options.DiscoverControlState = true;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported)
        << Result.Diagnostic;
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Reads.empty());
  }
}

StackProvider lowGuardedFrameControl(bool Spilled, bool Contradictory = false) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(48), {r(32), c(32)}),
                operation(NdOp::INT_SUB, r(40), {r(32), c(64)})});
  if (Spilled)
    P.add(0x101, {operation(NdOp::STORE, {}, {r(40), r(48)}),
                  operation(NdOp::COPY, r(48), {c(0)})});
  else
    P.add(0x101, {});
  P.add(0x102, {operation(NdOp::INT_AND, r(56), {r(32), c(3)}),
                operation(NdOp::INT_EQUAL, r(96, 1), {r(56), c(1)})});
  if (Contradictory)
    P.add(0x103, {operation(NdOp::INT_ADD, r(56), {r(32), c(1)}),
                  operation(NdOp::INT_AND, r(56), {r(56), c(3)}),
                  operation(NdOp::INT_EQUAL, r(97, 1), {r(56), c(1)}),
                  operation(NdOp::INT_AND, r(96, 1), {r(96, 1), r(97, 1)})});
  else
    P.add(0x103, {});
  P.add(0x104,
        {operation(NdOp::COND_BR, {}, {NdVar::cst(0x200, 8), r(96, 1)})});
  P.Code[0x104].Fallthrough = {0x300};
  for (auto [Address, Value] :
       {std::pair<va_t, uint64_t>{0x200, 7}, {0x300, 11}}) {
    if (Spilled)
      P.add(Address, {operation(NdOp::LOAD, r(48), {r(40)})});
    else
      P.add(Address, {});
    P.add(Address + 1, {operation(NdOp::STORE, {}, {r(48), c(Value, 1)}),
                        operation(NdOp::COPY, r(0), {c(Value)}), ret()});
  }
  return P;
}

TEST(NativeStackSpecialization, LowRootGuardKeepsAffineControlDomainUnbounded) {
  for (bool Spilled : {false, true})
    for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      SCOPED_TRACE(Spilled);
      SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
      auto P = lowGuardedFrameControl(Spilled);
      auto Options = stackOptions();
      Options.ByteOrder = Order;
      Options.MaxSolverQueries = 8;
      if (Spilled)
        Options.ControlFrameSlots = {{-64, 8}};
      else
        Options.ControlRegisters = {{48, 8}};
      const auto Result = specializeInterpreter(P, {0x100}, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_LE(Result.SolverQueries, Options.MaxSolverQueries);
      std::vector<int64_t> Observed;
      for (int64_t Offset = -80; Offset != 8; ++Offset)
        Observed.push_back(Offset);
      for (uint64_t Base : {uint64_t{0}, uint64_t{0x10000},
                            uint64_t{0x12345678fff0}, UINT64_MAX - 15})
        for (uint64_t Low = 0; Low != 16; ++Low) {
          const uint64_t Root = Base + Low;
          SCOPED_TRACE(Root);
          const uint64_t Expected = (Root & 3) == 1 ? 7 : 11;
          const auto Run =
              execute(Result.Residual, 0, 0, Order, Root, Observed);
          ASSERT_TRUE(Run);
          EXPECT_EQ(Run->Value, Expected);
          EXPECT_EQ(Run->Stack, Root);
          for (size_t I = 0; I < Observed.size(); ++I) {
            uint8_t Byte = 65 + I;
            const int64_t Offset = Observed[I];
            if (Spilled && Offset >= -64 && Offset < -56) {
              const unsigned Index = Offset + 64;
              Byte =
                  (Root - 32) >>
                  (8 * (Order == llvm::endianness::little ? Index : 7 - Index));
            }
            if (Offset == -32)
              Byte = Expected;
            EXPECT_EQ(Run->Memory[I], Byte) << Offset;
          }
        }
    }
}

TEST(NativeStackSpecialization, FreeAffineDomainDoesNotProveEdgeReachability) {
  for (bool Contradictory : {false, true}) {
    SCOPED_TRACE(Contradictory);
    auto P = lowGuardedFrameControl(false, Contradictory);
    P.Code.erase(0x200);
    auto Options = stackOptions();
    Options.ControlRegisters = {{48, 8}};
    Options.MaxSolverQueries = 8;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    if (!Contradictory) {
      EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
      EXPECT_TRUE(Result.Residual.Blocks.empty());
    } else {
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      for (uint64_t Root : {uint64_t{0}, uint64_t{1}, UINT64_MAX}) {
        const auto Run =
            execute(Result.Residual, 0, 0, llvm::endianness::little, Root);
        ASSERT_TRUE(Run);
        EXPECT_EQ(Run->Value, 11u);
        EXPECT_EQ(Run->Stack, Root);
      }
    }
  }
}

StackProvider guardedDecoder(bool Unknown) {
  StackProvider P;
  P.add(0x100,
        {operation(NdOp::COPY, r(16), {Unknown ? r(8) : c(0)}), branch(0x200)});
  // A shared decoder rejects invalid opcodes before its direct handler tests.
  // The two valid phases need a relation even though there is no indirect JMP.
  P.add(0x200,
        {operation(NdOp::INT_LESS, r(24, 1), {c(1), r(16)}),
         operation(NdOp::COND_BR, {}, {NdVar::cst(0xdead, 8), r(24, 1)})});
  P.add(0x201,
        {operation(NdOp::INT_EQUAL, r(24, 1), {r(16), c(0)}),
         operation(NdOp::COND_BR, {}, {NdVar::cst(0x300, 8), r(24, 1)})});
  P.add(0x202, {operation(NdOp::INT_ADD, r(0), {r(8), c(17)})});
  P.nativeReturn(0x203);
  P.add(0x300, {operation(NdOp::COPY, r(16), {c(1)}), branch(0x200)});
  return P;
}

TEST(NativeStackSpecialization, RefinesGuardsBeforeRejectingAnUnsupportedPath) {
  auto P = guardedDecoder(false);
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GT(Result.ControlRefinements, 0u);
  for (uint64_t Input : {uint64_t{0}, uint64_t{42}, UINT64_MAX}) {
    auto Run = execute(Result.Residual, Input);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input + 17);
  }
  Options.MaxControlRefinements = 0;
  auto Limited = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Limited.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Limited.Residual.Blocks.empty());
}

StackProvider finiteDispatchDecoder(bool Spilled, bool Unknown = false,
                                    bool LateInvalid = false) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(24)}),
                operation(NdOp::COPY, r(16), {Unknown ? r(8) : c(0)})});
  if (Spilled)
    P.add(0x101, {operation(NdOp::STORE, {}, {r(40), r(16)}), branch(0x200)});
  else
    P.add(0x101, {branch(0x200)});
  if (Spilled)
    P.add(0x200, {operation(NdOp::LOAD, r(16), {r(40)})});
  else
    P.add(0x200, {});
  // The dispatch has four finite candidates after a join loses the phase.
  // Only phases zero and one are reachable from the initialized entry.
  P.add(0x201, {operation(NdOp::INT_AND, r(24), {r(16), c(3)}),
                operation(NdOp::INT_MULT, r(24), {r(24), c(0x100)}),
                operation(NdOp::INT_ADD, r(24), {r(24), c(0x400)}),
                operation(NdOp::INDIR_BR, {}, {r(24)})});
  P.add(0x400, {operation(NdOp::COPY, r(16), {c(1)})});
  if (Spilled)
    P.add(0x401, {operation(NdOp::STORE, {}, {r(40), r(16)}), branch(0x200)});
  else
    P.add(0x401, {branch(0x200)});
  if (LateInvalid) {
    P.add(0x500, {operation(NdOp::COPY, r(16), {c(2)})});
    if (Spilled)
      P.add(0x501, {operation(NdOp::STORE, {}, {r(40), r(16)}), branch(0x200)});
    else
      P.add(0x501, {branch(0x200)});
  } else {
    P.add(0x500, {operation(NdOp::INT_ADD, r(0), {r(8), c(17)})});
    P.nativeReturn(0x501);
  }
  return P;
}

TEST(NativeStackSpecialization, RefinesFiniteDispatchBeforeUnsupportedArm) {
  for (bool Spilled : {false, true})
    for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      SCOPED_TRACE(Spilled);
      SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
      auto P = finiteDispatchDecoder(Spilled);
      auto O = stackOptions();
      O.ByteOrder = Order;
      const auto Baseline = specializeInterpreter(P, {0x100}, O);
      EXPECT_EQ(Baseline.Status, SpecializationStatus::Unsupported);
      EXPECT_TRUE(Baseline.Residual.Blocks.empty());
      O.DiscoverControlState = true;
      const auto R = specializeInterpreter(P, {0x100}, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      EXPECT_GT(R.ControlRefinements, 0u);
      for (uint64_t Input :
           {uint64_t{0}, uint64_t{1}, uint64_t{37}, UINT64_MAX}) {
        const auto Run = execute(R.Residual, Input, 0, Order);
        ASSERT_TRUE(Run);
        EXPECT_EQ(Run->Value, Input + 17);
        EXPECT_EQ(Run->Stack, 0x10000u);
      }
    }
}

TEST(NativeStackSpecialization, FiniteGuardsDoNotPreemptExistingRefinement) {
  auto P = guardedDecoder(false);
  // A runtime dispatch before the decoder has a large, irrelevant selector
  // expression. Its four valid arms converge before the native phase guard.
  // Only that guard needs refinement; eagerly walking the selector would
  // consume the discovery budget before the useful dependency is inspected.
  P.add(0x10, {operation(NdOp::COPY, r(56), {r(8)})});
  for (va_t Address = 0x11; Address != 0x91; ++Address)
    P.add(Address, {operation(NdOp::INT_MULT, r(56), {r(56), c(3)}),
                    operation(NdOp::INT_XOR, r(56), {r(56), r(8)})});
  P.add(0x91, {operation(NdOp::INT_AND, r(56), {r(56), c(3)}),
               operation(NdOp::INT_MULT, r(56), {r(56), c(0x100)}),
               operation(NdOp::INT_ADD, r(56), {r(56), c(0x800)}),
               operation(NdOp::INDIR_BR, {}, {r(56)})});
  for (va_t Address : {0x800, 0x900, 0xa00, 0xb00})
    P.add(Address, {branch(0x100)});
  auto O = stackOptions();
  O.DiscoverControlState = true;
  O.MaxDiscoveryVisits = 256;
  const auto R = specializeInterpreter(P, {0x10}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_GT(R.ControlRefinements, 0u);
  EXPECT_LT(R.DiscoveryVisits, O.MaxDiscoveryVisits);
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{42}, UINT64_MAX}) {
    const auto Run = execute(R.Residual, Input);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input + 17);
    EXPECT_EQ(Run->Stack, 0x10000u);
  }
}

TEST(NativeStackSpecialization,
     FiniteDispatchCannotHideReachableUnsupportedArm) {
  for (bool Spilled : {false, true})
    for (bool Late : {false, true}) {
      SCOPED_TRACE(Spilled);
      SCOPED_TRACE(Late);
      auto P = finiteDispatchDecoder(Spilled, !Late, Late);
      auto O = stackOptions();
      O.DiscoverControlState = true;
      const auto R = specializeInterpreter(P, {0x100}, O);
      EXPECT_FALSE(R.complete());
      EXPECT_TRUE(R.Residual.Blocks.empty());
      EXPECT_TRUE(R.Origins.empty());
      EXPECT_TRUE(R.Reads.empty());
    }
}

TEST(NativeStackSpecialization, FiniteDispatchRefinementHonorsBudgets) {
  for (bool Spilled : {false, true}) {
    SCOPED_TRACE(Spilled);
    auto P = finiteDispatchDecoder(Spilled);
    auto O = stackOptions();
    O.DiscoverControlState = true;
    const auto Complete = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(Complete.complete()) << Complete.Diagnostic;
    ASSERT_GT(Complete.DiscoveryVisits, 0u);
    // Successful attempts can spend optional discovery work. Locate a real
    // adjacent failure/success boundary rather than subtracting from the
    // reported total and assuming all those visits were necessary.
    uint64_t FirstComplete = 0;
    for (uint64_t Limit = 1; Limit <= Complete.DiscoveryVisits; ++Limit) {
      O.MaxDiscoveryVisits = Limit;
      const auto R = specializeInterpreter(P, {0x100}, O);
      if (R.complete()) {
        FirstComplete = Limit;
        break;
      }
      EXPECT_EQ(R.Status, SpecializationStatus::BudgetExceeded);
      EXPECT_TRUE(R.Residual.Blocks.empty());
    }
    ASSERT_GT(FirstComplete, 0u);
    O.MaxDiscoveryVisits = FirstComplete - 1;
    const auto Short = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Short.Residual.Blocks.empty());
    EXPECT_TRUE(Short.Origins.empty());
    EXPECT_TRUE(Short.Reads.empty());
    O.MaxDiscoveryVisits = Complete.DiscoveryVisits;
    O.MaxControlRefinements = 0;
    const auto NoRefinement = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(NoRefinement.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(NoRefinement.Residual.Blocks.empty());
  }
}

TEST(NativeStackSpecialization, FiniteDispatchDoesNotHideOuterGuardRefinement) {
  auto P = guardedDecoder(false);
  // The outer phase guard makes this entire arm unreachable. Its inner
  // dispatch depends on an independent runtime bit and cannot exclude either
  // unsupported leaf by refining that selector alone.
  P.add(0xdead, {operation(NdOp::INT_AND, r(24), {r(8), c(1)}),
                 operation(NdOp::INT_ADD, r(24), {r(24), c(0xf000)}),
                 operation(NdOp::INDIR_BR, {}, {r(24)})});
  auto O = stackOptions();
  O.DiscoverControlState = true;
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{42}, UINT64_MAX}) {
    const auto Run = execute(R.Residual, Input);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input + 17);
    EXPECT_EQ(Run->Stack, 0x10000u);
  }
}

TEST(NativeStackSpecialization, ExhaustedGuardKeepsDeferredProducerRefinement) {
  auto P = finiteDispatchDecoder(false);
  // The next phase is one for every input, but the cancellation crosses a
  // projection. Once the phase guard's own demands are exhausted, a finite
  // producer still needs its independent input bit to retain the relation.
  P.add(0x400, {operation(NdOp::INT_AND, r(16), {r(8), c(1)}), branch(0x450)});
  P.add(0x450, {operation(NdOp::INT_AND, r(56), {r(8), c(1)}),
                operation(NdOp::INT_XOR, r(16), {r(16), r(56)}),
                operation(NdOp::INT_ADD, r(16), {r(16), c(1)}), branch(0x200)});
  auto O = stackOptions();
  O.ControlRegisters = {{16, 8}};
  O.DiscoverControlState = true;
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{42}, UINT64_MAX}) {
    const auto Run = execute(R.Residual, Input);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input + 17);
    EXPECT_EQ(Run->Stack, 0x10000u);
  }
}

TEST(NativeStackSpecialization, DeferredProducerFitsRefinementBudget) {
  auto P = finiteDispatchDecoder(false);
  // The first failure needs the phase field: without the mask its target
  // domain is not finite. A deferred producer then needs the input bit across
  // the XOR to retain the cancellation. Both refinements must fit the budget
  // without spending another retry on optional finite-guard collection.
  P.add(0x201, {operation(NdOp::INT_MULT, r(24), {r(16), c(0x100)}),
                operation(NdOp::INT_ADD, r(24), {r(24), c(0x400)}),
                operation(NdOp::INDIR_BR, {}, {r(24)})});
  P.add(0x400, {operation(NdOp::INT_AND, r(16), {r(8), c(1)}), branch(0x450)});
  P.add(0x450, {operation(NdOp::INT_AND, r(56), {r(8), c(1)}),
                operation(NdOp::INT_XOR, r(16), {r(16), r(56)}),
                operation(NdOp::INT_ADD, r(16), {r(16), c(1)}), branch(0x200)});
  auto O = stackOptions();
  O.DiscoverControlState = true;
  O.MaxControlRefinements = 2;
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{42}, UINT64_MAX}) {
    const auto Run = execute(R.Residual, Input);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input + 17);
    EXPECT_EQ(Run->Stack, 0x10000u);
  }
}

TEST(NativeStackSpecialization,
     RefinesGuardsBeforeRejectingAnUnresolvedTarget) {
  auto P = guardedDecoder(false);
  P.add(0xdead, {operation(NdOp::INDIR_BR, {}, {r(96)})});
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GT(Result.ControlRefinements, 0u);
  for (uint64_t Input : {uint64_t{0}, uint64_t{42}, UINT64_MAX}) {
    const auto Run = execute(Result.Residual, Input);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input + 17);
  }
}

StackProvider guardedFrameRelation(uint64_t Offset = 3,
                                   bool NarrowGuard = false) {
  StackProvider P;
  const uint16_t GuardBytes = NarrowGuard ? 1 : 8;
  P.add(0x100,
        {operation(NdOp::INT_ADD, r(40), {r(32), r(8)}),
         operation(NdOp::INT_EQUAL, r(24, 1),
                   {r(8, GuardBytes), c(Offset, GuardBytes)}),
         operation(NdOp::COND_BR, {}, {NdVar::cst(0x200, 8), r(24, 1)})});
  P.add(0x101, {operation(NdOp::COPY, r(0), {r(8)}), ret()});
  P.add(0x200,
        {operation(NdOp::INT_ADD, r(48), {r(32), c(Offset)}),
         operation(NdOp::INT_EQUAL, r(24, 1), {r(40), r(48)}),
         operation(NdOp::COND_BR, {}, {NdVar::cst(0x300, 8), r(24, 1)})});
  P.add(0x201, {operation(NdOp::INDIR_BR, {}, {r(96)})});
  P.add(0x300, {operation(NdOp::COPY, r(0), {r(8)}), ret()});
  return P;
}

TEST(NativeStackSpecialization, GuardedFrameRelationSurvivesProjection) {
  for (bool Manual : {false, true}) {
    for (uint64_t Offset : {uint64_t{3}, UINT64_MAX}) {
      auto P = guardedFrameRelation(Offset);
      auto Options = stackOptions();
      Options.DiscoverControlState = true;
      if (Manual)
        Options.ControlRegisters = {{40, 8}};
      const auto Result = specializeInterpreter(P, {0x100}, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      for (uint64_t Root : {uint64_t{0}, uint64_t{0x10000}, UINT64_MAX})
        for (uint64_t Input :
             {uint64_t{0}, uint64_t{3}, uint64_t{42}, UINT64_MAX}) {
          const auto Run = execute(Result.Residual, Input, 0,
                                   llvm::endianness::little, Root);
          ASSERT_TRUE(Run);
          EXPECT_EQ(Run->Value, Input);
          EXPECT_EQ(Run->Stack, Root);
        }
    }
  }
}

TEST(NativeStackSpecialization, PartialGuardCannotEstablishAWholeFrameOffset) {
  auto P = guardedFrameRelation(3, true);
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

TEST(NativeStackSpecialization,
     GuardedFrameOffsetsDoNotLeakAcrossSiblingJoins) {
  for (bool DifferentOffset : {false, true}) {
    auto P = guardedFrameRelation();
    if (DifferentOffset)
      P.add(0x101,
            {operation(NdOp::INT_ADD, r(40), {r(32), c(4)}), branch(0x200)});
    else
      P.add(0x101, {branch(0x200)});
    auto Options = stackOptions();
    Options.DiscoverControlState = true;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Origins.empty());
    EXPECT_TRUE(Result.Reads.empty());
  }
}

TEST(NativeStackSpecialization, DemandedFrameProofSharesQueryLimits) {
  auto P = guardedFrameRelation();
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  const auto Baseline = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Baseline.complete()) << Baseline.Diagnostic;
  uint64_t Rejected = 1, Accepted = Baseline.SolverQueries;
  ASSERT_GT(Accepted, Rejected);
  while (Accepted - Rejected > 1) {
    auto Probe = Options;
    Probe.MaxSolverQueries = Rejected + (Accepted - Rejected) / 2;
    const auto Result = specializeInterpreter(P, {0x100}, Probe);
    EXPECT_LE(Result.SolverQueries, Probe.MaxSolverQueries);
    if (Result.complete())
      Accepted = Probe.MaxSolverQueries;
    else {
      EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
      EXPECT_TRUE(Result.Residual.Blocks.empty());
      EXPECT_TRUE(Result.Origins.empty());
      EXPECT_TRUE(Result.Reads.empty());
      Rejected = Probe.MaxSolverQueries;
    }
  }
  Options.MaxSolverQueries = Accepted;
  EXPECT_TRUE(specializeInterpreter(P, {0x100}, Options).complete());
  Options.MaxSolverQueries = Rejected;
  const auto Short = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_LE(Short.SolverQueries, Rejected);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
  EXPECT_TRUE(Short.Origins.empty());
  EXPECT_TRUE(Short.Reads.empty());
  Options = stackOptions();
  Options.DiscoverControlState = true;
  Options.MaxSolverGates = 1;
  const auto Unknown = specializeInterpreter(P, {0x100}, Options);
  EXPECT_FALSE(Unknown.complete());
  EXPECT_TRUE(Unknown.Residual.Blocks.empty());
  EXPECT_TRUE(Unknown.Origins.empty());
  EXPECT_TRUE(Unknown.Reads.empty());
}

TEST(NativeStackSpecialization, ReachableUnresolvedGuardIsNeverAssumedAway) {
  auto P = guardedDecoder(true);
  P.add(0xdead, {operation(NdOp::INDIR_BR, {}, {r(96)})});
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

TEST(NativeStackSpecialization,
     GuardCannotHideAnUnresolvedTargetAfterABackedge) {
  auto P = guardedDecoder(false);
  P.add(0x300, {operation(NdOp::COPY, r(16), {c(2)}), branch(0x200)});
  P.add(0xdead, {operation(NdOp::INDIR_BR, {}, {r(96)})});
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

TEST(NativeStackSpecialization, UnresolvedGuardRefinementHonorsBudgets) {
  auto P = guardedDecoder(false);
  P.add(0xdead, {operation(NdOp::INDIR_BR, {}, {r(96)})});
  for (bool LimitVisits : {false, true}) {
    auto Options = stackOptions();
    Options.DiscoverControlState = true;
    if (LimitVisits)
      Options.MaxDiscoveryVisits = 1;
    else
      Options.MaxControlRefinements = 0;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Origins.empty());
    EXPECT_TRUE(Result.Reads.empty());
  }
}

TEST(NativeStackSpecialization, UnresolvedGuardRefinementHasExactBudgetBounds) {
  auto P = guardedDecoder(false);
  P.add(0xdead, {operation(NdOp::INDIR_BR, {}, {r(96)})});
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  const auto Baseline = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Baseline.complete()) << Baseline.Diagnostic;
  ASSERT_GT(Baseline.DiscoveryVisits, 1u);
  ASSERT_GT(Baseline.ControlRefinements, 0u);
  // Completed graphs may spend optional discovery work after the last
  // required refinement. Find adjacent rejected/successful visit budgets;
  // total baseline consumption is not itself a minimum required budget.
  uint64_t RejectedVisits = 1;
  uint64_t AcceptedVisits = Baseline.DiscoveryVisits;
  while (AcceptedVisits - RejectedVisits > 1) {
    auto Probe = Options;
    Probe.MaxDiscoveryVisits =
        RejectedVisits + (AcceptedVisits - RejectedVisits) / 2;
    const auto Result = specializeInterpreter(P, {0x100}, Probe);
    if (Result.complete())
      AcceptedVisits = Probe.MaxDiscoveryVisits;
    else {
      ASSERT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
      EXPECT_TRUE(Result.Residual.Blocks.empty());
      EXPECT_TRUE(Result.Origins.empty());
      EXPECT_TRUE(Result.Reads.empty());
      RejectedVisits = Probe.MaxDiscoveryVisits;
    }
  }
  Options.MaxDiscoveryVisits = AcceptedVisits;
  Options.MaxControlRefinements = Baseline.ControlRefinements;
  const auto Exact = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Exact.complete()) << Exact.Diagnostic;
  EXPECT_LE(Exact.DiscoveryVisits, AcceptedVisits);
  EXPECT_EQ(Exact.ControlRefinements, Baseline.ControlRefinements);
  for (bool LimitVisits : {false, true}) {
    SCOPED_TRACE(LimitVisits ? "discovery visits" : "refinement attempts");
    auto Limited = Options;
    if (LimitVisits) {
      --Limited.MaxDiscoveryVisits;
      Limited.MaxControlRefinements = stackOptions().MaxControlRefinements;
    } else {
      --Limited.MaxControlRefinements;
      Limited.MaxDiscoveryVisits = stackOptions().MaxDiscoveryVisits;
    }
    const auto Result = specializeInterpreter(P, {0x100}, Limited);
    EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded)
        << "baseline visits=" << Baseline.DiscoveryVisits
        << " refinements=" << Baseline.ControlRefinements
        << " limited visits=" << Result.DiscoveryVisits
        << " refinements=" << Result.ControlRefinements;
    EXPECT_LE(Result.DiscoveryVisits, Limited.MaxDiscoveryVisits);
    EXPECT_LE(Result.ControlRefinements, Limited.MaxControlRefinements);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Origins.empty());
    EXPECT_TRUE(Result.Reads.empty());
  }
}

TEST(NativeStackSpecialization,
     GuardRefinementPrecedesUnrelatedFramePartitions) {
  for (uint64_t Alignment : {256, 65536}) {
    auto P = guardedDecoder(false);
    P.add(0xff, {operation(NdOp::INT_AND, r(112),
                           {r(32), c(uint64_t{0} - Alignment)}),
                 branch(0x100)});
    auto Options = stackOptions();
    Options.DiscoverControlState = true;
    Options.MaxContextsPerAddress = 512;
    const auto Baseline = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Baseline.complete()) << Baseline.Diagnostic;
    const auto Result = specializeInterpreter(P, {0xff}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_EQ(Result.ControlRefinements, Baseline.ControlRefinements);
    for (uint64_t Input : {uint64_t{0}, uint64_t{42}, UINT64_MAX}) {
      const auto Run = execute(Result.Residual, Input);
      ASSERT_TRUE(Run);
      EXPECT_EQ(Run->Value, Input + 17);
    }
  }
}

TEST(NativeStackSpecialization, ReachableUnsupportedGuardIsNeverAssumedAway) {
  auto P = guardedDecoder(true);
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, IncidentalFrameMaskCannotHideUnsupportedGuard) {
  auto P = guardedDecoder(true);
  P.add(0xff, {operation(NdOp::INT_AND, r(112), {r(32), c(uint64_t{0} - 16)}),
               branch(0x100)});
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  const auto Result = specializeInterpreter(P, {0xff}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

TEST(NativeStackSpecialization, GuardDiscoveryExhaustionPublishesNothing) {
  auto P = guardedDecoder(false);
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  Options.MaxDiscoveryVisits = 1;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_LE(Result.DiscoveryVisits, Options.MaxDiscoveryVisits);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, NarrowGuardsKeepWideCarrierCoordinates) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
    auto P = guardedDecoder(false);
    // The upper seven bytes remain unbounded business input. A manual wide
    // carrier cannot supply a constant context key; only the demanded low
    // byte can establish the two legal decoder phases.
    const auto Phase = r(16 + (Order == llvm::endianness::little ? 0 : 7), 1);
    P.add(0x100, {operation(NdOp::INT_AND, r(16), {r(8), c(~uint64_t{255})}),
                  branch(0x200)});
    P.add(0x200,
          {operation(NdOp::INT_LESS, r(24, 1), {c(1, 1), Phase}),
           operation(NdOp::COND_BR, {}, {NdVar::cst(0xdead, 8), r(24, 1)})});
    P.add(0x201,
          {operation(NdOp::INT_EQUAL, r(24, 1), {Phase, c(0, 1)}),
           operation(NdOp::COND_BR, {}, {NdVar::cst(0x300, 8), r(24, 1)})});
    P.add(0x300,
          {operation(NdOp::INT_OR, r(16), {r(16), c(1)}), branch(0x200)});
    auto Options = stackOptions();
    Options.ByteOrder = Order;
    Options.DiscoverControlState = true;
    Options.ControlRegisters.push_back({16, 8});
    auto Result = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_GT(Result.ControlRefinements, 0u);
    for (uint64_t Input : {uint64_t{0}, uint64_t{42}, UINT64_MAX}) {
      auto Run = execute(Result.Residual, Input, 0, Order);
      ASSERT_TRUE(Run);
      EXPECT_EQ(Run->Value, Input + 17);
    }
    P.add(0x300,
          {operation(NdOp::INT_OR, r(16), {r(16), c(2)}), branch(0x200)});
    auto Invalid = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Invalid.Status, SpecializationStatus::Unsupported);
    EXPECT_TRUE(Invalid.Residual.Blocks.empty());
  }
}

TEST(NativeStackSpecialization, RestoredEntryStackMayDiscardCalleeFrames) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(40), r(32)})});
  P.nativeCall(0x101, 0x200);
  // There is deliberately no instruction at the ordinary continuation.
  P.add(0x200, {operation(NdOp::INT_SUB, r(40), {r(32), c(8)}),
                operation(NdOp::LOAD, r(32), {r(40)}),
                operation(NdOp::COPY, r(0), {c(53)}), branch(0x210)});
  P.nativeReturn(0x210);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 53u);
  EXPECT_EQ(Run->Stack, 0x10000u);
}

TEST(NativeStackSpecialization, NativeReturnUsesActualStoredTarget) {
  StackProvider P;
  P.nativeCall(0x100, 0x200);
  P.add(0x200, {operation(NdOp::STORE, {}, {r(32), c(0x300)})});
  P.nativeReturn(0x201);
  P.add(0x300, {operation(NdOp::COPY, r(0), {c(71)})});
  P.nativeReturn(0x301);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 71u);
  EXPECT_EQ(Run->Stack, 0x10000u);
}

TEST(NativeStackSpecialization, UnknownStoreDoesNotPreserveInternalReturnSlot) {
  StackProvider P;
  P.nativeCall(0x100, 0x200);
  P.nativeReturn(0x101);
  P.add(0x200, {operation(NdOp::STORE, {}, {r(8), c(7)})});
  P.nativeReturn(0x201);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_FALSE(Result.complete());
  EXPECT_NE(Result.Diagnostic.find("native return"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, UncertifiedCallsStillFailClosed) {
  StackProvider P;
  P.add(0x100, {call(0x200)});
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, ExternalStoreSeparationRetainsNestedReturn) {
  StackProvider P;
  P.nativeCall(0x100, 0x200);
  P.nativeReturn(0x101);
  P.add(0x200, {operation(NdOp::STORE, {}, {r(8), c(7)}),
                operation(NdOp::LOAD, r(0), {r(8)})});
  P.nativeReturn(0x201);
  auto Options = stackOptions();
  Options.ExplicitMachineState = true;
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{-8, 8};
  Options.ExternalStoresDisjointEntryFrame = true;
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    Options.ByteOrder = Order;
    auto Result = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    auto Run = execute(Result.Residual, 0x20000, 0, Order);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, 7u);
    EXPECT_EQ(Run->Stack, 0x10000u);
    Options.MaxOperations = Result.EvaluatedOperations;
    EXPECT_TRUE(specializeInterpreter(P, {0x100}, Options).complete());
    --Options.MaxOperations;
    auto Short = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Short.Residual.Blocks.empty());
    Options.MaxOperations = 262144;
  }
  for (int64_t Begin : {-7, 0}) {
    Options.EntryFrameBounds->Begin = Begin;
    auto Partial = specializeInterpreter(P, {0x100}, Options);
    EXPECT_FALSE(Partial.complete());
    EXPECT_TRUE(Partial.Residual.Blocks.empty());
  }
}

TEST(NativeStackSpecialization, StoreSeparationDoesNotHideActualFrameWrites) {
  StackProvider P;
  P.nativeCall(0x100, 0x200);
  P.add(0x200, {operation(NdOp::STORE, {}, {r(8), c(7)}),
                operation(NdOp::STORE, {}, {r(32), c(0x300)})});
  P.nativeReturn(0x201);
  P.add(0x300, {operation(NdOp::COPY, r(0), {c(71)})});
  P.nativeReturn(0x301);
  auto Options = stackOptions();
  Options.ExplicitMachineState = true;
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{-16, 8};
  Options.ExternalStoresDisjointEntryFrame = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual, 0x20000);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 71u);
  EXPECT_EQ(Run->Stack, 0x10000u);
  P.add(0x200, {operation(NdOp::INT_ADD, r(40), {r(32), r(8)}),
                operation(NdOp::STORE, {}, {r(40), c(7)})});
  auto Unknown = specializeInterpreter(P, {0x100}, Options);
  EXPECT_FALSE(Unknown.complete());
  EXPECT_TRUE(Unknown.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, StoreSeparationRequiresExplicitFrameDomain) {
  StackProvider P;
  P.nativeReturn(0x100);
  auto Options = stackOptions();
  Options.ExternalStoresDisjointEntryFrame = true;
  EXPECT_EQ(specializeInterpreter(P, {0x100}, Options).Status,
            SpecializationStatus::InvalidInput);
  Options.ExplicitMachineState = true;
  EXPECT_EQ(specializeInterpreter(P, {0x100}, Options).Status,
            SpecializationStatus::InvalidInput);
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{-8, 8};
  Options.FrameBaseRegister->Offset = 40;
  EXPECT_EQ(specializeInterpreter(P, {0x100}, Options).Status,
            SpecializationStatus::InvalidInput);
}

TEST(NativeStackSpecialization, RecursiveStackGrowthIsBounded) {
  StackProvider P;
  P.nativeCall(0x100, 0x100);
  auto Options = stackOptions();
  Options.MaxNativeReturnSlots = 4;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_NE(Result.Diagnostic.find("return-slot context budget"),
            std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, CallNextPushAndPopRetainPhysicalValue) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(32), {r(32), c(8)}),
                operation(NdOp::STORE, {}, {r(32), c(0x101)})});
  P.add(0x101, {operation(NdOp::LOAD, r(0), {r(32)}),
                operation(NdOp::INT_ADD, r(32), {r(32), c(8)})});
  P.nativeReturn(0x102);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 0x101u);
  EXPECT_EQ(Run->Stack, 0x10000u);
}

TEST(NativeStackSpecialization, AffineFrameSpillSurvivesProjection) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(48), r(40)}), branch(0x110)});
  P.add(0x110, {operation(NdOp::LOAD, r(56), {r(48)}),
                operation(NdOp::STORE, {}, {r(56), c(29)}),
                operation(NdOp::LOAD, r(0), {r(40)})});
  P.nativeReturn(0x111);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 29u);
}

TEST(NativeStackSpecialization, PartialStoreInvalidatesWholeAffineSpill) {
  for (uint64_t Byte : {0u, 7u}) {
    StackProvider P;
    P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                  operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                  operation(NdOp::STORE, {}, {r(48), r(40)}), branch(0x110)});
    P.add(0x110, {operation(NdOp::INT_ADD, r(56), {r(48), c(Byte)}),
                  operation(NdOp::STORE, {}, {r(56), c(0, 1)}), branch(0x120)});
    P.add(0x120, {operation(NdOp::LOAD, r(56), {r(48)}),
                  operation(NdOp::STORE, {}, {r(56), c(29)}), ret()});
    auto Result = specializeInterpreter(P, {0x100}, stackOptions());
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(NativeStackSpecialization, UnknownStoreInvalidatesAffineSpill) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(48), r(40)}),
                operation(NdOp::STORE, {}, {r(8), c(0)}), branch(0x110)});
  P.add(0x110, {operation(NdOp::LOAD, r(56), {r(48)}),
                operation(NdOp::STORE, {}, {r(56), c(29)}), ret()});
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, ConditionalOverwriteInvalidatesJoinedSpill) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(48), r(40)}),
                operation(NdOp::COND_BR, {}, {NdVar::cst(0x200, 8), r(8)})});
  P.add(0x101, {operation(NdOp::STORE, {}, {r(48), c(0, 1)}), branch(0x300)});
  P.add(0x200, {branch(0x300)});
  P.add(0x300, {operation(NdOp::LOAD, r(56), {r(48)}),
                operation(NdOp::STORE, {}, {r(56), c(29)}), ret()});
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, StoreSeparationNeedsWholeAffineSpill) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(48), r(40)}),
                operation(NdOp::STORE, {}, {r(8), c(17)}), branch(0x110)});
  P.add(0x110, {operation(NdOp::LOAD, r(56), {r(48)}),
                operation(NdOp::STORE, {}, {r(56), c(29)}),
                operation(NdOp::LOAD, r(0), {r(40)})});
  P.nativeReturn(0x111);
  auto Options = stackOptions();
  Options.ExplicitMachineState = true;
  Options.ExternalStoresDisjointEntryFrame = true;
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    Options.ByteOrder = Order;
    Options.EntryFrameBounds = SpecializationEntryFrameBounds{-16, -8};
    auto Full = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Full.complete()) << Full.Diagnostic;
    auto Run = execute(Full.Residual, 0x20000, 0, Order);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, 29u);
    for (auto Bounds : {SpecializationEntryFrameBounds{-15, -8}, {-16, -9}}) {
      Options.EntryFrameBounds = Bounds;
      auto Partial = specializeInterpreter(P, {0x100}, Options);
      EXPECT_FALSE(Partial.complete());
      EXPECT_TRUE(Partial.Residual.Blocks.empty());
    }
  }
}

TEST(NativeStackSpecialization,
     StoreSeparationDoesNotRetainLateOverwrittenSpill) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(48), r(40)}),
                operation(NdOp::COND_BR, {}, {NdVar::cst(0x200, 8), r(16)})});
  P.add(0x101, {branch(0x102)});
  P.add(0x102, {branch(0x103)});
  P.add(0x103, {operation(NdOp::STORE, {}, {r(48), c(0, 1)}), branch(0x300)});
  P.add(0x200, {operation(NdOp::STORE, {}, {r(8), c(17)}), branch(0x300)});
  P.add(0x300, {operation(NdOp::LOAD, r(56), {r(48)}),
                operation(NdOp::STORE, {}, {r(56), c(29)})});
  P.nativeReturn(0x301);
  auto Options = stackOptions();
  Options.ExplicitMachineState = true;
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{-48, 8};
  Options.ExternalStoresDisjointEntryFrame = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, AdjacentStorePreservesWholeAffineSpill) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(48)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(48), r(40)}), branch(0x110)});
  P.add(0x110, {operation(NdOp::INT_ADD, r(56), {r(48), c(8)}),
                operation(NdOp::STORE, {}, {r(56), c(5)}), branch(0x120)});
  P.add(0x120, {operation(NdOp::LOAD, r(56), {r(48)}),
                operation(NdOp::STORE, {}, {r(56), c(29)}),
                operation(NdOp::LOAD, r(0), {r(40)})});
  P.nativeReturn(0x121);
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 29u);
}

TEST(NativeStackSpecialization, MismatchedNativeControlCertificateIsRejected) {
  StackProvider P;
  P.add(0x100, {branch(0x200)}, SpecializationNativeStackControl::Call);
  auto Call = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Call.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Call.Residual.Blocks.empty());
  P.nativeReturn(0x100);
  P.Code[0x100].Origin.Immediate = 8;
  auto Return = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Return.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Return.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, EntryFlagsRequireExplicitMachineState) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_ZEXT, r(0), {r(x86reg::CF, 1)}), ret()});
  auto Options = stackOptions();
  auto Ordinary = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Ordinary.Status, SpecializationStatus::Unsupported);
  Options.ExplicitMachineState = true;
  auto Machine = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Machine.complete()) << Machine.Diagnostic;
  for (uint64_t Flag : {0u, 1u}) {
    auto Run = execute(Machine.Residual, 0, Flag);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Flag);
  }
}

TEST(NativeStackSpecialization, MachinePreconditionsRequireMachineInterface) {
  StackProvider P;
  P.add(0x100, {ret()});
  for (bool Cet : {false, true}) {
    auto Options = stackOptions();
    Options.X64CetDisabled = Cet;
    Options.NormalNonfaultingExecution = !Cet;
    auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

LowOp repeatedBytes(NdVar Source, NdVar Destination, NdVar Count, NdVar DF) {
  return operation(NdOp::INTRINSIC, NdVar::tmp(0x10000, 8),
                   {c(static_cast<uint16_t>(Intrinsic::Movsb), 2), Source,
                    Destination, Count, DF});
}

TEST(NativeStackSpecialization, RepeatedCopyRetainsOverlapAndInstructionOrder) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(40)}),
                operation(NdOp::STORE, {}, {r(40), c(0x8877665544332211)}),
                operation(NdOp::INT_ADD, r(48), {r(40), c(1)}),
                operation(NdOp::COPY, r(56), {c(7)}),
                repeatedBytes(r(40), r(48), r(56), c(0, 1)),
                operation(NdOp::LOAD, r(0), {r(40)}), ret()});
  auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  auto Run = execute(Result.Residual);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 0x1111111111111111u);
  auto Exact = stackOptions();
  Exact.MaxOperations = Result.EvaluatedOperations;
  EXPECT_TRUE(specializeInterpreter(P, {0x100}, Exact).complete());
  --Exact.MaxOperations;
  const auto Short = specializeInterpreter(P, {0x100}, Exact);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
}

TEST(NativeStackSpecialization, ZeroCountKeepsUnknownPointersAndDirection) {
  StackProvider P;
  P.add(0x100, {repeatedBytes(r(40), r(48), c(0), r(x86reg::DF, 1)),
                operation(NdOp::COPY, r(0), {c(23)}), ret()});
  auto Options = stackOptions();
  EXPECT_EQ(specializeInterpreter(P, {0x100}, Options).Status,
            SpecializationStatus::Unsupported);
  Options.ExplicitMachineState = true;
  Options.MaxSolverQueries = 1;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.SolverQueries, 0u);
  for (const auto &Block : Result.Residual.Blocks)
    for (const auto &Op : Block.Ops) {
      EXPECT_NE(Op.Opcode, NdOp::LOAD);
      EXPECT_NE(Op.Opcode, NdOp::STORE);
      if (Op.Output.isReg()) {
        EXPECT_NE(Op.Output.Offset, 40u);
        EXPECT_NE(Op.Output.Offset, 48u);
        EXPECT_NE(Op.Output.Offset, x86reg::DF);
      }
    }
}

TEST(NativeStackSpecialization, RepeatedCopyProtectsEntryControlSlot) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, r(40), {r(32), c(16)}),
                operation(NdOp::STORE, {}, {r(40), c(31, 1)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(1)}),
                repeatedBytes(r(40), r(48), c(2), c(0, 1)), ret()});
  const auto Result = specializeInterpreter(P, {0x100}, stackOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_NE(Result.Diagnostic.find("entry return control slot"),
            std::string::npos);
}

TEST(NativeStackSpecialization, RepeatedCopyReprovesWholePointerAcrossNodes) {
  for (unsigned Kind : {0, 1, 2, 3}) {
    const bool Backward = Kind == 1, Clobber = Kind == 2, Overlap = Kind == 3;
    StackProvider P;
    P.add(0x100,
          {operation(NdOp::INT_SUB, r(40), {r(32), c(64)}),
           operation(NdOp::INT_SUB, r(48), {r(32), c(Overlap ? 63 : 48)}),
           operation(NdOp::INT_SUB, r(56), {r(32), c(16)}),
           operation(NdOp::STORE, {}, {r(40), r(56)}),
           operation(NdOp::INT_ADD, r(64), {r(40), c(Backward ? 7 : 0)}),
           operation(NdOp::INT_ADD, r(72), {r(48), c(Backward ? 7 : 0)}),
           repeatedBytes(r(64), r(72), c(8), c(Backward, 1)),
           Clobber ? operation(NdOp::STORE, {}, {r(48), c(0, 1)})
                   : operation(NdOp::NOP, {}, {}),
           branch(0x110)});
    P.add(0x110, {operation(NdOp::LOAD, r(56), {r(48)}),
                  operation(NdOp::STORE, {}, {r(56), c(47)}),
                  operation(NdOp::LOAD, r(0), {r(56)}), ret()});
    const auto Result = specializeInterpreter(P, {0x100}, stackOptions());
    if (Clobber || Overlap) {
      EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
      EXPECT_TRUE(Result.Residual.Blocks.empty());
    } else {
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      const auto Run = execute(Result.Residual);
      ASSERT_TRUE(Run);
      EXPECT_EQ(Run->Value, 47u);
    }
  }
}

TEST(NativeStackSpecialization, AlignedCopiesDoNotEnumerateFreeStackAddresses) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::COPY, r(72), {r(32)}),
                operation(NdOp::INT_AND, r(32), {r(32), c(uint64_t{0} - 16)}),
                operation(NdOp::INT_SUB, r(40), {r(32), c(64)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(32)}),
                repeatedBytes(r(40), r(48), c(13), c(0, 1)),
                operation(NdOp::COPY, r(32), {r(72)}),
                operation(NdOp::COPY, r(0), {c(19)}), ret()});
  auto Options = stackOptions();
  Options.MaxSolverQueries = 512;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_GT(Result.ControlRefinements, 0u);
  EXPECT_TRUE(Result.Reads.empty());
  EXPECT_LT(Result.SolverQueries, 512u);
}

TEST(NativeStackSpecialization, RepeatedCopyRequiresCompleteControlValues) {
  for (bool UnknownCount : {false, true}) {
    StackProvider P;
    P.add(0x100, {repeatedBytes(r(40), r(48), UnknownCount ? r(56) : c(3),
                                UnknownCount ? c(0, 1) : r(x86reg::DF, 1)),
                  ret()});
    auto Options = stackOptions();
    Options.ExplicitMachineState = true;
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(NativeStackSpecialization, LowRootGuardLeavesCopiedAddressesUnbounded) {
  StackProvider P;
  P.add(0x100, {operation(NdOp::INT_EQUAL, r(96, 1), {r(32, 1), c(5, 1)}),
                operation(NdOp::COND_BR, {}, {c(0x120), r(96, 1)})});
  P.add(0x101, {operation(NdOp::COPY, r(0), {c(7)}), ret()});
  P.add(0x120, {operation(NdOp::INT_SUB, r(40), {r(32), c(64)}),
                operation(NdOp::STORE, {}, {r(40), c(0x1717171717171717)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(32)}),
                repeatedBytes(r(40), r(48), c(23), c(0, 1)),
                operation(NdOp::LOAD, r(0), {r(48)}), ret()});
  auto Options = stackOptions();
  Options.ControlRegisters = {{32, 1}};
  Options.MaxSolverQueries = 128;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_TRUE(Result.Reads.empty());
  EXPECT_LT(Result.SolverQueries, 128u);
  for (uint64_t High : {uint64_t{0x10000}, uint64_t{0x123400010000}})
    for (uint64_t Low : {5u, 9u}) {
      const auto Run =
          execute(Result.Residual, 0, 0, llvm::endianness::little, High + Low);
      ASSERT_TRUE(Run);
      EXPECT_EQ(Run->Value, Low == 5 ? 0x1717171717171717u : 7u);
      EXPECT_EQ(Run->Stack, High + Low);
    }
}

TEST(NativeStackSpecialization,
     NonwrappingBoundsKeepGuardedFrameReadsUnbounded) {
  class CountingProvider : public StackProvider {
  public:
    unsigned ReadCalls = 0;
    std::optional<SpecializationImmutableRead>
    immutableRead(va_t, uint16_t) override {
      ++ReadCalls;
      return std::nullopt;
    }
  } P;
  P.add(0x100, {operation(NdOp::INT_EQUAL, r(96, 1), {r(32, 1), c(5, 1)}),
                operation(NdOp::COND_BR, {}, {c(0x120), r(96, 1)})});
  P.add(0x101, {operation(NdOp::COPY, r(0), {c(7)}), ret()});
  P.add(0x120, {operation(NdOp::INT_SUB, r(40), {r(32), c(64)}),
                operation(NdOp::STORE, {}, {r(40), r(8)}),
                operation(NdOp::INT_SUB, r(48), {r(32), c(32)}),
                repeatedBytes(r(40), r(48), c(23), c(0, 1)),
                operation(NdOp::LOAD, r(0), {r(48)}), ret()});
  auto Options = stackOptions();
  Options.ExplicitMachineState = true;
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{-64, 8};
  Options.ControlRegisters = {{32, 1}};
  Options.MaxSolverQueries = 512;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(P.ReadCalls, 0U);
  EXPECT_TRUE(Result.Reads.empty());
  unsigned Loads = 0;
  for (const auto &B : Result.Residual.Blocks)
    for (const auto &Op : B.Ops)
      Loads += Op.Opcode == NdOp::LOAD;
  EXPECT_GT(Loads, 0U);
  for (uint64_t High : {uint64_t{0x10000}, uint64_t{0x123400010000}})
    for (uint64_t Low : {5U, 9U})
      for (uint64_t Value : {uint64_t{0}, uint64_t{0x123456789abcdef0}}) {
        auto Run = execute(Result.Residual, Value, 0, llvm::endianness::little,
                           High + Low);
        ASSERT_TRUE(Run);
        EXPECT_EQ(Run->Value, Low == 5 ? Value : 7U);
        EXPECT_EQ(Run->Stack, High + Low);
      }
}

class TargetPointerProvider : public StackProvider {
public:
  llvm::endianness Order = llvm::endianness::little;
  std::optional<va_t> MissingEvidence;
  std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Bytes) override {
    if (MissingEvidence == Address || Bytes != 8 ||
        (Address != 0xa000 && Address != 0xa010))
      return std::nullopt;
    const uint64_t Target = Address == 0xa000 ? 0x300 : 0x400;
    SpecializationImmutableRead Read;
    Read.Evidence = "independent immutable target table";
    for (unsigned I = 0; I < 8; ++I)
      Read.Bytes.push_back(
          Target >> (8 * (Order == llvm::endianness::little ? I : 7 - I)));
    return Read;
  }
};

TargetPointerProvider targetPointerCarrier(bool NativeReturn, bool Frame) {
  TargetPointerProvider P;
  P.add(0x100,
        {operation(NdOp::INT_SUB, r(40), {r(32), c(32)}),
         operation(NdOp::INT_SUB, r(32), {r(32), c(NativeReturn ? 8 : 0)}),
         operation(NdOp::INT_AND, r(16), {r(8), c(1)}),
         operation(NdOp::COND_BR, {}, {c(0x120), r(16)})});
  for (unsigned Case = 0; Case < 2; ++Case) {
    const auto Set =
        Frame ? operation(NdOp::STORE, {}, {r(40), c(0xa000 + Case * 16)})
              : operation(NdOp::COPY, r(48), {c(0xa000 + Case * 16)});
    P.add(Case ? 0x120 : 0x101, {Set, branch(0x200)});
  }
  if (Frame)
    P.add(0x200, {operation(NdOp::LOAD, r(48), {r(40)}), branch(0x210)});
  else
    P.add(0x200, {branch(0x210)});
  if (NativeReturn) {
    P.add(0x210, {operation(NdOp::LOAD, r(56), {r(48)}),
                  operation(NdOp::STORE, {}, {r(32), r(56)})});
    P.nativeReturn(0x211);
  } else {
    P.add(0x210, {operation(NdOp::LOAD, r(56), {r(48)}),
                  operation(NdOp::INDIR_BR, {}, {r(56)})});
  }
  P.add(0x300, {operation(NdOp::INT_ADD, r(0), {r(8), c(5)})});
  P.nativeReturn(0x301);
  P.add(0x400, {operation(NdOp::INT_XOR, r(0), {r(8), c(0x91)})});
  P.nativeReturn(0x401);
  return P;
}

SpecializationOptions targetPointerOptions() {
  auto Options = stackOptions();
  Options.DiscoverControlState = true;
  Options.MaxControlTuples = 1;
  Options.MaxImmutableReadAddresses = 1;
  Options.MaxOperations = 1 << 20;
  return Options;
}

TEST(NativeStackSpecialization,
     TargetContextsRetainFiniteImagePointerCarriers) {
  for (bool NativeReturn : {false, true})
    for (bool Frame : {false, true})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
        SCOPED_TRACE(::testing::Message()
                     << NativeReturn << ":" << Frame << ":" << int(Order));
        auto P = targetPointerCarrier(NativeReturn, Frame);
        P.Order = Order;
        auto Options = targetPointerOptions();
        Options.ByteOrder = Order;
        const auto Result = specializeInterpreter(P, {0x100}, Options);
        ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
        EXPECT_GT(Result.DiscoveredContextFields, 0U);
        for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{2},
                               UINT64_MAX, uint64_t{0x1234567800000045}})
          for (uint64_t Stack : {uint64_t{0x10000}, uint64_t{0x123456781000}}) {
            const auto Run =
                execute(Result.Residual, Input, 0, Order, Stack, {-40, -24, 0});
            ASSERT_TRUE(Run);
            EXPECT_EQ(Run->Value, Input & 1 ? Input ^ 0x91 : Input + 5);
            EXPECT_EQ(Run->Stack, Stack);
            EXPECT_EQ(Run->Memory, (std::vector<uint8_t>{65, 66, 67}));
          }
      }
}

TEST(NativeStackSpecialization, TargetContextsRequireEveryImmutableRead) {
  for (bool NativeReturn : {false, true})
    for (bool Frame : {false, true}) {
      auto P = targetPointerCarrier(NativeReturn, Frame);
      // The other table address still supplies a valid read witness. A
      // missing certificate cannot publish that earlier successful path.
      P.MissingEvidence = 0xa010;
      const auto Result =
          specializeInterpreter(P, {0x100}, targetPointerOptions());
      EXPECT_FALSE(Result.complete());
      EXPECT_TRUE(Result.Residual.Blocks.empty());
      EXPECT_TRUE(Result.Origins.empty());
      EXPECT_TRUE(Result.Reads.empty());
    }
}

TEST(NativeStackSpecialization, TargetContextsRetainLateUnknownPredecessors) {
  for (bool NativeReturn : {false, true})
    for (bool Frame : {false, true}) {
      auto P = targetPointerCarrier(NativeReturn, Frame);
      P.add(0x80, {operation(NdOp::INT_AND, r(96), {r(8), c(2)}),
                   operation(NdOp::COND_BR, {}, {c(0x100), r(96)})});
      P.add(0x81,
            {operation(NdOp::INT_SUB, r(40), {r(32), c(32)}),
             operation(NdOp::INT_SUB, r(32), {r(32), c(NativeReturn ? 8 : 0)}),
             branch(0x90)});
      const auto Set = Frame ? operation(NdOp::STORE, {}, {r(40), r(8)})
                             : operation(NdOp::COPY, r(48), {r(8)});
      P.add(0x90, {Set, branch(0x200)});
      const auto Result =
          specializeInterpreter(P, {0x80}, targetPointerOptions());
      EXPECT_FALSE(Result.complete());
      EXPECT_TRUE(Result.Residual.Blocks.empty());
      EXPECT_TRUE(Result.Origins.empty());
      EXPECT_TRUE(Result.Reads.empty());
    }
}

TEST(NativeStackSpecialization, TargetContextsDoNotCompletePartialCarriers) {
  for (bool NativeReturn : {false, true}) {
    auto P = targetPointerCarrier(NativeReturn, false);
    for (unsigned Case = 0; Case < 2; ++Case)
      P.add(Case ? 0x120 : 0x101,
            {operation(NdOp::INT_AND, r(48), {r(8), c(~uint64_t{1})}),
             operation(NdOp::INT_OR, r(48), {r(48), c(Case)}), branch(0x200)});
    P.add(0x200,
          {operation(NdOp::INT_AND, r(48), {r(48), c(1)}),
           operation(NdOp::INT_LEFT, r(48), {r(48), c(4)}),
           operation(NdOp::INT_ADD, r(48), {r(48), c(0xa000)}), branch(0x210)});
    auto Options = targetPointerOptions();
    // Only bit zero is needed, but the rest of its byte remains dynamic.
    // A nominated byte cannot acquire a complete context value from a model.
    const auto Limited = specializeInterpreter(P, {0x100}, Options);
    EXPECT_FALSE(Limited.complete());
    EXPECT_TRUE(Limited.Residual.Blocks.empty());
    Options.MaxControlTuples = 2;
    Options.MaxImmutableReadAddresses = 2;
    const auto Complete = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Complete.complete()) << Complete.Diagnostic;
    for (uint64_t Input :
         {uint64_t{0}, uint64_t{1}, uint64_t{0x2468ace02468ace0}, UINT64_MAX}) {
      const auto Run = execute(Complete.Residual, Input);
      ASSERT_TRUE(Run);
      EXPECT_EQ(Run->Value, Input & 1 ? Input ^ 0x91 : Input + 5);
      EXPECT_EQ(Run->Stack, 0x10000U);
    }
  }
}

TEST(NativeStackSpecialization, TargetContextsShareRecoveryBudgets) {
  auto P = targetPointerCarrier(true, true);
  const auto Good = specializeInterpreter(P, {0x100}, targetPointerOptions());
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  // Optional producer discovery can stop before using all of the successful
  // run's visits. Find an actual adjacent refusal boundary instead of treating
  // every optional visit as necessary to certify this particular graph.
  uint64_t DiscoveryLimit = Good.DiscoveryVisits;
  while (DiscoveryLimit > 1) {
    auto Options = targetPointerOptions();
    Options.MaxDiscoveryVisits = DiscoveryLimit - 1;
    if (!specializeInterpreter(P, {0x100}, Options).complete())
      break;
    --DiscoveryLimit;
  }
  for (unsigned Kind = 0; Kind < 6; ++Kind) {
    auto Options = targetPointerOptions();
    switch (Kind) {
    case 0:
      Options.MaxContextsPerAddress = 2;
      break;
    case 1:
      Options.MaxControlRefinements = Good.ControlRefinements;
      break;
    case 2:
      Options.MaxDiscoveryVisits = DiscoveryLimit;
      break;
    case 3:
      Options.MaxOperations = Good.EvaluatedOperations;
      break;
    case 4:
      Options.MaxNodeEvaluations = Good.NodeEvaluations;
      break;
    case 5:
      Options.MaxSolverQueries = Good.SolverQueries;
      break;
    }
    const auto Exact = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Exact.complete()) << Kind << ": " << Exact.Diagnostic;
    switch (Kind) {
    case 0:
      --Options.MaxContextsPerAddress;
      break;
    case 1:
      --Options.MaxControlRefinements;
      break;
    case 2:
      --Options.MaxDiscoveryVisits;
      break;
    case 3:
      --Options.MaxOperations;
      break;
    case 4:
      --Options.MaxNodeEvaluations;
      break;
    case 5:
      --Options.MaxSolverQueries;
      break;
    }
    const auto Limited = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Limited.Status, SpecializationStatus::BudgetExceeded)
        << Kind << ": " << Limited.Diagnostic;
    EXPECT_TRUE(Limited.Residual.Blocks.empty());
    EXPECT_TRUE(Limited.Origins.empty());
    EXPECT_TRUE(Limited.Reads.empty());
  }
}

} // namespace
