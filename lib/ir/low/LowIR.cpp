//===- LowIR.cpp - Low-level IR structural validation --------------------===//

#include "neverd/ir/low/LowIR.h"

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Errc.h"

#include <algorithm>
#include <limits>

namespace neverd {
namespace {

llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, "%s",
                                 Message.str().c_str());
}

bool isKnownMode(InstructionMode Mode) {
  switch (Mode) {
  case InstructionMode::Default:
  case InstructionMode::ARM:
  case InstructionMode::Thumb:
    return true;
  }
  return false;
}

bool isKnownTargetMode(LowInstructionTargetMode Mode) {
  switch (Mode) {
  case LowInstructionTargetMode::Preserve:
  case LowInstructionTargetMode::ARM:
  case LowInstructionTargetMode::Thumb:
  case LowInstructionTargetMode::FromTargetBit0:
    return true;
  }
  return false;
}

llvm::Error validateMemoryAddressSpace(const LowOp &Op) {
  if (!isKnownMemoryAddressSpace(Op.MemoryAddressSpace))
    return invalid("LowOp has an unknown memory address space");
  const bool HasExplicitAddressSpace =
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default;
  if (HasExplicitAddressSpace &&
      !opcodeSupportsMemoryAddressSpace(Op.Opcode))
    return invalid("LowOp attaches a memory address space to a non-memory "
                   "opcode");
  if (Op.Opcode != NdOp::INTRINSIC)
    return llvm::Error::success();
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst()) {
    if (HasExplicitAddressSpace)
      return invalid(
          "segmented-memory intrinsic has no constant intrinsic ID");
    return llvm::Error::success();
  }
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (!HasExplicitAddressSpace && isX86StringIntrinsic(Id)) {
    if (!intrinsicStringShapeIsValid(
            Id, Op.NumInputs, Op.Output.Size,
            Op.NumInputs > 1 ? Op.Inputs[1].Size : 0))
      return invalid("x86 string intrinsic has an invalid operand/output "
                     "shape");
    return llvm::Error::success();
  }
  if (!intrinsicSupportsMemoryAddressSpace(Id)) {
    if (HasExplicitAddressSpace)
      return invalid("intrinsic does not support a memory address space");
    return llvm::Error::success();
  }
  if (!HasExplicitAddressSpace && intrinsicDefaultRegisterShapeIsValid(
                                      Id, Op.NumInputs, Op.Output.Size,
                                      Op.NumInputs > 1 ? Op.Inputs[1].Size : 0))
    return llvm::Error::success();
  if (!intrinsicMemoryAddressSpaceShapeIsValid(Id, Op.NumInputs,
                                               Op.Output.Size,
                                               Op.NumInputs > 1
                                                   ? Op.Inputs[1].Size
                                                   : 0,
                                               Op.NumInputs > 2
                                                   ? Op.Inputs[2].Size
                                                   : 0,
                                               Op.NumInputs > 3
                                                   ? Op.Inputs[3].Size
                                                   : 0))
    return invalid("memory intrinsic has an invalid operand/output shape");
  return llvm::Error::success();
}

struct DerivedControl {
  bool IsBranch = false;
  bool IsConditional = false;
  bool IsCall = false;
  bool IsReturn = false;
  bool IsIndirect = false;
  std::optional<uint64_t> BranchTarget;
  std::optional<uint64_t> CallTarget;
};

bool isInstructionGuardEffect(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::LOAD:
  case NdOp::STORE:
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
  case NdOp::ATOMIC_CMPXCHG:
  case NdOp::INTRINSIC:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
    return true;
  default:
    return false;
  }
}

bool hasCanonicalInstructionGuard(const LowBlock &Block,
                                  const LowInstructionBoundary &Boundary) {
  const uint64_t End = Boundary.FirstOp + Boundary.OpCount;
  const va_t NextAddress = Boundary.Address + Boundary.Size;
  std::optional<uint64_t> GuardIndex;

  for (uint64_t I = Boundary.FirstOp; I < End; ++I) {
    const LowOp &Op = Block.Ops[static_cast<size_t>(I)];
    if (Op.Opcode != NdOp::COND_BR)
      continue;
    if (GuardIndex || Op.NumInputs < 2 || !Op.Inputs[0].isConst() ||
        Op.Inputs[0].Offset != NextAddress)
      return false;
    GuardIndex = I;
  }
  if (!GuardIndex)
    return false;

  // A direct conditional ARM branch is itself represented as
  // `COND_BR next,!condition; BRANCH target`; the BRANCH is guest control
  // flow, not a guarded same-instruction effect.  Keep it out of the effect
  // set just as CFGBuilder's producer does.
  bool HasEffect = false;
  for (uint64_t I = Boundary.FirstOp; I < End; ++I) {
    const LowOp &Op = Block.Ops[static_cast<size_t>(I)];
    if (!isInstructionGuardEffect(Op.Opcode) || Op.Opcode == NdOp::COND_BR)
      continue;
    if (I < *GuardIndex)
      return false;
    HasEffect = true;
  }
  return HasEffect;
}

DerivedControl deriveControl(const LowBlock &Block,
                             const LowInstructionBoundary &Boundary) {
  DerivedControl Result;
  const uint64_t End = Boundary.FirstOp + Boundary.OpCount;
  for (uint64_t I = Boundary.FirstOp; I < End; ++I) {
    const LowOp &Op = Block.Ops[static_cast<size_t>(I)];
    switch (Op.Opcode) {
    case NdOp::BRANCH:
      Result.IsBranch = true;
      if (Op.NumInputs >= 1 && Op.Inputs[0].isConst())
        Result.BranchTarget = Op.Inputs[0].Offset;
      break;
    case NdOp::COND_BR:
      Result.IsBranch = true;
      Result.IsConditional = true;
      if (Op.NumInputs >= 1 && Op.Inputs[0].isConst())
        Result.BranchTarget = Op.Inputs[0].Offset;
      break;
    case NdOp::INDIR_BR:
      Result.IsBranch = true;
      Result.IsIndirect = true;
      break;
    case NdOp::CALL:
      Result.IsCall = true;
      if (Op.NumInputs >= 1 && Op.Inputs[0].isConst())
        Result.CallTarget = Op.Inputs[0].Offset;
      break;
    case NdOp::INDIR_CALL:
      Result.IsCall = true;
      Result.IsIndirect = true;
      break;
    case NdOp::RETURN:
      Result.IsReturn = true;
      break;
    default:
      break;
    }
  }
  return Result;
}

LowInstructionControl deriveControlClass(const DerivedControl &Control) {
  if (Control.IsCall && Control.IsReturn)
    return LowInstructionControl::TailCall;
  if (Control.IsBranch && Control.IsConditional && Control.IsReturn)
    return LowInstructionControl::ConditionalReturn;
  if (Control.IsBranch && Control.IsConditional && Control.IsCall)
    return LowInstructionControl::ConditionalCall;
  if (Control.IsBranch)
    return LowInstructionControl::Branch;
  if (Control.IsCall)
    return LowInstructionControl::Call;
  if (Control.IsReturn)
    return LowInstructionControl::Return;
  return LowInstructionControl::None;
}

llvm::Error validateControl(const LowBlock &Block,
                            const LowInstructionBoundary &Boundary) {
  constexpr uint16_t KnownFlags =
      static_cast<uint16_t>(LowInstructionControlFlag::Branch) |
      static_cast<uint16_t>(LowInstructionControlFlag::Conditional) |
      static_cast<uint16_t>(LowInstructionControlFlag::Call) |
      static_cast<uint16_t>(LowInstructionControlFlag::Return) |
      static_cast<uint16_t>(LowInstructionControlFlag::Indirect) |
      static_cast<uint16_t>(LowInstructionControlFlag::NoReturn) |
      static_cast<uint16_t>(LowInstructionControlFlag::Terminator) |
      static_cast<uint16_t>(LowInstructionControlFlag::Resumable) |
      static_cast<uint16_t>(LowInstructionControlFlag::InstructionGuard);
  const uint16_t Flags = static_cast<uint16_t>(Boundary.ControlFlags);
  if ((Flags & ~KnownFlags) != 0)
    return invalid("instruction boundary has unknown control flags");

  const auto Has = [&](LowInstructionControlFlag Flag) {
    return hasLowInstructionControlFlag(Boundary.ControlFlags, Flag);
  };
  const bool IsBranch = Has(LowInstructionControlFlag::Branch);
  const bool IsConditional = Has(LowInstructionControlFlag::Conditional);
  const bool IsCall = Has(LowInstructionControlFlag::Call);
  const bool IsReturn = Has(LowInstructionControlFlag::Return);
  const bool IsIndirect = Has(LowInstructionControlFlag::Indirect);
  const bool IsNoReturn = Has(LowInstructionControlFlag::NoReturn);
  const bool IsTerminator = Has(LowInstructionControlFlag::Terminator);
  const bool IsResumable = Has(LowInstructionControlFlag::Resumable);
  const bool IsInstructionGuard =
      Has(LowInstructionControlFlag::InstructionGuard);

  const bool HasCanonicalGuard = hasCanonicalInstructionGuard(Block, Boundary);
  if (IsInstructionGuard != HasCanonicalGuard)
    return invalid(IsInstructionGuard
                       ? "instruction-local guard metadata is not backed by a "
                         "canonical same-instruction guard"
                       : "canonical same-instruction guard is missing its "
                         "instruction-local metadata");

  if (IsInstructionGuard && ((!IsBranch || !IsConditional) ||
                             (Boundary.Mode != InstructionMode::ARM &&
                              Boundary.Mode != InstructionMode::Thumb)))
    return invalid(
        "instruction-local guard requires conditional ARM control flags");

  switch (Boundary.Control) {
  case LowInstructionControl::None:
    if (Flags != 0)
      return invalid("non-control instruction has control flags");
    if (Boundary.Immediate)
      return invalid("non-control instruction has a control immediate");
    break;
  case LowInstructionControl::Branch:
    if (!IsBranch || IsCall || IsReturn || IsNoReturn || IsTerminator ||
        IsResumable)
      return invalid("branch instruction has inconsistent control flags");
    if (!IsIndirect && !Boundary.Immediate)
      return invalid("direct branch is missing its target immediate");
    break;
  case LowInstructionControl::Call:
    if (IsBranch || IsConditional || !IsCall || IsReturn || IsTerminator ||
        IsResumable)
      return invalid("call instruction has inconsistent control flags");
    if (!IsIndirect && !Boundary.Immediate)
      return invalid("direct call is missing its target immediate");
    break;
  case LowInstructionControl::Return:
    if (IsBranch || IsConditional || IsCall || !IsReturn || IsIndirect ||
        IsNoReturn || IsTerminator || IsResumable)
      return invalid("return instruction has inconsistent control flags");
    break;
  case LowInstructionControl::TailCall:
    if (IsBranch || IsConditional || !IsCall || !IsReturn || IsNoReturn ||
        IsTerminator || IsResumable)
      return invalid("tail-call instruction has inconsistent control flags");
    if (!IsIndirect && !Boundary.Immediate)
      return invalid("direct tail call is missing its target immediate");
    break;
  case LowInstructionControl::ConditionalReturn:
    if (!IsBranch || !IsConditional || IsCall || !IsReturn || IsIndirect ||
        IsNoReturn || IsTerminator || IsResumable)
      return invalid(
          "conditional return instruction has inconsistent control flags");
    break;
  case LowInstructionControl::ConditionalCall:
    if (!IsBranch || !IsConditional || !IsCall || IsReturn || IsTerminator ||
        IsResumable)
      return invalid(
          "conditional call instruction has inconsistent control flags");
    if (!IsIndirect && !Boundary.Immediate)
      return invalid("conditional call is missing its target immediate");
    break;
  case LowInstructionControl::Terminator:
    if (IsBranch || IsConditional || IsCall || IsReturn || IsIndirect ||
        IsNoReturn || !IsTerminator)
      return invalid("opaque terminator has inconsistent control flags");
    break;
  default:
    return invalid("instruction boundary has an unknown control class");
  }

  if (Boundary.Immediate && *Boundary.Immediate == InvalidVA)
    return invalid("instruction boundary immediate uses the invalid address");

  if (!isKnownTargetMode(Boundary.TargetMode))
    return invalid("instruction boundary has an unknown target mode");
  const bool IsARMMode = Boundary.Mode == InstructionMode::ARM ||
                         Boundary.Mode == InstructionMode::Thumb;
  switch (Boundary.TargetMode) {
  case LowInstructionTargetMode::Preserve:
    break;
  case LowInstructionTargetMode::ARM:
  case LowInstructionTargetMode::Thumb:
    if (!IsARMMode || IsIndirect || (!IsBranch && !IsCall) || IsReturn)
      return invalid(
          "fixed target mode requires a direct ARM control transfer");
    break;
  case LowInstructionTargetMode::FromTargetBit0:
    if (!IsARMMode || (!IsIndirect && !IsReturn))
      return invalid(
          "target-bit mode exchange requires an ARM register transfer");
    break;
  }

  // Opaque traps deliberately carry no BRANCH/CALL/RETURN LowOp.  Their
  // terminator classification comes from the decoder, so this is the sole
  // explicit exception to slice-derived control validation.
  if (Boundary.Control == LowInstructionControl::Terminator)
    return llvm::Error::success();

  const DerivedControl Derived = deriveControl(Block, Boundary);
  if (deriveControlClass(Derived) != Boundary.Control)
    return invalid("instruction boundary control class disagrees with LowOps");
  if (Derived.IsBranch != IsBranch || Derived.IsConditional != IsConditional ||
      Derived.IsCall != IsCall || Derived.IsReturn != IsReturn ||
      Derived.IsIndirect != IsIndirect)
    return invalid("instruction boundary control flags disagree with LowOps");

  std::optional<uint64_t> DerivedTarget;
  const bool RequiresDirectTarget =
      (Derived.IsCall && !Derived.IsIndirect) ||
      (Derived.IsBranch && !Derived.IsIndirect && !Derived.IsReturn);
  if (Derived.IsCall && !Derived.IsIndirect)
    DerivedTarget = Derived.CallTarget;
  else if (Derived.IsBranch && !Derived.IsIndirect && !Derived.IsReturn)
    DerivedTarget = Derived.BranchTarget;
  if (RequiresDirectTarget && !DerivedTarget)
    return invalid("instruction LowOps are missing a direct target");
  if (DerivedTarget && Boundary.Immediate != DerivedTarget)
    return invalid("instruction boundary direct target disagrees with LowOps");
  return llvm::Error::success();
}

} // namespace

llvm::Error validateLowInstructionBoundaries(
    const LowBlock &Block, LowInstructionBoundaryRequirement Requirement) {
  if (Block.InstructionBoundaries.empty()) {
    if (Requirement == LowInstructionBoundaryRequirement::Optional)
      return llvm::Error::success();
    return invalid("instruction boundary metadata is missing");
  }

  if (Block.StartAddr == InvalidVA || Block.EndAddr <= Block.StartAddr)
    return invalid("instruction boundaries have an invalid block range");

  uint64_t ExpectedFirstOp = 0;
  va_t ExpectedAddress = Block.StartAddr;
  const uint64_t NumOps = static_cast<uint64_t>(Block.Ops.size());
  std::optional<InstructionMode> BlockMode;
  for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries) {
    if (Boundary.Address != ExpectedAddress)
      return invalid("instruction boundaries are not address-contiguous at 0x" +
                     llvm::utohexstr(Boundary.Address));
    if (Boundary.Address == InvalidVA || Boundary.Size == 0)
      return invalid("instruction boundary has an invalid address or size");
    if (Boundary.Size > std::numeric_limits<va_t>::max() - Boundary.Address)
      return invalid("instruction boundary address range overflows");
    const va_t End = Boundary.Address + Boundary.Size;
    if (End > Block.EndAddr)
      return invalid("instruction boundary extends beyond its block");
    if (!isKnownMode(Boundary.Mode))
      return invalid("instruction boundary has an unknown instruction mode");
    if (BlockMode && Boundary.Mode != *BlockMode)
      return invalid("instruction boundaries mix instruction modes within a "
                     "block");
    BlockMode = Boundary.Mode;

    if (Boundary.FirstOp != ExpectedFirstOp)
      return invalid("instruction boundary has a non-canonical op slice");
    if (Boundary.OpCount > NumOps - ExpectedFirstOp)
      return invalid("instruction boundary op slice is out of range");
    const uint64_t OpEnd = ExpectedFirstOp + Boundary.OpCount;
    for (uint64_t I = ExpectedFirstOp; I < OpEnd; ++I) {
      const LowOp &Op = Block.Ops[static_cast<size_t>(I)];
      if (Op.Addr != Boundary.Address)
        return invalid(
            "instruction boundary covers an op from another address");
      if (llvm::Error Error = validateMemoryAddressSpace(Op))
        return Error;
    }

    if (llvm::Error Error = validateControl(Block, Boundary))
      return Error;
    ExpectedFirstOp = OpEnd;
    ExpectedAddress = End;
  }

  if (ExpectedFirstOp != NumOps)
    return invalid("instruction boundaries do not cover every LowOp");
  if (ExpectedAddress != Block.EndAddr)
    return invalid("instruction boundaries do not cover the complete block");
  return llvm::Error::success();
}

llvm::Error validateLowInstructionBoundaries(
    const LowFunc &Function, LowInstructionBoundaryRequirement Requirement) {
  const bool AnyMetadata = std::any_of(
      Function.Blocks.begin(), Function.Blocks.end(),
      [](const LowBlock &Block) { return Block.hasInstructionBoundaries(); });
  if (!AnyMetadata) {
    if (Requirement == LowInstructionBoundaryRequirement::Optional)
      return llvm::Error::success();
    return invalid("instruction boundary metadata is missing");
  }

  for (const LowBlock &Block : Function.Blocks) {
    if (llvm::Error Error = validateLowInstructionBoundaries(
            Block, LowInstructionBoundaryRequirement::Required))
      return invalid("block " + llvm::Twine(Block.Id) + ": " +
                     llvm::toString(std::move(Error)));
  }
  return llvm::Error::success();
}

} // namespace neverd
