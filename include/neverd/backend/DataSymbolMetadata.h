//===- DataSymbolMetadata.h - Opaque external symbols ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the attachment that marks an undefined global standing for another
/// module's symbol whose address the module takes without knowing its type:
/// relocated data, or a function a pointer starts at before any call proves
/// its signature. Import identities remain distinct from object relocations
/// so C can use an established runtime prototype without retyping a data
/// object that happens to share the routine's name. Otherwise C uses an opaque
/// assembly-name alias instead of inventing a declaration.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_DATASYMBOLMETADATA_H
#define NEVERD_BACKEND_DATASYMBOLMETADATA_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Casting.h"

namespace neverd::data_symbol {

inline constexpr llvm::StringLiteral Attachment("neverd.data-symbol");
inline constexpr llvm::StringLiteral ImportKind("import");

inline void mark(llvm::GlobalVariable &Global) {
  Global.setMetadata(Attachment, llvm::MDNode::get(Global.getContext(), {}));
}

inline void markImport(llvm::GlobalVariable &Global) {
  auto &Context = Global.getContext();
  Global.setMetadata(
      Attachment,
      llvm::MDNode::get(Context, llvm::MDString::get(Context, ImportKind)));
}

inline bool isMarked(const llvm::GlobalVariable &Global) {
  return Global.getMetadata(Attachment) != nullptr;
}

inline bool isImport(const llvm::GlobalVariable &Global) {
  const auto *Metadata = Global.getMetadata(Attachment);
  if (!Metadata || Metadata->getNumOperands() != 1)
    return false;
  const auto *Kind = llvm::dyn_cast<llvm::MDString>(Metadata->getOperand(0));
  return Kind && Kind->getString() == ImportKind;
}

} // namespace neverd::data_symbol

#endif // NEVERD_BACKEND_DATASYMBOLMETADATA_H
