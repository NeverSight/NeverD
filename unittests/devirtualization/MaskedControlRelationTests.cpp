//===- MaskedControlRelationTests.cpp - Partial control-field joins -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/InterpreterSpecialization.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"

#include <map>
#include <optional>

using namespace neverd;
using namespace neverd::analysis;

namespace {
constexpr va_t Entry = 0x100;
constexpr va_t Taken = 0x200;
constexpr va_t Other = 0x300;
constexpr va_t Join = 0x400;
constexpr va_t Flip = 0x480;
constexpr va_t Dispatch = 0x490;
constexpr va_t Handler = 0x500;
constexpr va_t Table = 0x6000;
constexpr uint64_t Route = 48;
constexpr uint64_t Payload = 56;
constexpr uint64_t Word = 64;
constexpr uint64_t Index = 72;
constexpr uint64_t Address = 80;
constexpr uint64_t Target = 88;
constexpr uint64_t Condition = 96;
constexpr uint64_t ConstantWord = 0x4ad3b682715ec094;

NdVar r(uint64_t Offset, uint16_t Bytes = 8) {
  return NdVar::reg(Offset, Bytes);
}
NdVar c(uint64_t Value, uint16_t Bytes = 8) {
  return NdVar::scalar(Value, Bytes);
}
LowOp operation(NdOp Kind, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Kind;
  Op.Output = Output;
  for (auto Input : Inputs)
    Op.addInput(Input);
  return Op;
}
LowOp jump(va_t Destination) {
  return operation(NdOp::BRANCH, {}, {NdVar::cst(Destination, 8)});
}

class MaskedProvider final : public SpecializationProvider {
public:
  std::map<va_t, SpecializationInstruction> Code;
  std::map<va_t, uint8_t> Image;

  void add(va_t Address, llvm::ArrayRef<LowOp> Ops,
           va_t Fallthrough = InvalidVA) {
    SpecializationInstruction Instruction;
    Instruction.Ops.assign(Ops.begin(), Ops.end());
    Instruction.Origin.Address = Address;
    Instruction.Origin.Size = 1;
    Instruction.Origin.OpCount = Ops.size();
    Instruction.Fallthrough = {Fallthrough == InvalidVA ? Address + 1
                                                        : Fallthrough};
    for (size_t I = 0; I < Instruction.Ops.size(); ++I) {
      auto &Op = Instruction.Ops[I];
      Op.Addr = Address;
      Op.Seq = static_cast<int>(I);
      if (Op.Opcode == NdOp::RETURN) {
        Instruction.Origin.Control = LowInstructionControl::Return;
        Instruction.Origin.ControlFlags = LowInstructionControlFlag::Return;
      } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR ||
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
      }
    }
    Code.emplace(Address, std::move(Instruction));
  }

  llvm::Expected<SpecializationInstruction>
  instruction(SpecializationCursor Cursor) override {
    const auto It = Code.find(Cursor.Address);
    if (It == Code.end())
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "unknown public masked-control node");
    return It->second;
  }

  std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Bytes) override {
    SpecializationImmutableRead Read;
    Read.Evidence = "independently authored masked-control table";
    for (unsigned I = 0; I < Bytes; ++I) {
      const auto It = Image.find(Address + I);
      if (It == Image.end())
        return std::nullopt;
      Read.Bytes.push_back(It->second);
    }
    return Read;
  }
};

MaskedProvider makeProgram(llvm::endianness Order, bool ConstantFirst,
                           bool BranchAfterDispatch = false) {
  MaskedProvider Provider;
  for (unsigned I = 0; I < 8; ++I)
    Provider.Image[Table + I] =
        Handler >> (8 * (Order == llvm::endianness::little ? I : 7 - I));
  Provider.add(Entry, {operation(NdOp::COND_BR, {}, {c(Taken), r(Route, 1)})},
               Other);
  const auto Constant = operation(NdOp::COPY, r(Word), {c(ConstantWord)});
  const auto Dynamic =
      operation(NdOp::INT_AND, r(Word), {r(Payload), c(~uint64_t(1))});
  Provider.add(Taken, {ConstantFirst ? Constant : Dynamic, jump(Join)});
  Provider.add(Other, {ConstantFirst ? Dynamic : Constant, jump(Join)});
  if (BranchAfterDispatch) {
    // Keep the load and its indirect use together so discovery requests the
    // word's control bit directly, rather than also tracking a target register.
    Provider.add(Join,
                 {operation(NdOp::INT_AND, r(Index), {r(Word), c(1)}),
                  operation(NdOp::INT_MULT, r(Address), {r(Index), c(8)}),
                  operation(NdOp::INT_ADD, r(Address), {r(Address), c(Table)}),
                  operation(NdOp::LOAD, r(Target), {r(Address)}),
                  operation(NdOp::INDIR_BR, {}, {r(Target)})});
    Provider.add(
        Handler,
        {operation(NdOp::INT_AND, r(Index), {r(Word), c(0x20)}),
         operation(NdOp::INT_NOTEQUAL, r(Condition, 1), {r(Index), c(0)}),
         operation(NdOp::COND_BR, {}, {c(Flip), r(Condition, 1)})},
        Dispatch);
    Provider.add(Flip, {operation(NdOp::INT_XOR, r(Word), {r(Word), c(0x100)}),
                        operation(NdOp::RETURN, {}, {r(Word)})});
    Provider.add(Dispatch, {operation(NdOp::RETURN, {}, {r(Word)})});
    return Provider;
  }
  Provider.add(
      Join,
      {operation(NdOp::INT_AND, r(Index), {r(Word), c(1)}),
       operation(NdOp::INT_MULT, r(Address), {r(Index), c(8)}),
       operation(NdOp::INT_ADD, r(Address), {r(Address), c(Table)}),
       operation(NdOp::LOAD, r(Target), {r(Address)}),
       operation(NdOp::INT_AND, r(Index), {r(Word), c(0x20)}),
       operation(NdOp::INT_NOTEQUAL, r(Condition, 1), {r(Index), c(0)}),
       operation(NdOp::COND_BR, {}, {c(Flip), r(Condition, 1)})},
      Dispatch);
  Provider.add(Flip, {operation(NdOp::INT_XOR, r(Word), {r(Word), c(0x100)}),
                      jump(Dispatch)});
  Provider.add(Dispatch, {operation(NdOp::INDIR_BR, {}, {r(Target)})});
  Provider.add(Handler, {operation(NdOp::RETURN, {}, {r(Word)})});
  return Provider;
}

// Independent byte-level execution of the small residual instruction set.
// Unsupported operations, missing bytes, and uncertified residual loads fail.
std::optional<uint64_t> execute(const LowFunc &Function, llvm::endianness Order,
                                uint8_t RouteValue, uint64_t PayloadValue) {
  using ByteKey = std::pair<VnodeSpace, uint64_t>;
  std::map<ByteKey, uint8_t> Bytes;
  std::map<int, const LowBlock *> Blocks;
  for (const auto &Block : Function.Blocks)
    Blocks.emplace(Block.Id, &Block);
  if (Function.Blocks.empty())
    return std::nullopt;
  bool Valid = true;
  const auto shift = [&](unsigned I, unsigned Width) {
    return 8 * (Order == llvm::endianness::little ? I : Width - I - 1);
  };
  const auto write = [&](NdVar Location, uint64_t Value) {
    for (unsigned I = 0; I < Location.Size; ++I)
      Bytes[{Location.Space, Location.Offset + I}] =
          Value >> shift(I, Location.Size);
  };
  const auto read = [&](NdVar Value) {
    if (Value.isConst())
      return Value.Offset;
    uint64_t Result = 0;
    for (unsigned I = 0; I < Value.Size; ++I) {
      const auto It = Bytes.find({Value.Space, Value.Offset + I});
      if (It == Bytes.end()) {
        Valid = false;
        return uint64_t{0};
      }
      Result |= uint64_t(It->second) << shift(I, Value.Size);
    }
    return Result;
  };
  write(r(Route, 1), RouteValue);
  write(r(Payload), PayloadValue);
  int Current = Function.Blocks.front().Id;
  for (unsigned Step = 0; Step < 32; ++Step) {
    const auto At = Blocks.find(Current);
    if (At == Blocks.end())
      return std::nullopt;
    const auto &Block = *At->second;
    std::optional<int> Next;
    for (const auto &Op : Block.Ops) {
      switch (Op.Opcode) {
      case NdOp::COPY:
        write(Op.Output, read(Op.Inputs[0]));
        break;
      case NdOp::INT_AND:
        write(Op.Output, read(Op.Inputs[0]) & read(Op.Inputs[1]));
        break;
      case NdOp::INT_XOR:
        write(Op.Output, read(Op.Inputs[0]) ^ read(Op.Inputs[1]));
        break;
      case NdOp::INT_NOTEQUAL:
        write(Op.Output, read(Op.Inputs[0]) != read(Op.Inputs[1]));
        break;
      case NdOp::INT_ADD:
        write(Op.Output, read(Op.Inputs[0]) + read(Op.Inputs[1]));
        break;
      case NdOp::INT_MULT:
        write(Op.Output, read(Op.Inputs[0]) * read(Op.Inputs[1]));
        break;
      case NdOp::BRANCH:
      case NdOp::COND_BR: {
        const uint64_t Destination = read(Op.Inputs[0]);
        const bool Taken = Op.Opcode == NdOp::BRANCH || read(Op.Inputs[1]);
        for (int Successor : Block.Succs)
          if ((Blocks.at(Successor)->StartAddr == Destination) == Taken)
            Next = Successor;
        if (!Next && Block.Succs.size() == 1)
          Next = Block.Succs.front();
        break;
      }
      case NdOp::RETURN: {
        const auto Value = read(Op.Inputs[0]);
        return Valid ? std::optional<uint64_t>{Value} : std::nullopt;
      }
      case NdOp::NOP:
        break;
      default:
        return std::nullopt;
      }
      if (!Valid)
        return std::nullopt;
    }
    if (!Next)
      return std::nullopt;
    Current = *Next;
  }
  return std::nullopt;
}

void expectRuntimeValues(const LowFunc &Function, llvm::endianness Order,
                         bool ConstantFirst) {
  for (uint8_t RouteValue : {0, 1})
    for (uint64_t PayloadValue :
         {uint64_t{0}, uint64_t{1}, uint64_t{0x20}, uint64_t{0x85},
          uint64_t{0x123456789abcdef0}, ~uint64_t{0}}) {
      const bool ConstantPath = (RouteValue != 0) == ConstantFirst;
      uint64_t Expected =
          ConstantPath ? ConstantWord : PayloadValue & ~uint64_t(1);
      if (Expected & 0x20)
        Expected ^= 0x100;
      EXPECT_EQ(execute(Function, Order, RouteValue, PayloadValue), Expected);
    }
}
} // namespace

TEST(MaskedControlRelation,
     ConstantAndPartialPredecessorsKeepOtherBitsDynamic) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (bool ConstantFirst : {false, true}) {
      SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
      SCOPED_TRACE(ConstantFirst);
      auto Provider = makeProgram(Order, ConstantFirst);
      SpecializationOptions Options;
      Options.ByteOrder = Order;
      Options.DiscoverControlState = true;
      const auto Result = specializeInterpreter(Provider, {Entry}, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_GT(Result.DiscoveredControlFields, 0u);
      EXPECT_GT(Result.ControlRefinements, 0u);
      ASSERT_EQ(Result.Reads.size(), 1u);
      EXPECT_EQ(Result.Reads.front().Address, Table);
      expectRuntimeValues(Result.Residual, Order, ConstantFirst);
    }
}

TEST(MaskedControlRelation, ManualWideCarrierFallsBackWithoutAddingFields) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big})
    for (bool ConstantFirst : {false, true}) {
      SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
      SCOPED_TRACE(ConstantFirst);
      auto Provider = makeProgram(Order, ConstantFirst, true);
      SpecializationOptions Options;
      Options.ByteOrder = Order;
      Options.DiscoverControlState = true;
      Options.ControlRegisters = {{Word, 8}};
      Options.MaxControlFields = 1;
      const auto Result = specializeInterpreter(Provider, {Entry}, Options);
      ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
      EXPECT_EQ(Result.DiscoveredControlFields, 0u);
      EXPECT_EQ(Result.DiscoveredContextFields, 0u);
      EXPECT_GT(Result.ControlRefinements, 0u);
      ASSERT_FALSE(Result.Reads.empty());
      for (const auto &Read : Result.Reads)
        EXPECT_EQ(Read.Address, Table);
      expectRuntimeValues(Result.Residual, Order, ConstantFirst);
    }
}
