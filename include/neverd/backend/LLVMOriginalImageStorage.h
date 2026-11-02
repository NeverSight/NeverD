//===- LLVMOriginalImageStorage.h - Source image identity ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// An analysis view can refer to the loaded image as one external byte array.
/// Its provider owns relocation and runtime initialization. Keep the shared
/// storage identity through C projection instead of inventing independent
/// globals for overlapping fields or copying on-disk pointer values.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVMORIGINALIMAGESTORAGE_H
#define NEVERD_BACKEND_LLVMORIGINALIMAGESTORAGE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"

namespace neverd::llvm_original_image {

inline constexpr llvm::StringLiteral Attachment("neverd.source.image-storage");

inline void mark(llvm::GlobalVariable &Global) {
  Global.setMetadata(Attachment, llvm::MDNode::get(Global.getContext(), {}));
}

inline bool isMarked(const llvm::GlobalVariable &Global) {
  return Global.getMetadata(Attachment) != nullptr;
}

} // namespace neverd::llvm_original_image

#endif // NEVERD_BACKEND_LLVMORIGINALIMAGESTORAGE_H
