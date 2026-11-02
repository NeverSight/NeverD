//===- TerminalImportSpecializationTests.cpp - Explicit external exits ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <map>
#include <set>

using namespace neverd;
using namespace neverd::analysis;

namespace {

LowOp make(NdOp Opcode, NdVar Out, std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Out;
  for (auto V : Inputs)
    Op.addInput(V);
  return Op;
}

const SpecializationTerminalImport Binding{1, 0x200, "fixture.dll",
                                           "StopFixture"};

class TerminalProvider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;
  std::set<va_t> Fetched;
  unsigned ImmutableQueries = 0;

  void add(va_t Address, std::vector<LowOp> Ops) {
    SpecializationInstruction I;
    I.Ops = std::move(Ops);
    I.Origin.Address = Address;
    I.Origin.Size = 1;
    I.Origin.OpCount = I.Ops.size();
    I.Fallthrough = {Address + 1};
    for (size_t J = 0; J < I.Ops.size(); ++J) {
      auto &Op = I.Ops[J];
      Op.Addr = Address;
      Op.Seq = J;
      if (Op.Opcode == NdOp::RETURN) {
        I.Origin.Control = LowInstructionControl::Return;
        I.Origin.ControlFlags = LowInstructionControlFlag::Return;
      } else if (Op.Opcode == NdOp::COND_BR) {
        I.Origin.Control = LowInstructionControl::Branch;
        I.Origin.ControlFlags = LowInstructionControlFlag::Branch |
                                LowInstructionControlFlag::Conditional;
        I.Origin.Immediate = Op.Inputs[0].Offset;
      }
    }
    Code[Address] = std::move(I);
  }

  TerminalProvider() {
    SpecializationInstruction I;
    I.NativeBytes = {0xff, 0x15, 0xfa, 0, 0, 0};
    Decoder Decode;
    Decode.init(Arch::X64);
    Decode.setStrict(true);
    DecodedInsn Insn{};
    EXPECT_TRUE(Decode.decodeOneForLift(I.NativeBytes.data(), 6, 0x100, Insn));
    EXPECT_TRUE(
        Decode.liftX64MemoryCallToLow(Insn, I.Ops, &I.UndefinedEffects));
    I.Origin.Address = 0x100;
    I.Origin.Size = 6;
    I.Origin.OpCount = I.Ops.size();
    I.Origin.Control = LowInstructionControl::Call;
    I.Origin.ControlFlags =
        LowInstructionControlFlag::Call | LowInstructionControlFlag::Indirect;
    I.Fallthrough = {0x106};
    I.NativeStackControl = SpecializationNativeStackControl::Call;
    I.IsNativeCall = true;
    I.TerminalImport =
        SpecializationTerminalImportEvidence{Binding, std::string(64, 'A')};
    Code[0x100] = std::move(I);
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor C) override {
    Fetched.insert(C.Address);
    if (!Code.contains(C.Address))
      return llvm::createStringError(llvm::errc::not_supported,
                                     "unexpected instruction fetch");
    return Code.at(C.Address);
  }
  std::optional<SpecializationImmutableRead>
  immutableRead(va_t, uint16_t Bytes) override {
    ++ImmutableQueries;
    return SpecializationImmutableRead{std::vector<uint8_t>(Bytes, 0),
                                       "must not fold an IAT load"};
  }
};

SpecializationOptions terminalOptions() {
  SpecializationOptions O;
  O.ExplicitMachineState = O.NormalNonfaultingExecution = O.X64CetDisabled =
      true;
  O.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
  O.RequireRestoredFrameAtReturn = true;
  O.X64TerminalImports = {Binding};
  return O;
}

void refuses(const SpecializationResult &R) {
  EXPECT_FALSE(R.complete());
  EXPECT_FALSE(R.Diagnostic.empty());
  EXPECT_TRUE(R.Residual.Blocks.empty());
  EXPECT_TRUE(R.ExternalExits.empty());
}

TEST(TerminalImportSpecialization, RetainsPhysicalCallBeforeOpaqueExit) {
  TerminalProvider P;
  auto O = terminalOptions();
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(R.ExternalExits.size(), 1U);
  EXPECT_EQ(P.Fetched, std::set<va_t>{0x100});
  EXPECT_EQ(P.ImmutableQueries, 0U);
  EXPECT_TRUE(R.Reads.empty());
  auto Error = validateSpecializationExternalExits(R, O);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  const auto &Exit = R.ExternalExits.front();
  const auto At = std::find_if(
      R.Residual.Blocks.begin(), R.Residual.Blocks.end(),
      [&](const auto &B) { return B.StartAddr == Exit.ResidualAddress; });
  ASSERT_NE(At, R.Residual.Blocks.end());
  ASSERT_TRUE(At->Succs.empty());
  // Independent scalar oracle for the physical span, including a deliberately
  // aliasing machine state. The latter checks ordering, not PE-domain validity.
  for (uint64_t EntryStack : {uint64_t{0x10000}, Binding.Slot + 8}) {
    std::map<std::pair<VnodeSpace, uint64_t>, uint64_t> Values;
    std::map<uint64_t, uint64_t> Memory{{Binding.Slot, 0xdeadbeef}};
    Values[{VnodeSpace::REG, x86reg::RSP}] = EntryStack;
    const auto Read = [&](NdVar V) {
      if (V.isConst())
        return V.Offset;
      EXPECT_TRUE(Values.contains({V.Space, V.Offset}));
      return Values.at({V.Space, V.Offset});
    };
    unsigned Loads = 0, Stores = 0, Adjusts = 0, Exits = 0;
    for (const auto &Op : At->Ops) {
      const auto Key = std::pair{Op.Output.Space, Op.Output.Offset};
      switch (Op.Opcode) {
      case NdOp::COPY:
        Values[Key] = Read(Op.Inputs[0]);
        break;
      case NdOp::LOAD:
        ++Loads;
        Values[Key] = Memory.at(Read(Op.Inputs[0]));
        break;
      case NdOp::INT_ADD:
        Values[Key] = Read(Op.Inputs[0]) + Read(Op.Inputs[1]);
        break;
      case NdOp::INT_SUB:
        ++Adjusts;
        Values[Key] = Read(Op.Inputs[0]) - Read(Op.Inputs[1]);
        break;
      case NdOp::STORE:
        ++Stores;
        Memory[Read(Op.Inputs[0])] = Read(Op.Inputs[1]);
        break;
      case NdOp::INDIR_BR:
        ++Exits;
        EXPECT_EQ(Read(Op.Inputs[0]), 0xdeadbeefU);
        break;
      default:
        FAIL() << ndOpName(Op.Opcode);
      }
    }
    EXPECT_EQ(Loads, 1U);
    EXPECT_EQ(Stores, 1U);
    EXPECT_EQ(Adjusts, 1U);
    EXPECT_EQ(Exits, 1U);
    EXPECT_EQ(Memory.at(EntryStack - 8), 0x106U);
    EXPECT_EQ((Values.at({VnodeSpace::REG, x86reg::RSP})), EntryStack - 8);
  }
}

TEST(TerminalImportSpecialization, NormalSiblingAndBudgetsRemainRequired) {
  TerminalProvider P;
  P.add(0x80,
        {make(NdOp::COPY, NdVar::reg(x86reg::RCX, 8), {NdVar::scalar(42, 8)}),
         make(NdOp::COND_BR, {},
              {NdVar::cst(0x100, 8), NdVar::reg(x86reg::RDX, 1)})});
  P.add(0x81, {make(NdOp::RETURN, {}, {})});
  auto O = terminalOptions();
  auto R = specializeInterpreter(P, {0x80}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_EQ(R.ExternalExits.size(), 1U);
  EXPECT_TRUE(P.Fetched.contains(0x81));
  EXPECT_FALSE(P.Fetched.contains(0x106));
  ASSERT_GT(R.SolverQueries, 1U);
  O.MaxOperations = R.EvaluatedOperations;
  O.MaxSolverQueries = R.SolverQueries;
  EXPECT_TRUE(specializeInterpreter(P, {0x80}, O).complete());
  --O.MaxOperations;
  auto Limited = specializeInterpreter(P, {0x80}, O);
  EXPECT_EQ(Limited.Status, SpecializationStatus::BudgetExceeded);
  refuses(Limited);
  ++O.MaxOperations;
  --O.MaxSolverQueries;
  Limited = specializeInterpreter(P, {0x80}, O);
  EXPECT_EQ(Limited.Status, SpecializationStatus::BudgetExceeded);
  refuses(Limited);
  O = terminalOptions();
  P.Code.erase(0x81);
  refuses(specializeInterpreter(P, {0x80}, O));
}

TEST(TerminalImportSpecialization, DomainAndMachineGatesFailClosed) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    TerminalProvider P;
    auto O = terminalOptions();
    switch (Mutation) {
    case 0:
      O.X64TerminalImports.clear();
      break;
    case 1:
      O.ExplicitMachineState = false;
      break;
    case 2:
      O.NormalNonfaultingExecution = false;
      break;
    case 3:
      O.X64CetDisabled = false;
      break;
    case 4:
      O.ByteOrder = llvm::endianness::big;
      break;
    case 5:
      O.FrameBaseRegister.reset();
      break;
    case 6:
      O.FrameBaseRegister->Offset = x86reg::RAX;
      break;
    case 7:
      O.RequireRestoredFrameAtReturn = false;
      break;
    case 8:
      O.X64TerminalImports.push_back(Binding);
      break;
    case 9:
      O.X64TerminalImports[0].Version = 2;
      break;
    case 10:
      O.X64TerminalImports[0].Module.clear();
      break;
    case 11:
      O.X64TerminalImports[0].Symbol.push_back('\0');
      break;
    }
    refuses(specializeInterpreter(P, {0x100}, O));
  }
}

TEST(TerminalImportSpecialization, BytesAndCompleteLiftMustMatch) {
  for (unsigned Mutation = 0; Mutation < 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    TerminalProvider P;
    auto &I = P.Code.at(0x100);
    switch (Mutation) {
    case 0:
      I.NativeBytes.clear();
      break;
    case 1:
      I.NativeBytes[1] = 0x10;
      break; // register-relative memory call
    case 2:
      I.NativeBytes[2] += 8;
      break;
    case 3:
      I.Fallthrough.Address++;
      break;
    case 4:
      I.NativeStackControl = SpecializationNativeStackControl::None;
      break;
    case 5:
      I.IsNativeCall = false;
      break;
    case 6:
      I.Origin.ControlFlags |= LowInstructionControlFlag::NoReturn;
      break;
    case 7:
      I.TerminalImport->FixedImageDigest.clear();
      break;
    case 8:
      I.Ops.front().Inputs[0].Offset += 8;
      break;
    case 9:
      I.Ops.back().Inputs[0].Offset += 8;
      break;
    case 10:
      I.Ops.back().Output.Size = 4;
      break;
    case 11:
      I.Ops.back().Opcode = NdOp::CALL;
      I.Ops.back().Inputs[0] = NdVar::cst(0x200, 8);
      break;
    case 12:
      I.Ops.back().Inputs[0] = NdVar::reg(x86reg::RAX, 8);
      break;
    }
    // A refreshed generic op digest must not launder modified EA semantics.
    I.UndefinedEffects.OperationDigest = lowUndefinedOperationDigest(I.Ops);
    refuses(specializeInterpreter(P, {0x100}, terminalOptions()));
  }
}

TEST(TerminalImportSpecialization,
     PublicationReceiptCannotBeStrippedOrRewritten) {
  TerminalProvider P;
  auto O = terminalOptions();
  auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (unsigned Mutation = 0; Mutation < 16; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Bad = R;
    auto &E = Bad.ExternalExits.front();
    switch (Mutation) {
    case 0:
      Bad.ExternalExits.clear();
      break;
    case 1:
      E.ResidualAddress++;
      break;
    case 2:
      E.Version++;
      break;
    case 3:
      E.Original.NativeBytes[2]++;
      break;
    case 4:
      E.OriginalOperationDigest.clear();
      break;
    case 5:
      E.ExpandedOperationDigest.clear();
      break;
    case 6:
      E.ResidualOperationDigest.clear();
      break;
    case 7:
      E.Original.TerminalImport->Binding.Symbol += "Changed";
      break;
    case 8:
      Bad.ExternalExits.push_back(E);
      break;
    case 10:
      Bad.ExternalExits.clear();
      for (auto &B : Bad.Residual.Blocks)
        B.InstructionBoundaries.clear();
      break;
    case 11:
      Bad.Origins.clear();
      break;
    case 12:
      Bad.Origins.back().NativeInstruction.Size++;
      break;
    case 13:
      E.Original.TerminalImport->FixedImageDigest = "wrong";
      break;
    case 14:
      E.Original.TerminalImport->FixedImageDigest = std::string(64, 'B');
      break;
    case 15:
      E.IdentityDigest.clear();
      break;
    case 9:
      for (auto &B : Bad.Residual.Blocks)
        if (B.StartAddr == E.ResidualAddress) {
          for (auto &Op : B.Ops)
            if (Op.Opcode == NdOp::STORE)
              Op.Inputs[1].Offset = E.ResidualAddress + 1;
          E.ResidualOperationDigest = lowUndefinedOperationDigest(B.Ops);
        }
      break;
    }
    auto Error = validateSpecializationExternalExits(Bad, O);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}

TEST(TerminalImportSpecialization, DeclarationBudgetsAreCharged) {
  TerminalProvider P;
  auto O = terminalOptions();
  O.MaxImagePreparationRecords = 1;
  O.MaxImagePreparationBytes = Binding.Module.size() + Binding.Symbol.size();
  EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  --O.MaxImagePreparationBytes;
  auto R = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(R.Status, SpecializationStatus::BudgetExceeded);
  refuses(R);
  ++O.MaxImagePreparationBytes;
  O.MaxImagePreparationRecords = 0;
  R = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(R.Status, SpecializationStatus::BudgetExceeded);
  refuses(R);
}

} // namespace
