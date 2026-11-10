//===- MIPSSourceCompiler.h - MIPS source compiler constraints -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep source compiler constraints consistent across LLVM, C and helpers.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_MIPS_MIPS_SOURCE_COMPILER_H
#define NEVERD_BACKEND_MIPS_MIPS_SOURCE_COMPILER_H

#include "neverd/backend/MIPS/MIPSDSPSource.h"

namespace neverd::mips {

#define NEVERD_MIPS_SOURCE_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/backend/MIPS/MIPSSourceCompiler.def"

inline bool sourceAvoidsMSA(Arch Target, const BinaryImage *Image) {
  // Clang 22's big-endian MSA constant-splat bitcasts can swap the two words
  // of a scalar comparison bound (32 becomes 32 << 32). Keep scalar source
  // out of that instruction selector, including image-less source compiled
  // with external -mmsa. Little-endian and known non-MSA images are unchanged.
  return Target == Arch::MIPS32 &&
         (!Image || (Image->MIPS && Image->MIPS->HasMSA));
}

inline bool moduleAvoidsMSA(const llvm::Module &Module,
                            const BinaryImage *Image, Arch Target) {
  if (sourceAvoidsMSA(Target, Image))
    return true;
  for (const llvm::Function &Function : Module)
    if (Function.hasFnAttribute(NoMSAMarker))
      return true;
  return false;
}

inline void applyNoMSASourceAttributes(llvm::Function &Function) {
  if (Function.hasFnAttribute(NoMSAMarker))
    return;
  const auto Attribute = Function.getFnAttribute(DSPFeatureAttribute);
  std::string Features = Attribute.isStringAttribute()
                             ? Attribute.getValueAsString().str()
                             : std::string();
  if (!Features.empty())
    Features += ',';
  Features += NoMSAFeatures;
  Function.addFnAttr(DSPFeatureAttribute, Features);
  Function.addFnAttr(NoMSAMarker);
}

inline const char *sourceCompilerCAttribute(Arch Target,
                                            const BinaryImage *Image,
                                            bool HasDSP = false,
                                            bool AvoidMSA = false) {
  HasDSP |= sourcePreservesDSPControl(Target, Image);
  AvoidMSA |= sourceAvoidsMSA(Target, Image);
  // Multiple target attributes do not compose in Clang: a declaration's
  // attribute can replace a surrounding pragma. Emit one complete contract.
  return AvoidMSA ? (HasDSP ? DSPNoMSACAttribute : NoMSACAttribute)
                  : (HasDSP ? DSPSourceCAttribute : "");
}

inline const char *sourceCompilerCBegin(Arch Target, const BinaryImage *Image,
                                        bool HasDSP = false,
                                        bool AvoidMSA = false) {
  HasDSP |= sourcePreservesDSPControl(Target, Image);
  AvoidMSA |= sourceAvoidsMSA(Target, Image);
  return AvoidMSA ? (HasDSP ? DSPNoMSACBegin : NoMSACBegin)
                  : (HasDSP ? DSPSourceCBegin : "");
}

} // namespace neverd::mips

#endif // NEVERD_BACKEND_MIPS_MIPS_SOURCE_COMPILER_H
