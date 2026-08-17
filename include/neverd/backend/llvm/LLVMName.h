//===- LLVMName.h - Object symbol to LLVM global naming -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the format boundary between loader-native object symbol spellings
/// and canonical LLVM global names.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMNAME_H
#define NEVERD_BACKEND_LLVM_LLVMNAME_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"

namespace neverd::llvm_name {

/// Convert a proven object-file symbol spelling to the name LLVM IR uses.
/// Mach-O object symbols carry one Darwin decoration underscore which LLVM's
/// object writer will add again during recompilation.  Remove exactly that one
/// underscore; any remaining underscores are part of the semantic symbol.
inline llvm::StringRef fromObjectSymbol(llvm::StringRef Name,
                                        BinaryFormat Format) {
  if (Format == BinaryFormat::MachO && !Name.starts_with("\01") &&
      Name.starts_with("_"))
    return Name.drop_front(1);
  return Name;
}

} // namespace neverd::llvm_name

#endif // NEVERD_BACKEND_LLVM_LLVMNAME_H
