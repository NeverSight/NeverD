//===- COFFExceptionPatch.h - Safe PE exception-table rewrite -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFEXCEPTIONPATCH_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFEXCEPTIONPATCH_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace llvm {
class Module;
}

namespace neverd {

struct BinaryImage;
struct CompiledImage;

/// Return an executable personality thunk already validated in \p Image.
/// \p SymbolName may be either the canonical runtime name or the stable
/// `sub_<VA>` alias used for an address-backed lifted body. Language-handler
/// RVAs must name code, whereas an ordinary COFF import resolver commonly
/// returns the non-executable IAT data slot.
std::optional<va_t> findCOFFExceptionPersonalityVA(const BinaryImage &Image,
                                                   llvm::StringRef SymbolName);

/// Facts established before a PE patch is allowed to mutate the output image.
struct COFFExceptionPatchPlan {
  std::vector<va_t> ExceptionFunctionEntries;
  std::vector<va_t> LanguageExceptionFunctionEntries;
};

/// Authenticate the immutable source side of a COFF exception rewrite: every
/// defined Windows EH attachment, its exact named-table row, its
/// RewriteSourceIdentity, and the canonical primary source-image record must
/// form one closed mapping. This check is body-preserving and is safe to run
/// before source preparation externalizes preserved definitions.
llvm::Error
validateCOFFExceptionSourceIdentityClosure(const llvm::Module &Mod,
                                           const BinaryImage &Image);

/// Validate that every Microsoft C++ FuncInfo group touched by
/// \p ReplacedEntries is replaced as one complete source group and has an
/// exact module-level atomic rewrite contract. Groups with no replaced member
/// remain preserved and require no contract.
llvm::Error
validateCOFFCxxGroupReplacementClosure(const llvm::Module &Mod,
                                       const BinaryImage &Image,
                                       llvm::ArrayRef<va_t> ReplacedEntries);

/// Commit the already validated group-contract projection for
/// \p ReplacedEntries. Contracts for wholly preserved groups are removed so
/// the prepared module describes only definitions that will be compiled.
/// Validation completes before the named table is changed.
llvm::Error retainCOFFCxxGroupRewriteContractsForReplacement(
    llvm::Module &Mod, const BinaryImage &Image,
    llvm::ArrayRef<va_t> ReplacedEntries);

/// Validate every lifted Windows EH contract represented in \p Mod.  Each
/// defined attachment must be the canonical current-schema re-encoding of the
/// exact primary source-image record named by RewriteSourceIdentity and must
/// have one pointer-identical module-table row. Records that the current native
/// lowering cannot reproduce fail closed here, before the output file is
/// mutated.
llvm::Expected<COFFExceptionPatchPlan>
planCOFFExceptionPatch(const llvm::Module &Mod, const BinaryImage &Image,
                       Arch TargetArch);

enum class COFFGeneratedLanguageModel : uint8_t {
  SEH,
  CxxFH3,
  CxxFH4,
};

enum class COFFGeneratedLanguageOwnerRole : uint8_t {
  MappedRoot,
  CxxAuxiliary,
};

/// Exact pre-install identity of one generated language-handler runtime row.
/// RuntimeWords binds the unwind program selected by the directory row;
/// HandlerDataRVA and LanguageGroupRVA bind the decoded language payload.
struct COFFGeneratedLanguageOwnerReceipt {
  uint32_t RuntimeFunctionRVA = 0;
  std::array<uint32_t, 3> RuntimeWords{};
  uint8_t RuntimeWordCount = 0;
  uint32_t BeginRVA = 0;
  uint32_t EndRVA = 0;
  uint32_t UnwindRVA = 0;
  uint32_t HandlerRVA = 0;
  uint32_t HandlerDataRVA = 0;
  uint32_t LanguageGroupRVA = 0;
  /// True only when LLVM emitted a fresh machine-frame cookie and installed
  /// `__GSHandlerCheck_EH4` around the FH4 language payload.
  bool GSWrapped = false;
  /// Exact compiler-emitted x64 GS header (`cookie frame offset | 3`).
  uint32_t GSCookieHeader = 0;
  COFFGeneratedLanguageModel Model = COFFGeneratedLanguageModel::SEH;
  COFFGeneratedLanguageOwnerRole Role =
      COFFGeneratedLanguageOwnerRole::MappedRoot;

  friend bool operator==(const COFFGeneratedLanguageOwnerReceipt &,
                         const COFFGeneratedLanguageOwnerReceipt &) = default;
};

/// Digest of one exact generated section slice installed in `.ndtext`.
struct COFFGeneratedSectionReceipt {
  uint32_t RVA = 0;
  uint32_t Size = 0;
  std::array<uint8_t, 32> SHA256{};

  friend bool operator==(const COFFGeneratedSectionReceipt &,
                         const COFFGeneratedSectionReceipt &) = default;
};

enum class COFFGeneratedEHSemanticKind : uint8_t {
  SEHScope = 1,
  CxxCatch = 2,
};

/// Source-issued Windows EH identity joined to one exact compiler-emitted
/// language-table row and its immediate physical container. RecordBytes are
/// captured only after the immutable source graph, LLVM token, generated
/// owner, container, and raw row agree; the final PE gate rereads the same
/// bytes and requires complete physical-table closure. Model distinguishes the
/// incompatible fixed-width FH3 and compressed FH4 wire formats.
struct COFFGeneratedEHSemanticBinding {
  COFFGeneratedEHSemanticKind Kind = COFFGeneratedEHSemanticKind::SEHScope;
  COFFGeneratedLanguageModel Model = COFFGeneratedLanguageModel::SEH;
  uint64_t SourceFunctionVA = 0;
  uint32_t Region = 0;
  uint32_t Clause = 0;
  std::array<uint64_t, 4> SourceDigest{};
  uint32_t GeneratedOwnerRVA = 0;
  uint32_t ContainerRVA = 0;
  uint32_t RecordRVA = 0;
  std::vector<uint8_t> RecordBytes;
  uint32_t BeginRVA = 0;
  uint32_t EndRVA = 0;
  uint32_t HandlerRVA = 0;

  friend bool operator==(const COFFGeneratedEHSemanticBinding &,
                         const COFFGeneratedEHSemanticBinding &) = default;
};

struct COFFExceptionDirectoryUpdate {
  bool Apply = false;
  uint32_t RVA = 0;
  uint32_t Size = 0;
  std::vector<COFFGeneratedLanguageOwnerReceipt> GeneratedLanguageOwners;
  std::vector<COFFGeneratedSectionReceipt> GeneratedSections;
  std::vector<COFFGeneratedEHSemanticBinding> GeneratedEHSemantics;
};

/// Replacement Guard CF / EH continuation table locations.  Load-config
/// pointer fields contain image VAs (not RVAs), while the tables themselves
/// are sorted RVA arrays in the injected image.
struct COFFGuardTableUpdate {
  bool ApplyCF = false;
  uint32_t CFFunctionTableRVA = 0;
  uint64_t CFFunctionCount = 0;
  bool ApplyEHCont = false;
  uint32_t EHContinuationTableRVA = 0;
  uint64_t EHContinuationCount = 0;
};

/// Merge untouched original x64 or Windows ARM RUNTIME_FUNCTION records with
/// the generated `.pdata` records in \p Compiled. The sorted replacement table
/// is appended to Compiled::Bytes, while `.xdata` keeps its codegen-assigned
/// placement.
llvm::Expected<COFFExceptionDirectoryUpdate> prepareCOFFExceptionDirectory(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled, llvm::ArrayRef<va_t> PatchedOriginalEntries,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, Arch TargetArch);

/// Strict rewrite-module overload used when source-group provenance must be
/// resolved to compiler-authenticated output owners.
llvm::Expected<COFFExceptionDirectoryUpdate> prepareCOFFExceptionDirectory(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled, llvm::ArrayRef<va_t> PatchedOriginalEntries,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, Arch TargetArch, const llvm::Module *RewriteModule);

/// Install a previously prepared exception-directory entry in mutable PE
/// headers.  An applied zero-size update clears the directory.
llvm::Error
applyCOFFExceptionDirectoryUpdate(std::vector<uint8_t> &Binary,
                                  const COFFExceptionDirectoryUpdate &Update);

/// Merge original Guard CF/EHCont records with generated function and
/// continuation targets.  Unsupported load-config shapes fail before the
/// caller mutates or writes the PE.
llvm::Expected<COFFGuardTableUpdate> prepareCOFFGuardTables(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, Arch TargetArch,
    bool RequireGeneratedEHContinuations = false);

llvm::Error applyCOFFGuardTableUpdate(std::vector<uint8_t> &Binary,
                                      const BinaryImage &Image,
                                      const COFFGuardTableUpdate &Update);

/// Final in-memory write gate: require LLVM to accept the PE object and rewalk
/// the installed runtime-function, unwind, load-config, Guard CF, and Guard
/// EHCont structures before the caller writes the output file.
llvm::Error validatePatchedCOFFImage(llvm::ArrayRef<uint8_t> Binary,
                                     Arch TargetArch);

llvm::Error validatePatchedCOFFImage(llvm::ArrayRef<uint8_t> Binary,
                                     Arch TargetArch,
                                     bool RequireGeneratedExceptionDirectory);

/// Strict final gate for a prepared replacement. In addition to the ordinary
/// PE walk, require the installed directory, generated section bytes, and
/// language-runtime owners to match the pre-install receipt exactly.
llvm::Error validatePatchedCOFFImage(
    llvm::ArrayRef<uint8_t> Binary, Arch TargetArch,
    bool RequireGeneratedExceptionDirectory,
    const COFFExceptionDirectoryUpdate &ExpectedExceptionDirectory);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_COFF_COFFEXCEPTIONPATCH_H
