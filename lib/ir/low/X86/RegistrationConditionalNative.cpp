//===- RegistrationConditionalNative.cpp - Preserved SEH helper ABI ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Check physical frame observations in preserved x86 SEH helpers.

#include "RegistrationConditionalFrame.h"
#include "RegistrationFrameEffects.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86Regs.h"

namespace neverd::registration_state {

void checkConditionalNativeFrame(const FrameState &State, const LowOp &Op,
                                 const FrameTransfer &Transfer,
                                 ConditionalFrame &Inputs) {
  if (Op.Opcode == NdOp::INTRINSIC && !isFramePreservingIntrinsic(Op))
    Inputs.NativeFramePrivate = false;
  // The source proof already bounds every dereference, write and nested call.
  // A relocated caller additionally needs scalar observations independent of
  // physical stack placement and incoming nonvolatile register values.
  if (Op.Opcode != NdOp::COND_BR && Op.Opcode != NdOp::INDIR_BR &&
      Op.Opcode != NdOp::RETURN && Op.Opcode != NdOp::INTRINSIC)
    return;
  // Ordinary EAX observations need the separate joined constant-return proof.
  // Without it the caller must discard the result. Internal finally returns
  // retain provenance; a search filter exposes its result to the runtime.
  if (Op.Opcode == NdOp::RETURN && !State.SEHFilterEntry)
    return;
  for (unsigned I = 0; I != Op.NumInputs; ++I) {
    const auto Value = Transfer.read(Op.Inputs[I]);
    if (!Value.isFrameIndependentScalar())
      Inputs.NativeFramePrivate = false;
  }
  if (Op.Opcode == NdOp::RETURN) {
    const auto &Value = State.Registers[x86reg::RAX / x86reg::GeneralRegStride];
    if (!Value.isFrameIndependentScalar())
      Inputs.NativeFramePrivate = false;
  }
}

} // namespace neverd::registration_state
