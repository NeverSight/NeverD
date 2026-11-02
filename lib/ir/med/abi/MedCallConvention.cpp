//===- MedCallConvention.cpp - Calling-convention registry ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallConvention.h"

namespace neverd {

extern const CallArgumentConvention Win64CallArguments;
extern const CallArgumentConvention SysVX64CallArguments;
extern const CallArgumentConvention I386CallArguments;
extern const CallArgumentConvention DarwinAArch64CallArguments;
extern const CallArgumentConvention WindowsAArch64CallArguments;
extern const CallArgumentConvention AAPCS32CallArguments;
extern const CallArgumentConvention AAPCS64CallArguments;

const CallArgumentConvention *callArgumentConvention(Arch A, BinaryFormat F) {
  static constexpr const CallArgumentConvention *Conventions[] = {
      &Win64CallArguments,          &SysVX64CallArguments,
      &I386CallArguments,           &DarwinAArch64CallArguments,
      &WindowsAArch64CallArguments, &AAPCS32CallArguments,
      &AAPCS64CallArguments};
  for (BinaryFormat Wanted : {F, BinaryFormat::Unknown})
    for (const CallArgumentConvention *C : Conventions)
      if (C->TheArch == A && C->Format == Wanted)
        return C;
  return nullptr;
}

bool holdsUndefinedIncomingScratch(const MedFunc &Func, Arch TheArch,
                                   BinaryFormat Format, const MedVar &V) {
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  if (!isUndefinedIncomingScratch(
          TRI, TheArch, callArgumentConvention(TheArch, Format),
          TRI.integerArgumentLayout(Format).Registers, V))
    return false;
  const std::optional<unsigned> Family = gprFamilyOf(TheArch, V.RegOff);
  if (llvm::any_of(Func.Params, [&](const MedVar &Param) {
        return Param.RegOff != kNoParamReg &&
               gprFamilyOf(TheArch, Param.RegOff) == Family;
      }))
    return false;
  const auto Defines = [&](const MedVar &Value) {
    return Value.Kind == V.Kind && Value.Id == V.Id && Value.SSAVer == V.SSAVer;
  };
  if (llvm::any_of(Func.CallClobbers, [&](const MedCallClobber &Clobber) {
        return Defines(Clobber.Value);
      }))
    return false;
  for (const MedBlock &Block : Func.Blocks) {
    if (llvm::any_of(Block.Phis,
                     [&](const PhiNode &Phi) { return Defines(Phi.Output); }))
      return false;
    for (const MedOp &Op : Block.Ops) {
      // An entry self-copy names the incoming value; it defines none.
      if ((Defines(Op.Output) &&
           !(Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
             Op.Inputs[0] == Op.Output)) ||
          llvm::any_of(Op.IntrinsicOutputs, Defines))
        return false;
    }
  }
  return true;
}

std::optional<CallingConv> callingConventionOf(Arch A, BinaryFormat F) {
  switch (A) {
  case Arch::X64:
    return F == BinaryFormat::COFF ? CallingConv::Win64
                                   : CallingConv::SysV_AMD64;
  case Arch::AArch64:
  case Arch::ARM:
    return CallingConv::ARM_AAPCS;
  default:
    return std::nullopt;
  }
}

} // namespace neverd
