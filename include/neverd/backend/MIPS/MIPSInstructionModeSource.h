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

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Function.h"

#include <stdexcept>

namespace neverd::mips {

#define MIPS_SOURCE_MODE(Name, Feature, Exclude, Attribute)
#define MIPS_SOURCE_TEXT(Name, Value)                                          \
  inline constexpr llvm::StringLiteral Name(Value);
#include "neverd/backend/MIPS/MIPSInstructionModes.def"

/// A reconstructed definition cannot take the assembler name the compiler
/// generates again for its compressed callee. Only its C identifier changes;
/// the original function, call identity and source address mapping remain.
inline std::string entryStubDefinitionStem(const BinaryImage *Image,
                                           llvm::StringRef Name) {
  if (!Image || !Image->MIPS || !Image->MIPS->HasMIPS16 ||
      !Name.consume_front(CompilerEntryStubPrefix))
    return {};
  return (llvm::Twine(ReconstructedEntryStubPrefix) + Name).str();
}

/// Called only when a source value acquires a resolved code identity. Direct
/// control transfers do not use this boundary. The qualified compiler must
/// preserve the callable tag independently of the function's code alignment
/// (Fi8 in the NeverSight LLVM fork). Unknown import modes and unqualified
/// compressed source profiles still have no numerical projection.
inline void requireNumericCodePointerSource(const BinaryImage *Image,
                                            va_t CanonicalAddress) {
  if (!Image || !hasCompressedMIPSCode(Image->Mode))
    return;
  const auto Mode = Image->instructionModeAt(CanonicalAddress);
  if (CanonicalAddress != InvalidVA &&
      !Image->isImportStubAt(CanonicalAddress)) {
    if (Mode == InstructionMode::MIPSClassic)
      return;
    if (Image->MIPS && Image->MIPS->ISARevision == 2 &&
        !Image->MIPS->HistoricalISA) {
      const bool O32 =
          (Image->Arch == Arch::MIPS32 || Image->Arch == Arch::MIPS32EL) &&
          Image->MIPS->ABI == MIPSABI::O32;
      const bool N32 = (Image->Arch == Arch::MIPS64N32 ||
                        Image->Arch == Arch::MIPS64N32EL) &&
                       Image->MIPS->ABI == MIPSABI::N32;
      const bool N64 =
          (Image->Arch == Arch::MIPS64 || Image->Arch == Arch::MIPS64EL) &&
          Image->MIPS->ABI == MIPSABI::N64;
      if ((Mode == InstructionMode::MIPS16 && Image->MIPS->HasMIPS16 && O32) ||
          (Mode == InstructionMode::MicroMIPS && Image->MIPS->HasMicroMIPS &&
           (O32 || N32 || N64)))
        return;
    }
  }
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
