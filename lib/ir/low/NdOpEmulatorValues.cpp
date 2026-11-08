//===- NdOpEmulatorValues.cpp - Partial scalar values -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Transport defined scalar bits without choosing an implementation-dependent
/// value. Representative bits stay private to the emulator.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/NdOpEmulator.h"

#include "llvm/Support/MathExtras.h"

#include <algorithm>

namespace neverd {

uint64_t NdOpEmulator::unknownOperandBits(const NdVar &Operand) const {
  if (!Operand.isReg() && !Operand.isTemp())
    return 0;
  const auto It = UnknownBits.find(Operand.Offset);
  if (It == UnknownBits.end())
    return 0;
  return It->second &
         llvm::maskTrailingOnes<uint64_t>(
             (Operand.Size ? std::min<unsigned>(Operand.Size, 8) : 8) * 8);
}

void NdOpEmulator::setOutputUnknownBits(const NdVar &Output, uint64_t Bits) {
  if (UnknownBits.empty() && !Bits)
    return;
  const uint64_t Written =
      llvm::maskTrailingOnes<uint64_t>(std::min<unsigned>(Output.Size, 8) * 8);
  Bits &= Written;
  auto It = UnknownBits.find(Output.Offset);
  if (Output.isReg() && It != UnknownBits.end())
    Bits |= It->second & ~Written;
  if (Bits)
    UnknownBits[Output.Offset] = Bits;
  else if (It != UnknownBits.end())
    UnknownBits.erase(It);
}

bool NdOpEmulator::executeUncertainScalarCopy(const LowOp &Op) {
  const bool Pair = Op.Opcode == NdOp::CONCAT || Op.Opcode == NdOp::SUBBYTES;
  if (Op.NumInputs != (Pair ? 2 : 1) ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) || !Op.Output.Size ||
      Op.Output.Size > 8 || Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 0; I < Op.NumInputs; ++I) {
    const auto &Input = Op.Inputs[I];
    if ((!Input.isReg() && !Input.isTemp() && !Input.isConst()) ||
        !Input.Size || Input.Size > 8)
      return false;
  }

  if (Op.Opcode == NdOp::SUBBYTES) {
    if (!knownOperandBytes(Op.Inputs[1], 0, Op.Inputs[1].Size))
      return false;
    const auto Offset = readOperand(Op.Inputs[1]);
    if (!availableOperandBytes(Op.Inputs[0], Offset, Op.Output.Size))
      return false;
    writeOutput(Op.Output, readOperand(Op.Inputs[0]) >> (Offset * 8),
                unknownOperandBits(Op.Inputs[0]) >> (Offset * 8));
    return true;
  }

  // An absent register is still absent, even if another operand has tracked
  // unknown bits. Preserve the existing missing-source COPY contract.
  for (unsigned I = 0; I < Op.NumInputs; ++I) {
    const auto &Input = Op.Inputs[I];
    unsigned Needed = std::min(Input.Size, Op.Output.Size);
    if (Op.Opcode == NdOp::CONCAT && I == 0)
      Needed = Op.Output.Size > Op.Inputs[1].Size
                   ? std::min<unsigned>(Input.Size,
                                        Op.Output.Size - Op.Inputs[1].Size)
                   : 0;
    if (Needed && !availableOperandBytes(Input, 0, Needed)) {
      Registers.erase(Op.Output.Offset);
      WideRegisters.erase(Op.Output.Offset);
      UnknownBits.erase(Op.Output.Offset);
      return true;
    }
  }

  uint64_t Value = readOperand(Op.Inputs[0]);
  uint64_t Unknown = unknownOperandBits(Op.Inputs[0]);
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
    break;
  case NdOp::INT_SEXT:
    if (Op.Inputs[0].Size < Op.Output.Size) {
      const unsigned Bits = Op.Inputs[0].Size * 8;
      const auto High = ~llvm::maskTrailingOnes<uint64_t>(Bits);
      if (Value & (UINT64_C(1) << (Bits - 1)))
        Value |= High;
      if (Unknown & (UINT64_C(1) << (Bits - 1)))
        Unknown |= High;
    }
    break;
  case NdOp::CONCAT: {
    const unsigned LowBits = Op.Inputs[1].Size * 8;
    Value = (LowBits < 64 ? Value << LowBits : 0) | readOperand(Op.Inputs[1]);
    Unknown = (LowBits < 64 ? Unknown << LowBits : 0) |
              unknownOperandBits(Op.Inputs[1]);
    break;
  }
  default:
    return false;
  }
  writeOutput(Op.Output, Value, Unknown);
  return true;
}

} // namespace neverd
