//===- RegistrationSEHBoundCall.cpp - Source preserved SEH calls ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Validate source identities and argument recipes for preserved SEH calls.

#include "neverd/Limits.h"
#include "neverd/ir/RegistrationCRTControl.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86RegistrationCallResult.h"
#include "neverd/ir/med/X86RegistrationSEHBoundCall.h"
#include "neverd/lift/X86Regs.h"

namespace neverd {

std::optional<std::map<va_t, X86RegistrationSEHBoundCall>>
getX86RegistrationSEHBoundCalls(const MedFunc &Function) {
  if (!Function.RegistrationStates)
    return std::nullopt;
  const auto &States = *Function.RegistrationStates;
  std::map<va_t, X86RegistrationSEHBoundCall> Result;
  if (!States.HasBoundSEHCallees && States.SEHBoundCalls.empty())
    return Result;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.SEHBoundCallsComplete ||
      !States.HasBoundSEHCallees || States.SEHBoundCalls.empty() ||
      !Function.ExceptionMetadata || !Function.ExceptionMetadata->Registration)
    return std::nullopt;
  if (Function.ExceptionMetadata->Personality ==
          ExceptionPersonality::ExceptHandler4 &&
      (!States.SecurityCookiesComplete || !States.SecurityCookieVA ||
       States.SecurityCookieVA > UINT32_MAX - 3))
    return std::nullopt;
  size_t Work = 0;
  for (const auto &Call : States.SEHBoundCalls) {
    using CalleeKind = RegistrationSEHBoundCall::Kind;
    using ArgumentKind = RegistrationSEHBoundArgument::Kind;
    const bool ControlFPPrecision =
        Call.CalleeKind == CalleeKind::ControlFP &&
        Call.Arguments.size() == 3 &&
        Call.Arguments[0].ArgumentKind == ArgumentKind::NullPointer &&
        Call.Arguments[1].ArgumentKind == ArgumentKind::ConstantScalar &&
        Call.Arguments[2].ArgumentKind == ArgumentKind::ConstantScalar &&
        registration_crt::isX86ControlFPPrecisionSetting(
            Call.Arguments[1].ScalarValue, Call.Arguments[2].ScalarValue);
    if (Call.ConstantReturn &&
        (Call.CalleeKind != CalleeKind::PreservedHelper ||
         !Call.HasOrdinaryReturn))
      return std::nullopt;
    switch (Call.CalleeKind) {
    case CalleeKind::PreservedHelper:
      if (Call.EntryVA)
        return std::nullopt;
      break;
    case CalleeKind::RaiseException:
      if (!Call.HasOrdinaryReturn || Call.Arguments.size() != 4 ||
          Call.Arguments[0].ArgumentKind != ArgumentKind::Scalar ||
          Call.Arguments[1].ArgumentKind != ArgumentKind::Scalar ||
          Call.Arguments[2].ArgumentKind != ArgumentKind::Scalar ||
          Call.Arguments[3].ArgumentKind != ArgumentKind::NullPointer)
        return std::nullopt;
      break;
    case CalleeKind::ControlFP:
      if (Call.EntryVA || !Call.HasOrdinaryReturn ||
          Call.Arguments.size() != 3 ||
          (!ControlFPPrecision &&
           ((Call.Arguments[0].ArgumentKind != ArgumentKind::Object &&
             Call.Arguments[0].ArgumentKind != ArgumentKind::NullPointer) ||
            Call.Arguments[1].ArgumentKind != ArgumentKind::Scalar ||
            Call.Arguments[2].ArgumentKind != ArgumentKind::Scalar)))
        return std::nullopt;
      break;
    case CalleeKind::GetStartupInfoW:
      if (Call.EntryVA || !Call.HasOrdinaryReturn ||
          Call.Arguments.size() != 1 ||
          Call.Arguments[0].ArgumentKind != ArgumentKind::Object)
        return std::nullopt;
      break;
    case CalleeKind::HeapTermination:
      if (Call.EntryVA || !Call.HasOrdinaryReturn ||
          Call.Arguments.size() !=
              RegistrationSEHBoundCall::HeapTerminationArguments.size() ||
          Call.Arguments[0].ArgumentKind != ArgumentKind::NullPointer ||
          Call.Arguments[1].ArgumentKind != ArgumentKind::ConstantScalar ||
          Call.Arguments[2].ArgumentKind != ArgumentKind::NullPointer ||
          Call.Arguments[3].ArgumentKind != ArgumentKind::ConstantScalar)
        return std::nullopt;
      break;
    case CalleeKind::InterlockedCompareExchange:
      if (Call.EntryVA || !Call.HasOrdinaryReturn ||
          Call.Arguments.size() != 3 ||
          Call.Arguments[0].ArgumentKind != ArgumentKind::ImagePointer ||
          Call.Arguments[1].ArgumentKind != ArgumentKind::Scalar ||
          Call.Arguments[2].ArgumentKind != ArgumentKind::Scalar)
        return std::nullopt;
      break;
    case CalleeKind::Sleep:
      if (Call.EntryVA || !Call.HasOrdinaryReturn ||
          Call.Arguments.size() != 1 ||
          Call.Arguments[0].ArgumentKind != ArgumentKind::Scalar)
        return std::nullopt;
      break;
    default:
      return std::nullopt;
    }
    if (++Work > limits::kMaxRegistrationEHStateWork || Call.OpSeq < 0 ||
        Call.Address > UINT32_MAX - 5 || Call.TargetVA > UINT32_MAX ||
        Call.Arguments.size() > 31 ||
        !Result.emplace(Call.Address, X86RegistrationSEHBoundCall{&Call})
             .second)
      return std::nullopt;
    if (Call.EntryVA &&
        (Call.EntryVA > UINT32_MAX || Call.StackOffset > -16 ||
         Call.StackOffset < -int64_t(limits::kMaxSourceCallBorrowedBytes)))
      return std::nullopt;
    for (size_t I = 0; I != Call.Arguments.size(); ++I) {
      const auto &Arg = Call.Arguments[I];
      using Kind = RegistrationSEHBoundArgument::Kind;
      if (++Work > limits::kMaxRegistrationEHStateWork ||
          (Arg.ArgumentKind != Kind::ConstantScalar && Arg.ScalarValue) ||
          (Arg.ArgumentKind != Kind::ImagePointer && Arg.ImageVA))
        return std::nullopt;
      switch (Arg.ArgumentKind) {
      case Kind::Unused:
        if (Arg.Offset || Arg.Capture)
          return std::nullopt;
        break;
      case Kind::Scalar:
        if (Arg.Capture ||
            int64_t(Call.StackOffset) + int64_t(I * 4) != Arg.Offset)
          return std::nullopt;
        break;
      case Kind::Object:
        if (Arg.Capture ||
            (Call.CalleeKind == CalleeKind::ControlFP ||
                     Call.CalleeKind == CalleeKind::GetStartupInfoW
                 ? int64_t(Arg.Offset) + Call.borrowedObjectBytes() > 0 ||
                       Arg.Offset < int64_t(Call.StackOffset) +
                                        int64_t(Call.Arguments.size() * 4)
                 : Arg.Offset > -28 ||
                       Arg.Offset < int64_t(Call.StackOffset) + 128))
          return std::nullopt;
        break;
      case Kind::NullPointer:
        if (!((Call.CalleeKind == CalleeKind::RaiseException && I == 3) ||
              (Call.CalleeKind == CalleeKind::ControlFP && I == 0) ||
              (Call.CalleeKind == CalleeKind::HeapTermination &&
               (I == 0 || I == 2))) ||
            Arg.Capture || Arg.Offset)
          return std::nullopt;
        break;
      case Kind::ConstantScalar:
        if (Arg.Capture || Arg.Offset ||
            !((ControlFPPrecision && (I == 1 || I == 2)) ||
              (Call.CalleeKind == CalleeKind::HeapTermination &&
               (I == 1 || I == 3) &&
               Arg.ScalarValue ==
                   RegistrationSEHBoundCall::HeapTerminationArguments[I])))
          return std::nullopt;
        break;
      case Kind::ImagePointer:
        if (Call.CalleeKind != CalleeKind::InterlockedCompareExchange || I ||
            Arg.Capture || Arg.Offset || !Arg.ImageVA ||
            Arg.ImageVA > UINT32_MAX - 3 || Arg.ImageVA % 4)
          return std::nullopt;
        break;
      case Kind::JumpBuffer:
        Work += States.SEHJumpCaptures.size();
        if (Work > limits::kMaxRegistrationEHStateWork || Arg.Offset ||
            !Arg.Capture ||
            !llvm::any_of(States.SEHJumpCaptures, [&](const auto &Capture) {
              return Capture.Address == Arg.Capture;
            }))
          return std::nullopt;
        break;
      default:
        return std::nullopt;
      }
    }
  }
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      const auto Found = Result.find(Op.Addr);
      if (Found == Result.end() ||
          (Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL))
        continue;
      auto &Call = Found->second;
      const bool Imported = Call.Source->CalleeKind !=
                            RegistrationSEHBoundCall::Kind::PreservedHelper;
      const bool CorrectOpcode =
          Imported ? Op.Opcode == NdOp::INDIR_CALL ||
                         (Call.Source->CalleeKind ==
                              RegistrationSEHBoundCall::Kind::ControlFP &&
                          Op.Opcode == NdOp::CALL)
                   : Op.Opcode == NdOp::CALL;
      if (Call.Operation || Op.OriginSeq != Call.Source->OpSeq ||
          !CorrectOpcode || Op.NumInputs != 1 || !Op.Inputs[0].isConst() ||
          Op.Inputs[0].Size != 4 ||
          Op.Inputs[0].ConstVal != Call.Source->TargetVA ||
          Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return std::nullopt;
      Call.Operation = &Op;
      Call.Block = Block.Id;
    }
  if (llvm::any_of(Result,
                   [](const auto &Item) { return !Item.second.Operation; }))
    return std::nullopt;
  for (auto &[Address, Call] : Result) {
    if (!Call.Source->EntryVA)
      continue;
    for (const auto &Block : Function.Blocks) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Block.StartAddr != Call.Source->EntryVA)
        continue;
      for (const auto &Op : Block.Ops) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return std::nullopt;
        if (Op.RegistrationRoot !=
            MedOp::RegistrationRootKind::CallbackStackPointer)
          continue;
        if (Call.StackRoot || !hasValidRegistrationRootShape(Op) ||
            Op.Output.SSAVer <= 0 || Op.Addr != Block.StartAddr)
          return std::nullopt;
        Call.StackRoot = &Op;
      }
    }
    if (!Call.StackRoot)
      return std::nullopt;
  }
  for (auto &[Address, Call] : Result) {
    if (Call.Source->CalleeKind == RegistrationSEHBoundCall::Kind::ControlFP ||
        Call.Source->CalleeKind ==
            RegistrationSEHBoundCall::Kind::InterlockedCompareExchange) {
      if (Call.Operation->Output.Size != 4)
        return std::nullopt;
      Call.ObservesScalarReturn = true;
      continue;
    }
    RegistrationCallFrameEffect Effect;
    Effect.Address = Address;
    Effect.OpSeq = Call.Source->OpSeq;
    Effect.Target = Call.Source->TargetVA;
    if (Call.Source->ConstantReturn) {
      const auto &Output = Call.Operation->Output;
      if (Output.Size != 4 ||
          (Output.Kind != MedVar::Temp &&
           (Output.Kind != MedVar::Reg || Output.RegOff != x86reg::RAX)))
        return std::nullopt;
      const auto Use = getX86RegistrationCallResultUse(Function, Effect, Work);
      if (!Use)
        return std::nullopt;
      Call.ObservesScalarReturn =
          *Use == X86RegistrationCallResultUse::Observed;
      continue;
    }
    if (!hasUnusedX86RegistrationCallResult(
            Function, Effect, Work,
            Call.Source->CalleeKind !=
                RegistrationSEHBoundCall::Kind::PreservedHelper,
            Call.Source->CalleeKind ==
                    RegistrationSEHBoundCall::Kind::RaiseException
                ? Call.Source->EntryVA
                : 0))
      return std::nullopt;
  }
  return Result;
}

} // namespace neverd
