//===- InterpreterSpecializationTests.cpp - Partial evaluation contracts
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include <map>
#include <optional>
#include <set>
#include <utility>

using namespace neverd;
using namespace neverd::analysis;
using namespace neverd::symbolic;

namespace {

NdVar reg(uint64_t Offset, uint16_t Size = 8) {
  return NdVar::reg(Offset, Size);
}
NdVar constant(uint64_t Value, uint16_t Size = 8) {
  return NdVar::scalar(Value, Size);
}
LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}
LowOp jump(va_t Address) { return op(NdOp::BRANCH, {}, {constant(Address)}); }
LowOp ret() { return op(NdOp::RETURN, {}, {}); }

class Provider : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;
  std::map<va_t, uint8_t> Image;
  bool MalformedRead = false;

  void add(va_t Address, std::initializer_list<LowOp> Ops,
           va_t Fallthrough = InvalidVA) {
    SpecializationInstruction Instruction;
    Instruction.Ops = Ops;
    Instruction.Origin.Address = Address;
    Instruction.Origin.Size = 1;
    Instruction.Origin.OpCount = Ops.size();
    Instruction.Fallthrough = {Fallthrough == InvalidVA ? Address + 1
                                                        : Fallthrough};
    for (size_t I = 0; I < Instruction.Ops.size(); ++I) {
      auto &Op = Instruction.Ops[I];
      Op.Addr = Address;
      Op.Seq = static_cast<int>(I);
      if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
          Op.Opcode == NdOp::INDIR_BR) {
        Instruction.Origin.Control = LowInstructionControl::Branch;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Branch;
        if (Op.Opcode == NdOp::COND_BR)
          Instruction.Origin.ControlFlags |=
              LowInstructionControlFlag::Conditional;
        if (Op.Opcode == NdOp::INDIR_BR)
          Instruction.Origin.ControlFlags |=
              LowInstructionControlFlag::Indirect;
        else
          Instruction.Origin.Immediate = Op.Inputs[0].Offset;
      } else if (Op.Opcode == NdOp::RETURN) {
        Instruction.Origin.Control = LowInstructionControl::Return;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Return;
      } else if (Op.Opcode == NdOp::CALL) {
        Instruction.Origin.Control = LowInstructionControl::Call;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Call;
        Instruction.Origin.Immediate = Op.Inputs[0].Offset;
      }
    }
    Code[Address] = std::move(Instruction);
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto Found = Code.find(Cursor.Address);
    if (Found == Code.end())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "missing test instruction");
    return Found->second;
  }

  std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Size) override {
    SpecializationImmutableRead Read;
    Read.Evidence = "test immutable allocation";
    for (unsigned I = 0; I < Size; ++I) {
      const auto Found = Image.find(Address + I);
      if (Found == Image.end())
        return std::nullopt;
      Read.Bytes.push_back(Found->second);
    }
    if (MalformedRead)
      Read.Bytes.clear();
    return Read;
  }
};

void defineModelledFlags(Provider &P) {
  // Explicit guest writes make the later snapshot independent of caller flags.
  P.add(0x100, {op(NdOp::COPY, reg(x86reg::CF, 1), {constant(0, 1)}),
                op(NdOp::COPY, reg(x86reg::PF, 1), {constant(0, 1)}),
                op(NdOp::COPY, reg(x86reg::AF, 1), {constant(0, 1)}),
                op(NdOp::COPY, reg(x86reg::ZF, 1), {constant(0, 1)}),
                op(NdOp::COPY, reg(x86reg::SF, 1), {constant(0, 1)}),
                op(NdOp::COPY, reg(x86reg::DF, 1), {constant(0, 1)}),
                op(NdOp::COPY, reg(x86reg::OF, 1), {constant(0, 1)})});
}

size_t count(const LowFunc &Function, NdOp Opcode) {
  size_t Count = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      Count += Op.Opcode == Opcode;
  return Count;
}

/// Independent concrete oracle for the small integer corpus. It intentionally
/// does not use SymExec: it executes residual operands, branches and byte
/// lanes.
std::optional<uint64_t> execute(const LowFunc &Function,
                                std::map<uint64_t, uint64_t> Inputs = {},
                                std::map<uint64_t, uint8_t> Memory = {},
                                uint64_t Flags = 0) {
  using Location = std::pair<VnodeSpace, uint64_t>;
  std::map<Location, uint8_t> Bytes;
  bool Defined = true;
  const auto write = [&](NdVar Destination, uint64_t Value) {
    for (unsigned I = 0; I < Destination.Size; ++I)
      Bytes[{Destination.Space, Destination.Offset + I}] =
          static_cast<uint8_t>(Value >> (I * 8));
  };
  const auto read = [&](NdVar Value) {
    uint64_t Result = Value.isConst() ? Value.Offset : 0;
    if (!Value.isConst())
      for (unsigned I = 0; I < Value.Size; ++I) {
        const auto Found = Bytes.find({Value.Space, Value.Offset + I});
        if (Found == Bytes.end()) {
          Defined = false;
          return uint64_t{0};
        }
        Result |= uint64_t{Found->second} << (I * 8);
      }
    if (Value.Size < 8)
      Result &= (uint64_t{1} << (8 * Value.Size)) - 1;
    return Result;
  };
  for (const auto &[Offset, Value] : Inputs)
    write(reg(Offset), Value);
  int BlockId = 0;
  for (unsigned Step = 0; Step < 10000; ++Step) {
    const auto &Block = Function.Blocks.at(BlockId);
    int Next = -1;
    for (const auto &Op : Block.Ops) {
      const auto A = [&] { return read(Op.Inputs[0]); };
      const auto B = [&] { return read(Op.Inputs[1]); };
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
        write(Op.Output, A());
        break;
      case NdOp::INT_ADD:
        write(Op.Output, A() + B());
        break;
      case NdOp::INT_SUB:
        write(Op.Output, A() - B());
        break;
      case NdOp::INT_XOR:
        write(Op.Output, A() ^ B());
        break;
      case NdOp::INT_AND:
        write(Op.Output, A() & B());
        break;
      case NdOp::INT_OR:
        write(Op.Output, A() | B());
        break;
      case NdOp::INT_MULT:
        write(Op.Output, A() * B());
        break;
      case NdOp::INT_LEFT: {
        const auto Count = B();
        write(Op.Output, Count >= Op.Inputs[0].Size * 8 ? 0 : A() << Count);
        break;
      }
      case NdOp::INT_LESS:
        write(Op.Output, A() < B());
        break;
      case NdOp::INT_EQUAL:
        write(Op.Output, A() == B());
        break;
      case NdOp::SELECT:
        write(Op.Output, A() ? B() : read(Op.Inputs[2]));
        break;
      case NdOp::LOAD: {
        const auto Access = lowMemoryOperands(Op);
        const uint64_t Address = read(*Access.Address);
        uint64_t Value = 0;
        for (unsigned I = 0; I < Access.AccessSize; ++I) {
          const auto Found = Memory.find(Address + I);
          if (Found == Memory.end()) {
            Defined = false;
            break;
          }
          Value |= uint64_t{Found->second} << (8 * I);
        }
        write(Op.Output, Value);
        break;
      }
      case NdOp::STORE: {
        const auto Access = lowMemoryOperands(Op);
        const uint64_t Address = read(*Access.Address);
        const uint64_t Value = read(*Access.StoredValue);
        for (unsigned I = 0; I < Access.AccessSize; ++I)
          Memory[Address + I] = static_cast<uint8_t>(Value >> (8 * I));
        break;
      }
      case NdOp::BRANCH:
      case NdOp::COND_BR: {
        const bool Taken = Op.Opcode == NdOp::BRANCH || B();
        for (int Successor : Block.Succs)
          if ((Function.Blocks[Successor].StartAddr == A()) == Taken)
            Next = Successor;
        if (Next < 0 && Block.Succs.size() == 1)
          Next = Block.Succs.front();
        break;
      }
      case NdOp::RETURN: {
        const auto Value = read(reg(0));
        return Defined ? std::optional<uint64_t>(Value) : std::nullopt;
      }
      case NdOp::NOP:
        break;
      case NdOp::INTRINSIC:
        if (!Op.Inputs[0].isConst()) {
          ADD_FAILURE() << "oracle received a dynamic intrinsic ID";
          return std::nullopt;
        }
        if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Pushf))
          write(Op.Output, Flags);
        else if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Popf))
          Flags = read(Op.Inputs[1]);
        else {
          ADD_FAILURE() << "oracle received an unsupported intrinsic";
          return std::nullopt;
        }
        break;
      default:
        ADD_FAILURE() << "oracle does not implement " << ndOpName(Op.Opcode);
        return std::nullopt;
      }
      if (!Defined)
        return std::nullopt;
    }
    if (Next < 0)
      return std::nullopt;
    BlockId = Next;
  }
  return std::nullopt;
}

TEST(InterpreterSpecialization,
     ImmutableBytesSpecializeWithoutFreezingNativeInput) {
  Provider P;
  P.Image[0x300] = 7;
  P.add(0x100, {op(NdOp::COPY, reg(16), {constant(0x300)})});
  P.add(0x101, {op(NdOp::LOAD, reg(24, 1), {reg(16)})});
  P.add(0x102, {op(NdOp::INT_ADD, reg(0), {reg(8), reg(24, 1)}), ret()});
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}}; // Deliberately unknown at entry.
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 0u);
  ASSERT_EQ(Result.Reads.size(), 1u);
  EXPECT_EQ(Result.Reads[0].Address, 0x300u);
  for (uint64_t Input : std::initializer_list<uint64_t>{0, 1, 42, ~uint64_t{0}})
    EXPECT_EQ(execute(Result.Residual, {{8, Input}}), Input + 7);
}

TEST(InterpreterSpecialization, BusinessLoopConvergesByWeakeningConstants) {
  Provider P;
  P.add(0x100, {op(NdOp::COPY, reg(0), {constant(0)}), jump(0x110)});
  P.add(0x110,
        {op(NdOp::INT_ADD, reg(0), {reg(0), constant(1)}),
         op(NdOp::INT_LESS, reg(24, 1), {reg(0), reg(8)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(24, 1)})},
        0x120);
  P.add(0x120, {ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.Contexts, 3u);
  EXPECT_LE(Result.NodeEvaluations, 7u);
  EXPECT_GT(Result.NodeEvaluations, Result.Contexts);
  for (uint64_t Limit : {1ULL, 2ULL, 17ULL, 255ULL, 260ULL})
    EXPECT_EQ(execute(Result.Residual, {{8, Limit}}), Limit);
}

TEST(InterpreterSpecialization, JoinsDoNotLeakFirstPredecessorConstant) {
  Provider P;
  P.add(0x100, {op(NdOp::COND_BR, {}, {constant(0x110), reg(8, 1)})}, 0x120);
  P.add(0x110, {op(NdOp::COPY, reg(16), {constant(7)}), jump(0x130)});
  P.add(0x120, {op(NdOp::COPY, reg(16), {constant(9)}), jump(0x130)});
  P.add(0x130, {op(NdOp::INT_ADD, reg(0), {reg(16), constant(1)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(execute(Result.Residual, {{8, 0}}), 10u);
  EXPECT_EQ(execute(Result.Residual, {{8, 1}}), 8u);
}

TEST(InterpreterSpecialization, ControlConstantsCloneSharedNativeDispatch) {
  Provider P;
  P.Image[0x300] = 0x40;
  P.Image[0x301] = 0x50;
  P.add(0x10, {op(NdOp::COND_BR, {}, {constant(0x20), reg(8, 1)})}, 0x30);
  P.add(0x20, {op(NdOp::COPY, reg(16), {constant(0x300)}), jump(0x38)});
  P.add(0x30, {op(NdOp::COPY, reg(16), {constant(0x301)}), jump(0x38)});
  P.add(0x38, {op(NdOp::LOAD, reg(24, 1), {reg(16)}),
               op(NdOp::INDIR_BR, {}, {reg(24, 1)})});
  P.add(0x40, {op(NdOp::COPY, reg(0), {constant(11)}), ret()});
  P.add(0x50, {op(NdOp::COPY, reg(0), {constant(22)}), ret()});
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}};
  auto Result = specializeInterpreter(P, {0x10}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(execute(Result.Residual, {{8, 1}}), 11u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0}}), 22u);
  std::set<va_t> Labels;
  for (const auto &Block : Result.Residual.Blocks)
    EXPECT_TRUE(Labels.insert(Block.StartAddr).second);
  EXPECT_EQ(Result.Residual.Entry, 0x10u);
  EXPECT_EQ(Result.Residual.Blocks.front().StartAddr, 0x10u);
}

TEST(InterpreterSpecialization,
     FiniteSelectUsesLiveTargetAndPreservesBothArms) {
  Provider P;
  P.add(0x100, {op(NdOp::SELECT, reg(16),
                   {reg(8, 1), constant(0x110), constant(0x120)})});
  P.add(0x101, {op(NdOp::COPY, reg(8, 1), {constant(0, 1)}),
                op(NdOp::INDIR_BR, {}, {reg(16)})});
  P.add(0x110, {op(NdOp::COPY, reg(0), {constant(3)}), ret()});
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(5)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::INDIR_BR), 0u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0}}), 5u);
  EXPECT_EQ(execute(Result.Residual, {{8, 1}}), 3u);
}

TEST(InterpreterSpecialization, OrdinaryMemoryReadIsRetained) {
  Provider P;
  P.add(0x100, {op(NdOp::LOAD, reg(0, 1), {reg(8)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 1u);
  EXPECT_TRUE(Result.Reads.empty());
  EXPECT_EQ(execute(Result.Residual, {{0, 0}, {8, 0x300}}, {{0x300, 42}}), 42u);
}

TEST(InterpreterSpecialization, FiniteTargetBudgetAndUnknownArmFailClosed) {
  Provider P;
  P.add(0x100, {op(NdOp::SELECT, reg(16),
                   {reg(8, 1), constant(0x110), constant(0x120)}),
                op(NdOp::INDIR_BR, {}, {reg(16)})});
  P.add(0x110, {ret()});
  P.add(0x120, {ret()});
  SpecializationOptions Options;
  Options.MaxIndirectTargets = 1;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  P.Code[0x100].Ops[0].Inputs[2] = reg(24);
  Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, ImmutableReadHonorsConfiguredByteOrder) {
  Provider P;
  P.Image[0x300] = 0x12;
  P.Image[0x301] = 0xAB;
  P.add(0x100, {op(NdOp::LOAD, reg(0, 2), {constant(0x300)}), ret()});
  SpecializationOptions Options;
  Options.ByteOrder = llvm::endianness::big;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  bool Found = false;
  for (const auto &Block : Result.Residual.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::COPY) {
        ASSERT_EQ(Op.NumInputs, 1);
        EXPECT_TRUE(Op.Inputs[0].isConst());
        EXPECT_EQ(Op.Inputs[0].Offset, 0x12ABu);
        Found = true;
      }
  EXPECT_TRUE(Found);
}

TEST(InterpreterSpecialization, AliasingStoreForgetsEarlierMemoryConstant) {
  Provider P;
  P.add(0x100, {op(NdOp::STORE, {}, {constant(0x300), constant(7, 1)}),
                op(NdOp::STORE, {}, {reg(8), constant(9, 1)}),
                op(NdOp::LOAD, reg(16, 1), {constant(0x300)}),
                op(NdOp::COPY, reg(0), {reg(16, 1)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 1u);
  EXPECT_EQ(count(Result.Residual, NdOp::STORE), 2u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0x300}}), 9u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0x400}}), 7u);
}

TEST(InterpreterSpecialization,
     PartialRegisterWritesDoNotInventUpperConstants) {
  Provider P;
  P.add(0x100, {op(NdOp::COPY, reg(0, 1), {constant(0xA5, 1)}),
                op(NdOp::INT_ADD, reg(0), {reg(0), constant(1)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(execute(Result.Residual, {{0, 0x123400}}), 0x1234A6u);
  EXPECT_EQ(execute(Result.Residual, {{0, 0xFF00}}), 0xFFA6u);
}

TEST(InterpreterSpecialization, UnknownIndirectAndOpaqueEffectsPublishNothing) {
  for (const auto &Unsupported :
       {op(NdOp::INDIR_BR, {}, {reg(8)}), op(NdOp::CALL, {}, {constant(0x200)}),
        op(NdOp::INTRINSIC, reg(0), {constant(0)}),
        op(NdOp::INT_DIV, reg(0), {reg(0), reg(8)})}) {
    Provider P;
    P.add(0x100, {Unsupported});
    auto Result = specializeInterpreter(P, {0x100});
    EXPECT_FALSE(Result.complete());
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_FALSE(Result.Diagnostic.empty());
  }
}

TEST(InterpreterSpecialization,
     RuntimeFlagSnapshotsRetainEffectsAndBoundFiniteDispatch) {
  Provider P;
  defineModelledFlags(P);
  const NdVar Flags = NdVar::tmp(100, 8);
  P.add(0x101,
        {op(NdOp::INTRINSIC, Flags,
            {NdVar::cst(static_cast<uint64_t>(Intrinsic::Pushf), 2)}),
         op(NdOp::COPY, reg(40), {Flags}),
         op(NdOp::INT_AND, reg(16), {Flags, constant(uint64_t{1} << 9)}),
         op(NdOp::SELECT, reg(24), {reg(16), constant(0x110), constant(0x120)}),
         op(NdOp::INDIR_BR, {}, {reg(24)})});
  P.add(0x110,
        {op(NdOp::COPY, Flags, {reg(40)}),
         op(NdOp::INTRINSIC, {},
            {NdVar::cst(static_cast<uint64_t>(Intrinsic::Popf), 2), Flags}),
         op(NdOp::COPY, reg(0), {constant(11)}), ret()});
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(22)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::INTRINSIC), 2u);
  EXPECT_EQ(count(Result.Residual, NdOp::INDIR_BR), 0u);
  EXPECT_EQ(execute(Result.Residual, {}, {}, 0), 22u);
  EXPECT_EQ(execute(Result.Residual, {}, {}, uint64_t{1} << 9), 11u);
}

TEST(InterpreterSpecialization,
     UnknownFlagImageCannotBorrowTheExternalStoreNonaliasContract) {
  Provider P;
  defineModelledFlags(P);
  const NdVar Flags = NdVar::tmp(100, 8);
  P.add(0x101, {op(NdOp::INTRINSIC, Flags,
                   {NdVar::cst(static_cast<uint64_t>(Intrinsic::Pushf), 2)}),
                op(NdOp::STORE, {}, {Flags, constant(7, 1)}), ret()});
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.RequireRestoredFrameAtReturn = true;
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());

  P.add(0x101, {op(NdOp::INTRINSIC, Flags,
                   {NdVar::cst(static_cast<uint64_t>(Intrinsic::Pushf), 2)}),
                op(NdOp::INDIR_BR, {}, {Flags})});
  Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, UnboundEntryFlagsDoNotPublishSource) {
  Provider P;
  P.add(0x100, {op(NdOp::INTRINSIC, NdVar::tmp(100, 8),
                   {NdVar::cst(static_cast<uint64_t>(Intrinsic::Pushf), 2)}),
                ret()});
  auto Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, AlgebraCannotDefineAnUnboundEntryFlag) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_XOR, reg(x86reg::CF, 1),
                   {reg(x86reg::CF, 1), reg(x86reg::CF, 1)}),
                ret()});
  auto Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_NE(Result.Diagnostic.find("unbound entry flag"), std::string::npos);
}

TEST(InterpreterSpecialization, FlagDefinitionMustHoldAcrossEveryPredecessor) {
  Provider P;
  P.add(0x100, {op(NdOp::COND_BR, {}, {constant(0x110), reg(8, 1)})}, 0x120);
  P.add(0x110,
        {op(NdOp::COPY, reg(x86reg::CF, 1), {constant(0, 1)}), jump(0x130)});
  P.add(0x120, {jump(0x130)});
  P.add(0x130, {op(NdOp::COPY, reg(0, 1), {reg(x86reg::CF, 1)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, MalformedRuntimeFlagIntrinsicsFailClosed) {
  for (const LowOp &Bad :
       {op(NdOp::INTRINSIC, NdVar::tmp(100, 4),
           {NdVar::cst(static_cast<uint64_t>(Intrinsic::Pushf), 2)}),
        op(NdOp::INTRINSIC, reg(0),
           {NdVar::cst(static_cast<uint64_t>(Intrinsic::Pushf), 2)}),
        op(NdOp::INTRINSIC, {},
           {NdVar::cst(static_cast<uint64_t>(Intrinsic::Popf), 2)}),
        op(NdOp::INTRINSIC, reg(0),
           {NdVar::cst(static_cast<uint64_t>(Intrinsic::Popf), 2), reg(8)})}) {
    Provider P;
    P.add(0x100, {Bad, ret()});
    auto Result = specializeInterpreter(P, {0x100});
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, AtomicAndSegmentedEffectsAreRefused) {
  for (unsigned Variant = 0; Variant < 2; ++Variant) {
    Provider P;
    LowOp Load = op(NdOp::LOAD, reg(0), {reg(8)});
    if (Variant)
      Load.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    else
      Load.MemoryOrdering = NdMemoryOrdering::Acquire;
    P.add(0x100, {Load, ret()});
    auto Result = specializeInterpreter(P, {0x100});
    EXPECT_FALSE(Result.complete());
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     ContextAndOperationBudgetsNeverPublishPartialCFG) {
  Provider P;
  P.add(0x100, {op(NdOp::COPY, reg(16), {constant(0)}), jump(0x110)});
  P.add(0x110,
        {op(NdOp::INT_ADD, reg(16), {reg(16), constant(1)}), jump(0x110)});
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}};
  Options.MaxContextsPerAddress = 3;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  Options.ControlRegisters.clear();
  Options.MaxOperations = 2;
  Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, MalformedProviderCertificatesAreRefused) {
  Provider P;
  P.Image[0x300] = 7;
  P.MalformedRead = true;
  P.add(0x100, {op(NdOp::LOAD, reg(0, 1), {constant(0x300)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  P.MalformedRead = false;
  P.Code[0x100].Origin.OpCount = 1;
  Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, ConstantSnapshotExcludesMemoryAndUnknownBytes) {
  SymContext Ctx;
  SymState State(Ctx);
  State.write(SymSpace::Register, 0, Ctx.mkConst(16, 0xABCD));
  State.write(SymSpace::Register, 1, Ctx.mkVar("dynamic", 8));
  State.write(SymSpace::Temporary, 16, Ctx.mkConst(8, 7));
  State.store(Ctx.mkConst(64, 0x300), Ctx.mkConst(8, 42));
  const auto Before = State.numLiveBytes();
  const auto Snapshot = State.constantScalarBytes();
  ASSERT_EQ(Snapshot.size(), 2u);
  EXPECT_EQ(Snapshot[0].Space, SymSpace::Register);
  EXPECT_EQ(Snapshot[0].Offset, 0u);
  EXPECT_EQ(Snapshot[0].Value, 0xCDu);
  EXPECT_EQ(Snapshot[1].Space, SymSpace::Temporary);
  EXPECT_EQ(Snapshot[1].Value, 7u);
  EXPECT_EQ(State.numLiveBytes(), Before);
}

TEST(InterpreterSpecialization,
     SpilledCursorAndBusinessLoopUseFiniteEntryFrameFacts) {
  Provider P;
  P.Image[0x300] = 0x40;
  P.Image[0x301] = 0x50;
  P.add(0x10, {op(NdOp::INT_SUB, reg(32), {reg(32), constant(16)}),
               op(NdOp::COPY, reg(40), {reg(32)}),
               op(NdOp::INT_ADD, reg(48), {reg(40), constant(8)}),
               op(NdOp::STORE, {}, {reg(40), constant(0)}),
               op(NdOp::STORE, {}, {reg(48), constant(0x300)}), jump(0x30)});
  P.add(0x30, {op(NdOp::LOAD, reg(16), {reg(48)}),
               op(NdOp::LOAD, reg(24, 1), {reg(16)}),
               op(NdOp::INDIR_BR, {}, {reg(24, 1)})});
  P.add(0x40, {op(NdOp::LOAD, reg(0), {reg(40)}),
               op(NdOp::INT_ADD, reg(0), {reg(0), constant(1)}),
               op(NdOp::STORE, {}, {reg(40), reg(0)}),
               op(NdOp::INT_ADD, reg(16), {reg(16), constant(1)}),
               op(NdOp::STORE, {}, {reg(48), reg(16)}), jump(0x30)});
  P.add(0x50,
        {op(NdOp::LOAD, reg(0), {reg(40)}),
         op(NdOp::INT_LESS, reg(56, 1), {reg(0), reg(8)}),
         op(NdOp::COND_BR, {}, {constant(0x51), reg(56, 1)})},
        0x60);
  P.add(0x51, {op(NdOp::STORE, {}, {reg(48), constant(0x300)}), jump(0x30)});
  P.add(0x60, {ret()});
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.ControlFrameSlots = {{-8, 8}};
  auto Result = specializeInterpreter(P, {0x10}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_LE(Result.Contexts, 10u);
  EXPECT_LE(Result.NodeEvaluations, 40u);
  EXPECT_GT(count(Result.Residual, NdOp::LOAD), 0u);
  EXPECT_GT(count(Result.Residual, NdOp::STORE), 0u);
  for (uint64_t Limit : {1ULL, 2ULL, 17ULL, 260ULL})
    for (uint64_t Stack : {0x1000ULL, 0x9000ULL})
      EXPECT_EQ(execute(Result.Residual, {{8, Limit}, {32, Stack}}), Limit);
}

TEST(InterpreterSpecialization, MayAliasStoreInvalidatesFrameControlFacts) {
  for (bool Absolute : {false, true}) {
    Provider P;
    P.add(0x100, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(8)}),
                  op(NdOp::STORE, {}, {reg(40), constant(0x120)}),
                  op(NdOp::STORE, {},
                     {Absolute ? constant(0x300) : reg(8), constant(0x130)}),
                  jump(0x110)});
    P.add(0x110, {op(NdOp::LOAD, reg(16), {reg(40)}),
                  op(NdOp::INDIR_BR, {}, {reg(16)})});
    P.add(0x120, {ret()});
    P.add(0x130, {ret()});
    SpecializationOptions Options;
    Options.FrameBaseRegister = SymRegisterRange{32, 8};
    Options.ControlFrameSlots = {{-8, 8}};
    auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl)
        << Result.Diagnostic;
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     FrameJoinLosesConflictsUnlessSlotIsAContextHint) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_SUB, reg(40), {reg(32), constant(8)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(8, 1)})},
        0x120);
  P.add(0x110, {op(NdOp::STORE, {}, {reg(40), constant(0x140)}), jump(0x130)});
  P.add(0x120, {op(NdOp::STORE, {}, {reg(40), constant(0x150)}), jump(0x130)});
  P.add(0x130, {op(NdOp::LOAD, reg(16), {reg(40)}),
                op(NdOp::INDIR_BR, {}, {reg(16)})});
  P.add(0x140, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  P.add(0x150, {op(NdOp::COPY, reg(0), {constant(9)}), ret()});
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  auto Joined = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Joined.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Joined.Residual.Blocks.empty());
  Options.ControlFrameSlots = {{-8, 8}};
  auto Partitioned = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Partitioned.complete()) << Partitioned.Diagnostic;
  EXPECT_EQ(execute(Partitioned.Residual, {{8, 1}, {32, 0x1000}}), 7u);
  EXPECT_EQ(execute(Partitioned.Residual, {{8, 0}, {32, 0x1000}}), 9u);
}

TEST(InterpreterSpecialization, ChangedFrameRegisterIsNotReboundToEntryRoot) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(8)}),
                op(NdOp::STORE, {}, {reg(40), constant(0x120)}),
                op(NdOp::COPY, reg(32), {reg(8)}), jump(0x110)});
  P.add(0x110, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(8)}),
                op(NdOp::LOAD, reg(16), {reg(40)}),
                op(NdOp::INDIR_BR, {}, {reg(16)})});
  P.add(0x120, {ret()});
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.ControlFrameSlots = {{-8, 8}};
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl)
      << Result.Diagnostic;
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization,
     RegionSnapshotObservesUnifiedAliasInvalidation) {
  SymContext Ctx;
  SymState State(Ctx);
  const SymRef Root = Ctx.mkFreshVar(64, "entry_frame");
  EXPECT_TRUE(State.constantRegionBytes(Root).empty());
  EXPECT_EQ(State.numMemoryRegions(), 0u);
  State.store(Ctx.mkSub(Root, Ctx.mkConst(64, 8)), Ctx.mkConst(16, 0xABCD));
  const auto Snapshot = State.constantRegionBytes(Root);
  ASSERT_EQ(Snapshot.size(), 2u);
  EXPECT_EQ(Snapshot[0].Offset, uint64_t(-8));
  EXPECT_EQ(Snapshot[0].Value, 0xCDu);
  State.store(Ctx.mkFreshVar(64, "may_alias"), Ctx.mkConst(8, 0));
  EXPECT_TRUE(State.constantRegionBytes(Root).empty());
}

SpecializationOptions ordinaryReturnOptions() {
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.RequireRestoredFrameAtReturn = true;
  return Options;
}

TEST(InterpreterSpecialization, PushTargetRetIsNotARecoveredFunctionReturn) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_SUB, reg(32), {reg(32), constant(8)}),
                op(NdOp::STORE, {}, {reg(32), constant(0x200)}), ret()});
  auto Result = specializeInterpreter(P, {0x100}, ordinaryReturnOptions());
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Result.Diagnostic.find("restored entry frame"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, ReturnSlotOverwriteIsRefusedImmediately) {
  for (bool StraddlesEntry : {false, true}) {
    Provider P;
    if (StraddlesEntry)
      P.add(0x100, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(1)}),
                    op(NdOp::STORE, {}, {reg(40), constant(0, 2)}), ret()});
    else
      P.add(0x100, {op(NdOp::STORE, {}, {reg(32), constant(0x200)}), ret()});
    auto Options = ordinaryReturnOptions();
    Options.ExternalStoresPreserveEntryReturnSlot = true;
    auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
    EXPECT_NE(Result.Diagnostic.find("entry return control slot"),
              std::string::npos);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     NonAffineFrameAddressSurvivesProjectionAsUnsafe) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_XOR, reg(40), {reg(32), reg(8)}), jump(0x110)});
  P.add(0x110, {op(NdOp::STORE, {}, {reg(40), constant(0x200)}), ret()});
  auto Options = ordinaryReturnOptions();
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Result.Diagnostic.find("address origin"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, NonAffineFramePointerSpillReloadRemainsUnsafe) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_XOR, reg(40), {reg(32), reg(8)}),
                op(NdOp::INT_ADD, reg(48), {reg(32), constant(48)}),
                op(NdOp::STORE, {}, {reg(48), reg(40)}), jump(0x110)});
  P.add(0x110, {op(NdOp::LOAD, reg(56), {reg(48)}), jump(0x120)});
  P.add(0x120, {op(NdOp::STORE, {}, {reg(56), constant(0x200)}), ret()});
  auto Options = ordinaryReturnOptions();
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Result.Diagnostic.find("address origin"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization,
     ExternalStoreRequiresExplicitSourceABIContract) {
  Provider P;
  P.add(0x100, {op(NdOp::STORE, {}, {reg(8), constant(42)}),
                op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  auto Options = ordinaryReturnOptions();
  auto Refused = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Refused.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Refused.Diagnostic.find("nonalias contract"), std::string::npos);
  EXPECT_TRUE(Refused.Residual.Blocks.empty());
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Accepted = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Accepted.complete()) << Accepted.Diagnostic;
  EXPECT_EQ(count(Accepted.Residual, NdOp::STORE), 1u);
  EXPECT_EQ(execute(Accepted.Residual, {{8, 0x300}, {32, 0x1000}}), 7u);
}

TEST(InterpreterSpecialization, ExternalPointerFrameSpillPreservesProvenance) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_SUB, reg(32), {reg(32), constant(16)}),
                op(NdOp::STORE, {}, {reg(32), reg(8)}), jump(0x110)});
  P.add(0x110, {op(NdOp::LOAD, reg(40), {reg(32)}),
                op(NdOp::STORE, {}, {reg(40), constant(42)}), jump(0x120)});
  P.add(0x120, {op(NdOp::LOAD, reg(40), {reg(32)}),
                op(NdOp::STORE, {}, {reg(40), constant(43)}),
                op(NdOp::INT_ADD, reg(32), {reg(32), constant(16)}),
                op(NdOp::COPY, reg(0), {constant(9)}), ret()});
  auto Options = ordinaryReturnOptions();
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 2u);
  EXPECT_EQ(count(Result.Residual, NdOp::STORE), 3u);
  for (uint64_t Out : {0x300ULL, 0x2000ULL})
    EXPECT_EQ(execute(Result.Residual, {{8, Out}, {32, 0x1000}}), 9u);
}

TEST(InterpreterSpecialization, UndefinedTemporaryIsNotAnExternalABIInput) {
  Provider P;
  P.add(0x100,
        {op(NdOp::STORE, {}, {NdVar::tmp(0x10000, 8), constant(0)}), ret()});
  auto Options = ordinaryReturnOptions();
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Result.Diagnostic.find("unbound temporary"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  P.add(0x100,
        {op(NdOp::COPY, NdVar::tmp(0x10000, 8), {reg(8)}),
         op(NdOp::STORE, {}, {NdVar::tmp(0x10000, 8), constant(0)}), ret()});
  Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_TRUE(Result.complete()) << Result.Diagnostic;
}

TEST(InterpreterSpecialization, TemporaryReadsNeedCompleteDefinitions) {
  Provider P;
  const NdVar Temp = NdVar::tmp(0x10000, 8);
  P.add(0x100, {op(NdOp::INT_XOR, reg(0), {Temp, Temp}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());

  P.add(0x100, {op(NdOp::COPY, NdVar::tmp(0x10000, 4), {constant(7, 4)}),
                op(NdOp::COPY, reg(0), {Temp}), ret()});
  Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());

  P.add(0x100, {op(NdOp::COPY, Temp, {reg(8)}), op(NdOp::COPY, reg(0), {Temp}),
                ret()});
  Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(execute(Result.Residual, {{8, 42}}), 42u);
}

TEST(InterpreterSpecialization, PriorInstructionCannotDefineReusedTemporary) {
  Provider P;
  const NdVar Temp = NdVar::tmp(0x10000, 8);
  P.add(0x100, {op(NdOp::COPY, Temp, {constant(7)})});
  P.add(0x101, {op(NdOp::COPY, reg(0), {Temp}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, EntryConstantsCannotBindLifterTemporaries) {
  Provider P;
  P.add(0x100, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  SpecializationOptions Options;
  Options.EntryConstants.push_back({NdVar::tmp(0x10000, 8), 7});
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

void imageWord(Provider &P, va_t Address, uint64_t Value, unsigned Bytes = 8) {
  for (unsigned I = 0; I < Bytes; ++I)
    P.Image[Address + I] = static_cast<uint8_t>(Value >> (8 * I));
}

Provider finiteAddressProvider() {
  Provider P;
  imageWord(P, 0x800, 0x200);
  imageWord(P, 0x808, 0x210);
  P.add(0x100, {op(NdOp::INT_AND, reg(16), {reg(8), constant(1)}),
                op(NdOp::INT_MULT, reg(16), {reg(16), constant(8)}),
                op(NdOp::INT_ADD, reg(16), {reg(16), constant(0x800)}),
                op(NdOp::LOAD, reg(24), {reg(16)}),
                op(NdOp::INDIR_BR, {}, {reg(24)})});
  P.add(0x200, {op(NdOp::INT_ADD, reg(0), {reg(8), constant(19)}), ret()});
  P.add(0x210, {op(NdOp::INT_XOR, reg(0), {reg(8), constant(0xA5)}), ret()});
  return P;
}

TEST(InterpreterSpecialization,
     FiniteMaskedAddressesLowerToNumericSelectionsAndKeepBothInputCases) {
  auto P = finiteAddressProvider();
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 0u);
  EXPECT_GT(count(Result.Residual, NdOp::SELECT), 0u);
  EXPECT_EQ(count(Result.Residual, NdOp::INDIR_BR), 0u);
  EXPECT_EQ(Result.Reads.size(), 2u);
  EXPECT_GT(Result.SolverQueries, 0u);
  for (uint64_t Input = 0; Input < 256; ++Input)
    EXPECT_EQ(execute(Result.Residual, {{8, Input}}),
              Input & 1 ? Input ^ 0xA5 : Input + 19);
}

TEST(InterpreterSpecialization,
     FiniteReadNeedsEveryCertificateAndExhaustiveEnumeration) {
  auto P = finiteAddressProvider();
  P.Image.erase(0x80F);
  auto Missing = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Missing.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Missing.Residual.Blocks.empty());
  EXPECT_TRUE(Missing.Reads.empty());
  P = finiteAddressProvider();
  SpecializationOptions Options;
  Options.MaxImmutableReadAddresses = 1;
  auto Limited = specializeInterpreter(P, {0x100}, Options);
  EXPECT_FALSE(Limited.complete());
  EXPECT_TRUE(Limited.Residual.Blocks.empty());
  EXPECT_TRUE(Limited.Origins.empty());
  EXPECT_TRUE(Limited.Reads.empty());
  Options = {};
  Options.MaxSolverQueries = 1;
  auto Interrupted = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Interrupted.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_EQ(Interrupted.SolverQueries, 1u);
  EXPECT_TRUE(Interrupted.Residual.Blocks.empty());
  EXPECT_TRUE(Interrupted.Origins.empty());
  EXPECT_TRUE(Interrupted.Reads.empty());
}

TEST(InterpreterSpecialization,
     IncompleteOptionalReadProofRetainsOrdinaryRuntimeMemory) {
  auto P = finiteAddressProvider();
  P.add(0x100, {op(NdOp::INT_AND, reg(16), {reg(8), constant(1)}),
                op(NdOp::INT_MULT, reg(16), {reg(16), constant(8)}),
                op(NdOp::INT_ADD, reg(16), {reg(16), constant(0x800)}),
                op(NdOp::LOAD, reg(0), {reg(16)}), ret()});
  SpecializationOptions Options;
  Options.MaxImmutableReadAddresses = 1;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_TRUE(Result.Reads.empty());
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 1u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0}}, P.Image), 0x200u);
  EXPECT_EQ(execute(Result.Residual, {{8, 1}}, P.Image), 0x210u);
}

// Two independent predecessors each carry a pair of correlated pointer/key
// values. All records share one handler. Cross-pairing a pointer and key would
// produce an unsupported destination, so a Cartesian product cannot pass.
Provider correlatedControlsProvider() {
  Provider P;
  for (unsigned Lane = 0; Lane < 4; ++Lane) {
    const uint64_t Key = 11 + 37 * Lane;
    const uint64_t Target = 0x300 + 0x10 * Lane;
    imageWord(P, 0x800 + 16 * Lane, 0x200);
    imageWord(P, 0x808 + 16 * Lane, Target ^ Key);
    P.add(Target,
          {op(NdOp::INT_ADD, reg(0), {reg(8), constant(9 * Lane + 1)}), ret()});
  }
  P.add(0x100, {op(NdOp::COND_BR, {}, {constant(0x110), reg(72, 1)})}, 0x120);
  for (unsigned Side = 0; Side < 2; ++Side)
    P.add(Side == 0 ? 0x110 : 0x120,
          {op(NdOp::INT_AND, reg(48), {reg(8), constant(1)}),
           op(NdOp::INT_MULT, reg(16), {reg(48), constant(16)}),
           op(NdOp::INT_ADD, reg(16), {reg(16), constant(0x800 + Side * 32)}),
           op(NdOp::INT_MULT, reg(24), {reg(48), constant(37)}),
           op(NdOp::INT_ADD, reg(24), {reg(24), constant(11 + Side * 74)}),
           op(NdOp::LOAD, reg(40), {reg(16)}),
           op(NdOp::INDIR_BR, {}, {reg(40)})});
  P.add(0x200, {op(NdOp::INT_ADD, reg(56), {reg(16), constant(8)}),
                op(NdOp::LOAD, reg(40), {reg(56)}),
                op(NdOp::INT_XOR, reg(40), {reg(40), reg(24)}),
                op(NdOp::INDIR_BR, {}, {reg(40)})});
  return P;
}

TEST(InterpreterSpecialization,
     JointControlTuplesPreserveCorrelationAcrossAJoinAndSharedHandler) {
  auto P = correlatedControlsProvider();
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}, {24, 8}};
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 0u);
  EXPECT_GT(count(Result.Residual, NdOp::SELECT), 0u);
  for (uint64_t Side : {0ULL, 1ULL})
    for (uint64_t Input = 0; Input < 32; ++Input) {
      const auto Lane = (Input & 1) + (Side ? 0 : 2);
      EXPECT_EQ(execute(Result.Residual, {{8, Input}, {72, Side}}, P.Image),
                Input + 9 * Lane + 1);
    }
}

TEST(InterpreterSpecialization,
     JointControlWideningCannotPublishTheEarlierPartialGraph) {
  auto P = correlatedControlsProvider();
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}, {24, 8}};
  Options.MaxControlTuples = 2;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_FALSE(Result.complete());
  EXPECT_GT(Result.RelationalWidenings, 0u);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
}

TEST(InterpreterSpecialization,
     ConditionalEdgesRefineCurrentValuesWithoutReusingAnOverwrittenInput) {
  Provider P;
  // Only the two addresses reachable under the actual edge predicates exist.
  imageWord(P, 0x800, 0x300);
  imageWord(P, 0x818, 0x310);
  P.add(0x100,
        {op(NdOp::INT_AND, reg(48), {reg(8), constant(1)}),
         op(NdOp::INT_EQUAL, reg(64, 1), {reg(48), constant(1)}),
         op(NdOp::INT_XOR, reg(48), {reg(48), constant(1)}),
         op(NdOp::INT_MULT, reg(16), {reg(48), constant(8)}),
         op(NdOp::INT_ADD, reg(16), {reg(16), constant(0x800)}),
         op(NdOp::COND_BR, {}, {constant(0x200), reg(64, 1)})},
        0x210);
  P.add(0x200, {op(NdOp::LOAD, reg(24), {reg(16)}),
                op(NdOp::INDIR_BR, {}, {reg(24)})});
  P.add(0x210, {op(NdOp::INT_ADD, reg(16), {reg(16), constant(16)}),
                op(NdOp::LOAD, reg(24), {reg(16)}),
                op(NdOp::INDIR_BR, {}, {reg(24)})});
  P.add(0x300, {op(NdOp::COPY, reg(0), {constant(3)}), ret()});
  P.add(0x310, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}};
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input = 0; Input < 256; ++Input)
    EXPECT_EQ(execute(Result.Residual, {{8, Input}}, P.Image),
              Input & 1 ? 3u : 7u);
}

TEST(InterpreterSpecialization,
     AliasingStoreInvalidatesImportedFiniteFrameRelations) {
  Provider P;
  imageWord(P, 0x800, 0x300);
  imageWord(P, 0x808, 0x310);
  P.add(0x100, {op(NdOp::INT_AND, reg(16), {reg(8), constant(1)}),
                op(NdOp::INT_MULT, reg(16), {reg(16), constant(8)}),
                op(NdOp::INT_ADD, reg(16), {reg(16), constant(0x800)}),
                op(NdOp::INT_ADD, reg(48), {reg(32), constant(16)}),
                op(NdOp::STORE, {}, {reg(48), reg(16)}), jump(0x200)});
  P.add(0x200,
        {op(NdOp::STORE, {}, {reg(64), constant(0)}),
         op(NdOp::LOAD, reg(16), {reg(48)}), op(NdOp::LOAD, reg(24), {reg(16)}),
         op(NdOp::INDIR_BR, {}, {reg(24)})});
  P.add(0x300, {op(NdOp::COPY, reg(0), {constant(3)}), ret()});
  P.add(0x310, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.ControlFrameSlots = {{16, 8}};
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Reads.empty());
  // Removing the may-alias store establishes that it caused the lost proof.
  P.Code.at(0x200).Ops[0] = op(NdOp::NOP, {}, {});
  auto &NoStore = P.Code.at(0x200).Ops[0];
  NoStore.Addr = 0x200;
  NoStore.Seq = 0;
  Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {0ULL, 1ULL})
    EXPECT_EQ(execute(Result.Residual, {{8, Input}, {32, 0x2000}}, P.Image),
              Input ? 7u : 3u);
}

TEST(InterpreterSpecialization,
     RelationalLoopConvergesWithoutEnumeratingTheBusinessCounter) {
  Provider P;
  for (unsigned Lane = 0; Lane < 2; ++Lane) {
    imageWord(P, 0x900 + 16 * Lane, 0x200);
    imageWord(P, 0x908 + 16 * Lane, (1 + 2 * Lane) ^ (17 + Lane));
  }
  P.add(0x100, {op(NdOp::COPY, reg(0), {constant(0)}),
                op(NdOp::COPY, reg(88), {constant(0)}), jump(0x120)});
  P.add(0x120, {op(NdOp::INT_XOR, reg(48), {reg(8), reg(88)}),
                op(NdOp::INT_AND, reg(48), {reg(48), constant(1)}),
                op(NdOp::INT_MULT, reg(16), {reg(48), constant(16)}),
                op(NdOp::INT_ADD, reg(16), {reg(16), constant(0x900)}),
                op(NdOp::INT_ADD, reg(24), {reg(48), constant(17)}),
                op(NdOp::LOAD, reg(40), {reg(16)}),
                op(NdOp::INDIR_BR, {}, {reg(40)})});
  P.add(0x200,
        {op(NdOp::INT_ADD, reg(56), {reg(16), constant(8)}),
         op(NdOp::LOAD, reg(40), {reg(56)}),
         op(NdOp::INT_XOR, reg(40), {reg(40), reg(24)}),
         op(NdOp::INT_ADD, reg(0), {reg(0), reg(40)}),
         op(NdOp::INT_ADD, reg(88), {reg(88), constant(1)}),
         op(NdOp::INT_LESS, reg(80, 1), {reg(88), reg(64)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(80, 1)})},
        0x300);
  P.add(0x300, {ret()});
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}, {24, 8}};
  auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_LT(Result.Contexts, 12u);
  EXPECT_LT(Result.NodeEvaluations, 30u);
  for (uint64_t Input : {0ULL, 1ULL, 42ULL})
    for (uint64_t Limit = 1; Limit < 20; ++Limit) {
      uint64_t Expected = 0;
      for (uint64_t I = 0; I < Limit; ++I)
        Expected += 1 + 2 * ((Input ^ I) & 1);
      EXPECT_EQ(execute(Result.Residual, {{8, Input}, {64, Limit}}, P.Image),
                Expected);
    }
}

TEST(InterpreterSpecialization, IndependentOracleRefusesMissingRuntimeInputs) {
  Provider P;
  P.add(0x100, {op(NdOp::LOAD, reg(0), {reg(8)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_FALSE(execute(Result.Residual).has_value());
  EXPECT_FALSE(execute(Result.Residual, {{8, 0x800}}).has_value());
}

TEST(InterpreterSpecialization,
     ConditionalConstantProofDoesNotEraseTheEntryFrameOrigin) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_EQUAL, reg(64, 1), {reg(32), constant(0x1000)}),
         op(NdOp::INT_EQUAL, reg(72, 1), {reg(8), constant(0)}),
         op(NdOp::BOOL_AND, reg(80, 1), {reg(64, 1), reg(72, 1)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(80, 1)})},
        0x120);
  P.add(0x110, {op(NdOp::INT_XOR, reg(40), {reg(32), reg(8)}),
                op(NdOp::STORE, {}, {reg(40), constant(0)}), ret()});
  P.add(0x120, {ret()});
  auto Options = ordinaryReturnOptions();
  Options.ControlRegisters = {{32, 8}};
  Options.ExternalStoresPreserveEntryReturnSlot = true;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Result.Diagnostic.find("address origin"), std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization,
     IndirectTargetEqualityPreservesFrameOriginAndExternalStores) {
  for (uint64_t TargetRegister : {32u, 48u}) {
    Provider P;
    P.add(0x100,
          {op(NdOp::COPY, reg(TargetRegister), {reg(32)}),
           op(NdOp::INT_EQUAL, reg(64, 1),
              {reg(TargetRegister), constant(0x200)}),
           op(NdOp::COND_BR, {}, {constant(0x110), reg(64, 1)})},
          0x120);
    P.add(0x110, {op(NdOp::INDIR_BR, {}, {reg(TargetRegister)})});
    P.add(0x120, {op(NdOp::COPY, reg(0), {constant(9)}), ret()});
    // With entry frame 0x200 and input zero, this writes the return slot.
    // The indirect edge proves a number but must retain the frame provenance
    // of either the frame register itself or an ordinary affine copy of it.
    P.add(0x200, {op(NdOp::INT_XOR, reg(40), {reg(TargetRegister), reg(8)}),
                  op(NdOp::STORE, {}, {reg(40), constant(17)}), ret()});
    auto Options = ordinaryReturnOptions();
    Options.ControlRegisters = {{TargetRegister, 8}};
    Options.ExternalStoresPreserveEntryReturnSlot = true;
    auto Result = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
    EXPECT_NE(Result.Diagnostic.find("address origin"), std::string::npos);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Origins.empty());
    EXPECT_TRUE(Result.Reads.empty());

    // Keeping target provenance must not reject the recovered indirect edge
    // itself, or a genuinely external output pointer under the ABI contract.
    P.add(0x200, {op(NdOp::STORE, {}, {reg(8), constant(17)}),
                  op(NdOp::COPY, reg(0), {constant(9)}), ret()});
    Result = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_EQ(count(Result.Residual, NdOp::INDIR_BR), 0u);
    EXPECT_EQ(count(Result.Residual, NdOp::STORE), 1u);
    EXPECT_EQ(execute(Result.Residual, {{32, 0x200}, {8, 0x300}}), 9u);
    EXPECT_EQ(execute(Result.Residual, {{32, 0x800}, {8, 0x300}}), 9u);
  }
}

TEST(InterpreterSpecialization,
     TwoSatisfyingModelsDoNotReplaceTheRequiredExhaustionProof) {
  auto P = finiteAddressProvider();
  P.add(0x100, {op(NdOp::INT_AND, reg(16), {reg(8), constant(1)}),
                op(NdOp::INT_MULT, reg(16), {reg(16), constant(8)}),
                op(NdOp::INT_ADD, reg(16), {reg(16), constant(0x800)}),
                op(NdOp::LOAD, reg(0), {reg(16)}), ret()});
  SpecializationOptions Options;
  Options.MaxImmutableReadAddresses = 2;
  Options.MaxSolverQueries = 2;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_EQ(Result.SolverQueries, 2u);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Reads.empty());
  Options.MaxSolverQueries = 3;
  Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.SolverQueries, 3u);
  EXPECT_EQ(Result.Reads.size(), 2u);
}

TEST(InterpreterSpecialization,
     OneVaryingColumnReusesItsCompleteJointWitnesses) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_AND, reg(16), {reg(8), constant(1)}),
                op(NdOp::COPY, reg(24), {constant(17)}), jump(0x200)});
  P.add(0x200, {op(NdOp::INT_ADD, reg(0), {reg(16), reg(24)}), ret()});
  SpecializationOptions Options;
  Options.ControlRegisters = {{16, 8}, {24, 8}};
  // Two witnesses are not an exhaustive proof. The third query proves that
  // the varying column has no other values; the other column is constant on
  // this same path, so that proof already supplies the complete joint rows.
  Options.MaxSolverQueries = 2;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Reads.empty());
  EXPECT_TRUE(Result.Origins.empty());
  Options.MaxSolverQueries = 3;
  Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.SolverQueries, 3u);
  for (uint64_t Input : {0ULL, 1ULL, 2ULL, 0x123456789abcdef0ULL, ~0ULL})
    EXPECT_EQ(execute(Result.Residual, {{8, Input}}), 17 + (Input & 1));
}

TEST(InterpreterSpecialization,
     ConstantProjectionColumnsDoNotProveAnImpossiblePathReachable) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_AND, reg(16), {reg(8), constant(3)}),
         op(NdOp::INT_MULT, reg(24), {reg(16), reg(16)}),
         op(NdOp::INT_AND, reg(24), {reg(24), constant(3)}),
         op(NdOp::INT_EQUAL, reg(32, 1), {reg(24), constant(2)}),
         op(NdOp::COPY, reg(48), {constant(7)}),
         op(NdOp::COND_BR, {}, {constant(0xdead), reg(32, 1)})},
        0x200);
  P.add(0x200, {op(NdOp::COPY, reg(0), {reg(8)}), ret()});
  SpecializationOptions Options;
  Options.ControlRegisters = {{48, 8}};
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  // No square is 2 modulo 4. A constant control column alone is not a witness
  // that its incoming predicate is satisfiable; the missing node is dead.
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  for (uint64_t Input : {0ULL, 1ULL, 2ULL, 3ULL, ~0ULL})
    EXPECT_EQ(execute(Result.Residual, {{8, Input}}), Input);
}

TEST(InterpreterSpecialization,
     FiniteReadCapturesOverlappingRegisterAndTemporaryAddressBytes) {
  for (uint16_t Width : {1, 2, 4, 8})
    for (bool Temporary : {false, true}) {
      Provider P;
      const NdVar Address = Temporary ? NdVar::tmp(0x20000, 8) : reg(16);
      NdVar Output = Address;
      Output.Size = Width;
      const uint64_t Mask =
          Width == 8 ? ~uint64_t{0} : (uint64_t{1} << (Width * 8)) - 1;
      const uint64_t A = UINT64_C(0x123456789ABCDEF0) & Mask;
      const uint64_t B = UINT64_C(0xCBA9876543210012) & Mask;
      imageWord(P, 0xA00, A, Width);
      imageWord(P, 0xA08, B, Width);
      P.add(0x100, {op(NdOp::INT_AND, reg(48), {reg(8), constant(1)}),
                    op(NdOp::INT_MULT, Address, {reg(48), constant(8)}),
                    op(NdOp::INT_ADD, Address, {Address, constant(0xA00)}),
                    op(NdOp::LOAD, Output, {Address}),
                    op(NdOp::COPY, reg(0), {Address}), ret()});
      auto Result = specializeInterpreter(P, {0x100});
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 0u);
      EXPECT_EQ(Result.Reads.size(), 2u);
      EXPECT_EQ(execute(Result.Residual, {{8, 0}}), (0xA00 & ~Mask) | A);
      EXPECT_EQ(execute(Result.Residual, {{8, 1}}), (0xA08 & ~Mask) | B);
      bool Capture = false;
      for (const auto &Block : Result.Residual.Blocks)
        for (const auto &Op : Block.Ops) {
          if (Op.Opcode == NdOp::COPY && Op.Output.isTemp() &&
              Op.Output.Offset >= (uint64_t{1} << 62)) {
            EXPECT_EQ(Op.Inputs[0], Address);
            Capture = true;
          }
          if (Op.Opcode == NdOp::SELECT)
            EXPECT_EQ(Op.Inputs[1].Provenance,
                      ConstantAddressProvenance::Scalar);
          if (Op.Opcode == NdOp::INT_EQUAL)
            EXPECT_EQ(Op.Inputs[1].Provenance,
                      ConstantAddressProvenance::Scalar);
        }
      EXPECT_TRUE(Capture);
    }
}

TEST(InterpreterSpecialization,
     GeneratedReadSelectionsAreChargedAndReservedTempsCannotAliasNativeBytes) {
  auto P = finiteAddressProvider();
  SpecializationOptions Options;
  Options.MaxOperations = 4;
  auto Result = specializeInterpreter(P, {0x100}, Options);
  EXPECT_EQ(Result.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_NE(Result.Diagnostic.find("lowering operation budget"),
            std::string::npos);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  EXPECT_TRUE(Result.Origins.empty());
  EXPECT_TRUE(Result.Reads.empty());
  for (uint64_t Offset : {(uint64_t{1} << 62) - 1, (uint64_t{1} << 62) + 8}) {
    Provider Collision;
    Collision.add(
        0x100,
        {op(NdOp::COPY, NdVar::tmp(Offset, 2), {constant(0, 2)}), ret()});
    Result = specializeInterpreter(Collision, {0x100});
    EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
    EXPECT_TRUE(Result.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     ConsecutiveFiniteReadsReuseTempsWithoutLosingEarlierResults) {
  Provider P;
  imageWord(P, 0xA00, 17);
  imageWord(P, 0xA08, 31);
  imageWord(P, 0xA10, 101);
  imageWord(P, 0xA18, 211);
  P.add(0x100, {op(NdOp::INT_AND, reg(16), {reg(8), constant(1)}),
                op(NdOp::INT_MULT, reg(16), {reg(16), constant(8)}),
                op(NdOp::INT_ADD, reg(16), {reg(16), constant(0xA00)}),
                op(NdOp::LOAD, reg(0), {reg(16)}),
                op(NdOp::INT_ADD, reg(16), {reg(16), constant(16)}),
                op(NdOp::LOAD, reg(24), {reg(16)}),
                op(NdOp::INT_ADD, reg(0), {reg(0), reg(24)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 0u);
  EXPECT_EQ(count(Result.Residual, NdOp::SELECT), 2u);
  EXPECT_EQ(Result.Reads.size(), 4u);
  for (uint64_t Input = 0; Input < 256; ++Input)
    EXPECT_EQ(execute(Result.Residual, {{8, Input}}), Input & 1 ? 242u : 118u);
}

TEST(InterpreterSpecialization,
     EqualFiniteReadValuesStillNeedEveryCertificateBeforeCopy) {
  Provider P;
  imageWord(P, 0xA00, 71);
  imageWord(P, 0xA08, 71);
  P.add(0x100, {op(NdOp::INT_AND, reg(16), {reg(8), constant(1)}),
                op(NdOp::INT_MULT, reg(16), {reg(16), constant(8)}),
                op(NdOp::INT_ADD, reg(16), {reg(16), constant(0xA00)}),
                op(NdOp::LOAD, reg(0), {reg(16)}), ret()});
  auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 0u);
  EXPECT_EQ(count(Result.Residual, NdOp::SELECT), 0u);
  EXPECT_EQ(Result.Reads.size(), 2u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0}}), 71u);
  EXPECT_EQ(execute(Result.Residual, {{8, 1}}), 71u);
  const auto RuntimeImage = P.Image;
  P.Image.erase(0xA0F);
  Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 1u);
  EXPECT_EQ(count(Result.Residual, NdOp::SELECT), 0u);
  EXPECT_TRUE(Result.Reads.empty());
  EXPECT_EQ(execute(Result.Residual, {{8, 0}}, RuntimeImage), 71u);
  EXPECT_EQ(execute(Result.Residual, {{8, 1}}, RuntimeImage), 71u);
}

} // namespace

namespace {

TEST(InterpreterSpecialization,
     FreeFrameReadsDoNotSpendImmutableAddressEnumerationBudget) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_EQUAL, reg(64, 1), {reg(8), constant(0)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(64, 1)})},
        0x140);
  P.add(0x120, {op(NdOp::INT_SUB, reg(48), {reg(32), constant(16)}),
                op(NdOp::LOAD, reg(0), {reg(48)}), ret()});
  P.add(0x140, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.MaxSolverQueries = 5;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.SolverQueries, 4u);
  EXPECT_TRUE(Result.Reads.empty());
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 1u);
  std::map<uint64_t, uint8_t> Memory;
  for (unsigned I = 0; I < 8; ++I)
    Memory[0x1800 - 16 + I] = I ? 0 : 91;
  EXPECT_EQ(execute(Result.Residual, {{8, 0}, {32, 0x1800}}, Memory), 91u);
  EXPECT_EQ(execute(Result.Residual, {{8, 1}, {32, 0x1800}}, Memory), 7u);
}

TEST(InterpreterSpecialization,
     PathConstrainedFrameReadStillRequiresItsImmutableCertificate) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_EQUAL, reg(64, 1), {reg(32), constant(0x1800)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(64, 1)})},
        0x140);
  P.add(0x120, {op(NdOp::INT_SUB, reg(48), {reg(32), constant(16)}),
                op(NdOp::LOAD, reg(0), {reg(48)}), ret()});
  P.add(0x140, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  for (unsigned I = 0; I < 8; ++I)
    P.Image[0x1800 - 16 + I] = I ? 0 : 91;
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.ControlRegisters = {{32, 8}};
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  ASSERT_EQ(Result.Reads.size(), 1u);
  EXPECT_EQ(Result.Reads.front().Address, 0x1800 - 16);
  EXPECT_EQ(execute(Result.Residual, {{32, 0x1800}}, P.Image), 91u);
  EXPECT_EQ(execute(Result.Residual, {{32, 0x1900}}, P.Image), 7u);
}

} // namespace
