//===- MIPSControlTarget.h - Mode-qualified recovered call targets -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve a numeric machine control operand before discarding its LowIR
/// occurrence. Storage-slot addresses are not control operands.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_MIPS_MIPSCONTROLTARGET_H
#define NEVERD_IR_LOW_MIPS_MIPSCONTROLTARGET_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImageModel.h"

namespace neverd {

inline std::optional<va_t>
qualifiedMIPSControlTarget(const BinaryImage &Image, InstructionMode Source,
                           LowInstructionTargetMode Contract, va_t RawTarget) {
  if (!hasCompressedMIPSCode(Image.Mode))
    return RawTarget;
  auto Target = canonicalizeLowControlTarget(RawTarget, Source, Contract,
                                             Image.getPointerSize());
  if (!Target) {
    llvm::consumeError(Target.takeError());
    return std::nullopt;
  }
  // The VA-only function inventory requires local callee mode evidence.
  if (Image.instructionModeAt(Target->Address) != Target->Mode)
    return std::nullopt;
  return Target->Address;
}

inline std::optional<va_t> qualifiedMIPSCallTarget(const BinaryImage &Image,
                                                   const LowBlock &Block,
                                                   size_t Operation,
                                                   va_t RawTarget) {
  if (!hasCompressedMIPSCode(Image.Mode))
    return RawTarget;
  const auto End = std::upper_bound(
      Block.InstructionBoundaries.begin(), Block.InstructionBoundaries.end(),
      Operation, [](size_t Index, const LowInstructionBoundary &Boundary) {
        return Index < Boundary.FirstOp;
      });
  if (End == Block.InstructionBoundaries.begin())
    return std::nullopt;
  const auto &Boundary = *std::prev(End);
  if (Operation - Boundary.FirstOp >= Boundary.OpCount)
    return std::nullopt;
  return qualifiedMIPSControlTarget(Image, Boundary.Mode, Boundary.TargetMode,
                                    RawTarget);
}

} // namespace neverd

#endif // NEVERD_IR_LOW_MIPS_MIPSCONTROLTARGET_H
