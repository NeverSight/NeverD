//===- RegistrationCall.cpp - Bind original PE32 call contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/X86/X86RegistrationCall.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd {

llvm::ArrayRef<uint64_t> RegistrationCallABI::integerArgumentRegisters() const {
  static constexpr uint64_t ThisRegister[] = {x86reg::RCX};
  return BorrowsECX ? llvm::ArrayRef<uint64_t>(ThisRegister)
                    : llvm::ArrayRef<uint64_t>{};
}

namespace {
/// The cleanup a complete registration proof binds to this exact call.
std::optional<uint32_t> checkedCallStackPop(const LowFunc &Func,
                                            const LowBlock &Block,
                                            const LowOp &Call) {
  if (!Func.ExceptionMetadata || !Func.RegistrationStates ||
      !Func.RegistrationStates->Complete ||
      (Call.Opcode != NdOp::CALL && Call.Opcode != NdOp::INDIR_CALL) ||
      Call.NumInputs != 1 || !Call.Inputs[0].isConst() ||
      Call.Inputs[0].Size != 4 || Call.Seq < 0)
    return std::nullopt;
  const auto &States = *Func.RegistrationStates;
  if (Call.Opcode == NdOp::INDIR_CALL) {
    if (!States.CallbackStatesComplete ||
        !States.RegistrationLifetimeComplete || !States.SEHBoundCallsComplete ||
        !States.HasBoundSEHCallees ||
        States.SEHBoundCalls.size() > limits::kMaxRegistrationEHRecords ||
        Call.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        Call.MemoryOrdering != NdMemoryOrdering::None)
      return std::nullopt;
    const RegistrationSEHBoundCall *Runtime = nullptr;
    for (const auto &Bound : States.SEHBoundCalls)
      if (Bound.Address == Call.Addr && Bound.OpSeq == Call.Seq) {
        if (Runtime || Bound.TargetVA != Call.Inputs[0].Offset ||
            !Bound.HasOrdinaryReturn ||
            (Bound.CalleeKind !=
                 RegistrationSEHBoundCall::Kind::RaiseException &&
             Bound.CalleeKind !=
                 RegistrationSEHBoundCall::Kind::GetStartupInfoW &&
             Bound.CalleeKind !=
                 RegistrationSEHBoundCall::Kind::HeapTermination &&
             Bound.CalleeKind !=
                 RegistrationSEHBoundCall::Kind::InterlockedCompareExchange &&
             Bound.CalleeKind != RegistrationSEHBoundCall::Kind::Sleep))
          return std::nullopt;
        Runtime = &Bound;
      }
    const auto Boundary = llvm::find_if(
        Block.InstructionBoundaries, [&](const auto &Instruction) {
          return Instruction.Address == Call.Addr;
        });
    const auto Operation =
        llvm::find_if(Block.Ops, [&](const auto &Op) { return &Op == &Call; });
    if (!Runtime || Boundary == Block.InstructionBoundaries.end() ||
        Operation == Block.Ops.end() ||
        Boundary->Control != LowInstructionControl::Call ||
        Boundary->Size != 6 || Call.Addr < Block.StartAddr ||
        Call.Addr > UINT32_MAX - 6 || Call.Addr + 6 > Block.EndAddr ||
        !Func.ExceptionMetadata->ownsCode({Call.Addr, Call.Addr + 6}) ||
        size_t(Operation - Block.Ops.begin()) < Boundary->FirstOp ||
        size_t(Operation - Block.Ops.begin()) - Boundary->FirstOp >=
            Boundary->OpCount)
      return std::nullopt;
    // The source runtime contract owns the stdcall pop. Reflect it before
    // SSA so subsequent callback pushes use the same coordinate as analysis.
    const uint32_t Words =
        Runtime->CalleeKind ==
                    RegistrationSEHBoundCall::Kind::GetStartupInfoW ||
                Runtime->CalleeKind == RegistrationSEHBoundCall::Kind::Sleep
            ? 1
        : Runtime->CalleeKind ==
                RegistrationSEHBoundCall::Kind::InterlockedCompareExchange
            ? 3
            : 4;
    return Runtime->Arguments.size() == Words
               ? std::optional<uint32_t>{Words * 4}
               : std::nullopt;
  }
  if (!States.CallFrameEffectsComplete)
    return std::nullopt;
  const auto *Effect = States.callFrameEffect(Call.Addr, Call.Seq);
  if (!Effect || Effect->Target != Call.Inputs[0].Offset ||
      Effect->CalleeIndex >= States.CalleeContracts.size() ||
      Effect->DoesNotReturn || Call.Addr < Block.StartAddr ||
      Effect->EndAddress > Block.EndAddr || Effect->EndAddress <= Call.Addr ||
      !Func.ExceptionMetadata->ownsCode({Call.Addr, Effect->EndAddress}))
    return std::nullopt;
  const auto &Callee = States.CalleeContracts[Effect->CalleeIndex];
  if ((Callee.CalleeKind != RegistrationCalleeFrameContract::Kind::Leaf &&
       !Callee.isInitializer() && !Callee.isCxxParent()) ||
      Callee.Target != Effect->Target ||
      Callee.StackPopBytes != Effect->StackPopBytes ||
      Callee.StackPopBytes > UINT16_MAX || Callee.StackPopBytes % 4)
    return std::nullopt;
  return Callee.StackPopBytes;
}
} // namespace

std::optional<uint32_t> registrationCallStackPop(const LowFunc &Func,
                                                 const LowBlock &Block,
                                                 const LowOp &Call) {
  if (const auto Pop = checkedCallStackPop(Func, Block, Call))
    return Pop;
  // A direct callee pops the same bytes at every call that returns, so its
  // proven cleanup holds where the registration state stays unproven. The
  // call must still be an operation of this block.
  if (!Func.ExceptionMetadata || !Func.RegistrationStates ||
      Call.Opcode != NdOp::CALL || Call.NumInputs != 1 ||
      !Call.Inputs[0].isConst() || Call.Inputs[0].Size != 4 || Call.Seq < 0 ||
      llvm::none_of(Block.Ops, [&](const LowOp &Op) { return &Op == &Call; }))
    return std::nullopt;
  for (const auto &Stack : Func.RegistrationStates->CalleeStacks)
    if (!Stack.Indirect && Stack.Target == Call.Inputs[0].Offset)
      return Stack.StackPopBytes ? std::optional(Stack.StackPopBytes)
                                 : std::nullopt;
  return std::nullopt;
}

std::optional<RegistrationCallABI> registrationCallABI(const MedFunc &Func,
                                                       const MedBlock &Block,
                                                       const MedOp &Call) {
  if (!Func.ExceptionMetadata || !Func.RegistrationStates || Func.SkippedSSA ||
      Call.Opcode != NdOp::CALL || Call.Dead || Call.NumInputs != 1 ||
      !Call.Inputs[0].isConst() || Call.Inputs[0].Size != 4 ||
      Call.Inputs[0].ConstVal > UINT32_MAX || Call.OriginSeq < 0 ||
      Call.ExactArguments || Call.SourceCallHint ||
      Func.Blocks.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  const auto &EH = *Func.ExceptionMetadata;
  const auto &State = *Func.RegistrationStates;
  if (!EH.Registration || EH.Encoding != ExceptionEncoding::X86CxxFuncInfo ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      Func.Entry != EH.CodeRange.Begin || !State.Complete ||
      !State.CallFrameEffectsComplete ||
      State.CallFrameEffects.size() > limits::kMaxRegistrationEHRecords ||
      State.CalleeContracts.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  const RegistrationCallFrameEffect *Effect = nullptr;
  for (const auto &Candidate : State.CallFrameEffects)
    if (Candidate.Address == Call.Addr && Candidate.OpSeq == Call.OriginSeq) {
      if (Effect)
        return std::nullopt;
      Effect = &Candidate;
    }
  if (!Effect || Effect->Target != Call.Inputs[0].ConstVal ||
      Effect->CalleeIndex >= State.CalleeContracts.size() ||
      Effect->DoesNotReturn != Call.DoesNotReturn ||
      Call.Addr < Block.StartAddr || Effect->EndAddress <= Call.Addr ||
      Effect->EndAddress > Block.EndAddr ||
      !EH.ownsCode({Call.Addr, Effect->EndAddress}))
    return std::nullopt;
  const auto &Callee = State.CalleeContracts[Effect->CalleeIndex];
  if (Callee.Target != Effect->Target ||
      Callee.StackPopBytes != Effect->StackPopBytes ||
      Callee.StackPopBytes > UINT16_MAX || Callee.StackPopBytes % 4 ||
      Callee.StackPopBytes / 4 >= limits::kMaxCallArgs ||
      (Callee.isThrow() && Callee.StackPopBytes) ||
      Callee.isRuntimeThrow() != Effect->RuntimeThrow.has_value() ||
      Callee.DoesNotReturn != Effect->DoesNotReturn)
    return std::nullopt;
  const bool Borrow = !Callee.ECXReads.empty() || !Callee.ECXWrites.empty();
  if ((Callee.StackPopBytes && !Borrow) ||
      (Effect->ECXFrameOffset && Effect->ECXRuntimeObject) ||
      (Effect->ECXRuntimeObject &&
       Callee.CalleeKind != RegistrationCalleeFrameContract::Kind::Leaf) ||
      Borrow != (Effect->ECXFrameOffset.has_value() ||
                 Effect->ECXRuntimeObject.has_value()) ||
      (Callee.isThrow()
           ? !Callee.DoesNotReturn || Borrow
           : (Callee.CalleeKind !=
                  RegistrationCalleeFrameContract::Kind::Leaf &&
              !Callee.isInitializer() && !Callee.isCxxParent()) ||
                 (Callee.DoesNotReturn && (!Callee.isCxxParent() || Borrow ||
                                           Callee.HasIndependentScalarReturn))))
    return std::nullopt;

  size_t Work = 0;
  bool Found = false;
  for (const auto &CurrentBlock : Func.Blocks)
    for (const auto &Current : CurrentBlock.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Current.Addr != Call.Addr || Current.OriginSeq != Call.OriginSeq)
        continue;
      if (Found || &Current != &Call || &CurrentBlock != &Block)
        return std::nullopt;
      Found = true;
    }
  return Found ? std::optional(RegistrationCallABI{
                     Borrow, Callee.isRuntimeThrow(), Callee.isInitializer(),
                     Callee.StackPopBytes / 4})
               : std::nullopt;
}

} // namespace neverd
