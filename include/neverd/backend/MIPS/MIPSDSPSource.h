//===- MIPSDSPSource.h - DSP state source identity ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve the exact effectful state transfers in LLVM and reconstructed C.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_MIPS_MIPSDSP_SOURCE_H
#define NEVERD_BACKEND_MIPS_MIPSDSP_SOURCE_H

#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/loader/BinaryImageModel.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace neverd::mips {

#define NEVERD_MIPS_DSP_SOURCE_TEXT(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "neverd/backend/MIPS/MIPSDSPSource.def"

struct DSPPartialAssembly {
  Intrinsic Id;
  const char *LLVM, *C, *Constraints;
  unsigned Inputs;
};
inline constexpr DSPPartialAssembly DSPPartialOperations[] = {
#define NEVERD_DSP_COMPARE(Name, Value, Helper, Mnemonic)                      \
  {Intrinsic::Name, ".set push\n.set dsp\n" Mnemonic " $0,$1\n.set pop",       \
   "\".set push\\n.set dsp\\n" Mnemonic " %0,%1\\n.set pop\"",                 \
   DSPCompareConstraints, 2},
#include "neverd/ir/intrinsics/MIPS/MIPSDSPControlEffects.def"
    {Intrinsic::MIPSMthlipPosition, DSPPositionAsm, DSPPositionCAsm,
     DSPPositionConstraints, 0}};

inline const DSPPartialAssembly &dspPartialOperation(Intrinsic Id) {
  return DSPPartialOperations[unsigned(Id) - unsigned(Intrinsic::MIPSCmpEqPH)];
}
inline std::string dspPartialC(const DSPPartialAssembly &A, std::string X,
                               std::string Y) {
  std::string S = std::string("__asm__ volatile(") + A.C;
  if (!A.Inputs)
    return S + " ::: \"$8\", \"$9\", \"memory\");";
  return S + " :: \"r\"((uint32_t)(" + X + ")), \"r\"((uint32_t)(" + Y +
         ")) : \"memory\");";
}

inline bool imagePreservesDSPControl(const BinaryImage *Image) {
  return Image && Image->MIPS && Image->MIPS->HasDSP;
}

inline bool sourcePreservesDSPControl(Arch Target, const BinaryImage *Image) {
  // Standalone MIPS32 source has no image profile that can rule out DSP state.
  // Even pure lifted arithmetic must not acquire compiler-generated DSP writes.
  return imagePreservesDSPControl(Image) ||
         (!Image && (Target == Arch::MIPS32 || Target == Arch::MIPS32EL));
}

inline bool modulePreservesDSPControl(const llvm::Module &Module,
                                      const BinaryImage *Image, Arch Target) {
  if (sourcePreservesDSPControl(Target, Image))
    return true;
  for (const llvm::Function &Function : Module)
    if (Function.hasFnAttribute(DSPSourceMarker))
      return true;
  return false;
}

/// Normal wide integer/vector expressions may otherwise select ADDSC/ADDWC
/// or packed DSP operations and change state beyond the original transfers.
inline void applyDSPSourceAttributes(llvm::Function &Function) {
  if (Function.hasFnAttribute(DSPSourceMarker))
    return;
  const auto Attribute = Function.getFnAttribute(DSPFeatureAttribute);
  std::string Features = Attribute.isStringAttribute()
                             ? Attribute.getValueAsString().str()
                             : std::string();
  if (!Features.empty())
    Features += ',';
  Features += DSPSourceFeatures;
  Function.addFnAttr(DSPFeatureAttribute, Features);
  Function.addFnAttr(DSPSourceMarker);
}

/// A recognized transfer has a complete fixed register/side-effect contract.
inline std::optional<bool> dspControlSourceWrite(const llvm::CallBase &Call) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm ||
      (Asm->getAsmString() != DSPReadAsm && Asm->getAsmString() != DSPWriteAsm))
    return std::nullopt;
  const bool Write = Asm->getAsmString() == DSPWriteAsm;
  if (!Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      Asm->getConstraintString() !=
          (Write ? DSPWriteConstraints : DSPReadConstraints) ||
      Call.arg_size() != unsigned(Write) ||
      (Write ? (!Call.getType()->isVoidTy() ||
                !Call.getArgOperand(0)->getType()->isIntegerTy(32))
             : !Call.getType()->isIntegerTy(32)))
    throw std::invalid_argument(MalformedDSPSource);
  return Write;
}

} // namespace neverd::mips

#endif // NEVERD_BACKEND_MIPS_MIPSDSP_SOURCE_H
