//===- PDBLoader.h - PDB debug info loader ------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares PDBDebugContext which loads PDB debug information from
/// Windows PE binaries via LLVM's PDB library.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_PDBLOADER_H
#define NEVERD_DEBUG_PDBLOADER_H

#include "neverd/debug/DebugContext.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct BinaryImage;

namespace pdb_loader_detail {

/// A CodeView symbol plus the exact byte offset where its validated record
/// begins.  The record borrows its bytes from the owning PDB stream.
struct IndexedSymbolRecord {
  uint32_t Offset = 0;
  llvm::codeview::CVSymbol Symbol;
};

/// Validate an entire variable-length symbol stream transactionally.  A
/// malformed trailing record rejects the whole index rather than publishing
/// the valid prefix.
llvm::Expected<std::vector<IndexedSymbolRecord>>
indexSymbolRecords(const llvm::codeview::CVSymbolArray &Records);

/// Return a symbol only when Offset is one of the validated record starts.
/// This replaces LLVM's unsafe CVSymbolArray::at() for GSI-provided offsets.
const llvm::codeview::CVSymbol *
findSymbolAtExactOffset(llvm::ArrayRef<IndexedSymbolRecord> Records,
                        uint32_t Offset);

/// Sticky, order-independent policy for names supplied by independent PDB
/// symbol streams.  A conflicting name at the same address is not a semantic
/// identity source, regardless of which stream LLVM happens to visit first.
enum class FunctionNameState : uint8_t {
  Absent,
  Unique,
  Ambiguous,
};

class FunctionNameRegistry {
public:
  void observe(va_t Address, llvm::StringRef Name);
  FunctionNameState state(va_t Address) const;
  std::optional<std::string> name(va_t Address) const;

private:
  struct Record {
    FunctionNameState State = FunctionNameState::Absent;
    std::string Name;
  };
  std::map<va_t, Record> Records;
};

} // namespace pdb_loader_detail

class PDBDebugContext : public DebugContext {
public:
  ~PDBDebugContext() override;

  /// Load only a PDB whose Info GUID+age, DBI metadata, machine, and section
  /// table all agree with the already loaded PE image.  A mismatched companion
  /// is an error rather than a names-only context, because debug names affect
  /// downstream semantic classification too.
  static llvm::Expected<std::unique_ptr<PDBDebugContext>>
  load(const std::filesystem::path &PdbPath, const BinaryImage &Image);

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override;
  std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                             int64_t Offset) const override;
  std::optional<TypeSym> resolveType(uint64_t TypeId) const override;
  std::optional<SourceLoc> sourceLocation(va_t Addr) const override;
  std::vector<FunctionSym> allFunctions() const override;
  bool hasInfo() const override;

  bool hasAuthenticatedImageIdentity() const;
  /// Phase A never authorizes exact object metadata; identity-authenticated
  /// function names remain a separate, names-only capability.
  bool hasExactObjectMetadataPrerequisites() const;

private:
  PDBDebugContext() = default;

  struct Impl;
  std::unique_ptr<Impl> PImpl;
};

} // namespace neverd

#endif // NEVERD_DEBUG_PDBLOADER_H
