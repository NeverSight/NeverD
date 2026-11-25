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

#include "llvm/Support/MathExtras.h"

namespace neverd {

/// Convert an n32 or wide-o32 GPR to an ELF32 code coordinate. Only the
/// canonical sign extension is another representation of the same address.
inline va_t mipsRegisterCodeAddress(Arch Architecture, va_t Raw) {
  if (hasMIPS32BitAddressIn64BitGPR(Architecture) && Raw != InvalidVA &&
      llvm::isInt<32>(static_cast<int64_t>(Raw)))
    return static_cast<uint32_t>(Raw);
  return Raw;
}

inline llvm::Expected<LowControlTarget>
canonicalizeImageControlTarget(const BinaryImage &Image, InstructionMode Source,
                               LowInstructionTargetMode Contract, va_t Raw) {
  if (Contract == LowInstructionTargetMode::FromTargetBit0 ||
      Contract == LowInstructionTargetMode::FromMIPS16TargetBit0 ||
      Source == InstructionMode::Default)
    Raw = mipsRegisterCodeAddress(Image.Arch, Raw);
  return canonicalizeLowControlTarget(Raw, Source, Contract,
                                      Image.getPointerSize());
}

inline std::optional<va_t>
qualifiedMIPSControlTarget(const BinaryImage &Image, InstructionMode Source,
                           LowInstructionTargetMode Contract, va_t RawTarget) {
  if (!hasCompressedMIPSCode(Image.Mode))
    return RawTarget;
  auto Target =
      canonicalizeImageControlTarget(Image, Source, Contract, RawTarget);
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
  if (!hasCompressedMIPSCode(Image.Mode)) {
    if (hasMIPS32BitAddressIn64BitGPR(Image.Arch) &&
        Operation < Block.Ops.size() &&
        Block.Ops[Operation].Opcode == NdOp::INDIR_CALL)
      return mipsRegisterCodeAddress(Image.Arch, RawTarget);
    return RawTarget;
  }
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
