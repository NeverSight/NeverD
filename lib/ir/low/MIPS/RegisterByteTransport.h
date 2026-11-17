//===- RegisterByteTransport.h - Pure MIPS register transfers -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_MIPS_REGISTERBYTETRANSPORT_H
#define NEVERD_IR_LOW_MIPS_REGISTERBYTETRANSPORT_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

namespace neverd::mips {

// Values 0..255 are constant bytes; higher values identify an incoming
// register byte by significance. These are register views, not guest memory.
struct RegisterByteTransport {
  using Bytes = llvm::SmallVector<uint64_t, 16>;
  static constexpr uint64_t RegisterByteBase = 256;
  static constexpr uint64_t UnknownByte = UINT64_MAX;
  llvm::SmallDenseMap<uint64_t, uint64_t, 32> Registers;
  llvm::SmallDenseMap<uint64_t, uint64_t, 32> Temporaries;

  std::optional<Bytes> read(const NdVar &Value) const {
    if (!Value.Size || Value.Size > mipsreg::VectorBytes)
      return std::nullopt;
    Bytes Result;
    for (unsigned I = 0; I < Value.Size; ++I) {
      if (Value.isConst()) {
        if (Value.Size > sizeof(Value.Offset))
          return std::nullopt;
        Result.push_back((Value.Offset >> (8 * I)) & 255);
      } else if (Value.isReg()) {
        const auto Found = Registers.find(Value.Offset + I);
        Result.push_back(Found == Registers.end()
                             ? RegisterByteBase + Value.Offset + I
                             : Found->second);
      } else if (Value.isTemp()) {
        const auto Found = Temporaries.find(Value.Offset + I);
        if (Found == Temporaries.end())
          return std::nullopt;
        Result.push_back(Found->second);
      } else {
        return std::nullopt;
      }
    }
    return Result;
  }

  static std::optional<uint64_t> constant(llvm::ArrayRef<uint64_t> Value) {
    if (Value.size() > sizeof(uint64_t))
      return std::nullopt;
    uint64_t Result = 0;
    for (unsigned I = 0; I < Value.size(); ++I) {
      if (Value[I] >= RegisterByteBase)
        return std::nullopt;
      Result |= Value[I] << (8 * I);
    }
    return Result;
  }

  bool write(const NdVar &Output, llvm::ArrayRef<uint64_t> Value) {
    if (Output.Size != Value.size() || (!Output.isReg() && !Output.isTemp()))
      return false;
    auto &Bank = Output.isReg() ? Registers : Temporaries;
    for (unsigned I = 0; I < Value.size(); ++I)
      Bank[Output.Offset + I] = Value[I];
    return true;
  }

  bool step(const LowOp &Op) {
    if (Op.Opcode == NdOp::NOP)
      return Op.NumInputs == 0 && Op.Output.Size == 0;
    if (!Op.Output.Size || Op.NumInputs == 0 || Op.NumInputs > 2)
      return false;
    auto A = read(Op.Inputs[0]);
    auto B = Op.NumInputs == 2 ? read(Op.Inputs[1]) : std::optional<Bytes>{};
    if (!A || (Op.NumInputs == 2 && !B))
      return false;
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
      return write(Op.Output, *A);
    if (Op.Opcode == NdOp::SUBBYTES && B) {
      const auto Offset = constant(*B);
      if (!Offset || *Offset > A->size() ||
          Op.Output.Size > A->size() - *Offset)
        return false;
      return write(Op.Output,
                   llvm::ArrayRef(*A).slice(*Offset, Op.Output.Size));
    }
    if (Op.Opcode == NdOp::CONCAT && B) {
      B->append(A->begin(), A->end());
      return write(Op.Output, *B);
    }
    // Only address construction may perform arithmetic. An unknown argument
    // bit cannot acquire a value through arithmetic or an unsupported opcode.
    const auto Left = constant(*A);
    const auto Right = B ? constant(*B) : std::optional<uint64_t>{};
    if (!Left || !Right || Op.Output.Size > sizeof(uint64_t))
      return false;
    uint64_t Result;
    switch (Op.Opcode) {
    case NdOp::INT_ADD:
      Result = *Left + *Right;
      break;
    case NdOp::INT_OR:
      Result = *Left | *Right;
      break;
    case NdOp::INT_LEFT:
      if (*Right >= Op.Output.Size * 8)
        return false;
      Result = *Left << *Right;
      break;
    default:
      return false;
    }
    return write(Op.Output, *read(NdVar::cst(Result, Op.Output.Size)));
  }
};

} // namespace neverd::mips

#endif
