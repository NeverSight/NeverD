//===- EVMBytecodeRuntimeExtract.cpp - Static EVM runtime extraction -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMBytecodeDetail.h"

#include "neverd/evm/bytecode/EVMDecoder.h"
#include "neverd/evm/bytecode/EVMOpcodes.h"

#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::evm::detail {
namespace {

using AbstractValue = std::optional<uint64_t>;

AbstractValue pushedValue(const LowInstruction &Instruction) {
  // PUSH0 carries no immediate, so it has no decode status to be complete and
  // nothing that could have been truncated. It pushes zero.
  if (Instruction.Info.ImmediateBytes == 0)
    return uint64_t{0};
  // A push whose data ran off the end of the code pushes bytes the chain never
  // supplied, so its value is unknown rather than zero-padded.
  if (Instruction.ImmediateStatus != ImmediateDecodeStatus::Complete)
    return std::nullopt;
  if (Instruction.Immediate.getActiveBits() >
      std::numeric_limits<uint64_t>::digits)
    return std::nullopt;
  return Instruction.Immediate.getZExtValue();
}

struct StaticCopy {
  uint64_t Destination = 0;
  uint64_t Source = 0;
  uint64_t Size = 0;
};

} // namespace

std::optional<std::vector<uint8_t>>
extractStaticRuntime(llvm::ArrayRef<uint8_t> Code, Hardfork Fork) {
  std::vector<AbstractValue> Stack;
  std::optional<StaticCopy> LastCopy;
  auto Pop = [&]() -> AbstractValue {
    if (Stack.empty())
      return std::nullopt;
    AbstractValue Value = Stack.back();
    Stack.pop_back();
    return Value;
  };

  for (size_t PC = 0; PC < Code.size();) {
    const LowInstruction Instruction =
        decodeInstructionAt(Code, PC, Fork, /*Diagnostics=*/nullptr);
    PC = Instruction.NextPC;
    // A byte the fork does not execute faults, which ends the constructor's
    // linear path just as a terminator does.
    if (!Instruction.isExecutable())
      return std::nullopt;

    const Opcode Op = Instruction.opcode();
    if (isPush(Op)) {
      Stack.push_back(pushedValue(Instruction));
      continue;
    }
    if (isDup(Op)) {
      const size_t Depth = dupDepth(Op);
      Stack.push_back(Stack.size() >= Depth ? Stack[Stack.size() - Depth]
                                            : AbstractValue{});
      continue;
    }
    if (isSwap(Op)) {
      const size_t Depth = swapDepth(Op);
      if (Stack.size() > Depth)
        std::swap(Stack.back(), Stack[Stack.size() - 1 - Depth]);
      else
        Stack.clear();
      continue;
    }
    // The operand-indexed stack instructions reach an arbitrary depth chosen
    // by an immediate. No compiler emits one in a constructor wrapper, so
    // declining to model them costs nothing and keeps this walk from claiming
    // a provenance it did not follow.
    if (isDeepDup(Op) || isDeepSwap(Op) || isExchange(Op))
      return std::nullopt;
    if (Op == Opcode::POP) {
      (void)Pop();
      continue;
    }
    if (Op == Opcode::CODECOPY) {
      const AbstractValue Destination = Pop();
      const AbstractValue Source = Pop();
      const AbstractValue Size = Pop();
      if (Destination && Source && Size)
        LastCopy = StaticCopy{*Destination, *Source, *Size};
      else
        LastCopy.reset();
      continue;
    }
    if (Op == Opcode::RETURN) {
      const AbstractValue Offset = Pop();
      const AbstractValue Size = Pop();
      // A static constructor wrapper copies an embedded runtime that follows
      // its terminal RETURN. Requiring that provenance prevents ordinary
      // runtime CODECOPY/RETURN logic from being destructively reclassified.
      if (Offset && Size && LastCopy && LastCopy->Destination == *Offset &&
          LastCopy->Size == *Size && LastCopy->Source >= PC &&
          LastCopy->Source <= Code.size() &&
          LastCopy->Size <= Code.size() - LastCopy->Source) {
        const size_t Source = static_cast<size_t>(LastCopy->Source);
        const size_t Size = static_cast<size_t>(LastCopy->Size);
        const llvm::ArrayRef<uint8_t> Runtime = Code.slice(Source, Size);
        return std::vector<uint8_t>(Runtime.begin(), Runtime.end());
      }
      return std::nullopt;
    }

    // This bounded abstract interpreter follows only the constructor's linear
    // path. A terminal/control-transfer instruction ends that proof; scanning
    // fallthrough bytes would reinterpret unreachable runtime data as code.
    if (Instruction.Info.IsTerminator)
      return std::nullopt;
    // The extractor remembers that a CODECOPY established a byte-for-byte
    // provenance relationship with the creation code. Any later memory write
    // invalidates that conservative proof, including compound host operations
    // such as CALL and EXTCODECOPY whose primary effect is not memory access.
    if (mayWriteMemory(Instruction.Info))
      LastCopy.reset();
    for (uint8_t I = 0; I < Instruction.Info.StackPops; ++I)
      (void)Pop();
    for (uint8_t I = 0; I < Instruction.Info.StackPushes; ++I)
      Stack.push_back(std::nullopt);
  }
  return std::nullopt;
}

} // namespace neverd::evm::detail
