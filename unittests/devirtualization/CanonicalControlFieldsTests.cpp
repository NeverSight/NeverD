//===- CanonicalControlFieldsTests.cpp - Control carriers ----------------===//
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
constexpr va_t ReadLow = 0x200;
constexpr va_t DispatchLow = 0x300;
constexpr va_t ReadWide = 0x400;
constexpr va_t DispatchWide = 0x500;
constexpr va_t Exit = 0x600;
constexpr va_t LowTable = 0x6000;
constexpr va_t WideTable = 0x7000;
constexpr uint64_t Payload = 48;
constexpr uint64_t Word = 64;
constexpr uint64_t SelectorLow = 80;
constexpr uint64_t SelectorWide = 96;
constexpr uint64_t Address = 112;
constexpr uint64_t Target = 128;
constexpr uint64_t Scratch = 144;

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

class CanonicalProvider final : public SpecializationProvider {
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
                                     "unknown public canonical-control node");
    return It->second;
  }

  std::optional<SpecializationImmutableRead>
  immutableRead(va_t Address, uint16_t Bytes) override {
    SpecializationImmutableRead Read;
    Read.Evidence = "independently authored canonical-control table";
    for (unsigned I = 0; I < Bytes; ++I) {
      const auto It = Image.find(Address + I);
      if (It == Image.end())
        return std::nullopt;
      Read.Bytes.push_back(It->second);
    }
    return Read;
  }
};

CanonicalProvider makeProgram(llvm::endianness Order,
                              bool UnknownHigh = false) {
  CanonicalProvider Provider;
  const auto ImageWord = [&](va_t At, uint64_t Value) {
    for (unsigned I = 0; I < 8; ++I)
      Provider.Image[At + I] =
          Value >> (8 * (Order == llvm::endianness::little ? I : 7 - I));
  };
  ImageWord(LowTable, ReadWide);
  ImageWord(LowTable + 8, ReadWide);
  ImageWord(WideTable, Exit);
  const uint64_t PayloadLow =
      Payload + (Order == llvm::endianness::little ? 0 : 6);
  const uint64_t WordLow = Word + (Order == llvm::endianness::little ? 0 : 1);
  if (UnknownHigh)
    Provider.add(
        Entry,
        {operation(NdOp::INT_AND, r(Word, 2), {r(PayloadLow, 2), c(0xff00, 2)}),
         operation(NdOp::INT_AND, r(Scratch, 2), {r(PayloadLow, 2), c(1, 2)}),
         operation(NdOp::INT_OR, r(Word, 2), {r(Word, 2), r(Scratch, 2)}),
         jump(ReadLow)});
  else
    Provider.add(
        Entry,
        {operation(NdOp::INT_AND, r(Word, 2), {r(PayloadLow, 2), c(1, 2)}),
         operation(NdOp::INT_MULT, r(Word, 2), {r(Word, 2), c(0x101, 2)}),
         jump(ReadLow)});
  // The first unresolved dispatch demands the bank's low byte. The bank's
  // wider correlated value is only used after that dispatch has been proved.
  Provider.add(ReadLow,
               {operation(NdOp::COPY, r(SelectorLow, 1), {r(WordLow, 1)}),
                jump(DispatchLow)});
  Provider.add(DispatchLow,
               {operation(NdOp::INT_ZEXT, r(Address), {r(SelectorLow, 1)}),
                operation(NdOp::INT_MULT, r(Address), {r(Address), c(8)}),
                operation(NdOp::INT_ADD, r(Address), {r(Address), c(LowTable)}),
                operation(NdOp::LOAD, r(Target), {r(Address)}),
                operation(NdOp::INDIR_BR, {}, {r(Target)})});
  Provider.add(ReadWide,
               {operation(NdOp::COPY, r(SelectorWide, 2), {r(Word, 2)}),
                jump(DispatchWide)});
  // Both genuine words (0 and 0x101) make this carry-dependent bit zero.
  // Treating the bytes as independent admits 1 and 0x100, which do not.
  Provider.add(
      DispatchWide,
      {operation(NdOp::INT_ADD, r(Scratch, 2),
                 {r(SelectorWide, 2), c(0xff, 2)}),
       operation(NdOp::INT_AND, r(Scratch, 2), {r(Scratch, 2), c(0x100, 2)}),
       operation(NdOp::INT_ZEXT, r(Address), {r(Scratch, 2)}),
       operation(NdOp::INT_MULT, r(Address), {r(Address), c(8)}),
       operation(NdOp::INT_ADD, r(Address), {r(Address), c(WideTable)}),
       operation(NdOp::LOAD, r(Target), {r(Address)}),
       operation(NdOp::INDIR_BR, {}, {r(Target)})});
  Provider.add(Exit, {operation(NdOp::RETURN, {}, {r(Payload)})});
  return Provider;
}

// Independent byte-level execution of the small residual instruction set.
// Unsupported operations, missing bytes, and uncertified residual loads fail.
std::optional<uint64_t> execute(const LowFunc &Function, llvm::endianness Order,
                                uint64_t PayloadValue) {
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
      case NdOp::INT_ZEXT:
      case NdOp::COPY:
        write(Op.Output, read(Op.Inputs[0]));
        break;
      case NdOp::INT_AND:
        write(Op.Output, read(Op.Inputs[0]) & read(Op.Inputs[1]));
        break;
      case NdOp::INT_OR:
        write(Op.Output, read(Op.Inputs[0]) | read(Op.Inputs[1]));
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

} // namespace

TEST(CanonicalControlFields, LaterWideCarrierReplacesEarlierNarrowField) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
    auto Provider = makeProgram(Order);
    SpecializationOptions Options;
    Options.ByteOrder = Order;
    Options.DiscoverControlState = true;
    // Two selectors, the final bank carrier, and one upstream input byte.
    // Counting the replaced narrow bank range would require a fifth field.
    Options.MaxControlFields = 4;
    const auto Result = specializeInterpreter(Provider, {Entry}, Options);
    ASSERT_TRUE(Result.complete()) << Result.Diagnostic;
    EXPECT_GT(Result.ControlRefinements, 1u);
    ASSERT_FALSE(Result.Reads.empty());
    for (const auto &Read : Result.Reads)
      EXPECT_TRUE(Read.Address == LowTable || Read.Address == LowTable + 8 ||
                  Read.Address == WideTable);
    for (uint64_t Input : {uint64_t{0}, uint64_t{1}, uint64_t{0x100},
                           uint64_t{0x101}, uint64_t{0x123456789abcdef0},
                           uint64_t{0x123456789abcdef1}, ~uint64_t{0}})
      EXPECT_EQ(execute(Result.Residual, Order, Input), Input);
  }
}

TEST(CanonicalControlFields, UnknownHighByteDoesNotAcquireLowByteFacts) {
  for (auto Order : {llvm::endianness::little, llvm::endianness::big}) {
    SCOPED_TRACE(Order == llvm::endianness::little ? "little" : "big");
    auto Provider = makeProgram(Order, true);
    SpecializationOptions Options;
    Options.ByteOrder = Order;
    Options.DiscoverControlState = true;
    const auto Result = specializeInterpreter(Provider, {Entry}, Options);
    EXPECT_EQ(Result.Status, SpecializationStatus::UnresolvedControl)
        << Result.Diagnostic;
    EXPECT_TRUE(Result.Residual.Blocks.empty());
    EXPECT_TRUE(Result.Reads.empty());
    EXPECT_TRUE(Result.Origins.empty());
  }
}
