//===- BinaryRewriter.h - Common binary rewriting types ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines common types and base classes shared across COFF, ELF, and
/// Mach-O binary rewriting pipelines.  Concrete format-specific
/// implementations (e.g. COFFPatcher, ELFInplaceRewriter) derive from
/// these base abstractions.
///
/// Design follows LLVM conventions:
///   - Format-agnostic data structures live here.
///   - 32/64-bit handling uses template specialization or runtime
///     branching within each format's implementation.
///   - ISA-specific trampoline / relocation logic is parameterised via
///     the Arch enum rather than virtual dispatch.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_BINARYREWRITER_H
#define NEVERD_BACKEND_CODEGEN_BINARYREWRITER_H

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/codegen/RelocResolver.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

class RelocResolver;

// ===--------------------------------------------------------------------===//
// VARef — format-agnostic virtual-address reference descriptor
// ===--------------------------------------------------------------------===//

struct VARef {
  enum Kind : uint8_t { RelBranch, RIPRelative, AbsPtr };
  Kind TheKind;
  uint64_t FromFileoff;
  uint64_t TargetVA;
  uint8_t InsnLen;
  uint8_t FixupOffset;
  uint8_t FixupSize;
};

// ===--------------------------------------------------------------------===//
// InplaceMapping — mapping from an original function to its replacement
// ===--------------------------------------------------------------------===//

struct InplaceMapping {
  std::string Name;
  uint64_t OrigVA = 0;
  uint64_t OrigSize = 0;
  uint64_t NewOffsetInObj = 0;
  uint64_t NewSize = 0;
  int64_t Shift = 0;
};

/// One trampoline that was actually written, retaining the exact object symbol
/// whose generated address it targets.  Format metadata writers use this
/// installed identity instead of inferring a range relationship from numeric
/// addresses alone.
struct PatchedFunctionEntry {
  std::string OwnerSymbol;
  va_t OriginalVA = 0;
  va_t OwnerVA = 0;
  std::string SourceFunction;
};

/// Exact subset of compiled source functions whose original entries can host
/// a target trampoline without crossing another authenticated entry or a code
/// owner boundary. Unsafe definitions are externalized before compilation;
/// PreservedCount is a defensive signal that an unprepared definition reached
/// the post-codegen plan.
struct SourceTrampolinePlan {
  std::vector<llvm::mc_rewrite::RewriteSourceFunctionOwner> Owners;
  std::map<std::string, uint64_t> OriginalVAs;
  size_t PreservedCount = 0;
};

/// Result of preparing a module for an exact source-identity rewrite. A source
/// whose entry cannot safely host a trampoline is converted to an external
/// declaration when another source remains replaceable, so generated callers
/// bind to the authenticated original body and no duplicate unwind/code copy
/// is emitted. If no source is replaceable, the whole patch is a no-op and the
/// clone remains untouched.
struct SourceFunctionPreparation {
  bool HasExactSources = false;
  std::map<std::string, uint64_t> ReplaceableOriginalVAs;
  std::map<std::string, uint64_t> PreservedOriginalVAs;

  bool isExactNoOp() const {
    return HasExactSources && ReplaceableOriginalVAs.empty();
  }
};

// ===--------------------------------------------------------------------===//
// TextLayout — describes the location of the main code section
// ===--------------------------------------------------------------------===//

struct TextLayout {
  uint64_t SectionFileoff = 0;
  uint64_t SectionVA = 0;
  uint64_t SectionSize = 0;
};

// ===--------------------------------------------------------------------===//
// PatchOptionsBase — common fields for all format-specific PatchOptions
// ===--------------------------------------------------------------------===//

struct PatchOptionsBase {
  std::string SectionName = kNdTextSection.str();
};

// ===--------------------------------------------------------------------===//
// PatchLayoutBase — common fields for all format-specific PatchLayouts
// ===--------------------------------------------------------------------===//

struct PatchLayoutBase {
  bool Is64 = true;
  uint64_t TextVA = 0;
  uint64_t TextSize = 0;
  uint64_t TextFileOff = 0;
  uint64_t TextFileSize = 0;
  uint64_t MaxVA = 0;
  uint64_t MaxFileOff = 0;
};

// ===--------------------------------------------------------------------===//
// CompiledImage — multi-section rewrite-backend output ready for placement
// ===--------------------------------------------------------------------===//

/// One symbolic fixup owned by a CompiledSection.  Symbol strings are copied
/// while LLVM's MC callback is live; no numeric address is used as a substitute
/// for symbol identity.  SubtractSymbol is non-empty for an `Add - Sub`
/// expression that has not been reduced to a single symbol.  These are the
/// direct MCValue terms; consumers must fail closed when an expected identity
/// was hidden by a variable-symbol alias or otherwise folded away.
struct CompiledFixupReference {
  uint64_t Offset = 0; ///< Byte offset within the owning section.
  /// Opaque compiler-authenticated CFI fragment identity.  This is nonzero
  /// only when Symbol is the exact private begin label of a retained function
  /// range; consumers must not reconstruct it from an address or name.
  uint64_t FunctionRangeId = 0;
  unsigned Kind = 0;
  std::string Symbol;
  std::string SubtractSymbol;
  int64_t Addend = 0;
  uint32_t Specifier = 0;
  bool IsPCRel = false;
  bool IsResolved = false;
  unsigned BitWidth = 0;
};

/// One native section produced for a CompiledImage.  Allocated in-image
/// sections name a slice of CompiledImage::Bytes; externally placed sections
/// and non-allocated linker metadata retain their bytes in ExternalBytes.
struct CompiledSection {
  std::string Name;
  uint64_t Offset = 0;
  uint64_t VA = 0;
  uint64_t Size = 0;
  uint64_t Alignment = 1;
  llvm::mc_rewrite::RewriteSectionKind Kind =
      llvm::mc_rewrite::RewriteSectionKind::Other;
  bool IsAllocated = true;
  std::vector<llvm::mc_rewrite::RewriteSymbolIndexReference>
      SymbolIndexReferences;
  std::vector<CompiledFixupReference> FixupReferences;
  /// True when the section bytes live in CompiledImage::Bytes at Offset.
  bool IsInImage = true;
  /// Exact bytes for a section that does not live in the contiguous patch
  /// image: either non-allocated linker metadata or an allocated section with
  /// a format-owned external placement.
  std::vector<uint8_t> ExternalBytes;
};

using CompiledFunctionRange = llvm::mc_rewrite::RewriteFunctionRange;

/// Result of compiling a module to a single placement image.  Allocated
/// in-image sections are laid out contiguously from a chosen base VA; every
/// emitted section, including non-allocated linker metadata, retains its exact
/// logical identity and contents in Sections.
struct CompiledImage {
  std::vector<uint8_t> Bytes;                  ///< Combined image (text first).
  uint64_t BaseVA = 0;                         ///< VA of Bytes[0].
  Arch TargetArch = Arch::Unknown;             ///< Exact code-generation ISA.
  BinaryFormat Format = BinaryFormat::Unknown; ///< Exact object convention.
  std::string TargetTriple;                    ///< Exact backend ABI target.
  InstructionMode TargetMode = InstructionMode::Default;
  uint8_t PointerWidth = 0; ///< Target pointer bytes.
  llvm::endianness ByteOrder = llvm::endianness::little;
  std::vector<CompiledSection> Sections;       ///< Exact logical placements.
  std::map<std::string, uint64_t> SymbolAddrs; ///< Defined symbol → final VA.
  /// Provenance-only owner identities, including private compiler symbols.
  std::map<std::string, uint64_t> FunctionOwnerAddrs;
  /// Exact compiler-authenticated CFI fragment ownership.
  std::vector<CompiledFunctionRange> FunctionRanges;
  /// Exact rewrite-only source IR definition -> MC owner associations.
  std::vector<llvm::mc_rewrite::RewriteSourceFunctionOwner>
      SourceFunctionOwners;
  /// Exact lifted source-function identity -> original image entry.  These
  /// values come only from the validated IR attachment emitted by the lifter;
  /// patchers join them to SourceFunctionOwners by exact source identity.
  std::map<std::string, uint64_t> SourceFunctionOriginalVAs;
  bool FunctionRangesValid = true;
  std::vector<std::string> Unresolved; ///< Symbols left unresolved.
  bool Success = false;
};

using PatchSymbolResolver =
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef, uint32_t)>;
using ContextualPatchSymbolResolver =
    llvm::function_ref<std::optional<uint64_t>(
        const llvm::mc_rewrite::RewriteSymbolResolveRequest &)>;

/// Compile \p Mod to a placement-ready image rooted at \p BaseVA.
///
/// The rewrite backend may emit more than one output section (e.g. an absolute
/// 8-byte blockaddress / jump table in `__const`/`.rodata` referenced
/// PC-relative from `.text`). A single-section model would drop those, so this
/// performs a probe compile on a clone to learn each section's size, then lays
/// the sections out contiguously from \p BaseVA (text first, each section
/// honoring at least its native alignment) and recompiles with per-section VAs
/// until the layout stabilizes. The converged allocated sections are assembled
/// into one blob; non-allocated metadata remains external to that blob. Format
/// patchers use Sections to preserve unwind/data identity and choose compatible
/// physical permissions.
///
/// The legacy \p ResolveFn maps an external symbol/specifier pair to a target
/// VA (for example, a PLT/IAT/stub, an export, or `__nd_data_*`). Returns
/// CompiledImage::Success==false on failure.
CompiledImage compileImageForPatch(llvm::Module &Mod, Arch TargetArch,
                                   BinaryFormat Fmt, uint64_t BaseVA,
                                   PatchSymbolResolver ResolveFn,
                                   uint64_t ImageBaseVA = 0);

/// Context-aware variant.  The native MC backend supplies the exact fixup,
/// section, specifier, and add/subtract role so format patchers can keep code,
/// data, import-slot, TLS, and authenticated-pointer address spaces distinct.
CompiledImage compileImageForPatch(llvm::Module &Mod, Arch TargetArch,
                                   BinaryFormat Fmt, uint64_t BaseVA,
                                   ContextualPatchSymbolResolver ResolveFn,
                                   uint64_t ImageBaseVA = 0);

/// Exact-platform variant.  \p TargetTriple is propagated through the probe
/// compile and every layout-convergence compile; callers must authenticate it
/// against the input container before invoking this overload.
CompiledImage compileImageForPatch(llvm::Module &Mod, Arch TargetArch,
                                   BinaryFormat Fmt, uint64_t BaseVA,
                                   PatchSymbolResolver ResolveFn,
                                   uint64_t ImageBaseVA,
                                   llvm::StringRef TargetTriple);

CompiledImage compileImageForPatch(llvm::Module &Mod, Arch TargetArch,
                                   BinaryFormat Fmt, uint64_t BaseVA,
                                   ContextualPatchSymbolResolver ResolveFn,
                                   uint64_t ImageBaseVA,
                                   llvm::StringRef TargetTriple);

/// Compile like compileImageForPatch, but allow selected allocated sections to
/// be assigned format-owned VAs outside the contiguous patch image.  Such
/// sections have IsInImage=false and carry their bytes in ExternalBytes.
CompiledImage compileImageForPatchWithFixedSectionVAs(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    PatchSymbolResolver ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA = 0);

CompiledImage compileImageForPatchWithFixedSectionVAs(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    ContextualPatchSymbolResolver ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA = 0);

/// Exact-platform variant of compileImageForPatchWithFixedSectionVAs.
CompiledImage compileImageForPatchWithFixedSectionVAs(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    PatchSymbolResolver ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA, llvm::StringRef TargetTriple);

CompiledImage compileImageForPatchWithFixedSectionVAs(
    llvm::Module &Mod, Arch TargetArch, BinaryFormat Fmt, uint64_t BaseVA,
    ContextualPatchSymbolResolver ResolveFn,
    llvm::function_ref<std::optional<uint64_t>(llvm::StringRef)>
        FixedSectionVAFn,
    uint64_t ImageBaseVA, llvm::StringRef TargetTriple);

// ===--------------------------------------------------------------------===//
// BinaryPatcher — base class for new-section patching
// ===--------------------------------------------------------------------===//

/// Fill a buffer with NOP instructions appropriate for the target ISA.
void padWithNops(uint8_t *Dst, uint64_t Len, Arch TargetArch,
                 InstructionMode Mode = InstructionMode::Default);

/// Return the default single-byte fill/trap value for the target ISA.
/// x86/x64 = INT3 (0xCC), ARM/AArch64 = UDF #0 (0x00).
inline uint8_t getDefaultFillByte(Arch A) {
  return (A == Arch::X64 || A == Arch::X86) ? 0xCC : 0x00;
}

/// Serialize an analyzed export address according to the target ABI only when
/// the loaded image proves that the export points into executable code.
inline uint64_t serializeExportAddress(const BinaryImage &Image, uint64_t VA) {
  return Image.isCodeAddress(VA)
             ? serializeCodePointer(VA, Image.Arch, Image.Mode)
             : VA;
}

/// Classify a synthetic `__nd_data_<VA>` occurrence as a function identity
/// only when the loaded image supplies occurrence-level evidence. A typed
/// function symbol is sufficient even when CodeRefTargets is empty; a generic
/// executable address additionally needs an address-taken code reference.
inline bool isAuthenticatedCodeDataSymbol(const BinaryImage &Image,
                                          uint64_t VA) {
  return Image.hasFunctionSymbolAt(VA) ||
         (Image.isCodeAddress(VA) && Image.CodeRefTargets.count(VA) != 0);
}

/// Serialize an authenticated synthetic data-symbol address according to the
/// target ABI. Sharing this policy prevents the in-place, ELF, and COFF
/// resolvers from disagreeing about ARM/Thumb bit 0.
inline uint64_t serializeImageDataSymbolAddress(const BinaryImage &Image,
                                                uint64_t VA) {
  return isAuthenticatedCodeDataSymbol(Image, VA)
             ? serializeCodePointer(VA, Image.Arch, Image.Mode)
             : VA;
}

class BinaryPatcher {
public:
  virtual ~BinaryPatcher() = default;

  /// Factory: create a patcher for the given binary format
  /// (cf. llvm::object::ObjectFile::createObjectFile).
  static std::unique_ptr<BinaryPatcher> create(BinaryFormat Format);

  /// Compile Module directly to fixed-up image bytes and patch (new-section
  /// mode). Default returns unsuccessful; override in format-specific
  /// subclasses.
  virtual PatchResult patch(const std::filesystem::path &InputPath,
                            const std::filesystem::path &OutputPath,
                            llvm::Module &Mod, Arch TargetArch) {
    return PatchResult{};
  }

  /// The VA at which appendExecSegment() will place its \c Code blob, computed
  /// from the current layout *without modifying* \p Binary.  Lets callers
  /// compile code to its final VA before inserting it.  \p TargetArch selects
  /// the page size (it must match the appendExecSegment() call).  Returns 0 if
  /// the format does not support segment append.
  virtual uint64_t plannedExecSegmentVA(const std::vector<uint8_t> &Binary,
                                        Arch TargetArch) {
    return 0;
  }

  /// Append \p Code as a new executable (RX) segment/section named \p SegName,
  /// rewriting headers as needed.  Returns the VA where \p Code was placed
  /// (== plannedExecSegmentVA for the same buffer state), or 0 on failure.
  /// Shared by section-mode patching and the inplace grower-relocation path,
  /// so both go through one tested code path per format.
  virtual uint64_t appendExecSegment(std::vector<uint8_t> &Binary,
                                     llvm::ArrayRef<uint8_t> Code,
                                     llvm::StringRef SegName, Arch TargetArch) {
    return 0;
  }

  /// Optionally provide export symbols for trampoline resolution.
  /// Called before patch() when the caller has an already-parsed symbol
  /// table (avoids redundant re-parsing inside the patcher).
  void setExports(const std::vector<Export> *Exports) {
    CachedExports = Exports;
  }
  void setImports(const std::vector<Import> *Imports) {
    CachedImports = Imports;
  }

  void setImageContext(const BinaryImage *Image) {
    CachedImage = Image;
    CachedExports = Image ? &Image->Exports : nullptr;
    CachedImports = Image ? &Image->Imports : nullptr;
    CachedSymbols = Image ? &Image->Symbols : nullptr;
    CachedCodeRanges = Image ? &Image->KnownCodeRanges : nullptr;
    CachedMode = Image ? Image->Mode : InstructionMode::Default;
  }

  /// Force the name of the original code section used to locate the text region
  /// for trampoline placement.  When set (non-empty), the COFF and Mach-O
  /// section-mode patchers try this name before their format default, so a
  /// binary whose code section was renamed by a packer/protector (VMProtect
  /// ".vmp0", UPX "UPX1", Themida, randomised names) still gets trampolines.
  /// ELF section-mode patching is segment-based (picks the executable PT_LOAD)
  /// and is already name-agnostic, so it ignores this hint.
  void setTextSectionOverride(llvm::StringRef Name) {
    TextSectionOverride = Name.str();
  }

  /// Resolve original VAs for codegen functions using name-based lookup,
  /// then write trampolines at the original locations.  Returns the number
  /// of trampolines written.  \p SymbolAddrs maps symbol name → final VA.
  static size_t installTrampolines(
      std::vector<uint8_t> &Binary,
      const std::map<std::string, uint64_t> &SymbolAddrs, uint64_t OrigTextVA,
      uint64_t OrigTextSize, uint64_t OrigTextFileOff, uint64_t ImageBase,
      Arch TargetArch, InstructionMode Mode = InstructionMode::Default,
      const std::vector<Symbol> *Symbols = nullptr,
      const std::vector<std::pair<va_t, va_t>> *KnownRanges = nullptr,
      const std::vector<Export> *Exports = nullptr,
      std::vector<va_t> *PatchedOriginalEntries = nullptr,
      std::vector<std::pair<va_t, va_t>> *PatchedEntryMappings = nullptr,
      std::vector<PatchedFunctionEntry> *PatchedFunctions = nullptr);

  /// Provenance-aware variant used by binary rewrite output.  Original entries
  /// come only from the validated source-identity map; the installed target
  /// retains the exact private or public MC owner identity.  Source functions
  /// absent from the map are generated helpers and receive no entry trampoline.
  static size_t installTrampolines(
      std::vector<uint8_t> &Binary,
      const std::map<std::string, uint64_t> &SymbolAddrs, uint64_t OrigTextVA,
      uint64_t OrigTextSize, uint64_t OrigTextFileOff, uint64_t ImageBase,
      Arch TargetArch, InstructionMode Mode, const std::vector<Symbol> *Symbols,
      const std::vector<std::pair<va_t, va_t>> *KnownRanges,
      const std::vector<Export> *Exports,
      std::vector<va_t> *PatchedOriginalEntries,
      std::vector<std::pair<va_t, va_t>> *PatchedEntryMappings,
      std::vector<PatchedFunctionEntry> *PatchedFunctions,
      llvm::ArrayRef<llvm::mc_rewrite::RewriteSourceFunctionOwner>
          SourceFunctionOwners,
      const std::map<std::string, uint64_t> &SourceFunctionOriginalVAs = {});

  /// Common file I/O skeleton shared by all patchers: reads the input
  /// binary, calls \p PatchFn to modify the in-memory buffer and fill
  /// \c PatchResult fields, then writes the output.
  static PatchResult readPatchWrite(
      const std::filesystem::path &InputPath,
      const std::filesystem::path &OutputPath, bool SetExecPerm,
      llvm::StringRef DebugTag,
      llvm::unique_function<bool(std::vector<uint8_t> &, PatchResult &)>
          PatchFn);

protected:
  static std::vector<Export>
  authenticatedFunctionExports(const BinaryImage *Image);
  static bool validateSourceFunctionPatchPlan(const CompiledImage &Compiled,
                                              const BinaryImage *Image,
                                              std::string &Detail);
  static bool
  prepareSourceFunctionsForPatch(llvm::Module &Module, const BinaryImage *Image,
                                 SourceFunctionPreparation &Preparation,
                                 std::string &Detail);
  static SourceTrampolinePlan
  makeSourceTrampolinePlan(const CompiledImage &Compiled,
                           const BinaryImage *Image);
  static bool validatePatchedSourceTrampolineClosure(
      const SourceTrampolinePlan &Plan,
      llvm::ArrayRef<PatchedFunctionEntry> PatchedFunctions,
      size_t TrampolineCount, std::string &Detail);

  const BinaryImage *CachedImage = nullptr;
  const std::vector<Export> *CachedExports = nullptr;
  const std::vector<Import> *CachedImports = nullptr;
  const std::vector<Symbol> *CachedSymbols = nullptr;
  const std::vector<std::pair<va_t, va_t>> *CachedCodeRanges = nullptr;
  InstructionMode CachedMode = InstructionMode::Default;
  /// User-forced original code-section name (empty = use format default).
  std::string TextSectionOverride;

  static bool writeTrampoline(std::vector<uint8_t> &Data, uint64_t FromOff,
                              uint64_t TargetVA, uint64_t FromVA,
                              Arch TargetArch, InstructionMode Mode,
                              uint64_t MaxOverwriteBytes);
};

// ===--------------------------------------------------------------------===//
// InplaceRewriter — base class for in-place binary rewriting
// ===--------------------------------------------------------------------===//

class InplaceRewriter {
public:
  virtual ~InplaceRewriter() = default;

  /// Factory: create an inplace rewriter for the given binary format.
  static std::unique_ptr<InplaceRewriter> create(BinaryFormat Format);

  /// In-place rewrite: per-function compilation with correct final VAs.
  virtual PatchResult rewrite(const std::filesystem::path &InputPath,
                              const std::filesystem::path &OutputPath,
                              llvm::Module &Mod, const BinaryImage &Image,
                              Arch TargetArch);

  /// Force the name of the original code section to harden.  When set
  /// (non-empty), parseTextSection() tries this name before the format default
  /// and before the flag-based fallback, so the user can point the rewriter at
  /// a code section renamed by a packer/protector (VMProtect ".vmp0", UPX
  /// "UPX1", Themida, randomised names).
  void setTextSectionOverride(llvm::StringRef Name) {
    TextSectionOverride = Name.str();
  }

protected:
  /// User-forced original code-section name (empty = use format default).
  std::string TextSectionOverride;

  struct RewriteState {
    std::vector<uint8_t> Binary;
    TextLayout TL;
    std::vector<uint8_t> ObjText;
    std::vector<InplaceMapping> Mappings;
    size_t TrampolineCount = 0;
  };

  static PatchResult writeResult(const std::filesystem::path &OutputPath,
                                 const RewriteState &State, bool SetExecPerm);

  virtual llvm::StringRef getTextSectionName() const {
    return section_names::elf::Text;
  }

  /// The object format this rewriter targets. Distinct from
  /// getTextSectionName() because ELF and COFF share the ".text" name — the
  /// format cannot be recovered from the section name alone.
  virtual BinaryFormat getBinaryFormat() const = 0;

  virtual bool parseTextSection(const std::vector<uint8_t> &Binary,
                                const BinaryImage &Image, TextLayout &TL);

  virtual std::unique_ptr<RelocResolver> createRelocResolver() const = 0;
  virtual std::unique_ptr<BinaryPatcher> createBinaryPatcher() const {
    return BinaryPatcher::create(getBinaryFormat());
  }
  virtual bool needsExecPermission() const { return true; }
};

// ===--------------------------------------------------------------------===//
// findObjectTextSection — helper for InplaceRewriter subclasses
// ===--------------------------------------------------------------------===//

/// Find a named executable section via llvm::object::ObjectFile (COFF/ELF).
/// For COFF, \p SectionVA includes the image base.
bool findObjectTextSection(const std::vector<uint8_t> &Binary,
                           llvm::StringRef SectionName, TextLayout &TL);

inline bool isValidFunctionSymbol(const BinaryImage &Image, const Symbol &Sym) {
  if (!Sym.IsFunc)
    return false;
  if (Sym.Addr != 0)
    return true;
  const Section *Sec = Image.IsRelocatable ? Image.getSectionFor(0) : nullptr;
  return Sec && Sec->isExecutable();
}

/// Build the InternalFuncs map common to all three Inplace rewriters.
/// Populates both the original name and the underscore-aliased name
/// for MachO compatibility when \p AddUnderscoreAlias is true.
inline void buildInternalFuncMap(const std::vector<InplaceMapping> &Mappings,
                                 const BinaryImage &Image,
                                 std::map<std::string, uint64_t> &Out,
                                 bool AddUnderscoreAlias = false) {
  auto AddWithAlias = [&](const std::string &Name, uint64_t Addr) {
    Out[Name] = Addr;
    if (!AddUnderscoreAlias)
      return;
    if (!Name.empty() && Name[0] == '_')
      Out[Name.substr(1)] = Addr;
    else
      Out["_" + Name] = Addr;
  };
  for (const auto &M : Mappings)
    AddWithAlias(M.Name, M.OrigVA);
  for (const auto &Sym : Image.Symbols)
    if (isValidFunctionSymbol(Image, Sym))
      AddWithAlias(Sym.Name, Sym.Addr);
}

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_BINARYREWRITER_H
