//===- SBFInstructionValidation.h - Shared requisite rules -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the canonical, side-effect-free validation decision used by both
/// staged analysis and raw execution. Keeping rule precedence here prevents
/// malformed encodings from acquiring different meanings in different
/// consumers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYSIS_SBFINSTRUCTIONVALIDATION_H
#define NEVERD_SBF_ANALYSIS_SBFINSTRUCTIONVALIDATION_H

#include "neverd/sbf/SBFIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::sbf {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE validation_detail {

struct InstructionView {
  size_t Slot = 0;
  uint8_t RawOpcode = 0;
  uint8_t Dst = 0;
  uint8_t Src = 0;
  int16_t Offset = 0;
  int32_t Immediate = 0;
  const OpcodeInfo *Info = nullptr;
};

struct InstructionValidation {
  ValidationRule Rule = ValidationRule::None;
  size_t DiagnosticSlot = 0;
  bool HasLDDWContinuation = false;
  std::optional<size_t> BranchTarget;
  std::optional<uint8_t> CallXRegister;

  [[nodiscard]] bool valid() const { return Rule == ValidationRule::None; }
};

/// Apply the requisite instruction rules in their authoritative precedence.
InstructionValidation validateInstruction(llvm::ArrayRef<uint8_t> Text,
                                          Version TheVersion,
                                          const InstructionView &Instruction);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE validation_detail
} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYSIS_SBFINSTRUCTIONVALIDATION_H
