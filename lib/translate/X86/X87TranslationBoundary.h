//===- X87TranslationBoundary.h - Canonical terminal x87 slice -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X87TRANSLATIONBOUNDARY_H
#define NEVERD_LIB_TRANSLATE_X86_X87TRANSLATIONBOUNDARY_H

#include "neverd/ir/X86/X87Context.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/translate/TranslationBlock.h"
#include "neverd/translate/X86/RuntimeX87Helpers.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

inline constexpr uint16_t kX87TranslationBoundarySchemaV1 = 1;

inline bool isX87TranslationStep(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs != 0 &&
         Op.Inputs[0].isConst() &&
         Op.Inputs[0].Offset ==
             static_cast<uint16_t>(Intrinsic::X87ContextStep);
}

inline bool hasX87TranslationStep(llvm::ArrayRef<LowOp> Ops) {
  for (const auto &Op : Ops)
    if (isX87TranslationStep(Op))
      return true;
  return false;
}

struct X87TranslationInstructionV1 {
  RuntimeX87StepContractV1 Contract;
  uint64_t StepIndex = 0;
};

/// Inspect a descriptor already reconstructed from its exact source bytes.
/// EA operations precede Step; the helper consumes Step and its architectural
/// result tail as one instruction, so no cached register write can follow it.
inline llvm::Expected<X87TranslationInstructionV1>
x87TranslationInstructionV1(const TranslationBlockDescriptorV1 &Block) {
  const auto Reject = [] {
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "invalid or unqualified terminal x87 instruction");
  };
  if (Block.RuntimeProfile != TranslationRuntimeProfileV1::PortableX87 ||
      Block.Header.Terminator != TranslationBlockTerminatorKindV1::X87Runtime ||
      Block.InstructionBoundaries.empty())
    return Reject();
  const auto &Boundary = Block.InstructionBoundaries.back();
  if (Boundary.FirstOp > Block.Ops.size() ||
      Boundary.OpCount > Block.Ops.size() - Boundary.FirstOp)
    return Reject();
  std::optional<uint64_t> StepIndex;
  for (uint64_t Index = Boundary.FirstOp;
       Index != Boundary.FirstOp + Boundary.OpCount; ++Index) {
    if (!isX87TranslationStep(Block.Ops[Index]))
      continue;
    if (StepIndex)
      return Reject();
    StepIndex = Index;
  }
  if (!StepIndex)
    return Reject();
  const auto &Step = Block.Ops[*StepIndex];
  if (!x87ContextLowShapeIsValid(Step, Arch::X64) ||
      Step.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Step.Inputs[1] != NdVar::reg(x86reg::X87ContextHandle, 8) ||
      !Step.Inputs[4].isConst() || Step.Inputs[4].Offset != 0 ||
      Step.Inputs[5].Offset != Boundary.Address ||
      !isRuntimeX87InstructionV1(Step.Inputs[2].Offset))
    return Reject();
  for (uint64_t Index = Boundary.FirstOp; Index != *StepIndex; ++Index)
    if (!Block.Ops[Index].Output.isTemp() ||
        Block.Ops[Index].MemoryOrdering != NdMemoryOrdering::None ||
        Block.Ops[Index].MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return Reject();
  const auto Spec = x87ContextInstruction(Step.Inputs[2].Offset);
  const uint64_t End = Boundary.FirstOp + Boundary.OpCount;
  if (Spec->WritesAX) {
    if (*StepIndex + 2 != End)
      return Reject();
    const auto &Copy = Block.Ops[*StepIndex + 1];
    if (Copy.Opcode != NdOp::COPY || Copy.NumInputs != 1 ||
        Copy.Output != NdVar::reg(x86reg::RAX, 2) ||
        Copy.Inputs[0] != Step.Output ||
        Copy.MemoryOrdering != NdMemoryOrdering::None ||
        Copy.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return Reject();
  } else if (*StepIndex + 1 != End) {
    return Reject();
  }
  return X87TranslationInstructionV1{
      {static_cast<uint32_t>(Step.Inputs[2].Offset), Boundary.Address,
       Block.Header.FallthroughPC},
      *StepIndex};
}

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X87TRANSLATIONBOUNDARY_H
