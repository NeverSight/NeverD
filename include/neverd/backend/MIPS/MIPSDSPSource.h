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
