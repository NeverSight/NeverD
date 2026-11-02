//===- InterpreterSpecializationTests.cpp - Partial evaluation contracts
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/arch/x86_64/X64ControlTransitions.h"
#include "../../lib/analysis/arch/x86_64/X64Recovery.h"
#include "../../lib/analysis/arch/x86_64/X64TaskPriority.h"
#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExec.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
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
  uint16_t MalformedReadSize = 0;

  void add(va_t Address, std::vector<LowOp> Ops, va_t Fallthrough = InvalidVA) {
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
    if (MalformedRead || Size == MalformedReadSize)
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

struct CPUQueryObservation {
  uint32_t Leaf;
  uint32_t Subleaf;
  std::array<uint32_t, 4> Outputs;
};

struct MSRReadObservation {
  uint32_t Index;
  uint64_t Value;
};

/// Independent concrete oracle for the small integer corpus. It intentionally
/// does not use SymExec: it executes residual operands, branches and byte
/// lanes.
std::optional<uint64_t>
execute(const LowFunc &Function, std::map<uint64_t, uint64_t> Inputs = {},
        std::map<uint64_t, uint8_t> Memory = {}, uint64_t Flags = 0,
        std::map<NdMemoryAddressSpace, uint64_t> SegmentBases = {},
        std::optional<uint64_t> TaskPriority = std::nullopt,
        llvm::ArrayRef<uint64_t> Timestamps = {},
        std::map<uint64_t, uint8_t> *FinalMemory = nullptr,
        std::optional<uint32_t> MXCSR = std::nullopt,
        uint32_t WritableMask = 0xffbf, uint32_t *FinalMXCSR = nullptr,
        llvm::ArrayRef<CPUQueryObservation> CPUQueries = {},
        llvm::ArrayRef<MSRReadObservation> MSRReads = {}) {
  using Location = std::pair<VnodeSpace, uint64_t>;
  std::map<Location, uint8_t> Bytes;
  bool Defined = true;
  size_t TimestampIndex = 0;
  size_t CPUQueryIndex = 0;
  size_t MSRReadIndex = 0;
  std::vector<uint32_t> RuntimeOutputs;
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
    std::optional<va_t> Instruction;
    for (const auto &Op : Block.Ops) {
      if (Instruction != Op.Addr) {
        std::erase_if(Bytes, [&](const auto &Entry) {
          return Entry.first.first == VnodeSpace::TEMP &&
                 !Function.isFunctionTemporaryByte(Entry.first.second);
        });
        Instruction = Op.Addr;
      }
      const auto A = [&] { return read(Op.Inputs[0]); };
      const auto B = [&] { return read(Op.Inputs[1]); };
      switch (Op.Opcode) {
      case NdOp::COPY:
        // The retained multi-output convention binds instruction-local
        // placeholder copies to the ordered answers from this one read.
        if (!RuntimeOutputs.empty() && Op.Inputs[0].isTemp()) {
          write(Op.Inputs[0], RuntimeOutputs.front());
          RuntimeOutputs.erase(RuntimeOutputs.begin());
        }
        [[fallthrough]];
      case NdOp::INT_ZEXT:
        write(Op.Output, A());
        break;
      case NdOp::SUBBYTES: {
        const auto First = B();
        if (Op.Inputs[0].Size > 8 || First >= Op.Inputs[0].Size ||
            Op.Output.Size > Op.Inputs[0].Size - First)
          return std::nullopt;
        write(Op.Output, A() >> (8 * First));
        break;
      }
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
      case NdOp::BOOL_NOT:
        write(Op.Output, !A());
        break;
      case NdOp::BOOL_AND:
        write(Op.Output, bool(A()) && bool(B()));
        break;
      case NdOp::BOOL_OR:
        write(Op.Output, bool(A()) || bool(B()));
        break;
      case NdOp::BOOL_XOR:
        write(Op.Output, bool(A()) != bool(B()));
        break;
      case NdOp::INT_MULT:
        write(Op.Output, A() * B());
        break;
      case NdOp::INT_LEFT: {
        const auto Count = B();
        write(Op.Output, Count >= Op.Inputs[0].Size * 8 ? 0 : A() << Count);
        break;
      }
      case NdOp::INT_RIGHT: {
        const auto Count = B();
        write(Op.Output, Count >= Op.Inputs[0].Size * 8 ? 0 : A() >> Count);
        break;
      }
      case NdOp::INT_LESS:
        write(Op.Output, A() < B());
        break;
      case NdOp::INT_EQUAL:
        write(Op.Output, A() == B());
        break;
      case NdOp::INT_NOTEQUAL:
        write(Op.Output, A() != B());
        break;
      case NdOp::SELECT:
        write(Op.Output, A() ? B() : read(Op.Inputs[2]));
        break;
      case NdOp::LOAD: {
        const auto Access = lowMemoryOperands(Op);
        uint64_t Address = read(*Access.Address);
        if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
          const auto Base = SegmentBases.find(Op.MemoryAddressSpace);
          if (Base == SegmentBases.end())
            return std::nullopt;
          Address += Base->second;
        }
        if (Address > UINT64_MAX - (Access.AccessSize - 1))
          return std::nullopt;
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
        uint64_t Address = read(*Access.Address);
        if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
          const auto Base = SegmentBases.find(Op.MemoryAddressSpace);
          if (Base == SegmentBases.end())
            return std::nullopt;
          Address += Base->second;
        }
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
        if (!Defined || TimestampIndex != Timestamps.size() ||
            CPUQueryIndex != CPUQueries.size() ||
            MSRReadIndex != MSRReads.size() || !RuntimeOutputs.empty())
          return std::nullopt;
        if (FinalMemory)
          *FinalMemory = std::move(Memory);
        if (FinalMXCSR && MXCSR)
          *FinalMXCSR = *MXCSR;
        return Value;
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
        else if (Op.Inputs[0].Offset ==
                 static_cast<uint64_t>(Intrinsic::ReadCr8)) {
          if (!TaskPriority || *TaskPriority > 15)
            return std::nullopt;
          write(Op.Output, *TaskPriority);
        } else if (Op.Inputs[0].Offset ==
                   static_cast<uint64_t>(Intrinsic::WriteCr8)) {
          const auto Value = read(Op.Inputs[1]);
          if (!TaskPriority || Value > 15)
            return std::nullopt;
          TaskPriority = Value;
        } else if (Op.Inputs[0].Offset ==
                   static_cast<uint64_t>(Intrinsic::RequireAligned)) {
          if (Op.NumInputs != 3 || Op.Output.Size || Op.Inputs[0].Size != 2 ||
              Op.MemoryOrdering != NdMemoryOrdering::None ||
              Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
              Op.Inputs[1].Size != 8 || !Op.Inputs[2].isConst() ||
              Op.Inputs[2].Size != 8)
            return std::nullopt;
          const uint64_t Alignment = Op.Inputs[2].Offset;
          if (!Alignment || (Alignment & (Alignment - 1)) ||
              read(Op.Inputs[1]) % Alignment != 0)
            return std::nullopt;
        } else if (Op.Inputs[0].Offset ==
                   static_cast<uint64_t>(Intrinsic::Ldmxcsr)) {
          const auto Address = read(Op.Inputs[1]);
          if (!MXCSR || Address > UINT64_MAX - 3)
            return std::nullopt;
          uint32_t Word = 0;
          for (unsigned Byte = 0; Byte != 4; ++Byte) {
            const auto Found = Memory.find(Address + Byte);
            if (Found == Memory.end())
              return std::nullopt;
            Word |= uint32_t(Found->second) << (8 * Byte);
          }
          if (Word & ~WritableMask)
            return std::nullopt;
          MXCSR = Word;
        } else if (Op.Inputs[0].Offset ==
                   static_cast<uint64_t>(Intrinsic::X86ReadMXCSR)) {
          if (!MXCSR || (*MXCSR & ~WritableMask))
            return std::nullopt;
          write(Op.Output, *MXCSR);
        } else if (Op.Inputs[0].Offset ==
                   static_cast<uint64_t>(Intrinsic::X86WriteMXCSR)) {
          const auto Word = read(Op.Inputs[1]);
          if (!MXCSR || (Word & ~uint64_t(WritableMask)))
            return std::nullopt;
          MXCSR = static_cast<uint32_t>(Word);
        } else if (Op.Inputs[0].Offset ==
                   static_cast<uint64_t>(Intrinsic::Rdtsc)) {
          if (TimestampIndex == Timestamps.size() || !RuntimeOutputs.empty())
            return std::nullopt;
          const uint64_t Value = Timestamps[TimestampIndex++];
          write(Op.Output, Value);
          RuntimeOutputs = {uint32_t(Value), uint32_t(Value >> 32)};
        } else if (Op.Inputs[0].Offset ==
                   static_cast<uint64_t>(Intrinsic::Cpuid)) {
          if (CPUQueryIndex == CPUQueries.size() || !RuntimeOutputs.empty() ||
              Op.NumInputs != 3)
            return std::nullopt;
          const auto &Query = CPUQueries[CPUQueryIndex++];
          if (read(Op.Inputs[1]) != Query.Leaf ||
              read(Op.Inputs[2]) != Query.Subleaf)
            return std::nullopt;
          write(Op.Output, Query.Outputs[0]);
          RuntimeOutputs.assign(Query.Outputs.begin(), Query.Outputs.end());
        } else if (Op.Inputs[0].Offset ==
                   static_cast<uint64_t>(Intrinsic::Rdmsr)) {
          if (MSRReadIndex == MSRReads.size() || !RuntimeOutputs.empty() ||
              Op.NumInputs != 2)
            return std::nullopt;
          const auto &Read = MSRReads[MSRReadIndex++];
          if (Op.Inputs[1].Size != 4 || read(Op.Inputs[1]) != Read.Index)
            return std::nullopt;
          write(Op.Output, Read.Value);
        } else {
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

TEST(InterpreterSpecialization, TemporaryDispatchSnapshotsAreChargedAndScoped) {
  Provider P;
  const auto Target = NdVar::tmp(0, 8);
  P.add(0x100, {op(NdOp::INT_AND, Target, {reg(8), constant(3)}),
                op(NdOp::INT_LEFT, Target, {Target, constant(4)}),
                op(NdOp::INT_ADD, Target, {Target, constant(0x200)}),
                op(NdOp::COPY, reg(8), {constant(99)}),
                op(NdOp::INDIR_BR, {}, {Target})});
  for (uint64_t I = 0; I != 4; ++I)
    P.add(0x200 + I * 16, {op(NdOp::COPY, reg(0), {constant(I)}), ret()});
  const auto Good = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (uint64_t I = 0; I != 256; ++I) {
    EXPECT_EQ(execute(Good.Residual, {{8, I}}), I & 3);
    EXPECT_EQ(execute(Good.Residual, {{8, UINT64_MAX - I}}),
              (UINT64_MAX - I) & 3);
  }
  auto Unbound = Good.Residual;
  Unbound.FunctionTemporaries.clear();
  EXPECT_FALSE(execute(Unbound, {{8, 0}}));
  SpecializationOptions Exact;
  Exact.MaxOperations = Good.EvaluatedOperations;
  const auto Again = specializeInterpreter(P, {0x100}, Exact);
  ASSERT_TRUE(Again.complete()) << Again.Diagnostic;
  --Exact.MaxOperations;
  const auto Short = specializeInterpreter(P, {0x100}, Exact);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
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
     ExtendedOperandStorageRetainsUnsupportedEffectRefusal) {
  LowOp Opaque = op(NdOp::INTRINSIC, reg(0), {});
  for (size_t I = 0; I != std::size(Opaque.Inputs); ++I)
    Opaque.addInput(constant(0));
  Provider P;
  P.add(0x100, {Opaque});
  auto Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
  auto &Malformed = P.Code[0x100].Ops[0];
  Malformed.NumInputs = std::size(Malformed.Inputs) + 1;
  Result = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Result.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Result.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization,
     TrapIntrinsicsCannotFallThroughWithoutAnAuthenticatedExit) {
  for (Intrinsic Trap : {Intrinsic::Int3, Intrinsic::Int1})
    for (bool SameInstruction : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(Trap));
      SCOPED_TRACE(SameInstruction);
      Provider P;
      const auto Effect =
          op(NdOp::INTRINSIC, {}, {NdVar::cst(static_cast<uint64_t>(Trap), 2)});
      if (SameInstruction)
        P.add(0x100, {Effect, ret()});
      else {
        P.add(0x100, {Effect}, 0x101);
        P.add(0x101, {ret()});
      }
      const auto Result = specializeInterpreter(P, {0x100});
      EXPECT_EQ(Result.Status, SpecializationStatus::Unsupported);
      EXPECT_TRUE(Result.Residual.Blocks.empty());
      EXPECT_TRUE(Result.ExternalExits.empty());
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

TEST(InterpreterSpecialization, EntryObjectsRequirePreparedProviderEvidence) {
  Provider P;
  SpecializationOptions O;
  O.ExplicitMachineState = O.NormalNonfaultingExecution = O.X64CetDisabled =
      true;
  O.FrameBaseRegister = SymRegisterRange{x86reg::RSP, 8};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 8};
  O.EntryFrameExcludedObjects = {{{x86reg::RCX, 8}, 0, 32}};
  O.RequireRestoredFrameAtReturn = true;
  const auto R = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(R.Status, SpecializationStatus::InvalidInput);
  EXPECT_EQ(R.EvaluatedOperations, 0U);
  EXPECT_NE(R.Diagnostic.find("prepared provider"), std::string::npos);
  EXPECT_TRUE(R.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, EntryObjectPreparationAccountsForEveryRecord) {
  SpecializationOptions O;
  O.ExplicitMachineState = O.NormalNonfaultingExecution = O.X64CetDisabled =
      true;
  O.FrameBaseRegister = SymRegisterRange{x86reg::RSP, 8};
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 8};
  O.EntryFrameExcludedObjects = {{{x86reg::RCX, 8}, 0, 32},
                                 {{x86reg::RDX, 8}, -16, 16}};
  O.RequireRestoredFrameAtReturn = true;
  auto Cost = x64::validateRawEnvironmentDeclarations(O);
  ASSERT_TRUE(bool(Cost)) << llvm::toString(Cost.takeError());
  ASSERT_GT(Cost->Bytes, 0U);
  ASSERT_GT(Cost->Records, 0U);
  O.MaxImagePreparationBytes = Cost->Bytes;
  O.MaxImagePreparationRecords = Cost->Records;
  auto Exact = x64::validateRawEnvironmentDeclarations(O);
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  for (auto Member : {&SpecializationOptions::MaxImagePreparationBytes,
                      &SpecializationOptions::MaxImagePreparationRecords}) {
    auto Short = O;
    --(Short.*Member);
    auto Refused = x64::validateRawEnvironmentDeclarations(Short);
    ASSERT_FALSE(bool(Refused));
    EXPECT_NE(llvm::toString(Refused.takeError()).find("budget exhausted"),
              std::string::npos);
  }
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

TEST(InterpreterSpecialization, FirstUncertifiedAddressKeepsRuntimeRead) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_AND, reg(16), {reg(8), constant(15)}),
                op(NdOp::INT_MULT, reg(16), {reg(16), constant(8)}),
                op(NdOp::INT_ADD, reg(16), {reg(16), constant(0x800)}),
                op(NdOp::LOAD, reg(0), {reg(16)}), ret()});
  SpecializationOptions Options;
  Options.MaxSolverQueries = 4;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.SolverQueries, 1u);
  EXPECT_TRUE(Result.Reads.empty());
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 1u);
  for (uint64_t I = 0; I != 16; ++I)
    imageWord(P, 0x800 + 8 * I, I * 37 + 11);
  for (uint64_t I = 0; I != 64; ++I)
    EXPECT_EQ(execute(Result.Residual, {{8, I}}, P.Image), (I & 15) * 37 + 11);
}

TEST(InterpreterSpecialization,
     CachedAddressProofDoesNotCertifyADifferentReadExtent) {
  auto P = finiteAddressProvider();
  const auto RuntimeImage = P.Image;
  P.Image.erase(0x80F);
  P.add(0x100, {op(NdOp::INT_AND, reg(16), {reg(8), constant(1)}),
                op(NdOp::INT_MULT, reg(16), {reg(16), constant(8)}),
                op(NdOp::INT_ADD, reg(16), {reg(16), constant(0x800)}),
                op(NdOp::LOAD, reg(24, 1), {reg(16)}),
                op(NdOp::LOAD, reg(0), {reg(16)}), ret()});
  const auto Result = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(Result.SolverQueries, 3u);
  ASSERT_EQ(Result.Reads.size(), 2u);
  for (const auto &Read : Result.Reads)
    EXPECT_EQ(Read.Bytes.size(), 1u);
  EXPECT_EQ(count(Result.Residual, NdOp::LOAD), 1u);
  EXPECT_EQ(execute(Result.Residual, {{8, 0}}, RuntimeImage), 0x200u);
  EXPECT_EQ(execute(Result.Residual, {{8, 1}}, RuntimeImage), 0x210u);
  P.Image = RuntimeImage;
  P.MalformedReadSize = 8;
  const auto Malformed = specializeInterpreter(P, {0x100});
  EXPECT_EQ(Malformed.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Malformed.Residual.Blocks.empty());
  EXPECT_TRUE(Malformed.Reads.empty());
}

TEST(InterpreterSpecialization,
     ConstantAddressesReuseOnlyCompletedPredicateProof) {
  Provider P;
  std::vector<LowOp> Ops{op(NdOp::COPY, reg(0), {constant(5)})};
  uint64_t Expected = 5;
  for (uint64_t I = 0; I != 24; ++I) {
    const uint64_t Value = UINT64_C(0xA000000000000013) * I + 11;
    imageWord(P, 0xA00 + 8 * I, Value);
    Expected += Value;
    Ops.push_back(op(NdOp::LOAD, reg(8), {constant(0xA00 + 8 * I)}));
    Ops.push_back(op(NdOp::INT_ADD, reg(0), {reg(0), reg(8)}));
  }
  Ops.push_back(ret());
  P.add(0x100, std::move(Ops));
  SpecializationOptions O;
  O.ExplicitMachineState = true;
  O.FrameBaseRegister = SymRegisterRange{x86reg::RSP, 8};
  O.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
  O.MaxSolverQueries = 2;
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(R.SolverQueries, 2U);
  EXPECT_EQ(count(R.Residual, NdOp::LOAD), 0U);
  ASSERT_EQ(R.Reads.size(), 24U);
  for (size_t I = 0; I != R.Reads.size(); ++I) {
    const auto &Read = R.Reads[I];
    EXPECT_EQ(Read.Address, 0xA00 + 8 * I);
    ASSERT_EQ(Read.Bytes.size(), 8U);
    for (size_t B = 0; B != 8; ++B)
      EXPECT_EQ(Read.Bytes[B], P.Image.at(Read.Address + B));
  }
  for (uint64_t Stack : {UINT64_C(8), UINT64_C(0x2008), UINT64_MAX - 7})
    EXPECT_EQ(execute(R.Residual, {{x86reg::RSP, Stack}}), Expected);

  --O.MaxSolverQueries;
  const auto Short = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_EQ(Short.SolverQueries, 1U);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
  EXPECT_TRUE(Short.Origins.empty());
  EXPECT_TRUE(Short.Reads.empty());
}

TEST(InterpreterSpecialization,
     ConstantPredicateReuseStillRequiresEachReadCertificate) {
  Provider P;
  P.Image[0xA00] = 42;
  imageWord(P, 0xA08, UINT64_C(0xFEDCBA9876543210));
  const auto RuntimeImage = P.Image;
  P.Image.erase(0xA0F);
  P.add(0x100, {op(NdOp::LOAD, reg(8, 1), {constant(0xA00)}),
                op(NdOp::INT_ZEXT, reg(8), {reg(8, 1)}),
                op(NdOp::LOAD, reg(0), {constant(0xA08)}),
                op(NdOp::INT_ADD, reg(0), {reg(0), reg(8)}), ret()});
  SpecializationOptions O;
  O.ExplicitMachineState = true;
  O.FrameBaseRegister = SymRegisterRange{x86reg::RSP, 8};
  O.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
  O.MaxSolverQueries = 2;
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(R.SolverQueries, 2U);
  EXPECT_EQ(count(R.Residual, NdOp::LOAD), 1U);
  ASSERT_EQ(R.Reads.size(), 1U);
  EXPECT_EQ(R.Reads.front().Address, 0xA00U);
  EXPECT_EQ(R.Reads.front().Bytes, (std::vector<uint8_t>{42}));
  EXPECT_EQ(execute(R.Residual, {{x86reg::RSP, 0x2008}}, RuntimeImage),
            UINT64_C(0xFEDCBA987654323A));

  P.Image = RuntimeImage;
  P.MalformedReadSize = 8;
  const auto Malformed = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Malformed.Status, SpecializationStatus::InvalidInput);
  EXPECT_TRUE(Malformed.Residual.Blocks.empty());
  EXPECT_TRUE(Malformed.Reads.empty());

  // A rejected observation is not a completed predicate proof, even though
  // its first SAT witness existed. The next read still needs both queries.
  P.MalformedReadSize = 0;
  P.Image.erase(0xA00);
  const auto Incomplete = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Incomplete.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_EQ(Incomplete.SolverQueries, 2U);
  EXPECT_TRUE(Incomplete.Residual.Blocks.empty());
  EXPECT_TRUE(Incomplete.Reads.empty());
}

TEST(InterpreterSpecialization,
     ConstantPredicateReusePreservesInfeasibleControlEdges) {
  Provider P;
  P.Image[0xA00] = 19;
  P.Image[0xA10] = 71;
  imageWord(P, 0xA08, 101);
  P.MalformedReadSize = 8;
  P.add(0x100,
        {op(NdOp::LOAD, reg(0, 1), {constant(0xA00)}),
         op(NdOp::INT_AND, reg(16), {reg(x86reg::RSP), constant(15)}),
         op(NdOp::INT_EQUAL, reg(24, 1), {reg(16), constant(7)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(24, 1)})},
        0x140);
  P.add(0x120, {op(NdOp::LOAD, reg(0), {constant(0xA08)}), ret()});
  P.add(0x140, {op(NdOp::LOAD, reg(0, 1), {constant(0xA10)}),
                op(NdOp::INT_ZEXT, reg(0), {reg(0, 1)}), ret()});
  for (uint32_t Chaining : {0U, 8U}) {
    SpecializationOptions O;
    O.ExplicitMachineState = true;
    O.FrameBaseRegister = SymRegisterRange{x86reg::RSP, 8};
    O.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
    O.MaxChainedTransfers = Chaining;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::LOAD), 0U);
    ASSERT_EQ(R.Reads.size(), 2U);
    for (const auto &Read : R.Reads)
      EXPECT_NE(Read.Address, 0xA08U);
    EXPECT_EQ(execute(R.Residual, {{x86reg::RSP, 0x2008}}), 71U);

    O.EntryFrameAlignment = InterpreterEntryAlignment{16, 7};
    const auto Reachable = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(Reachable.Status, SpecializationStatus::InvalidInput);
    EXPECT_TRUE(Reachable.Residual.Blocks.empty());
    EXPECT_TRUE(Reachable.Reads.empty());
  }
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

TEST(InterpreterSpecialization,
     FiniteHighRootBitsRetainCompleteControlDestinations) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
    constexpr uint64_t FirstRoot = 0x1a00;
    constexpr uint64_t SecondRoot = 0x100001a00;
    constexpr uint64_t Displacement = 0x1600;
    Provider P;
    P.add(0x100,
          {op(NdOp::INT_SUB, reg(48), {reg(32), constant(Displacement)}),
           op(NdOp::INT_EQUAL, reg(64, 1), {reg(32), constant(FirstRoot)}),
           op(NdOp::INT_EQUAL, reg(65, 1), {reg(32), constant(SecondRoot)}),
           op(NdOp::INT_OR, reg(66, 1), {reg(64, 1), reg(65, 1)}),
           op(NdOp::COND_BR, {}, {constant(0x200), reg(66, 1)})},
          0x300);
    P.add(0x200, {op(NdOp::INDIR_BR, {}, {reg(48)})});
    P.add(0x300, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
    P.add(FirstRoot - Displacement,
          {op(NdOp::COPY, reg(0), {constant(29)}), ret()});
    P.add(SecondRoot - Displacement,
          {op(NdOp::COPY, reg(0), {constant(43)}), ret()});
    SpecializationOptions Options;
    Options.ByteOrder = Order;
    Options.FrameBaseRegister = SymRegisterRange{32, 8};
    Options.RequireRestoredFrameAtReturn = true;
    Options.ControlRegisters = {{32, 8}, {48, 8}};
    const auto Result = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_EQ(count(Result.Residual, NdOp::INDIR_BR), 0u);
    for (uint64_t Root : {uint64_t{0}, FirstRoot, SecondRoot, FirstRoot + 1,
                          SecondRoot + 1, UINT64_MAX}) {
      const uint64_t Expected = Root == FirstRoot    ? 29
                                : Root == SecondRoot ? 43
                                                     : 7;
      EXPECT_EQ(execute(Result.Residual, {{32, Root}}), Expected);
    }
    P.Code.erase(SecondRoot - Displacement);
    const auto Unsupported = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Unsupported.Status, SpecializationStatus::Unsupported);
    EXPECT_TRUE(Unsupported.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     FiniteAffineRootRelationRetainsEveryImmutableReadWitness) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_EQUAL, reg(64, 1), {reg(32), constant(0x1800)}),
         op(NdOp::INT_EQUAL, reg(65, 1), {reg(32), constant(0x1900)}),
         op(NdOp::INT_OR, reg(66, 1), {reg(64, 1), reg(65, 1)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(66, 1)})},
        0x140);
  P.add(0x120, {op(NdOp::INT_SUB, reg(48), {reg(32), constant(16)}),
                op(NdOp::LOAD, reg(0), {reg(48)}), ret()});
  P.add(0x140, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  for (unsigned I = 0; I < 8; ++I) {
    P.Image[0x1800 - 16 + I] = I ? 0 : 91;
    P.Image[0x1900 - 16 + I] = I ? 0 : 37;
  }
  SpecializationOptions Options;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.ControlRegisters = {{32, 8}};
  Options.MaxImmutableReadAddresses = 2;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  ASSERT_EQ(Result.Reads.size(), 2u);
  std::set<uint64_t> Addresses;
  for (const auto &Read : Result.Reads)
    Addresses.insert(Read.Address);
  EXPECT_EQ(Addresses, (std::set<uint64_t>{0x17f0, 0x18f0}));
  EXPECT_EQ(execute(Result.Residual, {{32, 0x1800}}, P.Image), 91u);
  EXPECT_EQ(execute(Result.Residual, {{32, 0x1900}}, P.Image), 37u);
  EXPECT_EQ(execute(Result.Residual, {{32, 0x1a00}}, P.Image), 7u);
}

TEST(InterpreterSpecialization, BoundedFrameReadsUseTheirOwnEnumerationLimit) {
  constexpr uint64_t Lower = uint64_t{1} << 63;
  constexpr uint64_t Upper = Lower + (uint64_t{2} << 32) - 1;
  constexpr uint64_t First = Lower + UINT32_MAX;
  constexpr uint64_t Second = First + (uint64_t{1} << 32);
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_EQUAL, reg(64, 1), {reg(32, 4), constant(UINT32_MAX, 4)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(64, 1)})},
        0x140);
  P.add(0x120, {op(NdOp::INT_SUB, reg(48), {reg(32), constant(16)}),
                op(NdOp::LOAD, reg(0), {reg(48)}), ret()});
  P.add(0x140, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  for (unsigned I = 0; I < 8; ++I) {
    P.Image[First - 16 + I] = I ? 0 : 91;
    P.Image[Second - 16 + I] = I ? 0 : 37;
  }
  for (uint32_t Tuples : {1, 2})
    for (uint32_t Reads : {1, 2})
      for (uint32_t Chain : {0, 8}) {
        SCOPED_TRACE(::testing::Message()
                     << Tuples << '/' << Reads << '/' << Chain);
        SpecializationOptions Options;
        Options.ExplicitMachineState = true;
        Options.FrameBaseRegister = SymRegisterRange{32, 8};
        Options.EntryFrameBounds = SpecializationEntryFrameBounds{
            std::numeric_limits<int64_t>::min(),
            static_cast<int64_t>(UINT64_MAX - Upper + 1)};
        // Keep the low-word guard independently when the full-word tuple
        // limit is one; the read has its own separate two-address budget.
        Options.ControlRegisters = {{32, 4}, {32, 8}};
        Options.MaxControlTuples = Tuples;
        Options.MaxImmutableReadAddresses = Reads;
        Options.MaxChainedTransfers = Chain;
        const auto Result = specializeInterpreter(P, {0x100}, Options);
        ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
        EXPECT_EQ(Result.Reads.size(), Reads == 2 ? 2U : 0U);
        EXPECT_EQ(count(Result.Residual, NdOp::LOAD), Reads == 2 ? 0U : 1U);
        EXPECT_EQ(execute(Result.Residual, {{32, First}}, P.Image), 91U);
        EXPECT_EQ(execute(Result.Residual, {{32, Second}}, P.Image), 37U);
        EXPECT_EQ(execute(Result.Residual, {{32, Lower}}, P.Image), 7U);
      }
}

TEST(InterpreterSpecialization, BoundedFrameDomainDoesNotProveReachability) {
  Provider P;
  // A low-word increment cannot equal its original value. Keep this as
  // arithmetic rather than a structurally constant false predicate, so the
  // large frame-domain refusal still needs the final joint feasibility query.
  P.add(0x100,
        {op(NdOp::INT_ADD, reg(48, 4), {reg(32, 4), constant(1, 4)}),
         op(NdOp::INT_EQUAL, reg(64, 1), {reg(48, 4), reg(32, 4)}),
         op(NdOp::COND_BR, {}, {constant(0xdead), reg(64, 1)})},
        0x140);
  P.add(0x140, {op(NdOp::COPY, reg(0), {constant(7)}), ret()});
  SpecializationOptions Options;
  Options.ExplicitMachineState = true;
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  Options.EntryFrameBounds = SpecializationEntryFrameBounds{-64, 8};
  Options.ControlRegisters = {{32, 8}};
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_EQ(count(Result.Residual, NdOp::COND_BR), 0U);
  EXPECT_TRUE(Result.Reads.empty());
  for (uint64_t Root : {uint64_t{0x10000}, uint64_t{0x1234ffffffff}})
    EXPECT_EQ(execute(Result.Residual, {{32, Root}}), 7U);
}

TEST(InterpreterSpecialization, NarrowGuardDoesNotEnumerateWideRuntimePayload) {
  Provider P;
  P.add(0x100,
        {op(NdOp::COPY, reg(72), {reg(8)}),
         op(NdOp::INT_AND, reg(64), {reg(72), constant(1)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(64, 1)})},
        0x140);
  P.add(0x120, {op(NdOp::INT_ADD, reg(0), {reg(72), constant(7)}), ret()});
  P.add(0x140, {op(NdOp::INT_XOR, reg(0), {reg(72), constant(17)}), ret()});
  SpecializationOptions Options;
  Options.ControlRegisters = {{72, 8}};
  Options.MaxSolverQueries = 8;
  const auto Result = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
  EXPECT_LE(Result.SolverQueries, 8U);
  EXPECT_TRUE(Result.Reads.empty());
  for (uint64_t High :
       {uint64_t{0}, uint64_t{0x123456789abcde00}, UINT64_MAX - 255})
    for (uint64_t Low : {0U, 5U, 17U, 255U}) {
      const auto Input = High | Low;
      EXPECT_EQ(execute(Result.Residual, {{8, Input}}),
                (Low & 1) ? Input + 7 : Input ^ 17);
    }
}

SpecializationOptions segmentedReadOptions() {
  SpecializationOptions Options;
  Options.ExplicitMachineState = true;
  Options.NormalNonfaultingExecution = true;
  Options.X64SegmentedReads = true;
  return Options;
}

LowOp segmentedLoad(NdMemoryAddressSpace Space, NdVar Output, NdVar Offset) {
  auto Load = op(NdOp::LOAD, Output, {Offset});
  Load.MemoryAddressSpace = Space;
  return Load;
}

TEST(InterpreterSpecialization, SegmentedReadRequiresExplicitContract) {
  for (auto Space :
       {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS}) {
    Provider P;
    P.add(0x100, {segmentedLoad(Space, reg(0), constant(16)), ret()});
    auto Options = segmentedReadOptions();
    Options.X64SegmentedReads = false;
    EXPECT_EQ(specializeInterpreter(P, {0x100}, Options).Status,
              SpecializationStatus::Unsupported);
    for (bool Explicit : {false, true}) {
      Options = segmentedReadOptions();
      Options.ExplicitMachineState = Explicit;
      Options.NormalNonfaultingExecution = !Explicit;
      const auto R = specializeInterpreter(P, {0x100}, Options);
      EXPECT_EQ(R.Status, SpecializationStatus::InvalidInput);
      EXPECT_TRUE(R.Residual.Blocks.empty());
    }
    const auto R = specializeInterpreter(P, {0x100}, segmentedReadOptions());
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::LOAD), 1u);
    EXPECT_TRUE(R.Reads.empty());
  }
}

TEST(InterpreterSpecialization, SegmentedOffsetsCannotCertifyImageReads) {
  for (auto Space :
       {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS}) {
    Provider P;
    imageWord(P, 0x300, 7);
    P.add(0x100, {op(NdOp::INT_ADD, reg(16), {constant(0x280), constant(0x80)}),
                  segmentedLoad(Space, reg(0), reg(16)), ret()});
    const auto R = specializeInterpreter(P, {0x100}, segmentedReadOptions());
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_TRUE(R.Reads.empty());
    ASSERT_EQ(count(R.Residual, NdOp::LOAD), 1u);
    for (const auto &Block : R.Residual.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::LOAD) {
          EXPECT_EQ(Op.MemoryAddressSpace, Space);
          ASSERT_TRUE(lowMemoryOperands(Op).Address->isConst());
          EXPECT_EQ(lowMemoryOperands(Op).Address->Offset, 0x300u);
        }
    for (uint64_t Base :
         std::initializer_list<uint64_t>{0, 0x1000, UINT64_MAX - 0x1ff}) {
      auto Memory = P.Image;
      const uint64_t Address = Base + 0x300;
      const uint64_t Value = Base ? 29 : 7;
      for (unsigned I = 0; I != 8; ++I)
        Memory[Address + I] = static_cast<uint8_t>(Value >> (8 * I));
      EXPECT_EQ(execute(R.Residual, {}, Memory, 0, {{Space, Base}}), Value);
    }
  }
}

TEST(InterpreterSpecialization, SegmentedOffsetCannotReuseFrameValue) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_ADD, reg(40), {reg(32), constant(16)}),
         op(NdOp::STORE, {}, {reg(40), constant(9)}),
         segmentedLoad(NdMemoryAddressSpace::X86GS, reg(0), reg(40)), ret()});
  auto Options = segmentedReadOptions();
  Options.FrameBaseRegister = SymRegisterRange{32, 8};
  const auto R = specializeInterpreter(P, {0x100}, Options);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_TRUE(R.Reads.empty());
  ASSERT_EQ(count(R.Residual, NdOp::LOAD), 1u);
  std::map<uint64_t, uint8_t> Memory;
  for (unsigned I = 0; I != 8; ++I)
    Memory[0x2110 + I] = I ? 0 : 27;
  EXPECT_EQ(execute(R.Residual, {{32, 0x2000}}, Memory, 0,
                    {{NdMemoryAddressSpace::X86GS, 0}}),
            9u);
  EXPECT_EQ(execute(R.Residual, {{32, 0x2000}}, Memory, 0,
                    {{NdMemoryAddressSpace::X86GS, 0x100}}),
            27u);
}

TEST(InterpreterSpecialization,
     SegmentedBasesMayAliasEachOtherAndOrdinaryStores) {
  Provider P;
  P.add(0x100,
        {segmentedLoad(NdMemoryAddressSpace::X86FS, reg(16, 1), constant(8)),
         op(NdOp::STORE, {}, {reg(8), constant(19, 1)}),
         segmentedLoad(NdMemoryAddressSpace::X86FS, reg(24, 1), constant(8)),
         segmentedLoad(NdMemoryAddressSpace::X86GS, reg(40, 1), constant(8)),
         op(NdOp::INT_ADD, reg(0), {reg(16, 1), reg(24, 1)}),
         op(NdOp::INT_ADD, reg(0), {reg(0), reg(40, 1)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, segmentedReadOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::LOAD), 3u);
  for (uint64_t FS : {0ULL, 0x2000ULL})
    for (uint64_t GS : {0ULL, 0x2000ULL})
      for (uint64_t Address : {8ULL, 0x2008ULL, 0x4000ULL}) {
        const uint64_t Before = FS ? 5 : 3;
        const uint64_t AfterFS = Address == FS + 8 ? 19 : Before;
        const uint64_t AfterGS = Address == GS + 8 ? 19 : (GS ? 5 : 3);
        EXPECT_EQ(execute(R.Residual, {{8, Address}}, {{8, 3}, {0x2008, 5}}, 0,
                          {{NdMemoryAddressSpace::X86FS, FS},
                           {NdMemoryAddressSpace::X86GS, GS}}),
                  Before + AfterFS + AfterGS);
      }
}

TEST(InterpreterSpecialization, SegmentedPointerStaysUnsafeAcrossSpillAndJoin) {
  for (auto Space : {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS})
    for (bool Reverse : {false, true})
      for (bool Partial : {false, true}) {
        Provider P;
        P.add(0x100, {op(NdOp::COND_BR, {}, {constant(0x110), reg(16, 1)})},
              0x120);
        P.add(Reverse ? 0x120 : 0x110,
              {segmentedLoad(Space, reg(40), constant(16)), jump(0x130)});
        P.add(Reverse ? 0x110 : 0x120,
              {op(NdOp::COPY, reg(40), {reg(8)}), jump(0x130)});
        P.add(0x130, {op(NdOp::COPY, reg(48), {reg(40)})});
        P.add(0x131, {Partial ? op(NdOp::COPY, reg(48, 1), {constant(1, 1)})
                              : op(NdOp::NOP, {}, {}),
                      op(NdOp::INT_ADD, reg(56), {reg(32), constant(16)}),
                      op(NdOp::STORE, {}, {reg(56), reg(48)}), jump(0x140)});
        P.add(0x140, {op(NdOp::LOAD, reg(64), {reg(56)}), jump(0x150)});
        P.add(0x150, {op(NdOp::STORE, {}, {reg(64), constant(7)}), ret()});
        auto Options = segmentedReadOptions();
        Options.FrameBaseRegister = SymRegisterRange{32, 8};
        Options.RequireRestoredFrameAtReturn = true;
        Options.ExternalStoresPreserveEntryReturnSlot = true;
        const auto R = specializeInterpreter(P, {0x100}, Options);
        EXPECT_EQ(R.Status, SpecializationStatus::Unsupported) << R.Diagnostic;
        EXPECT_NE(R.Diagnostic.find("address origin"), std::string::npos);
        EXPECT_TRUE(R.Residual.Blocks.empty());
      }
}

TEST(InterpreterSpecialization,
     SegmentedReadRetainsFeasibleBranchesAcrossNodes) {
  Provider P;
  P.add(0x100,
        {segmentedLoad(NdMemoryAddressSpace::X86FS, reg(16, 1), constant(0)),
         op(NdOp::INT_EQUAL, reg(24, 1), {reg(16, 1), constant(5, 1)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(24, 1)})},
        0x120);
  P.add(0x110,
        {segmentedLoad(NdMemoryAddressSpace::X86GS, reg(40, 1), constant(0)),
         op(NdOp::INT_EQUAL, reg(24, 1), {reg(16, 1), reg(40, 1)}),
         op(NdOp::COND_BR, {}, {constant(0x130), reg(24, 1)})},
        0x140);
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(2)}), ret()});
  P.add(0x130, {op(NdOp::COPY, reg(0), {constant(1)}), ret()});
  P.add(0x140, {op(NdOp::COPY, reg(0), {constant(0)}), ret()});
  auto Options = segmentedReadOptions();
  Options.ControlRegisters = {{16, 1}};
  for (uint32_t Chain : {0, 16}) {
    Options.MaxChainedTransfers = Chain;
    const auto R = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_GT(R.SolverQueries, 0u);
    for (uint8_t A : {5, 9})
      for (uint8_t B : {5, 11})
        EXPECT_EQ(execute(R.Residual, {}, {{0x2000, A}, {0x3000, B}}, 0,
                          {{NdMemoryAddressSpace::X86FS, 0x2000},
                           {NdMemoryAddressSpace::X86GS, 0x3000}}),
                  A == 5 ? uint64_t(A == B) : 2u);
    auto Exact = Options;
    Exact.MaxOperations = R.EvaluatedOperations;
    Exact.MaxSolverQueries = R.SolverQueries;
    EXPECT_TRUE(specializeInterpreter(P, {0x100}, Exact).complete());
    ASSERT_GT(R.EvaluatedOperations, 1u);
    --Exact.MaxOperations;
    const auto ShortOps = specializeInterpreter(P, {0x100}, Exact);
    EXPECT_EQ(ShortOps.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(ShortOps.Residual.Blocks.empty());
    Exact.MaxOperations = R.EvaluatedOperations;
    ASSERT_GT(Exact.MaxSolverQueries, 1u);
    --Exact.MaxSolverQueries;
    const auto ShortQueries = specializeInterpreter(P, {0x100}, Exact);
    EXPECT_EQ(ShortQueries.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(ShortQueries.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     SegmentedAdmissionRejectsMalformedAndOtherEffects) {
  const auto Space = NdMemoryAddressSpace::X86GS;
  std::vector<LowOp> Invalid{
      op(NdOp::LOAD, reg(0), {}),
      op(NdOp::LOAD, reg(0), {constant(0), constant(0), constant(0)}),
      op(NdOp::LOAD, reg(0), {reg(8), constant(0)}),
      op(NdOp::LOAD, constant(0), {constant(0)}),
      op(NdOp::LOAD, reg(0, 16), {constant(0)}),
      op(NdOp::LOAD, reg(0), {reg(8, 16)}),
      op(NdOp::STORE, {}, {constant(0), constant(7)}),
      op(NdOp::ATOMIC_XCHG, reg(0), {constant(0), constant(7)}),
      op(NdOp::COPY, reg(0), {constant(7)})};
  for (auto &Op : Invalid)
    Op.MemoryAddressSpace = Space;
  auto Ordered = segmentedLoad(Space, reg(0), constant(0));
  Ordered.MemoryOrdering = NdMemoryOrdering::Acquire;
  Invalid.push_back(Ordered);
  auto Unknown = segmentedLoad(static_cast<NdMemoryAddressSpace>(255), reg(0),
                               constant(0));
  Invalid.push_back(Unknown);
  for (auto Op : Invalid) {
    Provider P;
    P.add(0x100, {Op, ret()});
    const auto R = specializeInterpreter(P, {0x100}, segmentedReadOptions());
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
  Provider P;
  auto Valid = op(NdOp::LOAD, reg(0), {constant(0, 1), constant(16, 4)});
  Valid.MemoryAddressSpace = Space;
  P.add(0x100, {Valid, ret()});
  EXPECT_TRUE(
      specializeInterpreter(P, {0x100}, segmentedReadOptions()).complete());
}

SpecializationOptions taskPriorityOptions() {
  SpecializationOptions O;
  O.ExplicitMachineState = O.NormalNonfaultingExecution = O.X64CR8State = true;
  O.X64FlagsProfile = InterpreterMachineStateProfile::KernelX64NoFaultV1;
  return O;
}
LowOp readPriority(NdVar Output) {
  return op(NdOp::INTRINSIC, Output,
            {constant(static_cast<uint64_t>(Intrinsic::ReadCr8), 2)});
}
LowOp writePriority(NdVar Input) {
  return op(NdOp::INTRINSIC, {},
            {constant(static_cast<uint64_t>(Intrinsic::WriteCr8), 2), Input});
}

TEST(InterpreterSpecialization, TaskPriorityRequiresExplicitKernelDomain) {
  Provider P;
  P.add(0x100, {readPriority(reg(0)), ret()});
  EXPECT_EQ(specializeInterpreter(P, {0x100}).Status,
            SpecializationStatus::Unsupported);
  for (unsigned Missing = 0; Missing != 4; ++Missing) {
    auto O = taskPriorityOptions();
    if (Missing == 0)
      O.ExplicitMachineState = false;
    if (Missing == 1)
      O.NormalNonfaultingExecution = false;
    if (Missing == 2)
      O.X64FlagsProfile.reset();
    if (Missing == 3)
      O.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(R.Status, SpecializationStatus::InvalidInput);
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
  auto O = taskPriorityOptions();
  O.X64CR8State = false;
  EXPECT_EQ(specializeInterpreter(P, {0x100}, O).Status,
            SpecializationStatus::Unsupported);
}

TEST(InterpreterSpecialization, TaskPriorityReadIsBoundedButNeverAssumedZero) {
  Provider P;
  P.add(0x100,
        {readPriority(reg(0)),
         op(NdOp::INT_LESS, reg(16, 1), {constant(15), reg(0)}),
         op(NdOp::COND_BR, {}, {constant(0x200), reg(16, 1)})},
        0x110);
  P.add(0x110, {ret()}); // Out-of-domain destination deliberately absent.
  const auto R = specializeInterpreter(P, {0x100}, taskPriorityOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 1u);
  for (uint64_t Priority = 0; Priority != 16; ++Priority)
    EXPECT_EQ(execute(R.Residual, {}, {}, 0, {}, Priority), Priority);
}

TEST(InterpreterSpecialization,
     TaskPriorityRepeatedReadsAndWritesRemainEffects) {
  for (uint64_t Value = 0; Value != 16; ++Value) {
    Provider P;
    P.add(0x100, {writePriority(constant(Value))});
    P.add(0x101, {readPriority(reg(16))});
    P.add(0x102,
          {readPriority(reg(24)),
           op(NdOp::INT_EQUAL, reg(40, 1), {reg(16), reg(24)}),
           op(NdOp::COND_BR, {}, {constant(0x110), reg(40, 1)})},
          0x200);
    P.add(0x110, {op(NdOp::COPY, reg(0), {reg(24)}), ret()});
    const auto R = specializeInterpreter(P, {0x100}, taskPriorityOptions());
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 3u);
    for (uint64_t Entry : {0u, 3u, 15u})
      EXPECT_EQ(execute(R.Residual, {}, {}, 0, {}, Entry), Value);
  }
}

TEST(InterpreterSpecialization,
     TaskPriorityWritesNeedACompleteReservedBitProof) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_AND, reg(40), {reg(8), constant(15)}),
                writePriority(reg(40)), readPriority(reg(0)), ret()});
  const auto R = specializeInterpreter(P, {0x100}, taskPriorityOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 2u);
  for (uint64_t Input :
       std::initializer_list<uint64_t>{0, 1, 15, 16, 255, UINT64_MAX})
    EXPECT_EQ(execute(R.Residual, {{8, Input}}, {}, 0, {}, 7), Input & 15);
  auto Exact = taskPriorityOptions();
  Exact.MaxOperations = R.EvaluatedOperations;
  Exact.MaxSolverQueries = R.SolverQueries;
  ASSERT_GT(Exact.MaxSolverQueries, 1u);
  EXPECT_TRUE(specializeInterpreter(P, {0x100}, Exact).complete());
  --Exact.MaxOperations;
  const auto ShortOps = specializeInterpreter(P, {0x100}, Exact);
  EXPECT_EQ(ShortOps.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(ShortOps.Residual.Blocks.empty());
  Exact.MaxOperations = R.EvaluatedOperations;
  --Exact.MaxSolverQueries;
  const auto ShortQueries = specializeInterpreter(P, {0x100}, Exact);
  EXPECT_EQ(ShortQueries.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(ShortQueries.Residual.Blocks.empty());
  for (auto Invalid :
       {reg(8), constant(16), constant(255), constant(UINT64_MAX)}) {
    P = {};
    P.add(0x100, {writePriority(Invalid), ret()});
    const auto Bad = specializeInterpreter(P, {0x100}, taskPriorityOptions());
    EXPECT_EQ(Bad.Status, SpecializationStatus::Unsupported);
    EXPECT_NE(Bad.Diagnostic.find("reserved bits"), std::string::npos);
    EXPECT_TRUE(Bad.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, TaskPriorityWriteCanUseAProvedBranchDomain) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_LESS, reg(16, 1), {reg(8), constant(16)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(16, 1)})},
        0x120);
  P.add(0x110, {writePriority(reg(8)), readPriority(reg(0)), ret()});
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(99)}), ret()});
  auto O = taskPriorityOptions();
  O.ControlRegisters = {{8, 8}};
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Input :
       std::initializer_list<uint64_t>{0, 4, 15, 16, 255, UINT64_MAX})
    EXPECT_EQ(execute(R.Residual, {{8, Input}}, {}, 0, {}, 3),
              Input < 16 ? Input : 99);
}

TEST(InterpreterSpecialization,
     TaskPriorityProvenanceAndNodeRebuildStayConservative) {
  Provider P;
  P.add(0x100, {readPriority(reg(40)), jump(0x110)});
  P.add(0x110, {op(NdOp::STORE, {}, {reg(40), constant(7)}), ret()});
  auto O = taskPriorityOptions();
  O.FrameBaseRegister = SymRegisterRange{32, 8};
  O.RequireRestoredFrameAtReturn = true;
  O.ExternalStoresPreserveEntryReturnSlot = true;
  const auto Refused = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Refused.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Refused.Diagnostic.find("address origin"), std::string::npos);
  P = {};
  P.add(0x100, {writePriority(constant(5)), jump(0x110)});
  P.add(0x110,
        {readPriority(reg(16)),
         op(NdOp::INT_EQUAL, reg(24, 1), {reg(16), constant(5)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(24, 1)})},
        0x130);
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(1)}), ret()});
  P.add(0x130, {op(NdOp::COPY, reg(0), {constant(2)}), ret()});
  for (uint32_t Chain : {0, 8}) {
    O = taskPriorityOptions();
    O.MaxChainedTransfers = Chain;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    for (uint64_t Entry = 0; Entry != 16; ++Entry)
      EXPECT_EQ(execute(R.Residual, {}, {}, 0, {}, Entry), 1u);
  }
}

TEST(InterpreterSpecialization,
     TaskPriorityRejectsMalformedAndUnmodeledControls) {
  std::vector<LowOp> Invalid{
      readPriority(reg(0, 4)), readPriority(constant(0)),
      writePriority(reg(0, 4)), writePriority(reg(UINT64_MAX)),
      op(NdOp::INTRINSIC, reg(0),
         {constant(static_cast<uint64_t>(Intrinsic::ReadCr0), 2)})};
  auto Op = readPriority(reg(0));
  Op.Inputs[0].Size = 8;
  Invalid.push_back(Op);
  Op = readPriority(reg(0));
  Op.addInput(constant(0));
  Invalid.push_back(Op);
  Op = writePriority(reg(8));
  Op.Output = reg(0);
  Invalid.push_back(Op);
  Op = readPriority(reg(0));
  Op.MemoryOrdering = NdMemoryOrdering::Acquire;
  Invalid.push_back(Op);
  Op = readPriority(reg(0));
  Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  Invalid.push_back(Op);
  for (auto InvalidOp : Invalid) {
    Provider P;
    P.add(0x100, {InvalidOp, ret()});
    const auto R = specializeInterpreter(P, {0x100}, taskPriorityOptions());
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     TaskPriorityLoweringPreservesInvalidWriteState) {
  for (uint64_t Old = 0; Old != 16; ++Old)
    for (uint64_t New :
         std::initializer_list<uint64_t>{0, 4, 15, 16, UINT64_MAX}) {
      SymContext Ctx;
      SymState State(Ctx);
      SymExec Exec(Ctx, State);
      const auto Slot = reg(0);
      State.write(SymSpace::Register, Slot.Offset, Ctx.mkConst(64, Old));
      const auto T = analysis::detail::lowerX64TaskPriority(
          writePriority(constant(New)), Slot,
          [](uint16_t Bytes) { return NdVar::tmp(0x1000, Bytes); });
      ASSERT_TRUE(T);
      ASSERT_TRUE(T->Rejected);
      for (const auto &Scalar : T->Ops)
        ASSERT_EQ(Exec.step(Scalar), StepResult::Continue);
      const auto Value = Ctx.asConst(Exec.operandValue(Slot));
      const auto Bad = Ctx.asConst(Exec.operandValue(*T->Rejected));
      ASSERT_TRUE(Value);
      ASSERT_TRUE(Bad);
      EXPECT_EQ(Value->getZExtValue(), New < 16 ? New : Old);
      EXPECT_EQ(Bad->getZExtValue(), New >= 16);
    }
}

SpecializationOptions separatedStoreOptions() {
  auto O = ordinaryReturnOptions();
  O.ProveStoreReturnSlotSeparation = true;
  O.ExplicitMachineState = true;
  O.NormalNonfaultingExecution = true;
  O.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
  return O;
}

TEST(InterpreterSpecialization, StoreSeparationProvesCompleteAlignedExtent) {
  for (uint16_t Width : {1, 2, 4, 8}) {
    Provider P;
    P.add(0x100,
          {op(NdOp::LOAD, reg(40), {reg(8)}),
           op(NdOp::INT_AND, reg(40), {reg(40), constant(~uint64_t{15})}),
           op(NdOp::STORE, {}, {reg(40), constant(37, Width)}),
           op(NdOp::COPY, reg(0), {constant(37)}), ret()});
    auto O = separatedStoreOptions();
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::STORE), 1u);
    auto Default = O;
    Default.ProveStoreReturnSlotSeparation = false;
    EXPECT_EQ(specializeInterpreter(P, {0x100}, Default).Status,
              SpecializationStatus::Unsupported);
    for (uint64_t Root : {0x1008u, 0x2008u})
      for (uint64_t Pointer : {0x1000u, 0x2000u, 0x3000u}) {
        std::map<uint64_t, uint8_t> Memory;
        for (unsigned I = 0; I != 8; ++I)
          Memory[0x4000 + I] = static_cast<uint8_t>(Pointer >> (8 * I));
        EXPECT_EQ(execute(R.Residual, {{8, 0x4000}, {32, Root}}, Memory), 37u);
      }
    O.EntryFrameAlignment.reset();
    const auto NoDomain = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(NoDomain.Status, SpecializationStatus::Unsupported);
    EXPECT_TRUE(NoDomain.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, StoreSeparationChecksBothStraddlingEdges) {
  for (uint16_t Width : {1, 2, 4, 8})
    for (uint64_t Bias = 0; Bias != 16; ++Bias) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Bias);
      Provider P;
      P.add(0x100,
            {op(NdOp::LOAD, reg(40), {reg(8)}),
             op(NdOp::INT_AND, reg(40), {reg(40), constant(~uint64_t{15})}),
             op(NdOp::INT_ADD, reg(40), {reg(40), constant(Bias)}),
             op(NdOp::STORE, {}, {reg(40), constant(11, Width)}), ret()});
      const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
      // [aligned+Bias, +Width) must miss every possible [aligned+8, +16).
      const bool Disjoint = Bias + Width <= 8;
      EXPECT_EQ(R.complete(), Disjoint) << R.Diagnostic;
      if (!Disjoint) {
        EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
        EXPECT_TRUE(R.Residual.Blocks.empty());
      }
    }
}

TEST(InterpreterSpecialization, StoreSeparationKeepsOrdinaryAliasHavoc) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_ADD, reg(48), {reg(32), constant(8)}),
         op(NdOp::STORE, {}, {reg(48), constant(1)}),
         op(NdOp::LOAD, reg(40), {reg(8)}),
         op(NdOp::INT_AND, reg(40), {reg(40), constant(~uint64_t{15})}),
         op(NdOp::STORE, {}, {reg(40), constant(2)}),
         op(NdOp::LOAD, reg(0), {reg(48)}),
         op(NdOp::INT_EQUAL, reg(56, 1), {reg(0), constant(2)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(56, 1)})},
        0x120);
  P.add(0x110, {op(NdOp::COPY, reg(0), {constant(17)}), ret()});
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(19)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::COND_BR), 1u);
  for (uint64_t Pointer : {0x1010u, 0x2000u}) {
    std::map<uint64_t, uint8_t> Memory;
    for (unsigned I = 0; I != 8; ++I)
      Memory[0x4000 + I] = static_cast<uint8_t>(Pointer >> (8 * I));
    EXPECT_EQ(execute(R.Residual, {{8, 0x4000}, {32, 0x1008}}, Memory),
              Pointer == 0x1010 ? 17u : 19u);
  }
  // No proof of a preceding occurrence can bless the pointer's next use.
  P.add(0x110, {op(NdOp::INT_ADD, reg(40), {reg(40), constant(8)}),
                op(NdOp::STORE, {}, {reg(40), constant(3)}), ret()});
  const auto Refused =
      specializeInterpreter(P, {0x100}, separatedStoreOptions());
  EXPECT_EQ(Refused.Status, SpecializationStatus::Unsupported);
  EXPECT_TRUE(Refused.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, StoreSeparationUsesCurrentSymbolicRelation) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_AND, reg(40), {reg(8), constant(7)}),
                op(NdOp::INT_ADD, reg(40), {reg(40), constant(8)}),
                op(NdOp::INT_ADD, reg(40), {reg(40), reg(32)}),
                op(NdOp::STORE, {}, {reg(40), constant(23)}),
                op(NdOp::LOAD, reg(0), {reg(40)}), ret()});
  auto O = ordinaryReturnOptions();
  O.ProveStoreReturnSlotSeparation = true;
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Root :
       {uint64_t{0x1000}, uint64_t{0x8000000000000000}, UINT64_MAX - 31})
    for (uint64_t Index = 0; Index != 8; ++Index)
      EXPECT_EQ(execute(R.Residual, {{8, Index}, {32, Root}}), 23u);
  // A one-byte wider backward offset can touch the first control-slot byte.
  P.add(0x100, {op(NdOp::INT_AND, reg(40), {reg(8), constant(7)}),
                op(NdOp::INT_SUB, reg(40), {reg(40), constant(8)}),
                op(NdOp::INT_ADD, reg(40), {reg(40), reg(32)}),
                op(NdOp::STORE, {}, {reg(40), constant(23, 2)}), ret()});
  const auto Bad = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Bad.Status, SpecializationStatus::Unsupported);
}

TEST(InterpreterSpecialization, StoreSeparationNormalizesNarrowAddresses) {
  Provider P;
  P.add(0x100,
        {op(NdOp::LOAD, reg(40, 4), {reg(8)}),
         op(NdOp::INT_AND, reg(40, 4), {reg(40, 4), constant(0xfffffff0, 4)}),
         op(NdOp::STORE, {}, {reg(40, 4), constant(29)}),
         op(NdOp::COPY, reg(0), {constant(29)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (uint64_t Root : {uint64_t{0x1008}, UINT64_MAX - 7}) {
    std::map<uint64_t, uint8_t> Memory{
        {0x3000, 0xf0}, {0x3001, 0xff}, {0x3002, 0xff}, {0x3003, 0xff}};
    EXPECT_EQ(execute(R.Residual, {{8, 0x3000}, {32, Root}}, Memory), 29u);
  }
}

TEST(InterpreterSpecialization, StoreSeparationChargesProofBudgets) {
  Provider P;
  P.add(0x100, {op(NdOp::LOAD, reg(40), {reg(8)}),
                op(NdOp::INT_AND, reg(40), {reg(40), constant(~uint64_t{15})}),
                op(NdOp::STORE, {}, {reg(40), constant(37)}), ret()});
  auto O = separatedStoreOptions();
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_GT(R.SolverQueries, 1u);
  O.MaxSolverQueries = R.SolverQueries;
  EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  --O.MaxSolverQueries;
  auto Bad = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Bad.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Bad.Residual.Blocks.empty());
  O = separatedStoreOptions();
  O.MaxSolverGates = 1;
  Bad = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Bad.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Bad.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization,
     StoreSeparationHonorsBranchDomainAndZeroExtension) {
  for (bool HighRoot : {false, true}) {
    const uint64_t Root =
        HighRoot ? UINT64_C(0xffffffff80000000) : UINT64_C(0x80000000);
    Provider P;
    P.add(0x100,
          {op(NdOp::COPY, reg(0), {constant(31)}),
           op(NdOp::INT_EQUAL, reg(56, 1), {reg(32), constant(Root)}),
           op(NdOp::COND_BR, {}, {constant(0x110), reg(56, 1)})},
          0x120);
    P.add(0x110,
          {op(NdOp::LOAD, reg(40, 4), {reg(8)}),
           op(NdOp::INT_AND, reg(40, 4), {reg(40, 4), constant(0xfffffff0, 4)}),
           op(NdOp::INT_OR, reg(40, 4), {reg(40, 4), constant(0x80000000, 4)}),
           op(NdOp::STORE, {}, {reg(40, 4), constant(9)}), ret()});
    P.add(0x120, {ret()});
    auto O = ordinaryReturnOptions();
    O.ProveStoreReturnSlotSeparation = true;
    O.ControlRegisters = {{32, 8}};
    const auto R = specializeInterpreter(P, {0x100}, O);
    if (!HighRoot) {
      // Zero extension can hit this low root. Sign extension would falsely
      // prove separation, since the destination's sign bit is set.
      EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
      EXPECT_TRUE(R.Residual.Blocks.empty());
      continue;
    }
    // The branch bounds the frame root to the high address. Every zero-
    // extended 32-bit STORE is separated, even with its sign bit set.
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    for (uint64_t EntryRoot : {Root, uint64_t{0x1000}}) {
      std::map<uint64_t, uint8_t> Memory{
          {0x3000, 0}, {0x3001, 0}, {0x3002, 0}, {0x3003, 0x80}};
      EXPECT_EQ(execute(R.Residual, {{8, 0x3000}, {32, EntryRoot}}, Memory),
                31u);
    }
  }
}

// Byte-vector oracle independent of the symbolic expression implementation.
// The earlier scalar oracle intentionally remains limited to uint64 values.
struct WideByteOracle {
  using Key = std::pair<VnodeSpace, uint64_t>;
  std::map<Key, uint8_t> Registers;
  std::map<uint64_t, uint8_t> Memory;
  std::vector<va_t> AtomicAddresses;
  llvm::endianness Order;
  bool Valid = true;
  std::string Diagnostic;

  explicit WideByteOracle(llvm::endianness Order) : Order(Order) {}

  std::vector<uint8_t> read(NdVar V) {
    std::vector<uint8_t> Bytes(V.Size);
    for (unsigned I = 0; I < V.Size; ++I) {
      if (V.isConst()) {
        const unsigned Byte =
            Order == llvm::endianness::little ? I : V.Size - I - 1;
        Bytes[I] = Byte < 8 ? uint8_t(V.Offset >> (8 * Byte)) : 0;
      } else {
        const auto It = Registers.find({V.Space, V.Offset + I});
        if (It == Registers.end())
          Valid = false;
        else
          Bytes[I] = It->second;
      }
    }
    return Bytes;
  }
  uint64_t number(NdVar V) {
    if (!V.Size || V.Size > 8) {
      Valid = false;
      return 0;
    }
    const auto Bytes = read(V);
    uint64_t N = 0;
    for (unsigned I = 0; I < V.Size; ++I)
      N |= uint64_t(Bytes[I])
           << (8 * (Order == llvm::endianness::little ? I : V.Size - I - 1));
    return N;
  }
  void write(NdVar V, const std::vector<uint8_t> &Bytes) {
    if (V.Size != Bytes.size()) {
      Valid = false;
      return;
    }
    for (unsigned I = 0; I < V.Size; ++I)
      Registers[{V.Space, V.Offset + I}] = Bytes[I];
  }
  void word(NdVar V, uint64_t N) { write(V, read(constant(N, V.Size))); }

  bool run(const LowFunc &F) {
    int Block = 0;
    for (unsigned Step = 0; Step != 256; ++Step) {
      const auto &B = F.Blocks.at(Block);
      int Next = -1;
      std::optional<va_t> Instruction;
      for (const auto &O : B.Ops) {
        if (Instruction != O.Addr) {
          std::erase_if(Registers, [&](const auto &Entry) {
            return Entry.first.first == VnodeSpace::TEMP &&
                   !F.isFunctionTemporaryByte(Entry.first.second);
          });
          Instruction = O.Addr;
        }
        Diagnostic = "block " + std::to_string(Block) + ", " +
                     std::string(ndOpName(O.Opcode));
        switch (O.Opcode) {
        case NdOp::COPY:
          write(O.Output, read(O.Inputs[0]));
          break;
        case NdOp::CONCAT: {
          const auto High = read(O.Inputs[0]);
          const auto Low = read(O.Inputs[1]);
          if (High.size() + Low.size() != O.Output.Size)
            return false;
          auto Bytes = Order == llvm::endianness::little ? Low : High;
          const auto &Last = Order == llvm::endianness::little ? High : Low;
          Bytes.insert(Bytes.end(), Last.begin(), Last.end());
          write(O.Output, Bytes);
          break;
        }
        case NdOp::INT_ZEXT:
        case NdOp::INT_SEXT: {
          auto A = read(O.Inputs[0]);
          if (A.empty() || A.size() >= O.Output.Size)
            return false;
          if (Order == llvm::endianness::big)
            std::reverse(A.begin(), A.end());
          const uint8_t Fill =
              O.Opcode == NdOp::INT_SEXT && (A.back() & 0x80) ? 0xff : 0;
          A.resize(O.Output.Size, Fill);
          if (Order == llvm::endianness::big)
            std::reverse(A.begin(), A.end());
          write(O.Output, A);
          break;
        }
        case NdOp::INT_MULT: {
          auto A = read(O.Inputs[0]), B = read(O.Inputs[1]);
          if (A.size() != B.size() || A.size() != O.Output.Size)
            return false;
          if (Order == llvm::endianness::big) {
            std::reverse(A.begin(), A.end());
            std::reverse(B.begin(), B.end());
          }
          std::vector<uint8_t> Product(O.Output.Size, 0);
          for (unsigned I = 0; I < A.size(); ++I) {
            unsigned Carry = 0;
            for (unsigned J = 0; I + J < Product.size(); ++J) {
              const unsigned Digit =
                  Product[I + J] + unsigned(A[I]) * B[J] + Carry;
              Product[I + J] = uint8_t(Digit);
              Carry = Digit >> 8;
            }
          }
          if (Order == llvm::endianness::big)
            std::reverse(Product.begin(), Product.end());
          write(O.Output, Product);
          break;
        }
        case NdOp::SUBBYTES: {
          auto A = read(O.Inputs[0]);
          const auto Offset = number(O.Inputs[1]);
          if (Offset > A.size() || O.Output.Size > A.size() - Offset)
            return false;
          if (Order == llvm::endianness::big)
            std::reverse(A.begin(), A.end());
          std::vector<uint8_t> Part(A.begin() + Offset,
                                    A.begin() + Offset + O.Output.Size);
          if (Order == llvm::endianness::big)
            std::reverse(Part.begin(), Part.end());
          write(O.Output, Part);
          break;
        }
        case NdOp::INT_NOT: {
          auto A = read(O.Inputs[0]);
          for (auto &Byte : A)
            Byte = ~Byte;
          write(O.Output, A);
          break;
        }
        case NdOp::INT_AND:
        case NdOp::INT_OR:
        case NdOp::INT_XOR: {
          auto A = read(O.Inputs[0]);
          const auto V = read(O.Inputs[1]);
          if (A.size() != V.size())
            return false;
          for (unsigned I = 0; I < A.size(); ++I)
            A[I] = O.Opcode == NdOp::INT_AND  ? A[I] & V[I]
                   : O.Opcode == NdOp::INT_OR ? A[I] | V[I]
                                              : A[I] ^ V[I];
          write(O.Output, A);
          break;
        }
        case NdOp::INT_ADD:
          word(O.Output, number(O.Inputs[0]) + number(O.Inputs[1]));
          break;
        case NdOp::INT_SUB:
          word(O.Output, number(O.Inputs[0]) - number(O.Inputs[1]));
          break;
        case NdOp::INT_EQUAL:
        case NdOp::INT_NOTEQUAL:
          word(O.Output, (read(O.Inputs[0]) == read(O.Inputs[1])) ==
                             (O.Opcode == NdOp::INT_EQUAL));
          break;
        case NdOp::INT_SLESS: {
          const auto Bytes = O.Inputs[0].Size;
          if (!Bytes || Bytes > 8 || O.Inputs[1].Size != Bytes)
            return false;
          const uint64_t Sign = uint64_t{1} << (Bytes * 8 - 1);
          word(O.Output,
               (number(O.Inputs[0]) ^ Sign) < (number(O.Inputs[1]) ^ Sign));
          break;
        }
        case NdOp::ATOMIC_ADD:
        case NdOp::ATOMIC_XCHG:
        case NdOp::ATOMIC_CMPXCHG: {
          const bool Compare = O.Opcode == NdOp::ATOMIC_CMPXCHG;
          // Sequential byte semantics only; this is not a concurrency oracle.
          if (O.NumInputs != (Compare ? 3u : 2u) || O.Inputs[0].Size != 8 ||
              O.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
              O.MemoryOrdering != NdMemoryOrdering::SequentiallyConsistent ||
              (O.Output.Size != 1 && O.Output.Size != 2 && O.Output.Size != 4 &&
               O.Output.Size != 8) ||
              O.Inputs[1].Size != O.Output.Size ||
              (Compare && O.Inputs[2].Size != O.Output.Size))
            return false;
          const auto Address = number(O.Inputs[0]);
          const auto Expected = read(O.Inputs[1]);
          auto Desired = read(O.Inputs[Compare ? 2 : 1]);
          if (Address > UINT64_MAX - (O.Output.Size - 1))
            return false;
          std::vector<uint8_t> Old(O.Output.Size);
          for (unsigned I = 0; I < Old.size(); ++I) {
            const auto It = Memory.find(Address + I);
            if (It == Memory.end())
              return false;
            Old[I] = It->second;
          }
          if (O.Opcode == NdOp::ATOMIC_ADD) {
            unsigned Carry = 0;
            for (unsigned Byte = 0; Byte < Old.size(); ++Byte) {
              const unsigned I = Order == llvm::endianness::little
                                     ? Byte
                                     : Old.size() - Byte - 1;
              const unsigned Sum = Old[I] + Desired[I] + Carry;
              Desired[I] = uint8_t(Sum);
              Carry = Sum >> 8;
            }
          }
          if (!Compare || Old == Expected)
            for (unsigned I = 0; I < Old.size(); ++I)
              Memory[Address + I] = Desired[I];
          write(O.Output, Old);
          AtomicAddresses.push_back(O.Addr);
          break;
        }
        case NdOp::LOAD:
        case NdOp::STORE: {
          const auto M = lowMemoryOperands(O);
          const auto Address = number(*M.Address);
          if (O.Opcode == NdOp::STORE) {
            const auto V = read(*M.StoredValue);
            for (unsigned I = 0; I < V.size(); ++I)
              Memory[Address + I] = V[I];
          } else {
            std::vector<uint8_t> V(M.AccessSize);
            for (unsigned I = 0; I < V.size(); ++I) {
              const auto It = Memory.find(Address + I);
              if (It == Memory.end())
                return false;
              V[I] = It->second;
            }
            write(O.Output, V);
          }
          break;
        }
        case NdOp::SELECT:
          write(O.Output, read(O.Inputs[number(O.Inputs[0]) ? 1 : 2]));
          break;
        case NdOp::BRANCH:
        case NdOp::COND_BR:
        case NdOp::INDIR_BR: {
          const auto Address = number(O.Inputs[0]);
          const bool Taken = O.Opcode != NdOp::COND_BR || number(O.Inputs[1]);
          for (int S : B.Succs)
            if ((F.Blocks[S].StartAddr == Address) == Taken)
              Next = S;
          if (Next < 0 && B.Succs.size() == 1)
            Next = B.Succs.front();
          break;
        }
        case NdOp::RETURN:
          return Valid;
        case NdOp::NOP:
          break;
        default:
          return false;
        }
        if (!Valid)
          return false;
      }
      if (Next < 0) {
        if (B.Succs.size() != 1)
          return false;
        Next = B.Succs.front();
      }
      Block = Next;
    }
    return false;
  }
};

SpecializationOptions wideOptions() {
  SpecializationOptions O;
  O.ExplicitMachineState = O.NormalNonfaultingExecution = true;
  return O;
}

TEST(InterpreterSpecialization, WideTransportPreservesEveryByte) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    const auto A = reg(x86reg::XMM0, 16), B = reg(x86reg::XMM1, 16);
    Provider P;
    P.add(0x100,
          {op(NdOp::COPY, B, {A}), op(NdOp::STORE, {}, {constant(0x4000), B}),
           op(NdOp::INT_NOT, B, {B}),
           op(NdOp::INT_AND, B, {B, constant(0x8040201008040201, 16)}),
           op(NdOp::INT_OR, B, {B, A}),
           op(NdOp::INT_XOR, B, {B, constant(0x00ff00ff00ff00ff, 16)}),
           op(NdOp::STORE, {}, {constant(0x5000), B}),
           op(NdOp::LOAD, B, {constant(0x4000)}), ret()});
    auto O = wideOptions();
    O.ByteOrder = Order;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::STORE), 2u);
    EXPECT_EQ(count(R.Residual, NdOp::LOAD), 1u);
    for (unsigned Seed : {0U, 17U, 255U}) {
      WideByteOracle Oracle(Order);
      std::vector<uint8_t> Bytes(16), Expected(16);
      for (unsigned I = 0; I < 16; ++I)
        Bytes[I] = uint8_t(Seed + 29 * I);
      Oracle.write(A, Bytes);
      ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
      EXPECT_EQ(Oracle.read(B), Bytes);
      for (unsigned I = 0; I < 16; ++I) {
        const unsigned Byte = Order == llvm::endianness::little ? I : 15 - I;
        const uint8_t Mask =
            Byte < 8 ? uint8_t(uint64_t{0x8040201008040201} >> (Byte * 8)) : 0;
        const uint8_t Xor =
            Byte < 8 ? uint8_t(uint64_t{0x00ff00ff00ff00ff} >> (Byte * 8)) : 0;
        Expected[I] = ((uint8_t(~Bytes[I]) & Mask) | Bytes[I]) ^ Xor;
        EXPECT_EQ(Oracle.Memory[0x4000 + I], Bytes[I]);
        EXPECT_EQ(Oracle.Memory[0x5000 + I], Expected[I]);
      }
    }
  }
}

#include "WideImmutableReadTests.inc"

TEST(InterpreterSpecialization, WideMemoryAcceptsConstantSpaceToken) {
  const auto A = reg(x86reg::XMM0, 16), B = reg(x86reg::XMM1, 16);
  Provider P;
  P.add(0x100, {op(NdOp::STORE, {}, {constant(0, 1), reg(0), A}),
                op(NdOp::LOAD, B, {constant(0, 1), reg(0)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, wideOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  std::vector<uint8_t> Bytes(16);
  for (unsigned I = 0; I < 16; ++I)
    Bytes[I] = uint8_t(19 + 37 * I);
  WideByteOracle Oracle(llvm::endianness::little);
  Oracle.write(A, Bytes);
  Oracle.word(reg(0), 0x4000);
  ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
  EXPECT_EQ(Oracle.read(B), Bytes);
  // An independent executor must detect loss of only the upper half.
  auto Truncated = R.Residual;
  for (auto &Block : Truncated.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::STORE)
        Op.Inputs[2].Size = 8;
  WideByteOracle Negative(llvm::endianness::little);
  Negative.write(A, Bytes);
  Negative.word(reg(0), 0x4000);
  EXPECT_FALSE(Negative.run(Truncated));
}

TEST(InterpreterSpecialization, WideTransportRefinesHighHalfAcrossJoin) {
  const auto Vector = reg(x86reg::XMM0, 16), Reload = reg(x86reg::XMM1, 16);
  Provider P;
  P.add(0x100, {op(NdOp::COND_BR, {}, {constant(0x110), reg(8)})}, 0x101);
  for (auto [Address, Target] :
       {std::pair{0x101u, 0x200u}, std::pair{0x110u, 0x210u}})
    P.add(Address,
          {op(NdOp::COPY, reg(Vector.Offset, 8), {reg(16)}),
           op(NdOp::COPY, reg(Vector.Offset + 8, 8), {constant(Target)}),
           op(NdOp::INT_SUB, reg(40), {reg(32), constant(32)}),
           op(NdOp::STORE, {}, {reg(40), Vector}), jump(0x120)});
  P.add(0x120, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(32)}),
                op(NdOp::LOAD, Reload, {reg(40)}),
                op(NdOp::INDIR_BR, {}, {reg(Reload.Offset + 8, 8)})});
  P.add(0x200, {op(NdOp::COPY, reg(0), {constant(17)}), ret()});
  P.add(0x210, {op(NdOp::COPY, reg(0), {constant(31)}), ret()});
  auto O = wideOptions();
  O.FrameBaseRegister = SymRegisterRange{32, 8};
  O.DiscoverControlState = true;
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_GT(R.DiscoveredControlFields, 0u);
  ASSERT_GT(R.SolverQueries, 1u);
  O.MaxSolverQueries = R.SolverQueries;
  EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  --O.MaxSolverQueries;
  const auto Limited = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Limited.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Limited.Residual.Blocks.empty());
  for (unsigned Choice : {0U, 1U}) {
    WideByteOracle Oracle(llvm::endianness::little);
    Oracle.word(reg(8), Choice);
    Oracle.word(reg(16), 0xfedcba9876543210);
    Oracle.word(reg(32), 0x4000);
    ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
    EXPECT_EQ(Oracle.number(reg(0)), Choice ? 31u : 17u);
  }
}

TEST(InterpreterSpecialization, WideBitwiseHighHalfDrivesControl) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (auto Opcode :
         {NdOp::INT_NOT, NdOp::INT_AND, NdOp::INT_OR, NdOp::INT_XOR}) {
      const auto A = reg(x86reg::XMM0, 16), B = reg(x86reg::XMM1, 16);
      const unsigned High = Order == llvm::endianness::little ? 8 : 0;
      const unsigned Low = 8 - High;
      uint64_t First = 0xfedcba987654f240, Second = 0xfff;
      if (Opcode == NdOp::INT_NOT)
        First = ~uint64_t{0x240};
      if (Opcode == NdOp::INT_OR) {
        First = 0x200;
        Second = 0x40;
      }
      if (Opcode == NdOp::INT_XOR)
        Second = First ^ 0x240;
      const auto Compute =
          Opcode == NdOp::INT_NOT ? op(Opcode, A, {A}) : op(Opcode, A, {A, B});
      Provider P;
      P.add(0x100, {op(NdOp::COPY, reg(A.Offset + Low), {reg(16)}),
                    op(NdOp::COPY, reg(A.Offset + High), {constant(First)}),
                    op(NdOp::COPY, reg(B.Offset + Low), {reg(24)}),
                    op(NdOp::COPY, reg(B.Offset + High), {constant(Second)}),
                    Compute, op(NdOp::INDIR_BR, {}, {reg(A.Offset + High)})});
      P.add(0x240, {op(NdOp::COPY, reg(0), {constant(55)}), ret()});
      auto O = wideOptions();
      O.ByteOrder = Order;
      const auto R = specializeInterpreter(P, {0x100}, O);
      ASSERT_TRUE(R.complete()) << ndOpName(Opcode) << ": " << R.Diagnostic;
      for (uint64_t Input : {uint64_t{0}, UINT64_MAX}) {
        WideByteOracle Oracle(Order);
        Oracle.word(reg(16), Input);
        Oracle.word(reg(24), ~Input);
        ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
        EXPECT_EQ(Oracle.number(reg(0)), 55u);
      }
    }
}

TEST(InterpreterSpecialization, WideStoreChecksCompleteReturnExtent) {
  for (int Offset : {-32, -16, -15, -8, 0, 7, 8}) {
    Provider P;
    P.add(0x100,
          {op(NdOp::INT_ADD, reg(40), {reg(32), constant(uint64_t(Offset))}),
           op(NdOp::STORE, {}, {reg(40), reg(x86reg::XMM0, 16)}), ret()});
    auto O = ordinaryReturnOptions();
    O.ExplicitMachineState = O.NormalNonfaultingExecution = true;
    const auto R = specializeInterpreter(P, {0x100}, O);
    const bool Overlap = Offset < 8 && Offset + 16 > 0;
    EXPECT_EQ(R.complete(), !Overlap) << Offset << ": " << R.Diagnostic;
    if (Overlap)
      EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, WideUnknownStoreKeepsFrameAliasHavoc) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_SUB, reg(40), {reg(32), constant(16)}),
         op(NdOp::STORE, {}, {reg(40), constant(0x200)}),
         op(NdOp::STORE, {}, {reg(8), reg(x86reg::XMM0, 16)}),
         op(NdOp::LOAD, reg(0), {reg(40)}), op(NdOp::INDIR_BR, {}, {reg(0)})});
  P.add(0x200, {ret()});
  auto O = wideOptions();
  O.FrameBaseRegister = SymRegisterRange{32, 8};
  const auto R = specializeInterpreter(P, {0x100}, O);
  EXPECT_FALSE(R.complete());
  EXPECT_TRUE(R.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, WideAdmissionRejectsOtherShapes) {
  const auto A = reg(x86reg::XMM0, 16), B = reg(x86reg::XMM1, 16);
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    Provider P;
    P.add(0x100, {op(NdOp::COPY, B, {A}), ret()});
    auto O = wideOptions();
    O.ExplicitMachineState = Mode == 1;
    O.NormalNonfaultingExecution = Mode == 2;
    EXPECT_FALSE(specializeInterpreter(P, {0x100}, O).complete());
  }
  std::vector<LowOp> Bad = {
      op(NdOp::COPY, B, {reg(A.Offset, 8)}),
      op(NdOp::COPY, B, {}),
      op(NdOp::COPY, B, {A, A}),
      op(NdOp::INT_XOR, B, {A, constant(7, 8)}),
      op(NdOp::INT_ADD, B, {A, A}),
      op(NdOp::INT_LESS, reg(0, 1), {A, B}),
      op(NdOp::LOAD, B, {A}),
      op(NdOp::STORE, {}, {A, B}),
      op(NdOp::COPY, reg(UINT64_MAX - 7, 16), {A}),
      op(NdOp::LOAD, B, {}),
      op(NdOp::LOAD, B, {reg(0), reg(8)}),
      op(NdOp::LOAD, B, {constant(0), reg(8), reg(16)}),
      op(NdOp::STORE, {}, {A}),
      op(NdOp::STORE, {}, {reg(0), reg(8), A}),
      op(NdOp::STORE, {}, {constant(0), reg(8), A, reg(16)}),
      op(NdOp::STORE, reg(0), {reg(8), A}),
      op(NdOp::INDIR_BR, {}, {A}),
      op(NdOp::RETURN, {}, {A})};
  for (uint16_t Width : {9, 15, 17, 32})
    Bad.push_back(op(NdOp::COPY, reg(B.Offset, Width), {reg(A.Offset, Width)}));
  auto Segmented = op(NdOp::LOAD, B, {constant(0x188)});
  Segmented.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  Bad.push_back(Segmented);
  auto Ordered = op(NdOp::STORE, {}, {reg(0), A});
  Ordered.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  Bad.push_back(Ordered);
  for (const auto &Instruction : Bad) {
    Provider P;
    P.add(0x100, {Instruction, ret()});
    auto O = wideOptions();
    O.X64SegmentedReads = true;
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_FALSE(R.complete()) << ndOpName(Instruction.Opcode);
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, WideTransportHonorsBudgets) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_NOT, reg(x86reg::XMM1, 16), {reg(x86reg::XMM0, 16)}),
         op(NdOp::STORE, {}, {constant(0x4000), reg(x86reg::XMM1, 16)}),
         ret()});
  for (bool Nodes : {false, true}) {
    auto O = wideOptions();
    if (Nodes)
      O.MaxSymbolicNodes = 2;
    else
      O.MaxOperations = 1;
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(R.Status, SpecializationStatus::BudgetExceeded) << R.Diagnostic;
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     WideIntegerProductsPreserveHighHalfAndOverflow) {
  struct Case {
    uint64_t A, B, Low, High;
    bool Signed, Overflow;
  };
  const Case Cases[] = {
      {2, 3, 6, 0, true, false},
      {UINT64_MAX, 2, UINT64_MAX - 1, UINT64_MAX, true, false},
      {uint64_t{1} << 63, 1, uint64_t{1} << 63, UINT64_MAX, true, false},
      {uint64_t{1} << 63, UINT64_MAX, uint64_t{1} << 63, 0, true, true},
      {(uint64_t{1} << 63) - 1, 2, UINT64_MAX - 1, 0, true, true},
      {uint64_t{1} << 32, uint64_t{1} << 32, 0, 1, true, true},
      {2, 3, 6, 0, false, false},
      {UINT64_MAX, UINT64_MAX, 1, UINT64_MAX - 1, false, true},
      {UINT64_MAX, 2, UINT64_MAX - 1, 1, false, true},
      {uint64_t{1} << 63, 2, 0, 1, false, true}};
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (bool Signed : {false, true}) {
      Provider P;
      const auto A = NdVar::tmp(0x10000, 16), B = NdVar::tmp(0x10020, 16);
      const auto Full = NdVar::tmp(0x10040, 16);
      const auto Back = NdVar::tmp(0x10060, 16);
      const auto Extend = Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      P.add(0x100, {op(Extend, A, {reg(0)}), op(Extend, B, {reg(8)}),
                    op(NdOp::INT_MULT, Full, {A, B}),
                    op(NdOp::SUBBYTES, reg(16), {Full, constant(0, 4)}),
                    op(NdOp::SUBBYTES, reg(24), {Full, constant(8, 4)}),
                    op(Extend, Back, {reg(16)}),
                    op(NdOp::INT_NOTEQUAL, reg(40, 1), {Full, Back}),
                    op(NdOp::INT_EQUAL, reg(41, 1), {Full, Back}), ret()});
      auto O = wideOptions();
      O.ByteOrder = Order;
      const auto R = specializeInterpreter(P, {0x100}, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      EXPECT_EQ(count(R.Residual, NdOp::INT_MULT), 1U);
      EXPECT_EQ(count(R.Residual, NdOp::SUBBYTES), 2U);
      for (const auto &C : Cases) {
        if (C.Signed != Signed)
          continue;
        SCOPED_TRACE(::testing::Message()
                     << Signed << ": " << C.A << " * " << C.B);
        WideByteOracle Oracle(Order);
        Oracle.word(reg(0), C.A);
        Oracle.word(reg(8), C.B);
        ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
        EXPECT_EQ(Oracle.number(reg(16)), C.Low);
        EXPECT_EQ(Oracle.number(reg(24)), C.High);
        EXPECT_EQ(Oracle.number(reg(40, 1)), C.Overflow);
        EXPECT_EQ(Oracle.number(reg(41, 1)), !C.Overflow);
      }
    }
}

TEST(InterpreterSpecialization, WideIntegerUnknownHighHalfControlsBothPaths) {
  const auto A = reg(0x400, 16), B = reg(0x440, 16);
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (auto Compare : {NdOp::INT_EQUAL, NdOp::INT_NOTEQUAL}) {
      Provider P;
      P.add(0x100, {op(Compare, reg(32, 1), {A, B}),
                    op(NdOp::COND_BR, {}, {constant(0x200), reg(32, 1)})});
      P.add(0x101, {op(NdOp::COPY, reg(0), {constant(17)}), ret()});
      P.add(0x200, {op(NdOp::COPY, reg(0), {constant(31)}), ret()});
      auto O = wideOptions();
      O.ByteOrder = Order;
      const auto R = specializeInterpreter(P, {0x100}, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      ASSERT_GT(R.SolverQueries, 0U);
      EXPECT_EQ(count(R.Residual, NdOp::RETURN), 2U);
      O.MaxOperations = R.EvaluatedOperations;
      O.MaxSolverQueries = R.SolverQueries;
      EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
      for (bool Queries : {false, true}) {
        auto Limited = O;
        if (Queries)
          --Limited.MaxSolverQueries;
        else
          --Limited.MaxOperations;
        const auto Failure = specializeInterpreter(P, {0x100}, Limited);
        EXPECT_EQ(Failure.Status, SpecializationStatus::BudgetExceeded);
        EXPECT_TRUE(Failure.Residual.Blocks.empty());
      }
      for (bool HighDiffers : {false, true}) {
        WideByteOracle Oracle(Order);
        std::vector<uint8_t> Left(16, 0x5a), Right = Left;
        if (HighDiffers)
          Right[Order == llvm::endianness::little ? 15 : 0] ^= 0x80;
        Oracle.write(A, Left);
        Oracle.write(B, Right);
        ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
        EXPECT_EQ(Oracle.number(reg(0)),
                  (HighDiffers == (Compare == NdOp::INT_NOTEQUAL)) ? 31U : 17U);
      }
    }
}

TEST(InterpreterSpecialization, WideIntegerExtensionsAndNumericSlices) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint16_t Width = 1; Width <= 8; ++Width)
      for (bool Signed : {false, true}) {
        const auto Input = reg(0, Width), Wide = NdVar::tmp(0x10000, 16);
        Provider P;
        P.add(0x100,
              {op(Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Wide, {Input}),
               op(NdOp::SUBBYTES, reg(16), {Wide, constant(8, 1)}),
               op(NdOp::SUBBYTES, reg(24, 1), {Wide, constant(15, 4)}),
               op(NdOp::SUBBYTES, reg(32, Width), {Wide, constant(0, 8)}),
               ret()});
        auto O = wideOptions();
        O.ByteOrder = Order;
        const auto R = specializeInterpreter(P, {0x100}, O);
        ASSERT_TRUE(R.complete()) << R.Diagnostic;
        for (uint64_t Value : {uint64_t{0}, UINT64_MAX}) {
          WideByteOracle Oracle(Order);
          Oracle.word(Input, Value);
          ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
          EXPECT_EQ(Oracle.number(reg(16)), Signed && Value ? UINT64_MAX : 0);
          EXPECT_EQ(Oracle.number(reg(24, 1)), Signed && Value ? 255U : 0);
          EXPECT_EQ(Oracle.read(reg(32, Width)), Oracle.read(Input));
        }
      }
}

TEST(InterpreterSpecialization, WideIntegerConcatPreservesWordsAndAliases) {
  constexpr uint64_t High = 0x8172635445362718;
  constexpr uint64_t Low = 0x19a2b3c4d5e6f708;
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (unsigned Layout = 0; Layout != 5; ++Layout) {
      const auto A = reg(0x500), B = reg(0x508);
      const auto Wide = Layout == 0 ? NdVar::tmp(0x10000, 16)
                                    : reg(Layout == 1 ? 0x500 : 0x540, 16);
      const auto HighInput = Layout == 2 || Layout == 4 ? constant(High) : A;
      const auto LowInput = Layout == 3 || Layout == 4 ? constant(Low) : B;
      Provider P;
      P.add(0x100,
            {op(NdOp::CONCAT, Wide, {HighInput, LowInput}),
             op(NdOp::SUBBYTES, reg(0x580), {Wide, constant(0)}),
             op(NdOp::SUBBYTES, reg(0x588), {Wide, constant(8)}), ret()});
      auto O = wideOptions();
      O.ByteOrder = Order;
      const auto R = specializeInterpreter(P, {0x100}, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      EXPECT_EQ(count(R.Residual, NdOp::CONCAT), 1U);
      WideByteOracle Oracle(Order);
      Oracle.word(A, High);
      Oracle.word(B, Low);
      Oracle.word(reg(0x4f8), 0x1234);
      Oracle.word(reg(0x510), 0x5678);
      ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
      EXPECT_EQ(Oracle.number(reg(0x580)), Low);
      EXPECT_EQ(Oracle.number(reg(0x588)), High);
      EXPECT_EQ(Oracle.number(reg(0x4f8)), 0x1234U);
      EXPECT_EQ(Oracle.number(reg(0x510)), 0x5678U);
    }
}

/// Byte-vector concatenation retains every lane while constructing a 128-bit
/// value through intermediate whole-byte widths, including aliased operands.
TEST(InterpreterSpecialization, WideIntegerConcatIntermediateWidths) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint16_t Width = 9; Width <= 16; ++Width)
      for (uint16_t HighBytes = 1; HighBytes < Width; ++HighBytes)
        for (bool Aliased : {false, true}) {
          const uint16_t LowBytes = Width - HighBytes;
          const auto High = reg(0x500, HighBytes);
          const auto Low = reg(0x540, LowBytes);
          const auto Out = reg(Aliased ? High.Offset : 0x580, Width);
          Provider P;
          P.add(0x100, {op(NdOp::CONCAT, Out, {High, Low}), ret()});
          auto O = wideOptions();
          O.ByteOrder = Order;
          const auto R = specializeInterpreter(P, {0x100}, O);
          ASSERT_TRUE(R.complete()) << R.Diagnostic;
          WideByteOracle Actual(Order);
          std::vector<uint8_t> A(HighBytes), B(LowBytes);
          for (unsigned I = 0; I < A.size(); ++I)
            A[I] = uint8_t(0x81 + 17 * I);
          for (unsigned I = 0; I < B.size(); ++I)
            B[I] = uint8_t(0x3e - 29 * I);
          Actual.write(High, A);
          Actual.write(Low, B);
          auto Expected = Order == llvm::endianness::little ? B : A;
          const auto &Last = Order == llvm::endianness::little ? A : B;
          Expected.insert(Expected.end(), Last.begin(), Last.end());
          ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
          EXPECT_EQ(Actual.read(Out), Expected);
          EXPECT_EQ(Actual.read(Low), B);
          if (!Aliased)
            EXPECT_EQ(Actual.read(High), A);

          O.MaxOperations = R.EvaluatedOperations;
          EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
          --O.MaxOperations;
          const auto Short = specializeInterpreter(P, {0x100}, O);
          EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
          EXPECT_TRUE(Short.Residual.Blocks.empty());
        }
}

/// Signed saturating packs exercise the real lifter's progressive CONCAT
/// widths against independent arithmetic at both saturation boundaries.
TEST(InterpreterSpecialization, WideIntegerConcatDecodedSignedPacks) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  D.setStrict(true);
  for (bool Words : {false, true})
    for (bool Aliased : {false, true}) {
      const unsigned SourceBytes = Words ? 2 : 4;
      const unsigned ResultBytes = SourceBytes / 2;
      const int64_t Minimum = -(int64_t{1} << (8 * ResultBytes - 1));
      const int64_t Maximum = -Minimum - 1;
      const std::vector<int64_t> Values{-(int64_t{1} << (8 * SourceBytes - 1)),
                                        Minimum - 1,
                                        Minimum,
                                        -1,
                                        0,
                                        Maximum,
                                        Maximum + 1,
                                        (int64_t{1} << (8 * SourceBytes - 1)) -
                                            1};
      const uint8_t Bytes[]{0x66, 0x0f, uint8_t(Words ? 0x63 : 0x6b),
                            uint8_t(Aliased ? 0xc0 : 0xc1)};
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), 0x100, I),
                sizeof(Bytes));
      std::vector<LowOp> Ops;
      D.liftToLow(I, Ops);
      EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::CONCAT && Op.Output.Size > 8 &&
               Op.Output.Size < 16;
      }));
      Provider P;
      P.add(0x100, Ops, 0x104);
      P.Code.at(0x100).Origin.Size = sizeof(Bytes);
      P.add(0x104, {ret()});
      const auto R = specializeInterpreter(P, {0x100}, wideOptions());
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      for (unsigned Rotation = 0; Rotation < Values.size(); ++Rotation) {
        std::vector<uint8_t> A(16), B(16), Expected;
        for (unsigned Source = 0; Source != 2; ++Source)
          for (unsigned Lane = 0; Lane < 16 / SourceBytes; ++Lane) {
            const auto Value =
                Values[(Rotation + Lane + 3 * Source) % Values.size()];
            auto &Input = Source ? B : A;
            for (unsigned Byte = 0; Byte < SourceBytes; ++Byte)
              Input[Lane * SourceBytes + Byte] =
                  uint8_t(uint64_t(Value) >> (8 * Byte));
            const auto Packed = Source && Aliased
                                    ? Values[(Rotation + Lane) % Values.size()]
                                    : Value;
            const auto Saturated = std::clamp(Packed, Minimum, Maximum);
            for (unsigned Byte = 0; Byte < ResultBytes; ++Byte)
              Expected.push_back(uint8_t(uint64_t(Saturated) >> (8 * Byte)));
          }
        WideByteOracle Actual(llvm::endianness::little);
        Actual.write(reg(x86reg::XMM0, 16), A);
        Actual.write(reg(x86reg::XMM1, 16), B);
        Actual.word(reg(x86reg::CF, 1), 1);
        ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
        EXPECT_EQ(Actual.read(reg(x86reg::XMM0, 16)), Expected);
        EXPECT_EQ(Actual.read(reg(x86reg::XMM1, 16)), B);
        EXPECT_EQ(Actual.number(reg(x86reg::CF, 1)), 1u);
      }
    }
}

TEST(InterpreterSpecialization,
     WideIntegerConcatIntermediateWidthsRejectMalformedShapes) {
  const auto Wide = reg(0x500, 16);
  const std::vector<LowOp> Bad{
      op(NdOp::CONCAT, Wide, {reg(0x540, 0), reg(0x580, 16)}),
      op(NdOp::CONCAT, reg(0x500, 17), {reg(0x540, 9), reg(0x580, 8)}),
      op(NdOp::CONCAT, reg(0x500, 15), {reg(0x540, 9), reg(0x580, 7)}),
      op(NdOp::CONCAT, Wide, {reg(UINT64_MAX - 7, 9), reg(0x580, 7)}),
      op(NdOp::CONCAT, reg(UINT64_MAX - 8, 10),
         {reg(0x540, 9), reg(0x580, 1)})};
  for (const auto &Op : Bad) {
    Provider P;
    P.add(0x100, {Op, ret()});
    const auto R = specializeInterpreter(P, {0x100}, wideOptions());
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, WideIntegerConcatHighWordControlsBothPaths) {
  const auto Wide = NdVar::tmp(0x10000, 16);
  Provider P;
  P.add(0x100,
        {op(NdOp::CONCAT, Wide, {reg(0), reg(8)}),
         op(NdOp::SUBBYTES, reg(16), {Wide, constant(8)}),
         op(NdOp::INT_EQUAL, reg(24, 1), {reg(16), constant(0x8172)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(24, 1)})},
        0x110);
  P.add(0x110, {op(NdOp::COPY, reg(32), {constant(17)}), ret()});
  P.add(0x120, {op(NdOp::COPY, reg(32), {constant(31)}), ret()});
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    auto O = wideOptions();
    O.ByteOrder = Order;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::RETURN), 2U);
    for (uint64_t High : {uint64_t{0}, uint64_t{0x8172}, UINT64_MAX}) {
      WideByteOracle Oracle(Order);
      Oracle.word(reg(0), High);
      Oracle.word(reg(8), 0x8172);
      ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
      EXPECT_EQ(Oracle.number(reg(32)), High == 0x8172 ? 31U : 17U);
    }
  }
}

TEST(InterpreterSpecialization, WideIntegerConcatDecodedDwordShuffle) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  D.setStrict(true);
  const auto Source = reg(x86reg::XMM0, 16);
  const std::vector<uint8_t> Input{0x10, 0x91, 0x32, 0xd3, 0x54, 0x35,
                                   0xf6, 0x77, 0x88, 0x09, 0xaa, 0x4b,
                                   0xcc, 0xed, 0x6e, 0x1f};
  for (bool Aliased : {false, true})
    for (unsigned Selector = 0; Selector != 256; ++Selector) {
      const uint8_t Bytes[]{0x66, 0x0f, 0x70, uint8_t(Aliased ? 0xc0 : 0xc8),
                            uint8_t(Selector)};
      DecodedInsn I{};
      ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), 0x100, I),
                sizeof(Bytes));
      std::vector<LowOp> Ops;
      D.liftToLow(I, Ops);
      Provider P;
      P.add(0x100, Ops, 0x105);
      P.Code[0x100].Origin.Size = sizeof(Bytes);
      P.add(0x105, {ret()});
      const auto R = specializeInterpreter(P, {0x100}, wideOptions());
      ASSERT_TRUE(R.complete()) << Selector << ": " << R.Diagnostic;
      WideByteOracle Oracle(llvm::endianness::little);
      Oracle.write(Source, Input);
      Oracle.word(reg(x86reg::CF, 1), 1);
      ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
      std::vector<uint8_t> Expected(16);
      for (unsigned Lane = 0; Lane != 4; ++Lane)
        for (unsigned Byte = 0; Byte != 4; ++Byte)
          Expected[Lane * 4 + Byte] =
              Input[((Selector >> (2 * Lane)) & 3) * 4 + Byte];
      EXPECT_EQ(Oracle.read(reg(Aliased ? x86reg::XMM0 : x86reg::XMM1, 16)),
                Expected);
      if (!Aliased)
        EXPECT_EQ(Oracle.read(Source), Input);
      EXPECT_EQ(Oracle.number(reg(x86reg::CF, 1)), 1U);
    }
}

TEST(InterpreterSpecialization, WideIntegerConcatRejectsMalformedAndShortRuns) {
  const auto A = reg(0x500), B = reg(0x508), Wide = reg(0x540, 16);
  std::vector<LowOp> Bad{op(NdOp::CONCAT, Wide, {}),
                         op(NdOp::CONCAT, Wide, {A}),
                         op(NdOp::CONCAT, Wide, {A, B, B}),
                         op(NdOp::CONCAT, Wide, {reg(A.Offset, 4), B}),
                         op(NdOp::CONCAT, Wide, {A, reg(B.Offset, 16)}),
                         op(NdOp::CONCAT, reg(Wide.Offset, 9), {A, B}),
                         op(NdOp::CONCAT, constant(0, 16), {A, B}),
                         op(NdOp::CONCAT, NdVar::ram(0x600, 16), {A, B}),
                         op(NdOp::CONCAT, Wide, {NdVar::ram(0x600, 8), B}),
                         op(NdOp::CONCAT, reg(UINT64_MAX - 14, 16), {A, B}),
                         op(NdOp::CONCAT, Wide, {reg(UINT64_MAX - 6), B})};
  auto Ordered = op(NdOp::CONCAT, Wide, {A, B});
  Ordered.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  Bad.push_back(Ordered);
  auto Segmented = op(NdOp::CONCAT, Wide, {A, B});
  Segmented.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  Bad.push_back(Segmented);
  for (const auto &Instruction : Bad) {
    Provider P;
    P.add(0x100, {Instruction, ret()});
    const auto R = specializeInterpreter(P, {0x100}, wideOptions());
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
  Provider P;
  P.add(0x100, {op(NdOp::CONCAT, Wide, {A, B}), ret()});
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    auto O = wideOptions();
    O.ExplicitMachineState = Mode & 1;
    O.NormalNonfaultingExecution = Mode & 2;
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
  auto O = wideOptions();
  O.MaxOperations = 2;
  ASSERT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  O.MaxOperations = 1;
  const auto Short = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, WideIntegerConcatKeepsUnknownAddressOrigin) {
  const auto Wide = NdVar::tmp(0x10000, 16);
  Provider P;
  P.add(0x100, {op(NdOp::LOAD, reg(0), {reg(8)}),
                op(NdOp::CONCAT, Wide, {reg(0), constant(7)}),
                op(NdOp::SUBBYTES, reg(16), {Wide, constant(8)}),
                op(NdOp::STORE, {}, {reg(16), constant(42)}), ret()});
  auto O = wideOptions();
  O.FrameBaseRegister = SymRegisterRange{32, 8};
  O.RequireRestoredFrameAtReturn = true;
  O.ExternalStoresPreserveEntryReturnSlot = true;
  const auto R = specializeInterpreter(P, {0x100}, O);
  EXPECT_FALSE(R.complete());
  EXPECT_NE(R.Diagnostic.find("unknown address origin"), std::string::npos)
      << R.Diagnostic;
  EXPECT_TRUE(R.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, WideIntegerDecodedOverflowControlsResidual) {
  for (uint8_t Branch : {0x70, 0x72}) { // JO and JC
    const std::vector<uint8_t> Bytes{
        0x48,   0x0f, 0xaf, 0xc1,           // imul rax, rcx
        Branch, 6,                          // jo/jc overflow
        0xba,   17,   0,    0,    0, 0xc3,  // mov edx,17; ret
        0xba,   31,   0,    0,    0, 0xc3}; // overflow: mov edx,31; ret
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X64));
    D.setStrict(true);
    Provider P;
    for (size_t Offset = 0; Offset < Bytes.size();) {
      DecodedInsn I{};
      ASSERT_GT(D.decodeOneForLift(Bytes.data() + Offset, Bytes.size() - Offset,
                                   0x100 + Offset, I),
                0);
      std::vector<LowOp> Ops;
      D.liftToLow(I, Ops);
      P.add(I.Addr, Ops, I.Addr + I.Size);
      P.Code[I.Addr].Origin.Size = I.Size;
      Offset += I.Size;
    }
    const auto R = specializeInterpreter(P, {0x100}, wideOptions());
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::RETURN), 2U);
    // These have identical low products. Only the real signed overflow can
    // distinguish the residual outcomes; checking the low word is insufficient.
    for (bool Overflow : {false, true}) {
      WideByteOracle Oracle(llvm::endianness::little);
      Oracle.word(reg(x86reg::RAX),
                  Overflow ? (uint64_t{1} << 63) - 1 : UINT64_MAX);
      Oracle.word(reg(x86reg::RCX), 2);
      ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
      EXPECT_EQ(Oracle.number(reg(x86reg::RAX)), UINT64_MAX - 1);
      EXPECT_EQ(Oracle.number(reg(x86reg::RDX)), Overflow ? 31U : 17U);
      EXPECT_EQ(Oracle.number(reg(x86reg::CF, 1)), Overflow);
      EXPECT_EQ(Oracle.number(reg(x86reg::OF, 1)), Overflow);
    }
  }
}

TEST(InterpreterSpecialization, WideIntegerAdmissionRefusesMalformedShapes) {
  const auto A = reg(0x400, 16), B = reg(0x440, 16), C = reg(0x480, 16);
  std::vector<LowOp> Bad{
      op(NdOp::INT_SEXT, A, {}),
      op(NdOp::INT_SEXT, A, {reg(0), reg(8)}),
      op(NdOp::INT_SEXT, A, {reg(0, 0)}),
      op(NdOp::INT_ZEXT, A, {B}),
      op(NdOp::INT_SEXT, constant(0, 16), {reg(0)}),
      op(NdOp::INT_SEXT, reg(UINT64_MAX - 7, 16), {reg(0)}),
      op(NdOp::INT_MULT, C, {A, reg(B.Offset, 8)}),
      op(NdOp::INT_MULT, C, {A}),
      op(NdOp::INT_MULT, reg(C.Offset, 8), {A, B}),
      op(NdOp::INT_EQUAL, reg(0, 8), {A, B}),
      op(NdOp::INT_NOTEQUAL, reg(0, 1), {A, reg(B.Offset, 8)}),
      op(NdOp::INT_NOTEQUAL, reg(0, 1), {A, B, B}),
      op(NdOp::SUBBYTES, reg(0), {A, reg(16)}),
      op(NdOp::SUBBYTES, reg(0), {A, constant(9)}),
      op(NdOp::SUBBYTES, reg(0, 1), {A, constant(16)}),
      op(NdOp::SUBBYTES, reg(0), {A, constant(UINT64_MAX)}),
      op(NdOp::SUBBYTES, reg(0, 0), {A, constant(0)}),
      op(NdOp::SUBBYTES, C, {A, constant(0)}),
      op(NdOp::SUBBYTES, reg(0), {A, constant(0, 16)}),
      op(NdOp::INT_ADD, C, {A, B}),
      op(NdOp::INT_DIV, C, {A, B}),
      op(NdOp::INT_LEFT, C, {A, constant(1)}),
      op(NdOp::INDIR_BR, {}, {A})};
  for (uint16_t Width : {9, 15, 17, 32}) {
    Bad.push_back(op(NdOp::INT_SEXT, reg(A.Offset, Width), {reg(0)}));
    Bad.push_back(op(NdOp::INT_ZEXT, A, {reg(0, Width)}));
  }
  auto Segmented = op(NdOp::INT_MULT, C, {A, B});
  Segmented.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
  Bad.push_back(Segmented);
  auto Ordered = op(NdOp::INT_MULT, C, {A, B});
  Ordered.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  Bad.push_back(Ordered);
  for (const auto &Operation : Bad) {
    Provider P;
    P.add(0x100, {Operation, ret()});
    const auto R = specializeInterpreter(P, {0x100}, wideOptions());
    EXPECT_FALSE(R.complete()) << ndOpName(Operation.Opcode);
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
  for (unsigned Mode = 0; Mode != 4; ++Mode) {
    Provider P;
    P.add(0x100, {op(NdOp::INT_SEXT, A, {reg(0)}), ret()});
    auto O = wideOptions();
    O.ExplicitMachineState = Mode & 1;
    O.NormalNonfaultingExecution = Mode & 2;
    O.MaxSymbolicNodes = Mode == 3 ? 2 : O.MaxSymbolicNodes;
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
    if (Mode == 3)
      EXPECT_EQ(R.Status, SpecializationStatus::BudgetExceeded);
  }
}

TEST(InterpreterSpecialization, HighAddressEntriesKeepDisjointControlLabels) {
  constexpr va_t Half = uint64_t{1} << 63;
  for (va_t Entry : {va_t{0}, Half - 1, Half, Half + 1, InvalidVA - 2}) {
    Provider P;
    P.add(Entry, {op(NdOp::COPY, reg(0), {constant(Entry)}), ret()});
    const auto R = specializeInterpreter(P, {Entry});
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    ASSERT_FALSE(R.Residual.Blocks.empty());
    EXPECT_EQ(R.Residual.Entry, Entry);
    EXPECT_EQ(execute(R.Residual), Entry);
    for (size_t I = 1; I < R.Residual.Blocks.size(); ++I) {
      const auto &B = R.Residual.Blocks[I];
      EXPECT_TRUE(B.EndAddr <= Entry || B.StartAddr > Entry);
      EXPECT_LT(B.StartAddr, B.EndAddr);
      EXPECT_NE(B.StartAddr, InvalidVA);
      for (size_t J = I + 1; J < R.Residual.Blocks.size(); ++J) {
        const auto &Other = R.Residual.Blocks[J];
        EXPECT_TRUE(B.EndAddr <= Other.StartAddr ||
                    Other.EndAddr <= B.StartAddr);
      }
    }
  }
}

TEST(InterpreterSpecialization, HighAddressNativeExtentExcludesInteriorLabels) {
  constexpr va_t Half = uint64_t{1} << 63;
  Provider P;
  P.add(Half - 4, {op(NdOp::COPY, reg(0), {constant(Half)}), ret()}, Half + 4);
  P.Code.at(Half - 4).Origin.Size = 8;
  const auto R = specializeInterpreter(P, {Half - 4});
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(execute(R.Residual), Half);
  for (size_t I = 1; I < R.Residual.Blocks.size(); ++I) {
    const auto &B = R.Residual.Blocks[I];
    EXPECT_TRUE(B.EndAddr <= Half - 4 || B.StartAddr >= Half + 4);
  }
}

TEST(InterpreterSpecialization, HighAddressCallsPreserveNativeWordsAndStack) {
  constexpr va_t Half = uint64_t{1} << 63;
  for (const auto &[Entry, Callee] :
       {std::pair{va_t{0x100}, Half}, std::pair{Half - 1, va_t{0x200}},
        std::pair{Half, Half + 0x100}, std::pair{InvalidVA - 16, Half}}) {
    Provider P;
    P.add(Entry, {op(NdOp::CALL, reg(0), {constant(Callee)})});
    P.Code.at(Entry).NativeStackControl =
        SpecializationNativeStackControl::Call;
    P.add(Entry + 1, {ret()});
    P.Code.at(Entry + 1).NativeStackControl =
        SpecializationNativeStackControl::Return;
    P.add(Callee, {op(NdOp::LOAD, reg(0), {reg(32)}),
                   op(NdOp::INT_ADD, reg(0), {reg(0), reg(8)})});
    P.add(Callee + 1, {ret()});
    P.Code.at(Callee + 1).NativeStackControl =
        SpecializationNativeStackControl::Return;
    SpecializationOptions O;
    O.FrameBaseRegister = SymRegisterRange{32, 8};
    O.RequireRestoredFrameAtReturn = true;
    const auto R = specializeInterpreter(P, {Entry}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    for (uint64_t Input : {uint64_t{0}, uint64_t{37}, UINT64_MAX})
      for (va_t Stack : {va_t{0x10000}, va_t{0xffff800012341000}}) {
        WideByteOracle Oracle(llvm::endianness::little);
        Oracle.word(reg(8), Input);
        Oracle.word(reg(32), Stack);
        for (unsigned I = 0; I < 8; ++I)
          Oracle.Memory[Stack + I] = 0xa0 + I;
        ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
        EXPECT_EQ(Oracle.number(reg(0)), Entry + 1 + Input);
        EXPECT_EQ(Oracle.number(reg(32)), Stack);
        for (unsigned I = 0; I < 8; ++I) {
          EXPECT_EQ(Oracle.Memory.at(Stack - 8 + I),
                    static_cast<uint8_t>((Entry + 1) >> (8 * I)));
          EXPECT_EQ(Oracle.Memory.at(Stack + I), 0xa0 + I);
        }
      }
    O.MaxOperations = R.EvaluatedOperations;
    EXPECT_TRUE(specializeInterpreter(P, {Entry}, O).complete());
    --O.MaxOperations;
    const auto Short = specializeInterpreter(P, {Entry}, O);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Short.Residual.Blocks.empty());
  }
}

/// A joined pointer selects one of two saved roots. Discovery must retain its
/// proved producer relation before either an ordinary or native stack control.
TEST(InterpreterSpecialization, StackControlDiscoversSavedRootAddresses) {
  enum class Control { OrdinaryReturn, NativeReturn, NativeCall };
  for (auto Kind :
       {Control::OrdinaryReturn, Control::NativeReturn, Control::NativeCall})
    for (bool Spilled : {false, true})
      for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
        SCOPED_TRACE(static_cast<int>(Kind));
        SCOPED_TRACE(Spilled);
        SCOPED_TRACE(static_cast<int>(Order));
        Provider P;
        P.add(0x100,
              {op(NdOp::INT_SUB, reg(40), {reg(32), constant(16)}),
               op(NdOp::INT_SUB, reg(48), {reg(32), constant(8)}),
               op(NdOp::INT_SUB, reg(56), {reg(32), constant(24)}),
               op(NdOp::STORE, {}, {reg(40), reg(32)}),
               op(NdOp::STORE, {}, {reg(48), reg(32)}),
               op(NdOp::COND_BR, {}, {constant(0x110), reg(16, 1)})},
              0x120);
        const auto Save = [&](NdVar Value) {
          return Spilled ? op(NdOp::STORE, {}, {reg(56), Value})
                         : op(NdOp::COPY, reg(64), {Value});
        };
        P.add(0x110, {Save(reg(40)), jump(0x130)});
        P.add(0x120, {Save(reg(48)), jump(0x130)});
        std::vector<LowOp> Restore;
        if (Spilled)
          Restore.push_back(op(NdOp::LOAD, reg(64), {reg(56)}));
        Restore.push_back(op(NdOp::LOAD, reg(32), {reg(64)}));
        Restore.push_back(op(NdOp::COPY, reg(0), {reg(8)}));
        P.add(0x130, std::move(Restore));
        if (Kind == Control::NativeCall) {
          P.add(0x131, {op(NdOp::CALL, reg(0), {constant(0x200)})});
          P.Code.at(0x131).NativeStackControl =
              SpecializationNativeStackControl::Call;
          P.add(0x132, {ret()});
          P.Code.at(0x132).NativeStackControl =
              SpecializationNativeStackControl::Return;
          P.add(0x200, {op(NdOp::INT_ADD, reg(0), {reg(0), constant(1)})});
          P.add(0x201, {ret()});
          P.Code.at(0x201).NativeStackControl =
              SpecializationNativeStackControl::Return;
        } else {
          P.add(0x131, {ret()});
          if (Kind == Control::NativeReturn)
            P.Code.at(0x131).NativeStackControl =
                SpecializationNativeStackControl::Return;
        }
        SpecializationOptions O;
        O.FrameBaseRegister = SymRegisterRange{32, 8};
        O.RequireRestoredFrameAtReturn = true;
        O.ByteOrder = Order;
        const auto Without = specializeInterpreter(P, {0x100}, O);
        EXPECT_EQ(Without.Status, SpecializationStatus::Unsupported);
        EXPECT_TRUE(Without.Residual.Blocks.empty());

        O.DiscoverControlState = true;
        const auto R = specializeInterpreter(P, {0x100}, O);
        ASSERT_TRUE(R.complete()) << R.Diagnostic;
        EXPECT_GT(R.ControlRefinements, 0u);
        EXPECT_GT(R.DiscoveredControlFields, 0u);
        for (bool SelectFirst : {false, true})
          for (uint64_t Stack :
               {uint64_t{0}, uint64_t{0x10008}, uint64_t{0xffff900000001008}})
            for (uint64_t Input : {uint64_t{0}, uint64_t{37}, UINT64_MAX}) {
              WideByteOracle Actual(Order);
              Actual.word(reg(8), Input);
              Actual.word(reg(16, 1), SelectFirst);
              Actual.word(reg(32), Stack);
              for (int64_t Offset = -32; Offset != 16; ++Offset)
                Actual.Memory[Stack + uint64_t(Offset)] =
                    uint8_t(Offset ^ 0x5a);
              auto Expected = Actual.Memory;
              const uint64_t Pointer = Stack - (SelectFirst ? 16 : 8);
              const auto Store = [&](int64_t Offset, uint64_t Value) {
                for (unsigned I = 0; I != 8; ++I) {
                  const unsigned Shift =
                      8 * (Order == llvm::endianness::little ? I : 7 - I);
                  Expected[Stack + uint64_t(Offset) + I] =
                      uint8_t(Value >> Shift);
                }
              };
              Store(-16, Stack);
              Store(-8, Kind == Control::NativeCall ? 0x132 : Stack);
              if (Spilled)
                Store(-24, Pointer);
              ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
              EXPECT_EQ(Actual.number(reg(0)),
                        Input + (Kind == Control::NativeCall ? 1 : 0));
              EXPECT_EQ(Actual.number(reg(32)), Stack);
              EXPECT_EQ(Actual.number(reg(64)), Pointer);
              EXPECT_EQ(Actual.Memory, Expected);
            }
        auto Short = O;
        Short.MaxControlRefinements = 0;
        const auto Limited = specializeInterpreter(P, {0x100}, Short);
        EXPECT_EQ(Limited.Status, SpecializationStatus::BudgetExceeded);
        EXPECT_TRUE(Limited.Residual.Blocks.empty());

        // One arbitrary saved word cannot acquire the other word's root fact.
        P.Code.at(0x100).Ops[3].Inputs[1] = reg(24);
        const auto Unknown = specializeInterpreter(P, {0x100}, O);
        EXPECT_FALSE(Unknown.complete());
        EXPECT_TRUE(Unknown.Residual.Blocks.empty());
      }
}

TEST(InterpreterSpecialization,
     NativeReturnCellProofKeepsStoreAcrossRelativeZero) {
  // Keep the constant displacement separate from the symbolic base: the base
  // is 1 modulo 16, so base - 1 stores bytes [0, 8) of an aligned block.
  // Both physical return words occupy [8, 16), without a nonalias premise.
  const std::vector<LowOp> AddressOps{
      op(NdOp::LOAD, reg(40), {reg(x86reg::RDX)}),
      op(NdOp::INT_OR, reg(40), {reg(40), constant(1)}),
      op(NdOp::INT_AND, reg(40), {reg(40), constant(~uint64_t{14})}),
      op(NdOp::INT_SUB, reg(40), {reg(40), constant(1)})};
  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  for (const auto &O : AddressOps)
    ASSERT_EQ(Exec.step(O), StepResult::Continue);
  const auto Address = Exec.operandValue(reg(40));
  ASSERT_EQ(Ctx.width(Address), 64u);
  ASSERT_EQ(Ctx.op(Address), SymOp::Add);
  const auto Terms = Ctx.operands(Address);
  ASSERT_EQ(Terms.size(), 2u);
  ASSERT_TRUE(Ctx.isConst(Terms[0]));
  EXPECT_EQ(Ctx.constValue(Terms[0]).getZExtValue(), UINT64_MAX);
  EXPECT_FALSE(Ctx.isConst(Terms[1]));

  Provider P;
  P.add(0x100,
        {op(NdOp::INT_SUB, reg(x86reg::RSP), {reg(x86reg::RSP), constant(8)})});
  P.add(0x101, {op(NdOp::CALL, reg(0), {constant(0x200)})});
  P.Code.at(0x101).NativeStackControl = SpecializationNativeStackControl::Call;
  P.add(0x102,
        {op(NdOp::INT_ADD, reg(x86reg::RSP), {reg(x86reg::RSP), constant(8)})});
  P.add(0x103, {ret()});
  P.Code.at(0x103).NativeStackControl =
      SpecializationNativeStackControl::Return;
  auto Body = AddressOps;
  Body.push_back(op(NdOp::STORE, {}, {reg(40), reg(x86reg::RCX)}));
  Body.push_back(op(NdOp::LOAD, reg(0), {reg(40)}));
  P.add(0x200, std::move(Body));
  P.add(0x201, {ret()});
  P.Code.at(0x201).NativeStackControl =
      SpecializationNativeStackControl::Return;
  auto O = separatedStoreOptions();
  ASSERT_FALSE(O.ExternalStoresPreserveEntryReturnSlot);
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::STORE), 2u);
  EXPECT_EQ(count(R.Residual, NdOp::RETURN), 1u);

  for (uint64_t Stack : {uint64_t{0x8018}, uint64_t{0x9018}})
    for (uint64_t Pointer :
         {uint64_t{0x8000}, uint64_t{0x800f}, uint64_t{0x8010},
          uint64_t{0x801f}, uint64_t{0x400f}, uint64_t{0xffff80001234000f}})
      for (uint64_t Value :
           {uint64_t{0}, uint64_t{0x0123456789abcdef}, UINT64_MAX}) {
        SCOPED_TRACE(Stack);
        SCOPED_TRACE(Pointer);
        SCOPED_TRACE(Value);
        WideByteOracle Oracle(llvm::endianness::little);
        Oracle.word(reg(x86reg::RSP), Stack);
        Oracle.word(reg(x86reg::RCX), Value);
        Oracle.word(reg(x86reg::RDX), 0x4000);
        for (unsigned I = 0; I != 8; ++I) {
          Oracle.Memory[0x4000 + I] = uint8_t(Pointer >> (I * 8));
          Oracle.Memory[Stack + I] = 0xa0 + I;
        }
        ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
        EXPECT_EQ(Oracle.number(reg(0)), Value);
        EXPECT_EQ(Oracle.number(reg(x86reg::RSP)), Stack);
        const uint64_t Destination = Pointer & ~uint64_t{15};
        for (unsigned I = 0; I != 8; ++I) {
          EXPECT_EQ(Oracle.Memory.at(Destination + I),
                    uint8_t(Value >> (I * 8)));
          EXPECT_EQ(Oracle.Memory.at(Stack - 16 + I),
                    uint8_t(uint64_t{0x102} >> (I * 8)));
          EXPECT_EQ(Oracle.Memory.at(Stack + I), 0xa0 + I);
        }
      }

  O.ProveStoreReturnSlotSeparation = false;
  const auto NoProof = specializeInterpreter(P, {0x100}, O);
  EXPECT_FALSE(NoProof.complete());
  EXPECT_TRUE(NoProof.Residual.Blocks.empty());
  O.ProveStoreReturnSlotSeparation = true;
  O.EntryFrameAlignment.reset();
  const auto NoAlignment = specializeInterpreter(P, {0x100}, O);
  EXPECT_FALSE(NoAlignment.complete());
  EXPECT_TRUE(NoAlignment.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization,
     NativeReturnCellProofKeepsNarrowLowIRAddressConservative) {
  for (uint16_t AddressBytes : {4, 8}) {
    SCOPED_TRACE(AddressBytes);
    Provider P;
    P.add(0x100, {op(NdOp::INT_SUB, reg(x86reg::RSP),
                     {reg(x86reg::RSP), constant(8)})});
    P.add(0x101, {op(NdOp::CALL, reg(0), {constant(0x200)})});
    P.Code.at(0x101).NativeStackControl =
        SpecializationNativeStackControl::Call;
    P.add(0x102, {op(NdOp::INT_ADD, reg(x86reg::RSP),
                     {reg(x86reg::RSP), constant(8)})});
    P.add(0x103, {ret()});
    P.Code.at(0x103).NativeStackControl =
        SpecializationNativeStackControl::Return;
    // Unlike a decoded x64 addr32 instruction, this provider leaves the
    // executed address narrow. It cannot use the 64-bit preservation API.
    P.add(0x200, {op(NdOp::STORE, {},
                     {constant(0x8000, AddressBytes), reg(x86reg::RCX, 4)}),
                  op(NdOp::COPY, reg(0), {constant(37)})});
    P.add(0x201, {ret()});
    P.Code.at(0x201).NativeStackControl =
        SpecializationNativeStackControl::Return;
    auto O = separatedStoreOptions();
    O.ExternalStoresPreserveEntryReturnSlot = true;
    const auto R = specializeInterpreter(P, {0x100}, O);
    if (AddressBytes == 4) {
      EXPECT_EQ(R.Status, SpecializationStatus::UnresolvedControl)
          << R.Diagnostic;
      EXPECT_TRUE(R.Residual.Blocks.empty());
      continue;
    }
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    for (uint32_t Value : {uint32_t{0}, uint32_t{17}, UINT32_MAX}) {
      WideByteOracle Oracle(llvm::endianness::little);
      Oracle.word(reg(x86reg::RCX), Value);
      Oracle.word(reg(x86reg::RSP), 0x8018);
      ASSERT_TRUE(Oracle.run(R.Residual)) << Oracle.Diagnostic;
      EXPECT_EQ(Oracle.number(reg(0)), 37u);
      EXPECT_EQ(Oracle.number(reg(x86reg::RSP)), 0x8018u);
      for (unsigned I = 0; I != 4; ++I)
        EXPECT_EQ(Oracle.Memory.at(0x8000 + I), uint8_t(Value >> (I * 8)));
      for (unsigned I = 0; I != 8; ++I)
        EXPECT_EQ(Oracle.Memory.at(0x8008 + I),
                  uint8_t(uint64_t{0x102} >> (I * 8)));
    }
  }
}

TEST(InterpreterSpecialization,
     FrameNormalizationPreservesStoresAndNativeReturns) {
  for (auto [Order, Aligned] : {std::pair{llvm::endianness::little, false},
                                std::pair{llvm::endianness::little, true},
                                std::pair{llvm::endianness::big, false},
                                std::pair{llvm::endianness::big, true}}) {
    SCOPED_TRACE(Aligned);
    const uint64_t Displacement = Aligned ? 37 : 40;
    Provider P;
    P.add(0x100, {op(NdOp::CALL, reg(0), {constant(0x200)})});
    P.Code.at(0x100).NativeStackControl =
        SpecializationNativeStackControl::Call;
    P.add(0x101, {ret()});
    P.Code.at(0x101).NativeStackControl =
        SpecializationNativeStackControl::Return;
    std::vector<LowOp> Body{
        op(NdOp::INT_XOR, reg(0), {reg(32), reg(8)}),
        op(NdOp::INT_AND, reg(16), {reg(32), reg(8)}),
        op(NdOp::INT_MULT, reg(16), {reg(16), constant(2)}),
        op(NdOp::INT_ADD, reg(0), {reg(0), reg(16)}),
        op(NdOp::INT_SUB, reg(0), {reg(0), reg(8)}),
        op(NdOp::INT_ADD, reg(0), {reg(0), constant(uint64_t{0} - 40)})};
    if (Aligned) {
      Body.push_back(op(NdOp::INT_AND, reg(0), {reg(0), constant(-16)}));
      Body.push_back(op(NdOp::INT_ADD, reg(0), {reg(0), constant(11)}));
    }
    Body.insert(Body.end(),
                {op(NdOp::STORE, {}, {reg(0), reg(8)}),
                 op(NdOp::INT_ADD, reg(16), {reg(0), constant(3)}),
                 op(NdOp::STORE, {}, {reg(16), constant(0xa13c, 2)}),
                 op(NdOp::INT_ADD, reg(16),
                    {reg(32), constant(uint64_t{0} - Displacement)}),
                 op(NdOp::LOAD, reg(0), {reg(16)}),
                 op(NdOp::LOAD, reg(64), {reg(32)})});
    P.add(0x200, std::move(Body));
    P.add(0x201, {ret()});
    P.Code.at(0x201).NativeStackControl =
        SpecializationNativeStackControl::Return;
    SpecializationOptions O;
    O.FrameBaseRegister = SymRegisterRange{32, 8};
    O.RequireRestoredFrameAtReturn = true;
    O.ByteOrder = Order;
    O.MaxChainedTransfers = 8;
    O.MaxSolverConflicts = 1;
    if (Aligned) {
      O.ExplicitMachineState = true;
      O.EntryFrameAlignment = InterpreterEntryAlignment{16, 8};
      O.MaxSolverGates = 1024;
    }
    const auto Disabled = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(Disabled.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Disabled.Residual.Blocks.empty());
    O.MaxFrameOffsetNormalizationWorkPerProof = 65536;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_GT(R.EvaluatedOperations, Disabled.EvaluatedOperations);
    for (uint64_t Stack : {uint64_t{8}, uint64_t{0x10008},
                           uint64_t{0xffff900040000008}, UINT64_MAX - 7})
      for (uint64_t Input :
           {uint64_t{0}, uint64_t{0x123456789abcdef0}, UINT64_MAX}) {
        WideByteOracle Actual(Order);
        for (uint64_t Reg = 0; Reg < 128; Reg += 8)
          Actual.word(reg(Reg), 0xabcd000000000000 + Reg);
        Actual.word(reg(32), Stack);
        Actual.word(reg(8), Input);
        for (unsigned I = 0; I != 72; ++I)
          Actual.Memory[Stack - 56 + I] = I ^ 0x4d;
        auto Expected = Actual;
        const auto WriteMemory = [&](uint64_t At, uint64_t Value,
                                     unsigned Bytes) {
          for (unsigned I = 0; I != Bytes; ++I)
            Expected.Memory[At + I] = uint8_t(
                Value >>
                (8 * (Order == llvm::endianness::little ? I : Bytes - I - 1)));
        };
        WriteMemory(Stack - 8, 0x101, 8);
        WriteMemory(Stack - 8 - Displacement, Input, 8);
        WriteMemory(Stack - 5 - Displacement, 0xa13c, 2);
        uint64_t Result = 0;
        for (unsigned I = 0; I != 8; ++I)
          Result |= uint64_t(Expected.Memory.at(Stack - 8 - Displacement + I))
                    << (8 * (Order == llvm::endianness::little ? I : 7 - I));
        Expected.word(reg(0), Result);
        Expected.word(reg(16), Stack - 8 - Displacement);
        Expected.word(reg(64), 0x101);
        ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
        std::erase_if(Actual.Registers, [](const auto &V) {
          return V.first.first == VnodeSpace::TEMP;
        });
        EXPECT_EQ(Actual.Registers, Expected.Registers);
        EXPECT_EQ(Actual.Memory, Expected.Memory);
      }
    // Reducing the limit can let MBA finish an exact identity earlier. Find
    // adjacent successful/refused limits, rather than treating a generous
    // run's extra search as a mandatory lower bound.
    auto Exact = O;
    Exact.MaxOperations = R.EvaluatedOperations;
    bool FoundBoundary = false;
    for (unsigned Attempt = 0; Attempt != 128; ++Attempt) {
      const auto Boundary = specializeInterpreter(P, {0x100}, Exact);
      ASSERT_TRUE(Boundary.complete()) << Boundary.Diagnostic;
      ASSERT_LE(Boundary.EvaluatedOperations, Exact.MaxOperations);
      ASSERT_GT(Boundary.EvaluatedOperations, 1u);
      auto ShortOptions = Exact;
      ShortOptions.MaxOperations = Boundary.EvaluatedOperations - 1;
      const auto Short = specializeInterpreter(P, {0x100}, ShortOptions);
      if (!Short.complete()) {
        Exact.MaxOperations = ShortOptions.MaxOperations + 1;
        const auto Tight = specializeInterpreter(P, {0x100}, Exact);
        ASSERT_TRUE(Tight.complete()) << Tight.Diagnostic;
        EXPECT_LE(Tight.EvaluatedOperations, Exact.MaxOperations);
        const auto Repeated = specializeInterpreter(P, {0x100}, ShortOptions);
        EXPECT_EQ(Repeated.Status, SpecializationStatus::BudgetExceeded)
            << Repeated.Diagnostic;
        EXPECT_EQ(Short.Status, Repeated.Status);
        EXPECT_EQ(Short.EvaluatedOperations, Repeated.EvaluatedOperations);
        EXPECT_TRUE(Short.Residual.Blocks.empty());
        EXPECT_TRUE(Repeated.Residual.Blocks.empty());
        FoundBoundary = true;
        break;
      }
      ASSERT_LE(Short.EvaluatedOperations, ShortOptions.MaxOperations);
      Exact.MaxOperations = Short.EvaluatedOperations;
    }
    EXPECT_TRUE(FoundBoundary);
    O.MaxFrameOffsetNormalizationWorkPerProof = 1;
    const auto NoWork = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(NoWork.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_GT(NoWork.EvaluatedOperations, Disabled.EvaluatedOperations);
    EXPECT_TRUE(NoWork.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization,
     FrameNormalizationCacheStartsFreshAtIndependentNodes) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    Provider P;
    P.add(0x100, {op(NdOp::COND_BR, {}, {constant(0x200), reg(72, 1)})}, 0x300);
    const auto Body = [&](uint64_t Bias, uint64_t Tag) {
      return std::vector<LowOp>{
          op(NdOp::INT_XOR, reg(0), {reg(32), reg(8)}),
          op(NdOp::INT_AND, reg(16), {reg(32), reg(8)}),
          op(NdOp::INT_MULT, reg(16), {reg(16), constant(2)}),
          op(NdOp::INT_ADD, reg(0), {reg(0), reg(16)}),
          op(NdOp::INT_SUB, reg(0), {reg(0), reg(8)}),
          op(NdOp::INT_ADD, reg(0), {reg(0), constant(Bias)}),
          op(NdOp::STORE, {}, {reg(0), constant(Tag)}),
          op(NdOp::INT_ADD, reg(16), {reg(0), constant(8)}),
          op(NdOp::STORE, {}, {reg(16), constant(Tag + 1)}),
          op(NdOp::LOAD, reg(0), {reg(16)}),
          ret()};
    };
    P.add(0x200, Body(uint64_t{0} - 40, 0x1122334455667788));
    P.add(0x300, Body(24, 0x8899aabbccddeeff));
    SpecializationOptions Options;
    Options.FrameBaseRegister = SymRegisterRange{32, 8};
    Options.RequireRestoredFrameAtReturn = true;
    Options.ByteOrder = Order;
    Options.MaxChainedTransfers = 0;
    Options.MaxSolverConflicts = 1;
    const auto Disabled = specializeInterpreter(P, {0x100}, Options);
    EXPECT_EQ(Disabled.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Disabled.Residual.Blocks.empty());
    Options.MaxFrameOffsetNormalizationWorkPerProof = 65536;
    const auto R = specializeInterpreter(P, {0x100}, Options);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_GE(R.NodeEvaluations, 3u);
    for (bool Taken : {false, true})
      for (uint64_t Stack : {uint64_t{0}, uint64_t{0x10008}, UINT64_MAX - 7})
        for (uint64_t Data :
             {uint64_t{0}, uint64_t{0x123456789abcdef0}, UINT64_MAX}) {
          WideByteOracle Actual(Order);
          Actual.word(reg(32), Stack);
          Actual.word(reg(8), Data);
          Actual.word(reg(72, 1), Taken);
          for (unsigned I = 0; I != 96; ++I)
            Actual.Memory[Stack - 48 + I] = uint8_t(I ^ 0x4d);
          auto Expected = Actual.Memory;
          const uint64_t Bias = Taken ? uint64_t{0} - 40 : 24;
          const uint64_t Tag = Taken ? 0x1122334455667788 : 0x8899aabbccddeeff;
          for (unsigned Word = 0; Word != 2; ++Word)
            for (unsigned I = 0; I != 8; ++I)
              Expected[Stack + Bias + Word * 8 + I] = uint8_t(
                  (Tag + Word) >>
                  (8 * (Order == llvm::endianness::little ? I : 7 - I)));
          ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
          EXPECT_EQ(Actual.number(reg(0)), Tag + 1);
          EXPECT_EQ(Actual.number(reg(16)), Stack + Bias + 8);
          EXPECT_EQ(Actual.number(reg(32)), Stack);
          EXPECT_EQ(Actual.number(reg(8)), Data);
          EXPECT_EQ(Actual.Memory, Expected);
        }
  }
}

TEST(InterpreterSpecialization, HighAddressOverlappingOriginsShareExclusion) {
  constexpr va_t Half = uint64_t{1} << 63;
  Provider P;
  P.add(0x100, {op(NdOp::COND_BR, {}, {constant(Half - 3), reg(8, 1)})}, Half);
  P.add(Half - 3, {op(NdOp::COPY, reg(0), {constant(11)}), ret()}, Half + 5);
  P.add(Half, {op(NdOp::COPY, reg(0), {constant(17)}), ret()}, Half + 8);
  P.Code.at(Half - 3).Origin.Size = P.Code.at(Half).Origin.Size = 8;
  const auto R = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(execute(R.Residual, {{8, 0}}), 17U);
  EXPECT_EQ(execute(R.Residual, {{8, 1}}), 11U);
  for (size_t I = 1; I < R.Residual.Blocks.size(); ++I) {
    const auto &B = R.Residual.Blocks[I];
    EXPECT_TRUE(B.EndAddr <= Half - 3 || B.StartAddr >= Half + 8);
  }
}

TEST(InterpreterSpecialization, HighAddressLabelBitsDoNotRenameMemoryOrData) {
  constexpr va_t Half = uint64_t{1} << 63;
  Provider P;
  P.add(0x100, {op(NdOp::LOAD, reg(0), {constant(Half)}),
                op(NdOp::INT_XOR, reg(0), {reg(0), constant(Half)}), ret()});
  const auto R = specializeInterpreter(P, {0x100});
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  // Deliberately equal to the initial synthetic control label. Only control
  // operands may be renamed; these bits still name runtime memory and data.
  ASSERT_EQ(R.Residual.Blocks.at(1).StartAddr, Half);
  for (uint64_t Input : {uint64_t{0}, uint64_t{13}, UINT64_MAX}) {
    std::map<uint64_t, uint8_t> Memory;
    for (unsigned I = 0; I < 8; ++I)
      Memory[Half + I] = static_cast<uint8_t>(Input >> (8 * I));
    EXPECT_EQ(execute(R.Residual, {}, Memory), Input ^ Half);
  }
}

TEST(InterpreterSpecialization, HighAddressInvalidOrMissingCodeStillRefuses) {
  Provider P;
  P.add(0x100, {jump(InvalidVA)});
  P.add(InvalidVA - 1, {op(NdOp::NOP, {}, {})});
  P.add(uint64_t{1} << 63, {jump((uint64_t{1} << 63) + 10)});
  for (va_t Entry :
       {va_t{0x100}, InvalidVA, InvalidVA - 1, uint64_t{1} << 63}) {
    const auto R = specializeInterpreter(P, {Entry});
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
    EXPECT_TRUE(R.Origins.empty());
  }
  P.Code.at(InvalidVA - 1).Origin.Size = 2;
  P.Code.at(InvalidVA - 1).Fallthrough = {0};
  const auto Overflow = specializeInterpreter(P, {InvalidVA - 1});
  EXPECT_FALSE(Overflow.complete());
  EXPECT_TRUE(Overflow.Residual.Blocks.empty());
}

std::vector<va_t>
nativeAtomicAddresses(const SpecializationResult &R,
                      const std::vector<va_t> &ResidualAddresses) {
  std::vector<va_t> Native;
  for (va_t Address : ResidualAddresses) {
    unsigned Matches = 0;
    va_t Found = InvalidVA;
    for (const auto &Origin : R.Origins)
      if (Origin.ResidualAddress == Address) {
        ++Matches;
        Found = Origin.NativeInstruction.Address;
      }
    EXPECT_EQ(Matches, 1u) << "atomic residual address " << Address;
    Native.push_back(Found);
  }
  return Native;
}

LowOp atomicCmpxchg(NdVar Output, NdVar Address, NdVar Expected,
                    NdVar Desired) {
  auto O = op(NdOp::ATOMIC_CMPXCHG, Output, {Address, Expected, Desired});
  O.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  return O;
}

LowOp atomicWrite(NdOp Opcode, NdVar Output, NdVar Address, NdVar Value) {
  auto O = op(Opcode, Output, {Address, Value});
  O.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  return O;
}

Provider atomicNativeProvider(uint16_t Bytes = 8, uint64_t Bias = 0,
                              NdOp Opcode = NdOp::ATOMIC_CMPXCHG) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_SUB, reg(32), {reg(32), constant(8)})});
  P.add(0x101, {op(NdOp::CALL, reg(0), {constant(0x200)})});
  P.Code.at(0x101).NativeStackControl = SpecializationNativeStackControl::Call;
  P.add(0x102, {op(NdOp::INT_ADD, reg(32), {reg(32), constant(8)})});
  P.add(0x103, {ret()});
  P.Code.at(0x103).NativeStackControl =
      SpecializationNativeStackControl::Return;
  // The base is 1 modulo 16. The default target has displacement UINT64_MAX
  // from that base, and each native return word occupies residues [8, 16).
  std::vector<LowOp> Body{
      op(NdOp::LOAD, reg(40), {reg(16)}),
      op(NdOp::INT_OR, reg(40), {reg(40), constant(1)}),
      op(NdOp::INT_AND, reg(40), {reg(40), constant(~uint64_t{14})}),
      op(NdOp::INT_SUB, reg(40), {reg(40), constant(1)})};
  if (Bias)
    Body.push_back(op(NdOp::INT_ADD, reg(40), {reg(40), constant(Bias)}));
  Body.push_back(
      Opcode == NdOp::ATOMIC_CMPXCHG
          ? atomicCmpxchg(reg(0, Bytes), reg(40), reg(8, Bytes), reg(64, Bytes))
          : atomicWrite(Opcode, reg(0, Bytes), reg(40), reg(64, Bytes)));
  Body.push_back(op(NdOp::LOAD, reg(48, Bytes), {reg(40)}));
  P.add(0x200, std::move(Body));
  P.add(0x201, {ret()});
  P.Code.at(0x201).NativeStackControl =
      SpecializationNativeStackControl::Return;
  return P;
}

TEST(InterpreterSpecialization,
     AtomicCmpxchgKeepsNativeReturnWordsAndOrderedEffects) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (uint16_t Bytes : {1, 2, 4, 8}) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(Order == llvm::endianness::little);
      auto P = atomicNativeProvider(Bytes);
      auto O = separatedStoreOptions();
      O.ByteOrder = Order;
      ASSERT_FALSE(O.ExternalStoresPreserveEntryReturnSlot);
      const auto R = specializeInterpreter(P, {0x100}, O);
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      EXPECT_EQ(count(R.Residual, NdOp::ATOMIC_CMPXCHG), 1u);
      EXPECT_EQ(count(R.Residual, NdOp::STORE), 1u); // Native return word only.
      EXPECT_EQ(count(R.Residual, NdOp::RETURN), 1u);
      for (const auto &B : R.Residual.Blocks)
        for (const auto &A : B.Ops)
          if (A.Opcode == NdOp::ATOMIC_CMPXCHG) {
            EXPECT_EQ(nativeAtomicAddresses(R, {A.Addr}),
                      std::vector<va_t>{0x200});
            EXPECT_EQ(A.NumInputs, 3u);
            EXPECT_EQ(A.Output.Size, Bytes);
            EXPECT_EQ(A.Inputs[0].Size, 8u);
            EXPECT_EQ(A.MemoryOrdering,
                      NdMemoryOrdering::SequentiallyConsistent);
            EXPECT_EQ(A.MemoryAddressSpace, NdMemoryAddressSpace::Default);
          }
      // Targets can occupy either side of the live native return word, or a
      // separate allocation. Full memory comparison also checks guard bytes.
      for (uint64_t Target : {0x8000u, 0x8010u, 0x6000u})
        for (unsigned Pattern = 0; Pattern != 3; ++Pattern)
          for (bool Match : {false, true}) {
            SCOPED_TRACE(Target);
            SCOPED_TRACE(Pattern);
            SCOPED_TRACE(Match);
            constexpr uint64_t Stack = 0x8018;
            std::vector<uint8_t> Old(Bytes), Desired(Bytes);
            for (unsigned I = 0; I < Bytes; ++I) {
              Old[I] = Pattern == 0 ? 0 : Pattern == 1 ? 0xff : 0x31 + 7 * I;
              Desired[I] = 0xe2 - 9 * I;
            }
            auto Expected = Old;
            if (!Match)
              Expected.back() ^= 0x80;
            WideByteOracle Actual(Order);
            Actual.word(reg(32), Stack);
            Actual.word(reg(16), 0x4000);
            Actual.write(reg(8, Bytes), Expected);
            Actual.write(reg(64, Bytes), Desired);
            for (uint64_t A = Stack - 32; A != Stack + 24; ++A)
              Actual.Memory[A] = uint8_t(0x90 + A - Stack);
            for (uint64_t A = Target - 1; A != Target + Bytes + 1; ++A)
              Actual.Memory[A] = 0xa5;
            for (unsigned I = 0; I < Bytes; ++I)
              Actual.Memory[Target + I] = Old[I];
            for (unsigned I = 0; I != 8; ++I) {
              const unsigned Shift =
                  8 * (Order == llvm::endianness::little ? I : 7 - I);
              Actual.Memory[0x4000 + I] = uint8_t((Target | 15) >> Shift);
            }
            auto ExpectedMemory = Actual.Memory;
            for (unsigned I = 0; I < Bytes; ++I)
              ExpectedMemory[Target + I] = Match ? Desired[I] : Old[I];
            for (unsigned I = 0; I != 8; ++I) {
              const unsigned Shift =
                  8 * (Order == llvm::endianness::little ? I : 7 - I);
              ExpectedMemory[Stack - 16 + I] =
                  uint8_t(uint64_t{0x102} >> Shift);
            }
            ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
            EXPECT_EQ(Actual.read(reg(0, Bytes)), Old);
            EXPECT_EQ(Actual.read(reg(48, Bytes)), Match ? Desired : Old);
            EXPECT_EQ(Actual.number(reg(32)), Stack);
            EXPECT_EQ(Actual.Memory, ExpectedMemory);
            EXPECT_EQ(nativeAtomicAddresses(R, Actual.AtomicAddresses),
                      std::vector<va_t>{0x200});
          }
    }
}

TEST(InterpreterSpecialization,
     AtomicCmpxchgRetainsFailureAndUnusedResultEffects) {
  for (bool Immutable : {false, true}) {
    Provider P;
    if (Immutable)
      P.add(0xff, {op(NdOp::COPY, reg(0), {constant(0)})});
    else
      // A preceding STORE makes the failed/no-change comparisons constant in
      // the symbolic state, so this also tests effect retention after folding.
      P.add(0xff, {op(NdOp::STORE, {}, {constant(0x4000), constant(7)})});
    // Both first operations leave the bytes unchanged. Their unused outputs
    // do not authorize removing, merging or reordering the ordered accesses.
    P.add(0x100,
          {atomicCmpxchg(reg(8), constant(0x4000), constant(9), constant(11))});
    P.add(0x101,
          {atomicCmpxchg(reg(8), constant(0x4000), constant(7), constant(7))});
    std::vector<va_t> ExpectedOrigins{0x100, 0x101};
    if (Immutable) {
      imageWord(P, 0x4000, 7);
      P.add(0x102, {op(NdOp::COPY, reg(0), {constant(37)}), ret()});
    } else {
      P.add(0x102, {atomicCmpxchg(reg(8), constant(0x4000), constant(7),
                                  constant(11))});
      P.add(0x103, {atomicCmpxchg(reg(8), constant(0x4000), constant(7),
                                  constant(13))});
      P.add(0x104, {op(NdOp::COPY, reg(0), {constant(37)}), ret()});
      ExpectedOrigins.insert(ExpectedOrigins.end(), {0x102, 0x103});
    }
    const auto R = specializeInterpreter(P, {0xff}, wideOptions());
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::ATOMIC_CMPXCHG), ExpectedOrigins.size());
    EXPECT_EQ(count(R.Residual, NdOp::LOAD), 0u);
    EXPECT_EQ(count(R.Residual, NdOp::STORE), Immutable ? 0u : 1u);
    WideByteOracle Actual(llvm::endianness::little);
    for (unsigned I = 0; I != 8; ++I)
      Actual.Memory[0x4000 + I] = I ? 0 : 7;
    auto ExpectedMemory = Actual.Memory;
    ExpectedMemory[0x4000] = Immutable ? 7 : 11;
    ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
    EXPECT_EQ(nativeAtomicAddresses(R, Actual.AtomicAddresses),
              ExpectedOrigins);
    EXPECT_EQ(Actual.Memory, ExpectedMemory);
    EXPECT_EQ(Actual.number(reg(0)), 37u);
  }
}

TEST(InterpreterSpecialization,
     AtomicCmpxchgChecksTheCompleteReturnSlotFootprint) {
  // No natural-alignment restriction is invented for raw 2/4-byte accesses.
  // The other cases touch the first or last byte of a possible return word.
  for (const auto &[Bytes, Bias] : {std::pair<uint16_t, uint64_t>{2, 1},
                                    {4, 3},
                                    {8, 1},
                                    {2, 7},
                                    {1, 8},
                                    {2, 15}}) {
    SCOPED_TRACE(Bytes);
    SCOPED_TRACE(Bias);
    auto P = atomicNativeProvider(Bytes, Bias);
    const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
    const bool Separated = Bias + Bytes <= 8;
    EXPECT_EQ(R.complete(), Separated) << R.Diagnostic;
    if (!Separated) {
      EXPECT_TRUE(R.Residual.Blocks.empty());
      continue;
    }
    WideByteOracle Actual(llvm::endianness::little);
    Actual.word(reg(32), 0x8018);
    Actual.word(reg(16), 0x4000);
    Actual.word(reg(8, Bytes), 7);
    Actual.word(reg(64, Bytes), 11);
    for (unsigned I = 0; I != 8; ++I)
      Actual.Memory[0x4000 + I] = uint8_t(uint64_t{0x800f} >> (8 * I));
    for (unsigned I = 0; I < Bytes; ++I)
      Actual.Memory[0x8000 + Bias + I] = I ? 0 : 7;
    ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
    EXPECT_EQ(Actual.number(reg(0, Bytes)), 7u);
    EXPECT_EQ(Actual.number(reg(48, Bytes)), 11u);
    EXPECT_EQ(Actual.number(reg(32)), 0x8018u);
  }
  for (bool RemoveAlignment : {false, true}) {
    auto P = atomicNativeProvider();
    auto O = separatedStoreOptions();
    if (RemoveAlignment)
      O.EntryFrameAlignment.reset();
    else
      O.ProveStoreReturnSlotSeparation = false;
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
  for (uint64_t Back : {0u, 1u}) {
    Provider P;
    P.add(0x100,
          {op(NdOp::INT_SUB, reg(40), {reg(32), constant(Back)}),
           atomicCmpxchg(reg(0, 2), reg(40), constant(7, 2), constant(7, 2)),
           ret()});
    auto O = separatedStoreOptions();
    O.ExternalStoresPreserveEntryReturnSlot = true;
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, NonAlignmentSpillsRetainProvedFrameRoots) {
  for (bool Clobber : {false, true}) {
    Provider P;
    std::vector<LowOp> Body{op(NdOp::INT_XOR, reg(40), {reg(32), constant(32)}),
                            op(NdOp::INT_SUB, reg(48), {reg(32), constant(64)}),
                            op(NdOp::STORE, {}, {reg(48), reg(40)})};
    if (Clobber)
      Body.push_back(op(NdOp::STORE, {}, {reg(48), reg(8, 1)}));
    Body.push_back(jump(0x110));
    P.add(0x100, std::move(Body));
    P.add(0x110, {op(NdOp::LOAD, reg(40), {reg(48)}),
                  op(NdOp::INT_ADD, reg(32), {reg(40), constant(32)}),
                  op(NdOp::COPY, reg(0), {reg(8)})});
    P.add(0x111, {ret()});
    P.Code.at(0x111).NativeStackControl =
        SpecializationNativeStackControl::Return;
    auto O = separatedStoreOptions();
    O.EntryFrameAlignment = InterpreterEntryAlignment{64, 32};
    for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
      O.ByteOrder = Order;
      const auto R = specializeInterpreter(P, {0x100}, O);
      if (Clobber) {
        EXPECT_FALSE(R.complete());
        EXPECT_TRUE(R.Residual.Blocks.empty());
        continue;
      }
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      for (uint64_t Stack :
           {uint64_t{32}, uint64_t{0x120}, uint64_t{0xffff900000001020}})
        for (uint64_t Input : {uint64_t{0}, uint64_t{37}, UINT64_MAX}) {
          WideByteOracle Actual(Order);
          Actual.word(reg(32), Stack);
          Actual.word(reg(8), Input);
          for (int64_t Offset = -80; Offset < 16; ++Offset)
            Actual.Memory[Stack + uint64_t(Offset)] = uint8_t(Offset ^ 0x5a);
          auto Expected = Actual.Memory;
          for (unsigned I = 0; I != 8; ++I) {
            const unsigned Shift =
                8 * (Order == llvm::endianness::little ? I : 7 - I);
            Expected[Stack - 64 + I] = uint8_t((Stack ^ 32) >> Shift);
          }
          ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
          EXPECT_EQ(Actual.number(reg(0)), Input);
          EXPECT_EQ(Actual.number(reg(32)), Stack);
          EXPECT_EQ(Actual.number(reg(40)), Stack ^ 32);
          EXPECT_EQ(Actual.number(reg(48)), Stack - 64);
          EXPECT_EQ(Actual.Memory, Expected);
        }
      auto WrongDomain = O;
      WrongDomain.EntryFrameAlignment->Residue = 0;
      EXPECT_FALSE(specializeInterpreter(P, {0x100}, WrongDomain).complete());
      ASSERT_GT(R.SolverQueries, 0u);
      auto Exact = O;
      Exact.MaxSolverQueries = R.SolverQueries;
      EXPECT_TRUE(specializeInterpreter(P, {0x100}, Exact).complete());
      --Exact.MaxSolverQueries;
      const auto Short = specializeInterpreter(P, {0x100}, Exact);
      EXPECT_FALSE(Short.complete());
      EXPECT_TRUE(Short.Residual.Blocks.empty());
    }
  }
}

TEST(InterpreterSpecialization, ProvedFrameSeparationRetainsAtomicEffects) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_SUB, reg(48), {reg(32), constant(16)}),
                op(NdOp::STORE, {}, {reg(48), reg(32)})});
  P.add(0x101, {op(NdOp::CALL, reg(0), {constant(0x200)})});
  P.Code.at(0x101).NativeStackControl = SpecializationNativeStackControl::Call;
  P.add(0x102, {op(NdOp::LOAD, reg(32), {reg(48)}),
                op(NdOp::LOAD, reg(0), {constant(0x8080)})});
  P.add(0x103, {ret()});
  P.Code.at(0x103).NativeStackControl =
      SpecializationNativeStackControl::Return;
  P.add(0x200, {atomicCmpxchg(reg(64), constant(0x8080), reg(8), reg(16))});
  P.add(0x201, {ret()});
  P.Code.at(0x201).NativeStackControl =
      SpecializationNativeStackControl::Return;
  auto O = separatedStoreOptions();
  O.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 8};
  O.EntryFrameBounds->ExcludedAddressRanges = {{0x8000, 0x8100}};
  EXPECT_FALSE(specializeInterpreter(P, {0x100}, O).complete());
  O.ProveStoreFrameSeparation = true;
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    O.ByteOrder = Order;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, NdOp::ATOMIC_CMPXCHG), 1u);
    for (uint64_t Stack : {0x7ff8u, 0x8128u})
      for (bool Match : {false, true}) {
        WideByteOracle Actual(Order);
        Actual.word(reg(32), Stack);
        Actual.word(reg(8), Match ? 7 : 9);
        Actual.word(reg(16), 17);
        for (unsigned I = 0; I != 8; ++I) {
          const unsigned Shift =
              8 * (Order == llvm::endianness::little ? I : 7 - I);
          Actual.Memory[0x8080 + I] = uint8_t(uint64_t{7} >> Shift);
          Actual.Memory[Stack + I] = 0xa0 + I;
        }
        auto Expected = Actual.Memory;
        for (unsigned I = 0; I != 8; ++I) {
          const unsigned Shift =
              8 * (Order == llvm::endianness::little ? I : 7 - I);
          Expected[Stack - 16 + I] = uint8_t(Stack >> Shift);
          Expected[Stack - 8 + I] = uint8_t(uint64_t{0x102} >> Shift);
          Expected[0x8080 + I] = uint8_t(uint64_t{Match ? 17u : 7u} >> Shift);
        }
        ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
        EXPECT_EQ(Actual.number(reg(0)), Match ? 17u : 7u);
        EXPECT_EQ(Actual.number(reg(32)), Stack);
        EXPECT_EQ(Actual.number(reg(64)), 7u);
        EXPECT_EQ(Actual.Memory, Expected);
        EXPECT_EQ(nativeAtomicAddresses(R, Actual.AtomicAddresses),
                  std::vector<va_t>{0x200});
      }
  }
}

TEST(InterpreterSpecialization, AtomicCmpxchgBudgetsNeverPublishPartialCFG) {
  auto P = atomicNativeProvider();
  const auto Baseline =
      specializeInterpreter(P, {0x100}, separatedStoreOptions());
  ASSERT_TRUE(Baseline.complete()) << Baseline.Diagnostic;
  ASSERT_GT(Baseline.EvaluatedOperations, 1u);
  ASSERT_GT(Baseline.SolverQueries, 1u);
  for (bool QueryBudget : {false, true}) {
    auto O = separatedStoreOptions();
    if (QueryBudget)
      O.MaxSolverQueries = Baseline.SolverQueries;
    else
      O.MaxOperations = Baseline.EvaluatedOperations;
    EXPECT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
    if (QueryBudget)
      --O.MaxSolverQueries;
    else
      --O.MaxOperations;
    const auto Short = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded)
        << Short.Diagnostic;
    EXPECT_TRUE(Short.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, AtomicCmpxchgRequiresTheExplicitNarrowDomain) {
  const auto Valid =
      atomicCmpxchg(reg(0, 4), constant(0x4001), reg(8, 4), reg(16, 4));
  for (unsigned Domain = 0; Domain != 4; ++Domain) {
    Provider P;
    P.add(0x100, {Valid, ret()});
    auto O = wideOptions();
    O.ExplicitMachineState = Domain & 1;
    O.NormalNonfaultingExecution = Domain & 2;
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(R.complete(), Domain == 3) << R.Diagnostic;
    if (Domain != 3)
      EXPECT_TRUE(R.Residual.Blocks.empty());
  }

  std::vector<LowOp> Bad;
  for (auto Ordering :
       {NdMemoryOrdering::None, NdMemoryOrdering::Relaxed,
        NdMemoryOrdering::Acquire, NdMemoryOrdering::AcquireRelease,
        static_cast<NdMemoryOrdering>(255)}) {
    auto A = Valid;
    A.MemoryOrdering = Ordering;
    Bad.push_back(A);
  }
  for (auto Space : {NdMemoryAddressSpace::X86FS, NdMemoryAddressSpace::X86GS,
                     static_cast<NdMemoryAddressSpace>(255)}) {
    auto A = Valid;
    A.MemoryAddressSpace = Space;
    Bad.push_back(A);
  }
  for (uint16_t Width : {0, 3, 16}) {
    auto A = Valid;
    A.Output.Size = A.Inputs[1].Size = A.Inputs[2].Size = Width;
    Bad.push_back(A);
  }
  auto A = Valid;
  A.Inputs[0].Size = 4;
  Bad.push_back(A);
  A = Valid;
  A.NumInputs = 2;
  Bad.push_back(A);
  A = Valid;
  A.addInput(constant(0));
  Bad.push_back(A);
  A = Valid;
  A.Inputs[1].Size = 2;
  Bad.push_back(A);
  for (auto Output :
       {constant(0, 4), NdVar::ram(0x5000, 4), reg(UINT64_MAX, 4)}) {
    A = Valid;
    A.Output = Output;
    Bad.push_back(A);
  }
  for (unsigned Index : {0u, 1u, 2u}) {
    A = Valid;
    A.Inputs[Index] = NdVar::ram(0x5000, A.Inputs[Index].Size);
    Bad.push_back(A);
  }
  A = Valid;
  A.Inputs[2] = reg(UINT64_MAX, 4);
  Bad.push_back(A);
  A = Valid;
  A.Inputs[0] = constant(UINT64_MAX - 1);
  Bad.push_back(A);
  for (auto Ordinary : {op(NdOp::LOAD, reg(0, 4), {constant(0x4000)}),
                        op(NdOp::STORE, {}, {constant(0x4000), reg(8, 4)})}) {
    Ordinary.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    Bad.push_back(Ordinary);
  }
  for (unsigned I = 0; I < Bad.size(); ++I) {
    SCOPED_TRACE(I);
    Provider P;
    P.add(0x100, {Bad[I], ret()});
    const auto R = specializeInterpreter(P, {0x100}, wideOptions());
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, AtomicCmpxchgOldValueKeepsMemoryOrigin) {
  auto O = ordinaryReturnOptions();
  O.ExplicitMachineState = O.NormalNonfaultingExecution = true;
  O.ExternalStoresPreserveEntryReturnSlot = true;
  O.MaxChainedTransfers = 0;
  for (bool ReplaceOldWithInput : {false, true}) {
    Provider P;
    std::vector<LowOp> Body{
        atomicCmpxchg(reg(40), reg(8), constant(0), constant(1))};
    if (ReplaceOldWithInput)
      Body.push_back(op(NdOp::COPY, reg(40), {reg(16)}));
    Body.push_back(jump(0x110));
    P.add(0x100, std::move(Body));
    P.add(0x110, {op(NdOp::STORE, {}, {reg(40), constant(7)}),
                  op(NdOp::COPY, reg(0), {constant(9)}), ret()});
    const auto R = specializeInterpreter(P, {0x100}, O);
    if (ReplaceOldWithInput) {
      EXPECT_TRUE(R.complete()) << R.Diagnostic;
    } else {
      EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
      // Reject the use of Old as a pointer, not the preceding valid CAS.
      EXPECT_NE(R.Diagnostic.find("address origin"), std::string::npos)
          << R.Diagnostic;
      EXPECT_TRUE(R.Residual.Blocks.empty());
    }
  }

  // Old has a safe frame-spill origin before this write. Desired is a frame
  // pointer, so using the write's new provenance for Old would over-refuse.
  Provider SafeOld;
  SafeOld.add(0x100,
              {op(NdOp::INT_ADD, reg(40), {reg(32), constant(16)}),
               op(NdOp::STORE, {}, {reg(40), reg(8)}),
               atomicCmpxchg(reg(56), reg(40), reg(8), reg(32)), jump(0x110)});
  SafeOld.add(0x110, {op(NdOp::STORE, {}, {reg(56), constant(37)}),
                      op(NdOp::COPY, reg(0), {constant(9)}), ret()});
  const auto Good = specializeInterpreter(SafeOld, {0x100}, O);
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  WideByteOracle Actual(llvm::endianness::little);
  Actual.word(reg(32), 0x1000);
  Actual.word(reg(8), 0x4000);
  ASSERT_TRUE(Actual.run(Good.Residual)) << Actual.Diagnostic;
  EXPECT_EQ(Actual.number(reg(0)), 9u);
  for (unsigned I = 0; I != 8; ++I) {
    EXPECT_EQ(Actual.Memory.at(0x1010 + I),
              uint8_t(uint64_t{0x1000} >> (8 * I)));
    EXPECT_EQ(Actual.Memory.at(0x4000 + I), uint8_t(uint64_t{37} >> (8 * I)));
  }

  // A failed comparison keeps unknown/frame-derived bytes even when Desired
  // is external. The subsequent frame reload must retain that possibility.
  Provider UnsafeWrite;
  UnsafeWrite.add(0x100, {op(NdOp::INT_ADD, reg(40), {reg(32), constant(16)}),
                          op(NdOp::INT_XOR, reg(48), {reg(32), reg(24)}),
                          op(NdOp::STORE, {}, {reg(40), reg(48)}),
                          atomicCmpxchg(reg(56), reg(40), reg(16), reg(8)),
                          jump(0x110)});
  UnsafeWrite.add(0x110, {op(NdOp::LOAD, reg(64), {reg(40)}), jump(0x120)});
  UnsafeWrite.add(0x120, {op(NdOp::STORE, {}, {reg(64), constant(7)}), ret()});
  const auto Bad = specializeInterpreter(UnsafeWrite, {0x100}, O);
  EXPECT_EQ(Bad.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(Bad.Diagnostic.find("address origin"), std::string::npos)
      << Bad.Diagnostic;
  EXPECT_TRUE(Bad.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, AtomicCmpxchgInvalidatesAliasedAffineSpills) {
  Provider P;
  P.add(0x100,
        {op(NdOp::INT_ADD, reg(40), {reg(32), constant(8)}),
         op(NdOp::INT_ADD, reg(48), {reg(32), constant(32)}),
         op(NdOp::STORE, {}, {reg(40), reg(48)}),
         op(NdOp::COND_BR, {}, {constant(0x110), reg(24, 1)})},
        0x120);
  P.add(0x110,
        {op(NdOp::LOAD, reg(72), {reg(8)}),
         op(NdOp::INT_AND, reg(72), {reg(72), constant(~uint64_t{15})}),
         atomicCmpxchg(reg(64), reg(72), reg(48), reg(32)), jump(0x130)});
  P.add(0x120, {jump(0x130)});
  P.add(0x130,
        {op(NdOp::LOAD, reg(56), {reg(40)}),
         op(NdOp::INT_EQUAL, reg(80, 1), {reg(56), reg(32)}),
         op(NdOp::COND_BR, {}, {constant(0x140), reg(80, 1)})},
        0x150);
  P.add(0x140, {op(NdOp::COPY, reg(0), {constant(17)}), ret()});
  P.add(0x150, {op(NdOp::COPY, reg(0), {constant(19)}), ret()});
  auto O = separatedStoreOptions();
  O.MaxChainedTransfers = 0; // Exercise projection, join and re-seeding.
  ASSERT_FALSE(O.ExternalStoresPreserveEntryReturnSlot);
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_GE(count(R.Residual, NdOp::COND_BR), 2u);
  for (uint64_t Stack : {0x1008u, 0x2008u})
    for (bool TakeAtomic : {false, true})
      for (bool Alias : {false, true}) {
        SCOPED_TRACE(Stack);
        SCOPED_TRACE(TakeAtomic);
        SCOPED_TRACE(Alias);
        const uint64_t Target = Alias ? Stack + 8 : 0x5000;
        WideByteOracle Actual(llvm::endianness::little);
        Actual.word(reg(32), Stack);
        Actual.word(reg(8), 0x4000);
        Actual.word(reg(24, 1), TakeAtomic);
        for (unsigned I = 0; I != 8; ++I) {
          Actual.Memory[0x4000 + I] = uint8_t(Target >> (8 * I));
          Actual.Memory[0x5000 + I] = uint8_t((Stack + 32) >> (8 * I));
          Actual.Memory[Stack + I] = 0xa0 + I;
        }
        auto ExpectedMemory = Actual.Memory;
        for (unsigned I = 0; I != 8; ++I) {
          ExpectedMemory[Stack + 8 + I] = uint8_t((Stack + 32) >> (8 * I));
          if (TakeAtomic)
            ExpectedMemory[Target + I] = uint8_t(Stack >> (8 * I));
        }
        ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
        EXPECT_EQ(Actual.number(reg(0)), TakeAtomic && Alias ? 17u : 19u);
        EXPECT_EQ(Actual.number(reg(32)), Stack);
        EXPECT_EQ(Actual.Memory, ExpectedMemory);
        EXPECT_EQ(nativeAtomicAddresses(R, Actual.AtomicAddresses),
                  TakeAtomic ? std::vector<va_t>{0x110} : std::vector<va_t>{});
      }
}

TEST(InterpreterSpecialization, AtomicCmpxchgDoesNotInventAffineDesiredSpill) {
  Provider P;
  P.add(0x100, {op(NdOp::INT_ADD, reg(40), {reg(32), constant(16)}),
                op(NdOp::INT_ADD, reg(48), {reg(32), constant(32)}),
                op(NdOp::STORE, {}, {reg(40), reg(48)}),
                atomicCmpxchg(reg(56), reg(40), reg(8), reg(32)), jump(0x110)});
  P.add(0x110,
        {op(NdOp::LOAD, reg(64), {reg(40)}),
         op(NdOp::INT_EQUAL, reg(80, 1), {reg(64), reg(32)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(80, 1)})},
        0x130);
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(17)}), ret()});
  P.add(0x130, {op(NdOp::COPY, reg(0), {constant(19)}), ret()});
  auto O = ordinaryReturnOptions();
  O.ExplicitMachineState = O.NormalNonfaultingExecution = true;
  O.MaxChainedTransfers = 0; // Reload through node projection and re-seeding.
  const auto R = specializeInterpreter(P, {0x100}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::ATOMIC_CMPXCHG), 1u);
  EXPECT_GE(count(R.Residual, NdOp::COND_BR), 1u);
  // Desired is an exact frame pointer, but it describes only a successful
  // comparison. Importing it as an unconditional spill would erase mismatch.
  for (uint64_t Stack : {0x1000u, 0x2000u})
    for (bool Match : {false, true}) {
      SCOPED_TRACE(Stack);
      SCOPED_TRACE(Match);
      WideByteOracle Actual(llvm::endianness::little);
      Actual.word(reg(32), Stack);
      Actual.word(reg(8), Stack + 32 + (Match ? 0 : 1));
      for (uint64_t A = Stack + 8; A < Stack + 32; ++A)
        Actual.Memory[A] = uint8_t(0xa0 + A - Stack);
      auto ExpectedMemory = Actual.Memory;
      const uint64_t FinalPointer = Match ? Stack : Stack + 32;
      for (unsigned I = 0; I != 8; ++I)
        ExpectedMemory[Stack + 16 + I] = uint8_t(FinalPointer >> (8 * I));
      ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
      EXPECT_EQ(Actual.number(reg(0)), Match ? 17u : 19u);
      EXPECT_EQ(Actual.number(reg(32)), Stack);
      EXPECT_EQ(Actual.number(reg(56)), Stack + 32);
      EXPECT_EQ(Actual.number(reg(64)), FinalPointer);
      EXPECT_EQ(Actual.Memory, ExpectedMemory);
      EXPECT_EQ(nativeAtomicAddresses(R, Actual.AtomicAddresses),
                std::vector<va_t>{0x100});
    }
}

TEST(InterpreterSpecialization, AtomicUpdatesKeepNativeReturnsAndByteEffects) {
  for (auto Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG})
    for (auto Order : {llvm::endianness::little, llvm::endianness::big})
      for (uint16_t Bytes : {1, 2, 4, 8}) {
        SCOPED_TRACE(static_cast<unsigned>(Opcode));
        SCOPED_TRACE(Bytes);
        auto P = atomicNativeProvider(Bytes, 0, Opcode);
        // Old overlaps the input value. The following load still uses Address.
        P.Code.at(0x200).Ops.at(4).Output = reg(64, Bytes);
        auto O = separatedStoreOptions();
        O.ByteOrder = Order;
        const auto R = specializeInterpreter(P, {0x100}, O);
        ASSERT_TRUE(R.complete()) << R.Diagnostic;
        EXPECT_EQ(count(R.Residual, Opcode), 1u);
        EXPECT_EQ(count(R.Residual, NdOp::STORE), 1u);
        for (uint64_t Target : {0x8000u, 0x8010u, 0x6000u})
          for (uint64_t Old : {uint64_t{0}, uint64_t{0x7fffffff}, UINT64_MAX}) {
            constexpr uint64_t Stack = 0x8018;
            const uint64_t Mask =
                Bytes == 8 ? UINT64_MAX : (uint64_t{1} << (8 * Bytes)) - 1;
            const uint64_t Value = 19;
            const uint64_t Updated =
                (Opcode == NdOp::ATOMIC_ADD ? Old + Value : Value) & Mask;
            WideByteOracle Actual(Order);
            Actual.word(reg(32), Stack);
            Actual.word(reg(16), 0x4000);
            Actual.word(reg(64, Bytes), Value);
            for (uint64_t A = Stack - 32; A < Stack + 24; ++A)
              Actual.Memory[A] = uint8_t(A ^ 0xa5);
            for (uint64_t A = Target - 1; A < Target + Bytes + 1; ++A)
              Actual.Memory[A] = 0x5a;
            for (unsigned I = 0; I < Bytes; ++I) {
              const unsigned Shift =
                  8 * (Order == llvm::endianness::little ? I : Bytes - I - 1);
              Actual.Memory[Target + I] = uint8_t(Old >> Shift);
            }
            for (unsigned I = 0; I < 8; ++I) {
              const unsigned Shift =
                  8 * (Order == llvm::endianness::little ? I : 7 - I);
              Actual.Memory[0x4000 + I] = uint8_t((Target | 15) >> Shift);
            }
            auto Expected = Actual.Memory;
            for (unsigned I = 0; I < Bytes; ++I) {
              const unsigned Shift =
                  8 * (Order == llvm::endianness::little ? I : Bytes - I - 1);
              Expected[Target + I] = uint8_t(Updated >> Shift);
            }
            for (unsigned I = 0; I < 8; ++I) {
              const unsigned Shift =
                  8 * (Order == llvm::endianness::little ? I : 7 - I);
              Expected[Stack - 16 + I] = uint8_t(uint64_t{0x102} >> Shift);
            }
            ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
            EXPECT_EQ(Actual.number(reg(64, Bytes)), Old & Mask);
            EXPECT_EQ(Actual.number(reg(48, Bytes)), Updated);
            EXPECT_EQ(Actual.number(reg(32)), Stack);
            EXPECT_EQ(Actual.Memory, Expected);
            EXPECT_EQ(nativeAtomicAddresses(R, Actual.AtomicAddresses),
                      std::vector<va_t>{0x200});
          }
      }
}

TEST(InterpreterSpecialization, AtomicUpdatesRetainOrderedNoChangeEffects) {
  for (auto Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG}) {
    Provider P;
    P.add(0x100, {op(NdOp::STORE, {}, {constant(0x4001), constant(7)}),
                  op(NdOp::COPY, reg(8), {constant(0x4001)})});
    P.add(0x101, {atomicWrite(Opcode, reg(8), reg(8),
                              constant(Opcode == NdOp::ATOMIC_ADD ? 0 : 7))});
    P.add(0x102,
          {atomicWrite(Opcode, reg(16), constant(0x4001), constant(11))});
    P.add(0x103, {op(NdOp::COPY, reg(0), {constant(37)}), ret()});
    const auto R = specializeInterpreter(P, {0x100}, wideOptions());
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, Opcode), 2u);
    for (const auto &B : R.Residual.Blocks)
      for (const auto &A : B.Ops)
        if (A.Opcode == Opcode) {
          EXPECT_EQ(A.NumInputs, 2u);
          EXPECT_EQ(A.MemoryOrdering, NdMemoryOrdering::SequentiallyConsistent);
          EXPECT_EQ(A.MemoryAddressSpace, NdMemoryAddressSpace::Default);
        }
    WideByteOracle Actual(llvm::endianness::little);
    ASSERT_TRUE(Actual.run(R.Residual)) << Actual.Diagnostic;
    EXPECT_EQ(Actual.number(reg(8)), 7u);
    EXPECT_EQ(Actual.number(reg(16)), 7u);
    EXPECT_EQ(Actual.number(reg(0)), 37u);
    std::map<uint64_t, uint8_t> Expected;
    for (unsigned I = 0; I < 8; ++I)
      Expected[0x4001 + I] = I ? 0 : Opcode == NdOp::ATOMIC_ADD ? 18 : 11;
    EXPECT_EQ(Actual.Memory, Expected);
    EXPECT_EQ(nativeAtomicAddresses(R, Actual.AtomicAddresses),
              (std::vector<va_t>{0x101, 0x102}));
  }
}

TEST(InterpreterSpecialization, AtomicUpdatesCheckCompleteReturnFootprints) {
  for (auto Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG})
    for (const auto &[Bytes, Bias] : {std::pair<uint16_t, uint64_t>{2, 1},
                                      {4, 3},
                                      {8, 1},
                                      {2, 7},
                                      {1, 8},
                                      {2, 15}}) {
      auto P = atomicNativeProvider(Bytes, Bias, Opcode);
      const auto R = specializeInterpreter(P, {0x100}, separatedStoreOptions());
      EXPECT_EQ(R.complete(), Bias + Bytes <= 8) << R.Diagnostic;
      if (!R.complete())
        EXPECT_TRUE(R.Residual.Blocks.empty());
    }
}

TEST(InterpreterSpecialization, AtomicUpdatesKeepOldAndNewMemoryOrigins) {
  auto O = ordinaryReturnOptions();
  O.ExplicitMachineState = O.NormalNonfaultingExecution = true;
  O.ExternalStoresPreserveEntryReturnSlot = true;
  O.MaxChainedTransfers = 0;
  for (auto Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG}) {
    for (bool ReplaceOld : {false, true}) {
      Provider P;
      std::vector<LowOp> Body{
          atomicWrite(Opcode, reg(40), reg(8), constant(1))};
      if (ReplaceOld)
        Body.push_back(op(NdOp::COPY, reg(40), {reg(16)}));
      Body.push_back(jump(0x110));
      P.add(0x100, std::move(Body));
      P.add(0x110, {op(NdOp::STORE, {}, {reg(40), constant(7)}), ret()});
      const auto R = specializeInterpreter(P, {0x100}, O);
      EXPECT_EQ(R.complete(), ReplaceOld) << R.Diagnostic;
      if (!ReplaceOld) {
        EXPECT_NE(R.Diagnostic.find("address origin"), std::string::npos);
        EXPECT_TRUE(R.Residual.Blocks.empty());
      }
    }
    // The old frame pointer remains a dependency of ADD, while XCHG
    // replaces it with an external input. Test after node projection.
    Provider P;
    P.add(0x100, {op(NdOp::INT_ADD, reg(40), {reg(32), constant(16)}),
                  op(NdOp::STORE, {}, {reg(40), reg(32)}),
                  atomicWrite(Opcode, reg(56), reg(40), reg(8)), jump(0x110)});
    P.add(0x110, {op(NdOp::LOAD, reg(48), {reg(40)}),
                  op(NdOp::STORE, {}, {reg(48), constant(7)}), ret()});
    const auto R = specializeInterpreter(P, {0x100}, O);
    EXPECT_EQ(R.complete(), Opcode == NdOp::ATOMIC_XCHG) << R.Diagnostic;
    if (Opcode == NdOp::ATOMIC_ADD)
      EXPECT_TRUE(R.Residual.Blocks.empty());
    // Old was external before the write. A frame-derived new value must
    // not retroactively change the provenance of the returned old value.
    Provider SafeOld;
    SafeOld.add(0x100,
                {op(NdOp::INT_ADD, reg(40), {reg(32), constant(16)}),
                 op(NdOp::STORE, {}, {reg(40), reg(8)}),
                 atomicWrite(Opcode, reg(56), reg(40), reg(32)), jump(0x110)});
    SafeOld.add(0x110, {op(NdOp::STORE, {}, {reg(56), constant(7)}), ret()});
    const auto Safe = specializeInterpreter(SafeOld, {0x100}, O);
    EXPECT_TRUE(Safe.complete()) << Safe.Diagnostic;
  }
}

TEST(InterpreterSpecialization, AtomicUpdatesRequireExplicitNarrowDomain) {
  for (auto Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG}) {
    const auto Valid =
        atomicWrite(Opcode, reg(0, 4), constant(0x4001), reg(8, 4));
    for (unsigned Domain = 0; Domain != 4; ++Domain) {
      Provider P;
      P.add(0x100, {Valid, ret()});
      auto O = wideOptions();
      O.ExplicitMachineState = Domain & 1;
      O.NormalNonfaultingExecution = Domain & 2;
      const auto R = specializeInterpreter(P, {0x100}, O);
      EXPECT_EQ(R.complete(), Domain == 3) << R.Diagnostic;
      if (Domain != 3)
        EXPECT_TRUE(R.Residual.Blocks.empty());
    }
    std::vector<LowOp> Bad;
    for (unsigned Case = 0; Case != 7; ++Case) {
      auto A = Valid;
      switch (Case) {
      case 0:
        A.MemoryOrdering = NdMemoryOrdering::Relaxed;
        break;
      case 1:
        A.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
        break;
      case 2:
        A.Output.Size = A.Inputs[1].Size = 3;
        break;
      case 3:
        A.Inputs[0].Size = 4;
        break;
      case 4:
        A.addInput(constant(0));
        break;
      case 5:
        A.Inputs[1] = NdVar::ram(0x5000, 4);
        break;
      case 6:
        A.Inputs[0] = constant(UINT64_MAX - 1);
        break;
      }
      Bad.push_back(A);
    }
    for (const auto &A : Bad) {
      Provider P;
      P.add(0x100, {A, ret()});
      const auto R = specializeInterpreter(P, {0x100}, wideOptions());
      EXPECT_FALSE(R.complete());
      EXPECT_TRUE(R.Residual.Blocks.empty());
    }
  }
}

TEST(InterpreterSpecialization, AtomicUpdatesPreserveFrameOnlyWithProof) {
  for (auto Opcode : {NdOp::ATOMIC_ADD, NdOp::ATOMIC_XCHG}) {
    Provider P;
    P.add(0x100, {op(NdOp::INT_SUB, reg(40), {reg(32), constant(16)}),
                  op(NdOp::STORE, {}, {reg(40), reg(32)}),
                  atomicWrite(Opcode, reg(64), constant(0x8080), reg(8)),
                  jump(0x110)});
    P.add(0x110, {op(NdOp::LOAD, reg(32), {reg(40)}), ret()});
    auto O = separatedStoreOptions();
    O.EntryFrameBounds = SpecializationEntryFrameBounds{-32, 8};
    O.EntryFrameBounds->ExcludedAddressRanges = {{0x8000, 0x8100}};
    EXPECT_FALSE(specializeInterpreter(P, {0x100}, O).complete());
    O.ProveStoreFrameSeparation = true;
    const auto R = specializeInterpreter(P, {0x100}, O);
    ASSERT_TRUE(R.complete()) << R.Diagnostic;
    EXPECT_EQ(count(R.Residual, Opcode), 1u);
    auto Exact = O;
    Exact.MaxOperations = R.EvaluatedOperations;
    EXPECT_TRUE(specializeInterpreter(P, {0x100}, Exact).complete());
    --Exact.MaxOperations;
    const auto Short = specializeInterpreter(P, {0x100}, Exact);
    EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
    EXPECT_TRUE(Short.Residual.Blocks.empty());
  }
}

void addTimestamp(Provider &P, va_t Address) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  D.setStrict(true);
  const uint8_t Bytes[]{0x0f, 0x31};
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes, sizeof(Bytes), Address, I),
            sizeof(Bytes));
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  D.liftToLow(I, Ops, {}, {}, &Effects);
  P.add(Address, std::move(Ops), Address + sizeof(Bytes));
  auto &Instruction = P.Code.at(Address);
  Instruction.Origin.Size = sizeof(Bytes);
  Instruction.NativeBytes.assign(std::begin(Bytes), std::end(Bytes));
  Instruction.UndefinedEffects = std::move(Effects);
}

TEST(InterpreterSpecialization, TimestampRetainsCompleteSampleAndLiveState) {
  Provider P;
  P.add(0xf0,
        {op(NdOp::STORE, {}, {constant(0x400), constant(0xab)}),
         op(NdOp::COPY, reg(x86reg::CF, 1), {constant(1, 1)})},
        0x100);
  addTimestamp(P, 0x100);
  P.add(0x102,
        {op(NdOp::INT_RIGHT, reg(80), {reg(x86reg::RAX), constant(32)}),
         op(NdOp::INT_RIGHT, reg(88), {reg(x86reg::RDX), constant(32)}),
         op(NdOp::INT_OR, reg(80), {reg(80), reg(88)}),
         // Check the memory in this observation, before a CFG projection can
         // discard unrelated absolute-memory constants.
         op(NdOp::LOAD, reg(88), {constant(0x400)}),
         op(NdOp::INT_NOTEQUAL, reg(88, 1), {reg(88), constant(0xab)}),
         op(NdOp::BOOL_OR, reg(80, 1), {reg(80), reg(88, 1)}),
         op(NdOp::BOOL_NOT, reg(88, 1), {reg(x86reg::CF, 1)}),
         op(NdOp::BOOL_OR, reg(80, 1), {reg(80, 1), reg(88, 1)}),
         op(NdOp::COND_BR, {}, {constant(0xdead), reg(80, 1)})},
        0x120);
  P.add(0x120,
        {op(NdOp::INT_LEFT, reg(x86reg::RDX), {reg(x86reg::RDX), constant(32)}),
         op(NdOp::INT_OR, reg(0), {reg(0), reg(x86reg::RDX)}),
         op(NdOp::INT_XOR, reg(0), {reg(0), reg(x86reg::R8)}), ret()});
  const auto R = specializeInterpreter(P, {0xf0}, wideOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 1u);
  EXPECT_EQ(count(R.Residual, NdOp::COND_BR), 0u);
  for (uint64_t Sample : {uint64_t{0}, uint64_t{1}, uint64_t{0xffffffff},
                          UINT64_C(0x8000000000000000),
                          UINT64_C(0xfedcba9876543210), UINT64_MAX}) {
    const uint64_t Inputs = UINT64_C(0x123456789abcdef0);
    EXPECT_EQ(execute(R.Residual,
                      {{x86reg::RAX, UINT64_MAX},
                       {x86reg::RDX, UINT64_MAX},
                       {x86reg::R8, Inputs}},
                      {}, 0, {}, std::nullopt, {Sample}),
              Sample ^ Inputs);
  }
}

TEST(InterpreterSpecialization, TimestampReadsRemainIndependent) {
  Provider P;
  addTimestamp(P, 0x100);
  P.add(0x102,
        {op(NdOp::COPY, reg(64), {reg(0)}),
         op(NdOp::COPY, reg(72), {reg(x86reg::RDX)})},
        0x110);
  addTimestamp(P, 0x110);
  P.add(0x112,
        {op(NdOp::INT_EQUAL, reg(80, 1), {reg(0), reg(64)}),
         op(NdOp::INT_EQUAL, reg(88, 1), {reg(x86reg::RDX), reg(72)}),
         op(NdOp::BOOL_AND, reg(80, 1), {reg(80, 1), reg(88, 1)}),
         op(NdOp::COND_BR, {}, {constant(0x120), reg(80, 1)})},
        0x130);
  P.add(0x120, {op(NdOp::COPY, reg(0), {constant(17)}), ret()});
  P.add(0x130, {op(NdOp::COPY, reg(0), {constant(23)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, wideOptions());
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  EXPECT_EQ(count(R.Residual, NdOp::INTRINSIC), 2u);
  EXPECT_GT(count(R.Residual, NdOp::COND_BR), 0u);
  for (uint64_t First : {uint64_t{0}, UINT64_C(0x123456789abcdef0), UINT64_MAX})
    for (uint64_t Second : {First, First ^ 1, First ^ (uint64_t{1} << 63)})
      EXPECT_EQ(
          execute(R.Residual, {}, {}, 0, {}, std::nullopt, {First, Second}),
          First == Second ? 17u : 23u);
}

TEST(InterpreterSpecialization, TimestampRepeatedOccurrenceAndExactBudget) {
  Provider P;
  P.add(0xf0,
        {op(NdOp::COPY, reg(80), {constant(0)}),
         op(NdOp::COPY, reg(88), {constant(3)})},
        0x100);
  addTimestamp(P, 0x100);
  P.add(0x102,
        {op(NdOp::INT_ADD, reg(80), {reg(80), reg(0)}),
         op(NdOp::INT_SUB, reg(88), {reg(88), constant(1)}),
         op(NdOp::COND_BR, {}, {constant(0x100), reg(88)})},
        0x110);
  P.add(0x110, {op(NdOp::COPY, reg(0), {reg(80)}), ret()});
  auto O = wideOptions();
  O.ControlRegisters = {{88, 8}};
  const auto R = specializeInterpreter(P, {0xf0}, O);
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  for (const std::vector<uint64_t> Samples :
       {std::vector<uint64_t>{1, 2, 3},
        {UINT64_C(0xffffffff00000001), UINT64_MAX, UINT64_C(0x80000001)}}) {
    const uint64_t Expected = uint64_t(uint32_t(Samples[0])) +
                              uint32_t(Samples[1]) + uint32_t(Samples[2]);
    EXPECT_EQ(execute(R.Residual, {}, {}, 0, {}, std::nullopt, Samples),
              Expected);
  }
  O.MaxOperations = R.EvaluatedOperations;
  EXPECT_TRUE(specializeInterpreter(P, {0xf0}, O).complete());
  --O.MaxOperations;
  const auto Short = specializeInterpreter(P, {0xf0}, O);
  EXPECT_EQ(Short.Status, SpecializationStatus::BudgetExceeded);
  EXPECT_TRUE(Short.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, TimestampRejectsIncompleteBundleAndDomain) {
  Provider P;
  addTimestamp(P, 0x100);
  P.add(0x102, {ret()});
  ASSERT_EQ(P.Code.at(0x100).Ops.size(), 5u);
  auto O = wideOptions();
  ASSERT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Bad = P;
    auto &I = Bad.Code.at(0x100);
    switch (Mutation) {
    case 0:
      I.NativeBytes.clear();
      break;
    case 1:
      I.NativeBytes[1] = 0x32;
      break;
    case 2:
      I.NativeBytes.insert(I.NativeBytes.begin(), 0x48);
      break;
    case 3:
      I.Ops[0].Output = reg(x86reg::RCX);
      break;
    case 4:
      I.Ops[0].addInput(constant(0));
      break;
    case 5:
      I.Ops[1].Inputs[0] = reg(0x500, 4);
      break;
    case 6:
      I.Ops[2].Inputs[0] = I.Ops[1].Inputs[0];
      break;
    case 7:
      I.Ops[1].Inputs[0].Size = 8;
      break;
    case 8:
      I.Ops[3].Inputs[0] = reg(x86reg::RDX, 4);
      break;
    case 9:
      I.Ops[4].Output = reg(x86reg::RCX);
      break;
    case 10:
      I.Ops[0].MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      break;
    case 11:
      I.Ops[0].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 12:
      I.IsNativeCall = true;
      break;
    case 13:
      I.Ops[1].Inputs[0].Offset = UINT64_MAX - 1;
      break;
    case 14:
      I.Ops[0].Inputs[0].Size = 1;
      break;
    }
    I.UndefinedEffects.OperationDigest = lowUndefinedOperationDigest(I.Ops);
    const auto R = specializeInterpreter(Bad, {0x100}, O);
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
  for (unsigned Missing = 0; Missing != 3; ++Missing) {
    auto Bad = O;
    if (Missing == 0)
      Bad.ExplicitMachineState = false;
    if (Missing == 1)
      Bad.NormalNonfaultingExecution = false;
    if (Missing == 2)
      Bad.ByteOrder = llvm::endianness::big;
    const auto R = specializeInterpreter(P, {0x100}, Bad);
    EXPECT_FALSE(R.complete());
    EXPECT_TRUE(R.Residual.Blocks.empty());
  }
}

TEST(InterpreterSpecialization, TimestampAddressRemainsUnknownOrigin) {
  Provider P;
  addTimestamp(P, 0x100);
  P.add(0x102, {ret()});
  auto O = ordinaryReturnOptions();
  O.ExplicitMachineState = O.NormalNonfaultingExecution = true;
  ASSERT_TRUE(specializeInterpreter(P, {0x100}, O).complete());
  P.add(0x102,
        {op(NdOp::INT_LEFT, reg(x86reg::RDX), {reg(x86reg::RDX), constant(32)}),
         op(NdOp::INT_OR, reg(0), {reg(0), reg(x86reg::RDX)}),
         op(NdOp::STORE, {}, {reg(0), constant(7)}), ret()});
  const auto R = specializeInterpreter(P, {0x100}, O);
  EXPECT_EQ(R.Status, SpecializationStatus::Unsupported);
  EXPECT_NE(R.Diagnostic.find("unknown address origin"), std::string::npos);
  EXPECT_TRUE(R.Residual.Blocks.empty());
}

TEST(InterpreterSpecialization, TimestampTransitionMatchesCompleteStateOracle) {
  Provider P;
  addTimestamp(P, 0x100);
  const auto &I = P.Code.at(0x100);
  const auto Sample = NdVar::tmp(0x1000, 8);
  const auto Transition = x64::lowerRuntimeTimestampRead(I, Sample);
  ASSERT_TRUE(Transition);
  for (uint64_t Value : {uint64_t{0}, uint64_t{1}, uint64_t{0xffffffff},
                         UINT64_C(0x8000000000000000),
                         UINT64_C(0x12345678abcdef01), UINT64_MAX}) {
    SymContext C;
    SymState S(C);
    const auto Canary = [](uint64_t Offset) {
      return UINT64_C(0xdeadbeef87654321) ^ Offset;
    };
    for (uint64_t Offset = 0; Offset < 16 * 8; Offset += 8)
      S.write(SymSpace::Register, Offset, C.mkConst(64, Canary(Offset)));
    for (uint64_t Flag : x64::ModelledFlags)
      S.write(SymSpace::Register, Flag, C.mkConst(8, 1));
    S.write(SymSpace::Register, x86reg::XMM0,
            C.mkConst(llvm::APInt(128, Canary(0), true)));
    const auto FS = C.mkConst(64, Canary(0x100));
    const auto GS = C.mkConst(64, Canary(0x200));
    ASSERT_TRUE(
        S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, FS));
    ASSERT_TRUE(
        S.setX64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, GS));
    ASSERT_TRUE(S.store(C.mkConst(64, 0x400), C.mkConst(64, Canary(0x400))));
    S.write(SymSpace::Temporary, Sample.Offset, C.mkConst(64, Value));
    SymExec E(C, S);
    for (const auto &Op : Transition->Ops)
      ASSERT_EQ(E.step(Op), StepResult::Continue);
    for (const auto &Op : llvm::ArrayRef(I.Ops).drop_front())
      ASSERT_EQ(E.step(Op), StepResult::Continue);
    for (uint64_t Offset = 0; Offset < 16 * 8; Offset += 8) {
      const uint64_t Expected = Offset == x86reg::RAX   ? uint32_t(Value)
                                : Offset == x86reg::RDX ? Value >> 32
                                                        : Canary(Offset);
      const auto Actual = C.asConst(S.read(SymSpace::Register, Offset, 8));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(Actual->getZExtValue(), Expected) << Offset;
    }
    for (uint64_t Flag : x64::ModelledFlags)
      EXPECT_EQ(C.asConst(S.read(SymSpace::Register, Flag, 1))->getZExtValue(),
                1u);
    EXPECT_EQ(*C.asConst(S.read(SymSpace::Register, x86reg::XMM0, 16)),
              llvm::APInt(128, Canary(0), true));
    EXPECT_EQ(C.asConst(S.load(C.mkConst(64, 0x400), 8))->getZExtValue(),
              Canary(0x400));
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS), FS);
    EXPECT_EQ(S.x64MemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS), GS);
    EXPECT_EQ(E.opaqueOperationCount(), 0u);
  }
  for (auto Invalid :
       {NdVar::reg(0, 8), NdVar::tmp(0x1000, 4), NdVar::tmp(UINT64_MAX - 3, 8),
        NdVar::tmp(Transition->PlaceholderOutputs[0].Offset, 8)})
    EXPECT_FALSE(x64::lowerRuntimeTimestampRead(I, Invalid));
}

#include "AbsoluteMemoryFactsTests.inc"
#include "AlignmentRecoveryTests.inc"
#include "CPUQueryRecoveryTests.inc"
#include "ControlRangeTests.inc"
#include "MXCSRRecoveryTests.inc"
#include "PageGlobalRecoveryTests.inc"
#include "RepeatedCountRecoveryTests.inc"
#include "SavedAddressCellsTests.inc"
#include "SavedScalarCellsTests.inc"
#include "SegmentedStoreTests.inc"
#include "SupervisorReadRecoveryTests.inc"

} // namespace
