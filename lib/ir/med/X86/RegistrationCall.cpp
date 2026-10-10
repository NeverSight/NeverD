//===- RegistrationCall.cpp - Bind original PE32 call contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/med/X86RegistrationCall.h"

namespace neverd {

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
      Effect->StackPopBytes || Effect->DoesNotReturn != Call.DoesNotReturn ||
      Call.Addr < Block.StartAddr || Effect->EndAddress <= Call.Addr ||
      Effect->EndAddress > Block.EndAddr ||
      !EH.ownsCode({Call.Addr, Effect->EndAddress}))
    return std::nullopt;
  const auto &Callee = State.CalleeContracts[Effect->CalleeIndex];
  if (Callee.Target != Effect->Target || Callee.StackPopBytes ||
      Callee.DoesNotReturn != Effect->DoesNotReturn)
    return std::nullopt;
  const bool Borrow = !Callee.ECXReads.empty() || !Callee.ECXWrites.empty();
  if (Borrow != Effect->ECXFrameOffset.has_value() ||
      (Callee.isThrow()
           ? !Callee.DoesNotReturn || Borrow
           : Callee.CalleeKind != RegistrationCalleeFrameContract::Kind::Leaf ||
                 Callee.DoesNotReturn))
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
  return Found ? std::optional(
                     RegistrationCallABI{Borrow, Callee.isRuntimeRethrow()})
               : std::nullopt;
}

} // namespace neverd
