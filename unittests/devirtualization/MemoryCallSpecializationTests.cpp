//===- MemoryCallSpecializationTests.cpp - Ordered native call targets
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

namespace {

constexpr uint64_t StackBase = 0x10000;

NdVar reg(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar temp(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::tmp(Offset, Bytes);
}
NdVar constant(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp operation(NdOp Opcode, NdVar Output,
                std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const NdVar &Input : Inputs)
    Op.addInput(Input);
  return Op;
}
LowOp indirectCall(NdVar Target) {
  return operation(NdOp::INDIR_CALL, reg(0), {Target});
}

class MemoryCallProvider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;

  void add(va_t Address, std::vector<LowOp> Ops,
           SpecializationNativeStackControl Native =
               SpecializationNativeStackControl::None) {
    SpecializationInstruction Insn;
    Insn.Ops = std::move(Ops);
    Insn.Origin.Address = Address;
    Insn.Origin.Size = 1;
    Insn.Origin.OpCount = Insn.Ops.size();
    Insn.Fallthrough = {Address + 1};
    Insn.NativeStackControl = Native;
    for (size_t I = 0; I < Insn.Ops.size(); ++I) {
      LowOp &Op = Insn.Ops[I];
      Op.Addr = Address;
      Op.Seq = static_cast<int>(I);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        Insn.Origin.Control = LowInstructionControl::Call;
        Insn.Origin.ControlFlags = LowInstructionControlFlag::Call;
        if (Op.Opcode == NdOp::INDIR_CALL) {
          Insn.Origin.ControlFlags |= LowInstructionControlFlag::Indirect;
          Insn.Origin.Immediate.reset();
        } else
          Insn.Origin.Immediate = Op.Inputs[0].Offset;
      } else if (Op.Opcode == NdOp::RETURN) {
        Insn.Origin.Control = LowInstructionControl::Return;
        Insn.Origin.ControlFlags = LowInstructionControlFlag::Return;
      }
    }
    Code[Address] = std::move(Insn);
  }

  void call(va_t Address, std::vector<LowOp> Ops) {
    add(Address, std::move(Ops), SpecializationNativeStackControl::Call);
  }

  void ret(va_t Address) {
    add(Address, {operation(NdOp::RETURN, {}, {reg(0)})},
        SpecializationNativeStackControl::Return);
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto At = Code.find(Cursor.Address);
    if (At == Code.end())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "unknown synthetic call destination");
    return At->second;
  }
};

SpecializationOptions options() {
  SpecializationOptions Options;
  Options.ExplicitMachineState = true;
  Options.NormalNonfaultingExecution = true;
  Options.X64CetDisabled = true;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.RequireRestoredFrameAtReturn = true;
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  return Options;
}

struct Execution {
  uint64_t Value;
  uint64_t Stack;
  uint64_t CalleeReturnAddress;
  uint64_t CalleeCarry;
  uint64_t PushedWord;
};

std::optional<Execution> execute(const LowFunc &Function, uint64_t Input,
                                 uint64_t Carry) {
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, 0, Ctx.mkConst(64, 0));
  State.write(SymSpace::Register, 8, Ctx.mkConst(64, Input));
  State.write(SymSpace::Register, 16, Ctx.mkConst(64, 0));
  State.write(SymSpace::Register, 24, Ctx.mkConst(64, 0));
  State.write(SymSpace::Register, 32, Ctx.mkConst(64, StackBase));
  State.write(SymSpace::Register, x86reg::CF, Ctx.mkConst(8, Carry));
  va_t Address = Function.Entry;
  for (unsigned Step = 0; Step < 1000; ++Step) {
    const auto Block =
        std::find_if(Function.Blocks.begin(), Function.Blocks.end(),
                     [&](const LowBlock &B) { return B.StartAddr == Address; });
    if (Block == Function.Blocks.end())
      return std::nullopt;
    SymExec Exec(Ctx, State);
    bool Transferred = false;
    for (const LowOp &Op : Block->Ops) {
      const auto Flow = Exec.step(Op);
      if (Flow == StepResult::Continue)
        continue;
      if (Flow == StepResult::Return) {
        const auto Value = Ctx.asConst(State.read(SymSpace::Register, 0, 8));
        const auto Stack = Ctx.asConst(State.read(SymSpace::Register, 32, 8));
        const auto Return = Ctx.asConst(State.read(SymSpace::Register, 16, 8));
        const auto Flag = Ctx.asConst(State.read(SymSpace::Register, 24, 8));
        const auto Pushed =
            Ctx.asConst(State.load(Ctx.mkConst(64, StackBase - 8), 8));
        if (!Value || !Stack || !Return || !Flag || !Pushed)
          return std::nullopt;
        return Execution{Value->getZExtValue(), Stack->getZExtValue(),
                         Return->getZExtValue(), Flag->getZExtValue(),
                         Pushed->getZExtValue()};
      }
      if (Flow == StepResult::CondBranch) {
        const auto Condition = Ctx.asConst(Exec.branchCondition());
        if (!Condition || Block->Succs.size() != 2)
          return std::nullopt;
        const int Next = Block->Succs[Condition->isZero() ? 1 : 0];
        const auto Target =
            std::find_if(Function.Blocks.begin(), Function.Blocks.end(),
                         [&](const LowBlock &B) { return B.Id == Next; });
        if (Target == Function.Blocks.end())
          return std::nullopt;
        Address = Target->StartAddr;
      } else if (Flow == StepResult::Branch) {
        const auto Target = Ctx.asConst(Exec.branchTarget());
        if (!Target)
          return std::nullopt;
        Address = Target->getZExtValue();
      } else {
        return std::nullopt;
      }
      Transferred = true;
      break;
    }
    if (!Transferred)
      return std::nullopt;
  }
  return std::nullopt;
}

void expectNoPublication(const SpecializationResult &Result) {
  EXPECT_FALSE(Result.complete());
  EXPECT_FALSE(Result.Diagnostic.empty());
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

void addCallee(MemoryCallProvider &P, va_t Address, NdOp Opcode,
               NdVar Operand) {
  P.add(Address, {operation(NdOp::LOAD, reg(16), {reg(32)}),
                  operation(NdOp::INT_ZEXT, reg(24), {reg(x86reg::CF, 1)}),
                  operation(Opcode, reg(0), {reg(0), Operand})});
  P.ret(Address + 1);
}

MemoryCallProvider overwrittenTargetSlot() {
  MemoryCallProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, reg(40), {reg(32), constant(8)}),
                operation(NdOp::STORE, {}, {reg(40), constant(0x200)}),
                operation(NdOp::COPY, reg(0), {constant(17)})});
  P.call(0x101,
         {operation(NdOp::INT_SUB, temp(0), {reg(32), constant(8)}),
          operation(NdOp::LOAD, temp(8), {temp(0)}), indirectCall(temp(8))});
  P.ret(0x102);
  addCallee(P, 0x200, NdOp::INT_ADD, reg(8));
  return P;
}

TEST(MemoryCallSpecialization, TargetLoadPrecedesPushToTheSameGuestSlot) {
  auto P = overwrittenTargetSlot();
  auto Result = specializeInterpreter(P, {0x100}, options());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{19}, UINT64_MAX})
    for (uint64_t Carry : {0u, 1u}) {
      auto Run = execute(Result.Residual, Input, Carry);
      ASSERT_TRUE(Run);
      // The call must read the old target before overwriting it with 0x102.
      // The callee observes the new word and the original RAX and carry flag.
      EXPECT_EQ(Run->Value, Input + 17);
      EXPECT_EQ(Run->Stack, StackBase);
      EXPECT_EQ(Run->CalleeReturnAddress, 0x102u);
      EXPECT_EQ(Run->PushedWord, 0x102u);
      EXPECT_EQ(Run->CalleeCarry, Carry);
    }
}

MemoryCallProvider finiteFrameTargets() {
  MemoryCallProvider P;
  P.add(0x100, {operation(NdOp::INT_SUB, reg(40), {reg(32), constant(24)}),
                operation(NdOp::INT_AND, temp(0), {reg(8), constant(1)}),
                operation(NdOp::SELECT, temp(8),
                          {temp(0), constant(0x200), constant(0x300)}),
                operation(NdOp::STORE, {}, {reg(40), temp(8)}),
                operation(NdOp::COPY, reg(0), {reg(8)})});
  P.call(0x101,
         {operation(NdOp::INT_SUB, temp(0), {reg(32), constant(24)}),
          operation(NdOp::LOAD, temp(8), {temp(0)}), indirectCall(temp(8))});
  P.ret(0x102);
  addCallee(P, 0x200, NdOp::INT_ADD, constant(17));
  addCallee(P, 0x300, NdOp::INT_XOR, constant(0x1234));
  return P;
}

TEST(MemoryCallSpecialization, InitializedFrameSlotSelectsBothFiniteCallees) {
  auto P = finiteFrameTargets();
  auto Result = specializeInterpreter(P, {0x100}, options());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{42}, UINT64_MAX}) {
    auto Run = execute(Result.Residual, Input, 1);
    ASSERT_TRUE(Run);
    EXPECT_EQ(Run->Value, Input & 1 ? Input + 17 : Input ^ 0x1234);
    EXPECT_EQ(Run->Stack, StackBase);
    EXPECT_EQ(Run->CalleeReturnAddress, 0x102u);
    EXPECT_EQ(Run->PushedWord, 0x102u);
    EXPECT_EQ(Run->CalleeCarry, 1u);
  }
  for (const auto &Block : Result.Residual.Blocks)
    for (const auto &Op : Block.Ops) {
      EXPECT_NE(Op.Opcode, NdOp::CALL);
      EXPECT_NE(Op.Opcode, NdOp::INDIR_CALL);
      EXPECT_NE(Op.Opcode, NdOp::INDIR_BR);
    }
}

TEST(MemoryCallSpecialization, UndefinedTargetTemporaryIsNotAnEntryInput) {
  for (bool PreviousInstructionDefinition : {false, true}) {
    SCOPED_TRACE(PreviousInstructionDefinition);
    MemoryCallProvider P;
    P.add(0x100, {operation(NdOp::COPY,
                            PreviousInstructionDefinition ? temp(8) : reg(0),
                            {constant(0x200)})});
    P.call(0x101, {indirectCall(temp(8))});
    P.ret(0x102);
    addCallee(P, 0x200, NdOp::INT_ADD, constant(17));
    expectNoPublication(specializeInterpreter(P, {0x100}, options()));
  }
}

TEST(MemoryCallSpecialization, PartiallyDefinedTargetTemporaryIsRejected) {
  MemoryCallProvider P;
  P.call(0x100, {operation(NdOp::COPY, temp(8, 4), {constant(0x200, 4)}),
                 indirectCall(temp(8))});
  P.ret(0x101);
  addCallee(P, 0x200, NdOp::INT_ADD, constant(17));
  expectNoPublication(specializeInterpreter(P, {0x100}, options()));
}

TEST(MemoryCallSpecialization, UndefinedAddressTemporaryIsRejected) {
  for (bool PartiallyDefined : {false, true}) {
    SCOPED_TRACE(PartiallyDefined);
    MemoryCallProvider P;
    std::vector<LowOp> Prefix;
    if (PartiallyDefined)
      Prefix.push_back(operation(NdOp::COPY, temp(0, 4), {constant(0x200, 4)}));
    Prefix.push_back(operation(NdOp::LOAD, temp(8), {temp(0)}));
    Prefix.push_back(indirectCall(temp(8)));
    P.call(0x100, std::move(Prefix));
    const auto Result = specializeInterpreter(P, {0x100}, options());
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
    EXPECT_NE(Result.Diagnostic.find("unbound temporary"), std::string::npos);
    expectNoPublication(Result);
  }
}

TEST(MemoryCallSpecialization, ConstantSlotOperandIsNotAnExplicitTargetLoad) {
  MemoryCallProvider P;
  // Even an existing code address does not turn the legacy slot spelling
  // into a direct-value call certificate.
  P.call(0x100, {indirectCall(constant(0x200))});
  P.ret(0x101);
  addCallee(P, 0x200, NdOp::INT_ADD, constant(17));
  const auto Result = specializeInterpreter(P, {0x100}, options());
  EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
  expectNoPublication(Result);
}

TEST(MemoryCallSpecialization, PrefixCannotWriteAPhysicalRegisterOrMemory) {
  for (const auto &Prefix :
       {operation(NdOp::COPY, reg(16), {constant(0x200)}),
        operation(NdOp::COPY, reg(x86reg::CF, 1), {constant(1, 1)}),
        operation(NdOp::STORE, {}, {reg(48), constant(29)})}) {
    SCOPED_TRACE(static_cast<unsigned>(Prefix.Opcode));
    MemoryCallProvider P;
    P.add(0x100, {operation(NdOp::INT_SUB, reg(40), {reg(32), constant(24)}),
                  operation(NdOp::INT_SUB, reg(48), {reg(32), constant(32)}),
                  operation(NdOp::STORE, {}, {reg(40), constant(0x200)})});
    // The extra STORE writes a different valid slot, so allowing that effect
    // would not fail later merely because it corrupted the call target.
    P.call(0x101, {Prefix, operation(NdOp::LOAD, temp(8), {reg(40)}),
                   indirectCall(temp(8))});
    P.ret(0x102);
    addCallee(P, 0x200, NdOp::INT_ADD, constant(17));
    expectNoPublication(specializeInterpreter(P, {0x100}, options()));
  }
}

TEST(MemoryCallSpecialization, PrefixCannotContainAnotherControlTransfer) {
  for (const auto &Prefix :
       {operation(NdOp::CALL, reg(0), {NdVar::cst(0x200, 8)}),
        operation(NdOp::BRANCH, {}, {NdVar::cst(0x200, 8)}),
        operation(NdOp::COND_BR, {}, {NdVar::cst(0x200, 8), reg(8)}),
        operation(NdOp::RETURN, {}, {reg(0)})}) {
    SCOPED_TRACE(static_cast<unsigned>(Prefix.Opcode));
    MemoryCallProvider P;
    P.add(0x100, {operation(NdOp::INT_SUB, reg(40), {reg(32), constant(24)}),
                  operation(NdOp::STORE, {}, {reg(40), constant(0x200)})});
    P.call(0x101, {Prefix, operation(NdOp::LOAD, temp(8), {reg(40)}),
                   indirectCall(temp(8))});
    P.ret(0x102);
    addCallee(P, 0x200, NdOp::INT_ADD, constant(17));
    expectNoPublication(specializeInterpreter(P, {0x100}, options()));
  }
}

TEST(MemoryCallSpecialization, SegmentedTargetLoadsAreRefused) {
  for (auto Space :
       {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS}) {
    SCOPED_TRACE(static_cast<unsigned>(Space));
    auto P = overwrittenTargetSlot();
    P.Code.at(0x101).Ops[1].MemoryAddressSpace = Space;
    expectNoPublication(specializeInterpreter(P, {0x100}, options()));
  }
}

TEST(MemoryCallSpecialization, OrderedTargetLoadsAreRefused) {
  for (auto Order :
       {NdMemoryOrdering::Acquire, NdMemoryOrdering::SequentiallyConsistent}) {
    SCOPED_TRACE(static_cast<unsigned>(Order));
    auto P = overwrittenTargetSlot();
    P.Code.at(0x101).Ops[1].MemoryOrdering = Order;
    expectNoPublication(specializeInterpreter(P, {0x100}, options()));
  }
}

TEST(MemoryCallSpecialization, PrefixCannotForgeReservedDispatchTemporaries) {
  constexpr uint64_t ReservedBegin = uint64_t{1} << 62;
  for (uint64_t Offset :
       {ReservedBegin, ReservedBegin + 32, ReservedBegin - 4}) {
    SCOPED_TRACE(Offset);
    auto P = overwrittenTargetSlot();
    // Include the exact captured-call scratch and an eight-byte temporary
    // whose first four bytes are legal but whose tail overlaps the reserve.
    P.call(0x101, {operation(NdOp::INT_SUB, temp(0), {reg(32), constant(8)}),
                   operation(NdOp::LOAD, temp(Offset), {temp(0)}),
                   indirectCall(temp(Offset))});
    const auto Result = specializeInterpreter(P, {0x100}, options());
    EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
    EXPECT_NE(Result.Diagnostic.find("reserved dispatch range"),
              std::string::npos);
    expectNoPublication(Result);
  }
}

TEST(MemoryCallSpecialization,
     LastLegalTemporaryCanBeBothLoadAddressAndResult) {
  constexpr uint64_t LastTemporary = (uint64_t{1} << 62) - 8;
  auto P = overwrittenTargetSlot();
  P.call(0x101,
         {operation(NdOp::INT_SUB, temp(LastTemporary), {reg(32), constant(8)}),
          operation(NdOp::LOAD, temp(LastTemporary), {temp(LastTemporary)}),
          indirectCall(temp(LastTemporary))});
  const auto Result = specializeInterpreter(P, {0x100}, options());
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  const auto Run = execute(Result.Residual, 9, 1);
  ASSERT_TRUE(Run);
  EXPECT_EQ(Run->Value, 26u);
  EXPECT_EQ(Run->Stack, StackBase);
  EXPECT_EQ(Run->CalleeReturnAddress, 0x102u);
  EXPECT_EQ(Run->PushedWord, 0x102u);
  EXPECT_EQ(Run->CalleeCarry, 1u);
}

TEST(MemoryCallSpecialization, PrefixCannotIntroduceAnotherMemoryRead) {
  auto P = overwrittenTargetSlot();
  P.call(0x101,
         {operation(NdOp::INT_SUB, temp(0), {reg(32), constant(8)}),
          operation(NdOp::LOAD, temp(8), {temp(0)}),
          operation(NdOp::LOAD, temp(16), {temp(0)}), indirectCall(temp(16))});
  const auto Result = specializeInterpreter(P, {0x100}, options());
  EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
  expectNoPublication(Result);
}

TEST(MemoryCallSpecialization,
     TargetMustConsumeTheFinalLoadWithoutSubstitution) {
  for (unsigned Variant = 0; Variant < 3; ++Variant) {
    SCOPED_TRACE(Variant);
    auto P = overwrittenTargetSlot();
    addCallee(P, 0x300, NdOp::INT_XOR, constant(0x55));
    std::vector<LowOp> Prefix{
        operation(NdOp::INT_SUB, temp(0), {reg(32), constant(8)})};
    if (Variant == 2)
      Prefix.push_back(operation(NdOp::COPY, temp(16), {constant(0x300)}));
    Prefix.push_back(operation(NdOp::LOAD, temp(8), {temp(0)}));
    if (Variant == 0)
      Prefix.push_back(operation(NdOp::COPY, temp(16), {temp(8)}));
    else if (Variant == 1)
      Prefix.push_back(operation(NdOp::COPY, temp(8), {constant(0x300)}));
    Prefix.push_back(indirectCall(Variant == 1 ? temp(8) : temp(16)));
    P.call(0x101, std::move(Prefix));
    const auto Result = specializeInterpreter(P, {0x100}, options());
    EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
    expectNoPublication(Result);
  }
}

TEST(MemoryCallSpecialization, UninitializedTargetSlotNeverInventsACallee) {
  auto P = overwrittenTargetSlot();
  P.Code.at(0x100).Ops[1] = operation(NdOp::COPY, reg(48), {constant(0)});
  P.Code.at(0x100).Ops[1].Addr = 0x100;
  P.Code.at(0x100).Ops[1].Seq = 1;
  const auto Result = specializeInterpreter(P, {0x100}, options());
  expectNoPublication(Result);
}

TEST(MemoryCallSpecialization, FiniteTargetLimitNeverPublishesOneCallee) {
  auto P = finiteFrameTargets();
  auto Options = options();
  Options.MaxIndirectTargets = 1;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_TRUE(Result.Status == SpecializationStatus::UnresolvedControl ||
              Result.Status == SpecializationStatus::BudgetExceeded);
  expectNoPublication(Result);
}

TEST(MemoryCallSpecialization, ExpandedCallOperationBudgetRefusesPublication) {
  auto P = overwrittenTargetSlot();
  auto Options = options();
  // Three setup operations plus the EA/load/capture/stack adjustment fit;
  // the physical return-address STORE and final transfer do not.
  Options.MaxOperations = 7;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  expectNoPublication(Result);
}

} // namespace
