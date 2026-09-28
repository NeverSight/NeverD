//===- LLVMValueProvenance.h - Semantic value provenance ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines internal LLVM IR provenance for semantic data producers and source
/// returns that explicitly read a private frame slot. A producer can still be
/// dead; consumers must inspect value flow rather than treating a marker's
/// presence as a use.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVMVALUEPROVENANCE_H
#define NEVERD_BACKEND_LLVMVALUEPROVENANCE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"

namespace neverd::llvm_value_provenance {

inline constexpr llvm::StringLiteral
    SemanticProducerAttachment("neverd.value.semantic-producer");
inline constexpr llvm::StringLiteral
    ExplicitMemoryReturnAttachment("neverd.return.explicit-memory-value");

inline void markSemanticProducer(llvm::Instruction &Instruction) {
  Instruction.setMetadata(SemanticProducerAttachment,
                          llvm::MDNode::get(Instruction.getContext(), {}));
}

inline bool isSemanticProducer(const llvm::Instruction &Instruction) {
  return Instruction.getMetadata(SemanticProducerAttachment) != nullptr;
}

/// An explicit source return may read its value from a private frame slot.
/// SROA can remove that load, so keep the return's value-flow evidence on the
/// terminator for C-level void inference after optimization.
inline void markExplicitMemoryReturn(llvm::ReturnInst &Return) {
  Return.setMetadata(ExplicitMemoryReturnAttachment,
                     llvm::MDNode::get(Return.getContext(), {}));
}

inline bool isExplicitMemoryReturn(const llvm::ReturnInst &Return) {
  return Return.getMetadata(ExplicitMemoryReturnAttachment) != nullptr;
}

} // namespace neverd::llvm_value_provenance

#endif // NEVERD_BACKEND_LLVMVALUEPROVENANCE_H
