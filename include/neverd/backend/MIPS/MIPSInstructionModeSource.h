//===- MIPSInstructionModeSource.h - Source instruction mode spelling ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source mode spelling and the qualification boundary for numeric code values.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_MIPS_MIPSINSTRUCTIONMODESOURCE_H
#define NEVERD_BACKEND_MIPS_MIPSINSTRUCTIONMODESOURCE_H

#include "neverd/Common.h"
#include "neverd/loader/BinaryImageModel.h"

#include "llvm/IR/Function.h"

#include <stdexcept>

namespace neverd::mips {

#define MIPS_SOURCE_MODE(Name, Feature, Exclude, Attribute)
#define MIPS_SOURCE_TEXT(Name, Value) inline constexpr char Name[] = Value;
#include "neverd/backend/MIPS/MIPSInstructionModes.def"

/// Called only when a source value acquires a resolved code identity. Direct
/// control transfers do not use this boundary. Current Clang MIPS data layouts
/// can fold a microMIPS function pointer's low bit to zero, including in static
/// initializers; a mode attribute alone does not qualify numeric observations.
inline void requireNumericCodePointerSource(const BinaryImage *Image,
                                            va_t CanonicalAddress) {
  if (!Image || !hasCompressedMIPSCode(Image->Mode))
    return;
  if (Image->isImportStubAt(CanonicalAddress) ||
      Image->instructionModeAt(CanonicalAddress) !=
          InstructionMode::MIPSClassic)
    throw std::invalid_argument("compressed MIPS numeric code-pointer source "
                                "projection is not qualified");
}

inline const char *instructionModeCAttribute(InstructionMode Mode) {
  switch (Mode) {
#define MIPS_SOURCE_MODE(Name, Feature, Exclude, Attribute)                    \
  case InstructionMode::Name:                                                  \
    return Attribute;
#include "neverd/backend/MIPS/MIPSInstructionModes.def"
  default:
    return "";
  }
}

inline void applyInstructionModeLLVMAttributes(llvm::Function &Function,
                                               InstructionMode Mode) {
  switch (Mode) {
#define MIPS_SOURCE_MODE(Name, Feature, Exclude, Attribute)                    \
  case InstructionMode::Name:                                                  \
    Function.addFnAttr(Feature);                                               \
    Function.addFnAttr(Exclude);                                               \
    return;
#include "neverd/backend/MIPS/MIPSInstructionModes.def"
  default:
    return;
  }
}

inline InstructionMode instructionModeFromLLVM(const llvm::Function &Function) {
#define MIPS_SOURCE_MODE(Name, Spelling, Exclude, Attribute)                   \
  if (Function.hasFnAttribute(Spelling))                                       \
    return InstructionMode::Name;
#include "neverd/backend/MIPS/MIPSInstructionModes.def"
  return InstructionMode::Default;
}

} // namespace neverd::mips

#endif // NEVERD_BACKEND_MIPS_MIPSINSTRUCTIONMODESOURCE_H
