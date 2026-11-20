//===- BlockAddressUse.h - Source body replacement rules --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_REWRITE_BLOCKADDRESSUSE_H
#define NEVERD_LIB_BACKEND_CODEGEN_REWRITE_BLOCKADDRESSUSE_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
class Function;
}

namespace neverd {
/// Return the diagnostic if deleting this body would invalidate a label
/// referenced outside the complete set of bodies being removed.
llvm::StringRef blockAddressEscape(
    const llvm::Function &Body,
    llvm::function_ref<bool(const llvm::Function *)> RemovesBody);
} // namespace neverd
#endif
