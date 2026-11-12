//===- MedCallConventionMIPS.cpp - MIPS o32/n64 scalar call arguments ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// o32 uses four 32-bit argument registers and aligned pairs for 64-bit
/// integers; n64 uses eight 64-bit carriers. TargetRegInfo owns the distinct
/// stack bases. Fixed scalar FP signatures share source parameter placement;
/// aggregate contracts require separate admission.

#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd {

extern const CallArgumentConvention MIPSO32CallArguments;
extern const CallArgumentConvention MIPSELO32CallArguments;
extern const CallArgumentConvention MIPSN64CallArguments;
extern const CallArgumentConvention MIPSELN64CallArguments;

const CallArgumentConvention MIPSO32CallArguments = {
    .TheArch = Arch::MIPS32,
    .ImportArgumentsFromPrototype = true,
    .PositionalArgumentSlots = true,
    .PairAlignedWideArguments = true,
    .IntegerParametersUseFullRegisters = true,
    .ArgumentsFromCallSetup = true,
};
const CallArgumentConvention MIPSELO32CallArguments = {
    .TheArch = Arch::MIPS32EL,
    .ImportArgumentsFromPrototype = true,
    .PositionalArgumentSlots = true,
    .PairAlignedWideArguments = true,
    .IntegerParametersUseFullRegisters = true,
    .ArgumentsFromCallSetup = true,
};
const CallArgumentConvention MIPSN64CallArguments = {
    .TheArch = Arch::MIPS64,
    .ImportArgumentsFromPrototype = true,
    .PositionalArgumentSlots = true,
    .IntegerParametersUseFullRegisters = true,
    .ArgumentsFromCallSetup = true,
};
const CallArgumentConvention MIPSELN64CallArguments = {
    .TheArch = Arch::MIPS64EL,
    .ImportArgumentsFromPrototype = true,
    .PositionalArgumentSlots = true,
    .IntegerParametersUseFullRegisters = true,
    .ArgumentsFromCallSetup = true,
};

// o32's leading f12/f14 parameters consume integer word positions. Keep
// those logical arguments before the remaining integer carriers, and remove
// only unused GPR placeholders covered by the classified FP prefix.
void orderMIPSO32FloatingParameters(MedFunc &Func, const TargetRegInfo &TRI,
                                    BinaryFormat Fmt) {
  std::vector<MedVar> Prefix;
  std::vector<TypeRef> Types;
  const auto FP = TRI.floatingParamRegs(Fmt);
  for (size_t I = 0; I < FP.size(); ++I) {
    const auto P = llvm::find_if(
        Func.Params, [&](const MedVar &V) { return V.RegOff == FP[I]; });
    if (P == Func.Params.end()) {
      if (I == 0 && llvm::any_of(Func.Params, [&](const MedVar &V) {
            return V.RegOff == FP.back();
          }))
        throw LowToMedConversionError(
            "MIPS o32 FP prefix requires its leading parameter width");
      break;
    }
    const auto Width = Func.FPParamScalarBytes.find(P->RegOff);
    if (Width == Func.FPParamScalarBytes.end() ||
        (Width->second != 4 && Width->second != 8))
      throw LowToMedConversionError("MIPS source requires scalar FP arguments");
    Prefix.push_back(*P);
    Prefix.back().Size = Width->second;
    Types.push_back(NdType::makeFloat(Width->second));
  }
  if (!Prefix.empty()) {
    Types.push_back(NdType::makeInt(4, false));
    const auto Placement =
        placeSourceParameters(TRI.TheArch, Fmt, NdType::makeVoid(), Types);
    const auto &Next = Placement->Parameters.back().front().Location;
    const auto Layout = TRI.integerArgumentLayout(Fmt);
    const size_t UsedWords = Next.Kind == SourceABICarrierKind::Stack
                                 ? Layout.Registers.size()
                                 : Layout.registerIndex(Next.RegisterOffset);
    for (const auto &P : Func.Params) {
      if (TRI.isFPArgReg(P.RegOff, Fmt))
        continue;
      const auto Slot = Layout.registerIndex(P.RegOff);
      if (Slot >= 0 && size_t(Slot) < UsedWords) {
        if (P.Id >= 0)
          throw LowToMedConversionError(
              "MIPS o32 entry reads overlapping FP and integer arguments");
        continue;
      }
      Prefix.push_back(P);
    }
    Func.Params = std::move(Prefix);
  }
}

} // namespace neverd
