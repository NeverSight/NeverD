//===- CRecordEntry.h - MIPS by-value C entry points ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_MIPS_CRECORDENTRY_H
#define NEVERD_LIB_BACKEND_C_MIPS_CRECORDENTRY_H

#include "../DebugParamBinding.h"

#include "llvm/ADT/StringRef.h"

namespace neverd {
struct CEmitterOptions;

namespace mips {
/// A recovered parameter's actual C carrier. A source prototype supplies the
/// object layout, while this site establishes which bytes the lifted body uses.
struct CEntryCarrier {
  RecoveredParamSite Site;
  NdTypeKind Kind;
  std::string Spelling;
};

struct CRecordEntry {
  std::string BodyName;
  std::string Declaration;
  std::string Definition;
};

bool hasRecordEntryDeclaration(const FunctionSym &Function);

/// Build the public by-value entry around the recovered machine-body signature.
/// Each operand must match a register or an entry-SP slot in the same ELF ABI.
std::optional<CRecordEntry>
cRecordEntry(const CEmitterOptions &Options, const DebugContext *Debug,
             const FunctionSym &Function, va_t Entry, llvm::StringRef Name,
             llvm::ArrayRef<CEntryCarrier> Inputs, const CEntryCarrier &Result);
} // namespace mips
} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_MIPS_CRECORDENTRY_H
