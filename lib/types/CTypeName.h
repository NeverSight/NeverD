//===- CTypeName.h - Catalog spelling of C++ type names --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Token identity for references to already instantiated catalog types.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_TYPES_CTYPENAME_H
#define NEVERD_LIB_TYPES_CTYPENAME_H

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd::types {
/// Ignore whitespace and an initial global scope qualifier, preserving token
/// boundaries and literal encodings. Adjacent template closers read alike.
std::string cTypeNameKey(llvm::StringRef Name);
} // namespace neverd::types

#endif // NEVERD_LIB_TYPES_CTYPENAME_H
