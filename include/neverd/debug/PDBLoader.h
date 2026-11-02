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
#include "neverd/loader/BinaryImageDynamic.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace llvm::pdb {
class PDBFile;
} // namespace llvm::pdb

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

/// One S_PROCREF or S_LPROCREF decoded from a contiguous symbol stream: where
/// a module's symbols hold a procedure's record.  Name borrows from that
/// stream; callers that keep the name must copy it.
struct ParsedProcRef {
  /// The module, counted from one as the record counts it.
  uint16_t Module = 0;
  /// The procedure record's offset in the module's symbol stream.
  uint32_t Offset = 0;
  llvm::StringRef Name;
};

/// Decode the procedure reference whose record starts at \p Offset, or say
/// that the global there is of another kind.  A globals GSI offset that is
/// not an exact record boundary is an error.
llvm::Expected<bool> parseProcRefAt(llvm::ArrayRef<uint8_t> Stream,
                                    uint32_t Offset, ParsedProcRef &Out);

/// Where a procedure's code begins, as a section and an offset in it.
struct ProcedureStart {
  uint16_t Segment = 0;
  uint32_t Offset = 0;
};

/// Where the procedure \p Record holds begins, when it is a procedure record
/// (S_GPROC32, S_LPROC32 and their ID forms) named \p Name: what a
/// procedure reference of that name points at.
std::optional<ProcedureStart>
parseReferencedProcedure(llvm::ArrayRef<uint8_t> Record, llvm::StringRef Name);

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
/// Loader-owned image header storage is excluded; it is not a
/// native COFF section and cannot supply a PDB segment number.
bool recordedSectionsMatch(const BinaryImage &Image,
                           llvm::ArrayRef<RecordedSection> Recorded);

/// Win64 often reuses an incoming register home for a later local. Keep the
/// non-parameter name. Two distinct locals or two distinct parameters stay
/// ambiguous so HighC does not guess.
void publishNamedOffset(std::map<int64_t, VariableSym> &Slots,
                        std::set<int64_t> &Ambiguous, int64_t Offset,
                        VariableSym VS);

} // namespace pdb_loader_detail

llvm::Expected<std::unique_ptr<class PDBDebugContext>>
loadPdb20DebugContext(const std::filesystem::path &PdbPath,
                      const BinaryImage &Image);

/// The build identity \p PdbPath records, read from its Info and DBI streams
/// alone: the RSDS GUID with the age a PE's CodeView record names, or a PDB
/// 2.00 file's NB10 signature and age.  Says which PDB a file is without
/// loading it; loading still checks machine and sections too.
llvm::Expected<PDBBuildIdentity>
readPDBBuildIdentity(const std::filesystem::path &PdbPath);

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

  /// Explicit display-name import may inspect a PE without a CodeView
  /// identity. Machine and section layout must still agree. A present
  /// identity must match; malformed, ambiguous and mismatched identities
  /// are never bypassed. The caller must not publish an unauthenticated
  /// context as analysis evidence.
  static llvm::Expected<std::unique_ptr<PDBDebugContext>>
  inspect(const std::filesystem::path &PdbPath, const BinaryImage &Image,
          const LoadProgress &Progress = {});

  /// Load a user-confirmed PDB as analysis input even when the PE's CodeView
  /// identity is absent. Machine, sections and any present identity still
  /// must match. This grants the same analysis authority as a matching PDB
  /// without claiming that its build identity was verified.
  static llvm::Expected<std::unique_ptr<PDBDebugContext>>
  loadUserSelected(const std::filesystem::path &PdbPath,
                   const BinaryImage &Image, const LoadProgress &Progress = {});

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override;
  std::optional<std::string> functionName(va_t Addr) const override;
  std::optional<VariableSym> resolveVariable(va_t FuncAddr,
                                             int64_t Offset) const override;
  std::optional<VariableSym>
  resolveStackPointerVariable(va_t FuncAddr, int64_t Offset) const override;
  void completeType(const TypeRef &Ty) const override;
  std::optional<TypeSym> resolveType(uint64_t TypeId) const override;
  std::optional<SourceLoc> sourceLocation(va_t Addr) const override;
  std::vector<FunctionSym> allFunctions() const override;
  std::vector<DataObjectSym> allDataObjects() const override;
  std::optional<DataObjectSym> resolveDataObject(va_t Addr) const override;
  std::pair<std::optional<va_t>, std::optional<va_t>>
  dataSymbolsAround(va_t Addr) const override;
  std::optional<va_t> precedingDataSymbol(va_t Addr) const override;
  bool hasInfo() const override;
  bool hasAuthenticatedFunctionSignatures() const override;
  bool hasAuthenticatedObjectExtents() const override;

  std::vector<AuthenticatedRecordParameter>
  resolveAuthenticatedRecordParameters(va_t Addr) const override;

  std::optional<AuthenticatedRecordParameter>
  resolveAuthenticatedRecordReturn(va_t Addr) const override;

  /// Runs \p Use on the open PDB with its type streams to itself, for a
  /// reader that walks them whole, such as the local type import.  An error
  /// when the context keeps no file open, as for a PDB 2.00 file.
  llvm::Error
  withPDBFile(llvm::function_ref<llvm::Error(llvm::pdb::PDBFile &)> Use) const;

  bool hasAuthenticatedImageIdentity() const;
  bool hasAnalysisAuthority() const;
  /// RSDS Phase A never authorizes exact object metadata.  Names-only
  /// S_LOCAL / S_REGREL32 recovery does not change that.  A PDB 2.00 JG
  /// companion that parsed TPI and S_BPREL32_ST without stream malformation
  /// may authorize extents for that image only.
  bool hasExactObjectMetadataPrerequisites() const;

private:
  PDBDebugContext();
  enum class LoadPolicy { MatchingIdentity, Inspect, UserSelected };
  static llvm::Expected<std::unique_ptr<PDBDebugContext>>
  loadImpl(const std::filesystem::path &PdbPath, const BinaryImage &Image,
           const LoadProgress &Progress, LoadPolicy Policy);
  void commitFunctions(std::vector<FunctionSym> Functions, bool Authenticated);
  void commitDebugFacts(std::vector<FunctionSym> Functions,
                        std::map<va_t, std::map<int64_t, VariableSym>> Locals,
                        bool Authenticated, bool Signatures, bool Extents);
  void ensureLocalsForAddress(va_t Addr) const;
  friend llvm::Expected<std::unique_ptr<PDBDebugContext>>
  loadPdb20DebugContext(const std::filesystem::path &PdbPath,
                        const BinaryImage &Image);

  struct Impl;
  std::unique_ptr<Impl> PImpl;
};

} // namespace neverd

#endif // NEVERD_DEBUG_PDBLOADER_H
