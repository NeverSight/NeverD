//===- LLVMValueProvenance.h - Semantic value provenance ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines internal LLVM IR provenance for values deliberately introduced by
/// the lifter as semantic data producers.  Such a producer can still be dead;
/// consumers must inspect the value flow rather than treating the marker's
/// presence as a use.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVMVALUEPROVENANCE_H
#define NEVERD_BACKEND_LLVMVALUEPROVENANCE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Metadata.h"

namespace neverd::llvm_value_provenance {

inline constexpr llvm::StringLiteral
    SemanticProducerAttachment("neverd.value.semantic-producer");

inline void markSemanticProducer(llvm::Instruction &Instruction) {
  Instruction.setMetadata(SemanticProducerAttachment,
                          llvm::MDNode::get(Instruction.getContext(), {}));
}

inline bool isSemanticProducer(const llvm::Instruction &Instruction) {
  return Instruction.getMetadata(SemanticProducerAttachment) != nullptr;
}

} // namespace neverd::llvm_value_provenance

#endif // NEVERD_BACKEND_LLVMVALUEPROVENANCE_H
