//===- MedABIPassI386.cpp - i386 call-ABI recovery steps ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The i386 policy of recoverCallAbi.  cdecl passes every argument on the
/// stack, and clang constant-propagates an argument the callee never reads
/// into it without storing the argument's slot (a variadic `vfn(3, &G..)`
/// whose count is folded into the callee never stores arg0).  The real stack
/// arguments then start at slot 1 with slot 0 empty, and a strict first-gap
/// cutoff would drop them all.  An ordinary call, and a forwarder whose
/// first slot is always present, keeps the strict cutoff.
///
//===----------------------------------------------------------------------===//

#include "../MedABIPassDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/X86/X86RegistrationCall.h"
#include "neverd/lift/X86/X86Regs.h"

namespace neverd {

bool recoverI386RegistrationCookieCall(const MedFunc &Func,
                                       const MedBlock &Block, size_t CallIndex,
                                       const TargetRegInfo &TRI,
                                       MedCallInfo &Info) {
  if (!Func.ExceptionMetadata || !Func.RegistrationStates || Func.SkippedSSA ||
      CallIndex >= Block.Ops.size())
    return false;
  const auto &EH = *Func.ExceptionMetadata;
  const auto &States = *Func.RegistrationStates;
  const auto &Call = Block.Ops[CallIndex];
  const bool CxxCookies =
      EH.Encoding == ExceptionEncoding::X86CxxFuncInfo && EH.Cxx &&
      EH.Personality == ExceptionPersonality::CxxFrameHandler3 &&
      !States.CxxCookieConditions.empty() &&
      States.CxxCookieConditions.size() <= 2 && States.SecurityCookieVA &&
      States.SecurityCookieVA <= UINT32_MAX - 3;
  if (!EH.Registration || EH.ParseStatus != ExceptionParseStatus::Complete ||
      (EH.Encoding != ExceptionEncoding::X86ScopeTableEH4 && !CxxCookies) ||
      Func.Entry != EH.CodeRange.Begin || !States.SecurityCookiesComplete ||
      States.CookieChecks.size() > limits::kMaxRegistrationEHRecords ||
      !States.CookieCheckVA || States.CookieCheckVA > UINT32_MAX || Call.Dead ||
      Call.Opcode != NdOp::CALL || Call.NumInputs != 1 ||
      !Call.Inputs[0].isConst() || Call.Inputs[0].Size != 4 ||
      Call.Inputs[0].ConstVal != States.CookieCheckVA || Call.OriginSeq < 0 ||
      Call.ExactArguments || Call.SourceCallHint || Call.DoesNotReturn)
    return false;
  const RegistrationCookieCheck *Check = nullptr;
  for (const auto &Candidate : States.CookieChecks)
    if (Candidate.Address == Call.Addr && Candidate.OpSeq == Call.OriginSeq) {
      if (Check)
        return false;
      Check = &Candidate;
    }
  if (!Check || Call.Addr < Block.StartAddr || Check->EndAddress <= Call.Addr ||
      Check->EndAddress > Block.EndAddr ||
      !EH.ownsCode({Call.Addr, Check->EndAddress}))
    return false;
  size_t Work = 0;
  bool Found = false;
  for (const auto &CurrentBlock : Func.Blocks)
    for (const auto &Current : CurrentBlock.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      if (Current.Addr != Call.Addr || Current.OriginSeq != Call.OriginSeq)
        continue;
      if (Found || &Current != &Call || &CurrentBlock != &Block)
        return false;
      Found = true;
    }
  if (!Found)
    return false;

  // The LowIR receipt proves the checker and its decoded cookie input. Use
  // current SSA definitions for that input; EDX, XMM values and private stack
  // spills are not additional arguments of the compiler's cookie protocol.
  static constexpr uint64_t CookieRegisters[] = {x86reg::RCX};
  auto Layout = TRI.integerArgumentLayout(BinaryFormat::COFF);
  Layout.Registers = CookieRegisters;
  std::optional<MedVar> Argument;
  for (size_t I = CallIndex; I != 0; --I) {
    const auto &Op = Block.Ops[I - 1];
    if (isAbiRecoveryBarrier(Op, /*RegistersOnly=*/true))
      break;
    if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == x86reg::RCX &&
        Op.Output.Size == 4) {
      Argument = argRegSourceValueInBlock(Block, I - 1, TRI, Layout);
      break;
    }
  }
  if (!Argument)
    Argument =
        findReachingArgReg(Func, TRI, Arch::X86, Block.Id, 0, Layout,
                           /*AllowUnknownLiveIn=*/false, nullptr, nullptr,
                           static_cast<int>(CallIndex), BinaryFormat::COFF);
  if (!Argument || Argument->Size != 4 || Argument->Kind == MedVar::Unspecified)
    return false;
  Info.Args = {*Argument};
  Info.IntegerRegisterArgCount = 1;
  Info.FloatingRegisterArgCount = 0;
  Info.ArgumentsInCalleeRegisterOrder = true;
  Info.StackArgumentSlotBytes = 4;
  return true;
}

std::optional<RegistrationCookieCallABI>
registrationCookieCallABI(const BinaryImage &Image, const MedFunc &Func,
                          const MedBlock &Block, size_t CallIndex) {
  MedCallInfo Info;
  if (Image.Arch != Arch::X86 || Image.Format != BinaryFormat::COFF ||
      Image.Bits != Bitness::Bits32 ||
      Func.Blocks.size() > limits::kMaxRegistrationEHRecords ||
      !recoverI386RegistrationCookieCall(Func, Block, CallIndex,
                                         getTargetRegInfo(Arch::X86), Info))
    return std::nullopt;
  const auto &Call = Block.Ops[CallIndex];
  const auto &States = *Func.RegistrationStates;
  const auto *Check = States.cookieCheck(Call.Addr, Call.OriginSeq);
  const auto Success = coff_loader::getCheckedX86CookieCheckSuccessPath(
      Image, States.CookieCheckVA);
  if (!Check || Call.Addr > UINT32_MAX - 5 ||
      Check->EndAddress != Call.Addr + 5 || !Success ||
      Success->SecurityCookieVA != States.SecurityCookieVA ||
      !hasCheckedX86RegistrationDirectCallBytes(Image, Call.Addr,
                                                States.CookieCheckVA))
    return std::nullopt;
  return RegistrationCookieCallABI{Info.Args.front(), States.CookieCheckVA,
                                   Success->SecurityCookieVA};
}

extern const AbiCallPolicy I386AbiCallPolicy;
const AbiCallPolicy I386AbiCallPolicy = {
    .TheArch = Arch::X86,
    .LeadingStackGapIsUnusedArgument = true,
};

} // namespace neverd
