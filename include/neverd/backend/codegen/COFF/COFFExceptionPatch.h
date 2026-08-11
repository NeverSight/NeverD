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
/// Language-handler RVAs must name code, whereas an ordinary COFF import
/// resolver commonly returns the non-executable IAT data slot.
std::optional<va_t> findCOFFExceptionPersonalityVA(const BinaryImage &Image,
                                                   llvm::StringRef SymbolName);

/// Facts established before a PE patch is allowed to mutate the output image.
struct COFFExceptionPatchPlan {
  std::vector<va_t> ExceptionFunctionEntries;
  std::vector<va_t> LanguageExceptionFunctionEntries;
};

/// Validate every lifted Windows EH contract represented in \p Mod.  Records
/// that the current native lowering cannot reproduce fail closed here, before
/// the output file is written.
llvm::Expected<COFFExceptionPatchPlan>
planCOFFExceptionPatch(const llvm::Module &Mod, const BinaryImage &Image,
                       Arch TargetArch);

struct COFFExceptionDirectoryUpdate {
  bool Apply = false;
  uint32_t RVA = 0;
  uint32_t Size = 0;
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

/// Merge untouched original x64 RUNTIME_FUNCTION records with the generated
/// `.pdata` records in \p Compiled.  The sorted replacement table is appended
/// to Compiled::Bytes, while `.xdata` keeps its codegen-assigned placement.
llvm::Expected<COFFExceptionDirectoryUpdate> prepareCOFFExceptionDirectory(
    llvm::ArrayRef<uint8_t> OriginalBinary, const BinaryImage &Image,
    CompiledImage &Compiled, llvm::ArrayRef<va_t> PatchedOriginalEntries,
    llvm::ArrayRef<std::pair<va_t, va_t>> PatchedEntryMappings,
    uint64_t NewSectionVA, Arch TargetArch);

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

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_COFF_COFFEXCEPTIONPATCH_H
