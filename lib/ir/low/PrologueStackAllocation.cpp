//===- PrologueStackAllocation.cpp - Exact prologue allocations -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps decoded scalar allocation sizes visible across compiler stack probes.

#include "PrologueStackAllocation.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <map>
#include <optional>
#include <utility>

namespace neverd {

void canonicalizePrologueStackAllocation(const BinaryImage &Image,
                                         LowFunc &Function) {
  if (Image.Arch != Arch::X64 || Image.abiFormat() != BinaryFormat::COFF ||
      !Function.ExceptionMetadata || Function.Blocks.empty() ||
      Function.Blocks.front().StartAddr != Function.Entry)
    return;
  const uint64_t Size = Function.ExceptionMetadata->PrologueSize;
  if (!Size || Size > InvalidVA - Function.Entry)
    return;
  const va_t End = Function.Entry + Size;
  LowBlock &Block = Function.Blocks.front();
  if (auto Error = validateLowInstructionBoundaries(
          Block, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return;
  }
  const TargetRegInfo &TRI = getTargetRegInfo(Image.Arch);
  const NdVar SP = NdVar::reg(TRI.StackPointer, TRI.PointerSize);
  std::map<uint64_t, uint8_t> Registers;
  std::map<uint64_t, std::pair<uint16_t, uint64_t>> Temporaries;
  auto ValueOf = [&](const NdVar &V) -> std::optional<uint64_t> {
    if (!V.Size || V.Size > 8)
      return std::nullopt;
    if (V.isConst()) {
      if (isAddressProvenance(V.Provenance) ||
          (V.Provenance == ConstantAddressProvenance::Unknown &&
           Image.isPotentiallyRelocatableAddress(V.Offset)))
        return std::nullopt;
      return V.Offset & (UINT64_MAX >> ((8 - V.Size) * 8));
    }
    if (V.isTemp()) {
      const auto At = Temporaries.find(V.Offset);
      return At != Temporaries.end() && At->second.first == V.Size
                 ? std::optional<uint64_t>{At->second.second}
                 : std::nullopt;
    }
    if (!V.isReg() || V.Offset > UINT64_MAX - V.Size)
      return std::nullopt;
    uint64_t Bits = 0;
    for (uint16_t I = 0; I < V.Size; ++I) {
      const auto Byte = Registers.find(V.Offset + I);
      if (Byte == Registers.end())
        return std::nullopt;
      Bits |= uint64_t{Byte->second} << (I * 8);
    }
    return Bits;
  };
  for (const auto &Boundary : Block.InstructionBoundaries) {
    if (Boundary.Address >= End || Boundary.Size > End - Boundary.Address ||
        (Boundary.Address != Function.Entry &&
         Function.ModuleAnalysisRoots.count(Boundary.Address)) ||
        (Boundary.Control != LowInstructionControl::None &&
         Boundary.Control != LowInstructionControl::Call) ||
        hasLowInstructionControlFlag(
            Boundary.ControlFlags, LowInstructionControlFlag::InstructionGuard))
      break;
    Temporaries.clear();
    for (size_t I = Boundary.FirstOp; I < Boundary.FirstOp + Boundary.OpCount;
         ++I) {
      LowOp &Op = Block.Ops[I];
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        // This authority also owns stack-probe handling in ABI recovery and
        // LowToMed. A guessed helper name and an ordinary call prove nothing.
        const bool Probe = Op.Opcode == NdOp::CALL && Op.NumInputs &&
                           Op.Inputs[0].isConst() &&
                           libc::stackProbeEffect(Image, Op.Inputs[0].Offset) ==
                               libc::StackProbeEffect::Probe;
        if (!Probe)
          Registers.clear();
        else
          std::erase_if(Registers, [](const auto &Byte) {
            return Byte.first >= x86reg::GPRSpaceEnd;
          });
        Temporaries.clear();
        continue;
      }
      if (Op.Opcode == NdOp::INTRINSIC) {
        Registers.clear();
        Temporaries.clear();
      }
      if (Op.Opcode == NdOp::INT_SUB && Op.Output == SP && Op.NumInputs == 2 &&
          Op.Inputs[0] == SP && Op.Inputs[1].Size == SP.Size &&
          !Op.Inputs[1].isConst())
        if (const auto Bytes = ValueOf(Op.Inputs[1]))
          Op.Inputs[1] = NdVar::scalar(*Bytes, SP.Size);
      const auto Value = lowScalarConstantResult(Op, ValueOf);
      const NdVar &Output = Op.Output;
      if (Output.isTemp()) {
        if (Value)
          Temporaries[Output.Offset] = {Output.Size, *Value};
        else
          Temporaries.erase(Output.Offset);
      } else if (Output.isReg() && Output.Offset <= UINT64_MAX - Output.Size) {
        const unsigned Written =
            TRI.writeZeroExtends(Output.Offset, Output.Size) ? 8 : Output.Size;
        for (unsigned Byte = 0; Byte < Written; ++Byte)
          if (Byte >= Output.Size)
            Registers[Output.Offset + Byte] = 0;
          else if (Value)
            Registers[Output.Offset + Byte] = uint8_t(*Value >> (8 * Byte));
          else
            Registers.erase(Output.Offset + Byte);
      }
    }
  }
}

} // namespace neverd
