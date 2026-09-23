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

/// One S_PUB32 decoded from a contiguous symbol stream.  Name borrows from
/// that stream; callers that keep the name must copy it.
struct ParsedPublicSym32 {
  bool IsFunction = false;
  uint16_t Segment = 0;
  uint32_t Offset = 0;
  llvm::StringRef Name;
};

/// Decode the S_PUB32 whose record starts at \p Offset.  A GSI/address-map
/// offset that is not an exact public-record boundary is an error.
llvm::Error parsePublicSym32At(llvm::ArrayRef<uint8_t> Stream, uint32_t Offset,
                               ParsedPublicSym32 &Out);

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

/// One section header as a PDB recorded it at link time.
struct RecordedSection {
  llvm::StringRef Name;
  uint32_t VirtualAddress = 0;
  uint32_t VirtualSize = 0;
  uint32_t PointerToRawData = 0;
  uint32_t SizeOfRawData = 0;
  uint32_t Characteristics = 0;
};

/// True when a PDB's link-time section table still describes \p Image.
/// Every section must keep its name and characteristics.  Trailing .rsrc and
/// .reloc sections may be re-laid out, because post-link resource stamping
/// rewrites them without relinking and neither holds a code or data symbol;
/// every other section must match exactly.
bool recordedSectionsMatch(const BinaryImage &Image,
                           llvm::ArrayRef<RecordedSection> Recorded);

} // namespace pdb_loader_detail

llvm::Expected<std::unique_ptr<class PDBDebugContext>>
loadPdb20DebugContext(const std::filesystem::path &PdbPath,
                      const BinaryImage &Image);

class PDBDebugContext : public DebugContext {
public:
  ~PDBDebugContext() override;

  /// Load only a PDB whose Info identity (RSDS GUID+age or NB10
  /// signature+age), DBI metadata, machine, and section table all agree with
  /// the already loaded PE image.  A mismatched companion is an error rather
  /// than a names-only context, because debug names affect downstream
  /// semantic classification too.
  static llvm::Expected<std::unique_ptr<PDBDebugContext>>
  load(const std::filesystem::path &PdbPath, const BinaryImage &Image,
       const LoadProgress &Progress = {});

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override;
  std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                             int64_t Offset) const override;
  std::optional<TypeSym> resolveType(uint64_t TypeId) const override;
  std::optional<SourceLoc> sourceLocation(va_t Addr) const override;
  std::vector<FunctionSym> allFunctions() const override;
  std::vector<DataObjectSym> allDataObjects() const override;
  bool hasInfo() const override;
  bool hasAuthenticatedFunctionSignatures() const override;
  bool hasAuthenticatedObjectExtents() const override;

  bool hasAuthenticatedImageIdentity() const;
  /// RSDS Phase A never authorizes exact object metadata.  A PDB 2.00 JG
  /// companion that parsed TPI and S_BPREL32_ST without stream malformation
  /// may authorize extents for that image only.
  bool hasExactObjectMetadataPrerequisites() const;

private:
  PDBDebugContext();
  void commitFunctions(std::vector<FunctionSym> Functions, bool Authenticated);
  void commitDebugFacts(std::vector<FunctionSym> Functions,
                        std::map<va_t, std::map<int64_t, VariableSym>> Locals,
                        bool Authenticated, bool Signatures, bool Extents);
  friend llvm::Expected<std::unique_ptr<PDBDebugContext>>
  loadPdb20DebugContext(const std::filesystem::path &PdbPath,
                        const BinaryImage &Image);

  struct Impl;
  std::unique_ptr<Impl> PImpl;
};

} // namespace neverd

#endif // NEVERD_DEBUG_PDBLOADER_H
