//===- X64Recovery.cpp - Native x64 recovery contracts -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "X64Recovery.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

namespace neverd::analysis::x64 {
/// The x64 lifter uses these exact shapes to read and restore the system
/// portion of RFLAGS. The residual keeps both intrinsics. During analysis a
/// PUSHFQ snapshot is an unconstrained runtime value: this overapproximates
/// every architectural flag image without inventing a constant or an alias
/// fact. POPFQ has no modelled scalar output; later snapshots are fresh again.
bool runtimeFlagsIntrinsic(const LowOp &Op) {
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2)
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (Id == Intrinsic::Pushf)
    return Op.NumInputs == 1 && Op.Output.isTemp() && Op.Output.Size == 8;
  if (Id == Intrinsic::Popf)
    return Op.NumInputs == 2 && Op.Output.Size == 0 && Op.Inputs[1].Size == 8;
  return false;
}

// A retained trap has exact semantics even though its exception-state outputs
// have no undefined-effect audit. It may only be collected, never executed in
// this nonfaulting proof. Do not turn its Missing sidecar into Complete.
bool isRetainedNativeTrap(const SpecializationInstruction &Insn) {
  if (Insn.Origin.Control != LowInstructionControl::Terminator ||
      Insn.Origin.Immediate || Insn.IsNativeCall ||
      Insn.NativeStackControl != SpecializationNativeStackControl::None ||
      Insn.ProfileProjection != InterpreterProfileProjection::None ||
      Insn.Ops.size() != 1 || !Insn.UndefinedEffects.Effects.empty() ||
      Insn.UndefinedEffects.Coverage != LowUndefinedCoverage::Missing ||
      Insn.UndefinedEffects.OperationDigest !=
          lowUndefinedOperationDigest(Insn.Ops))
    return false;
  const auto &Op = Insn.Ops.front();
  if (Op.Opcode != NdOp::INTRINSIC || Op.Output.Size || Op.NumInputs != 1 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  auto Flags = LowInstructionControlFlag::Terminator;
  if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Int3))
    Flags |= LowInstructionControlFlag::Resumable;
  else if (Op.Inputs[0].Offset != static_cast<uint64_t>(Intrinsic::Ud2))
    return false;
  return Insn.Origin.ControlFlags == Flags;
}

// Intel SDM: disabled shadow stacks make RDSSPD/RDSSPQ a NOP, including no
// 32-bit destination zero-extension; INCSSPD/INCSSPQ instead raise #UD. Check
// the provider's classification against canonical bytes and exact operations.
// Missing coverage stays Missing. No other CET instruction is authorized.
bool isCetDisabledProjection(const SpecializationInstruction &Insn) {
  const bool Read = Insn.ProfileProjection ==
                    InterpreterProfileProjection::CetDisabledReadShadowStackV1;
  const bool Trap =
      Insn.ProfileProjection ==
      InterpreterProfileProjection::CetDisabledIncrementShadowStackTrapV1;
  if ((!Read && !Trap) || Insn.IsNativeCall ||
      Insn.NativeStackControl != SpecializationNativeStackControl::None ||
      Insn.Origin.Control != (Trap ? LowInstructionControl::Terminator
                                   : LowInstructionControl::None) ||
      Insn.Origin.ControlFlags != (Trap ? LowInstructionControlFlag::Terminator
                                        : LowInstructionControlFlag::None) ||
      Insn.Origin.Immediate || Insn.Ops.size() != 1 ||
      Insn.UndefinedEffects.Coverage != LowUndefinedCoverage::Missing ||
      !Insn.UndefinedEffects.Effects.empty() ||
      Insn.UndefinedEffects.OperationDigest !=
          lowUndefinedOperationDigest(Insn.Ops))
    return false;
  const auto &Op = Insn.Ops.front();
  if (Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  // Preserve the strict lifter's opaque intrinsic result. It is never
  // executed: this profile faults before any architectural state update.
  if (Read ? (Op.Opcode != NdOp::NOP || Op.Output.Size || Op.NumInputs)
           : (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 1 ||
              Op.Output != NdVar::reg(x86reg::RAX, 8) ||
              !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
              Op.Inputs[0].Offset !=
                  static_cast<uint64_t>(Intrinsic::CetIncSsp)))
    return false;
  const auto &Bytes = Insn.NativeBytes;
  if ((Bytes.size() != 4 && Bytes.size() != 5) || Bytes[0] != 0xf3)
    return false;
  size_t I = 1;
  if (Bytes.size() == 5) {
    // REX.W and REX.B select width/bank. Extra prefix forms are not certified.
    if (Bytes[I] != 0x40 && Bytes[I] != 0x41 && Bytes[I] != 0x48 &&
        Bytes[I] != 0x49)
      return false;
    ++I;
  }
  return Bytes[I] == 0x0f && Bytes[I + 1] == (Read ? 0x1e : 0xae) &&
         (Bytes[I + 2] & 0xf8) == (Read ? 0xc8 : 0xe8);
}

bool bindCetDisabledPreservedState(SpecializationInstruction &Insn) {
  Insn.PreservedState = {};
  if (Insn.ProfileProjection !=
          InterpreterProfileProjection::CetDisabledReadShadowStackV1 ||
      !isCetDisabledProjection(Insn))
    return false;
  auto &F = Insn.PreservedState;
  F.Audit = LowPreservedStateAudit::CetDisabledReadShadowStackV1;
  F.StateSet = LowPreservedStateSet::LegacyIntegerOpaqueV1;
  F.SemanticsVersion = 1;
  F.Architecture = Arch::X64;
  F.Mode = Insn.Origin.Mode;
  F.Address = Insn.Origin.Address;
  F.Size = Insn.Origin.Size;
  F.OpCount = Insn.Ops.size();
  F.NativeBytesDigest = llvm::toHex(llvm::SHA256::hash(Insn.NativeBytes), true);
  F.OperationDigest = lowUndefinedOperationDigest(Insn.Ops);
  if (!matchesLowPreservedState(F, Insn.Origin, Insn.NativeBytes, Insn.Ops)) {
    F = {};
    return false;
  }
  return true;
}

} // namespace neverd::analysis::x64
