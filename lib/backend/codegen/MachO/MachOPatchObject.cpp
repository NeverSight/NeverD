//===- MachOPatchObject.cpp - Native Mach-O replacement installation -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachOArm64eABI.h"
#include "MachODependencyState.h"
#include "MachOExportLayout.h"
#include "MachOLiftedObject.h"
#include "MachOPublishedData.h"
#include "MachOSourceDependencies.h"
#include "MachOTLSRuntime.h"

#include "neverd/analysis/AArch64UnwindRecovery.h"
#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/MachO/MachOAuthenticatedFixups.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/MachO/MachORegisterResults.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/codegen/PatchStorage.h"
#include "neverd/backend/codegen/SourcePatchPreparation.h"
#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/FunctionBody.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/MachO/MachOPlatform.h"
#include "neverd/loader/MachO/MachORelocations.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/AArch64Literal.h"
#include "neverd/support/BranchEncoding.h"
#include "neverd/support/DwarfEH.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <set>

namespace neverd {
namespace {
using Kind = llvm::mc_rewrite::RewriteSectionKind;
using namespace llvm::MachO;
namespace arm64e = macho_loader::arm64e;
#define MACHO_OBJECT_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define MACHO_OBJECT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "MachOPatchObject.def"
using macho_arm64e_abi::PAuthEncoding;

llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 kObjectErrorPrefix + Message);
}
std::string canonical(llvm::StringRef Name) {
  Name.consume_front(macho_arm64e_abi::kSymbolPrefix);
  return Name.str();
}
struct Symbol {
  std::string Name;
  llvm::StringRef LinkerName; // Borrows the input object's string table.
  uint32_t Section = 0;
  uint64_t Offset = 0, Size = 0;
  bool Function = false, Thumb = false, External = false;
};
struct Relocation {
  size_t Section;
  uint64_t Offset;
  unsigned Type, Width, SymbolIndex;
  bool External, PCRel, Scattered;
  uint64_t ScatteredVA = 0;
  std::optional<uint64_t> SubtractVA;
  std::optional<unsigned> SubtractSymbol;
  unsigned HalfLength = 0, OtherHalf = 0;
  int64_t InstructionAddend = 0;
};
struct NativeMachO {
  CompiledImage Image;
  std::map<uint32_t, size_t> Sections;
  std::map<uint32_t, uint64_t> OldSectionVAs;
  std::map<uint32_t, Symbol> Symbols;
  std::map<std::string, uint32_t> Names;
  std::vector<Relocation> Relocations;
  std::vector<std::pair<size_t, MachODataInCodeRange>> Embedded;
  std::vector<NativeMachOCompactRange> Compact;
  std::vector<DwarfEHFrameRecord> OldFDEs;
  // Check the source signing rows against the relocated code before publishing.
  llvm::DenseMap<va_t, AArch64ClassicReturnCFI> ClassicReturnFrames;
  std::optional<size_t> EH, CompactSection;
  va_t EHSectionVA = 0;
  bool SjLj = false;
  bool CompactOnly = false;
  std::map<unsigned, SourcePatchImport> Imports;
  std::map<uint64_t, unsigned> IndirectSymbols;
  std::map<uint64_t, uint64_t> IndirectSlots;
  struct Thunk {
    size_t Section;
    uint64_t Slot, FrameOffset, FrameSize;
  };
  std::map<unsigned, Thunk> Thunks;
  struct GOTSlot {
    std::optional<size_t> Section;
    uint64_t Address = 0;
  };
  std::map<std::pair<unsigned, bool>, GOTSlot> GOT;
  struct TLVReference {
    va_t Address;
    bool Direct;
  };
  std::map<unsigned, TLVReference> TLV;
  std::optional<size_t> Descriptors, TemplatePrefix;
  std::set<size_t> TemplateSections;
  MachORuntimeSections Runtime;
  std::optional<size_t> InitializerPointers, InitializerOffsets;
  std::map<std::string, uint64_t> TLSExports;
  std::vector<size_t> ThreadInitializers;
  struct ThreadRuntime {
    size_t Code, Config, Anchor, Guard;
    uint64_t DeltaOffset, CallbackCount;
    struct Field {
      size_t Section;
      uint64_t Offset;
      CompiledPointerAuthentication Schema;
    };
    std::vector<Field> SignedFields;
    std::vector<va_t> OldInitializers;
    std::vector<va_t> OldDescriptors;
  };
  std::optional<ThreadRuntime> PerThread;
  std::vector<MachOAuthenticatedFixup>
      TemplateFixups; // Template-relative until placed.
  struct AuthenticatedCallStub {
    size_t Code, Slot;
  };
  std::map<unsigned, AuthenticatedCallStub> AuthenticatedCallStubs;
  // Section index to the sole public weak definition that owns its storage.
  std::map<size_t, unsigned> WeakData;

  // C symbols are indexed first. A bare assembly symbol can keep its old
  // shorthand unless a distinct C symbol already owns that source name.
  std::string sourceName(llvm::StringRef LinkerName) const {
    llvm::StringRef Name = LinkerName;
    const bool Prefixed = Name.consume_front(macho_arm64e_abi::kSymbolPrefix);
    if (Name.starts_with(macho_arm64e_abi::kVerbatimPrefix) ||
        (Prefixed && Name.empty()))
      return (macho_arm64e_abi::kVerbatimPrefix + LinkerName).str();
    std::string Key = Name.str();
    if (!Prefixed) {
      const auto I = Names.find(Key);
      if (I != Names.end() && Symbols.at(I->second).LinkerName != LinkerName)
        return (macho_arm64e_abi::kVerbatimPrefix + LinkerName).str();
    }
    return Key;
  }
};

// New data mappings and dyld runtime sections require segment commands.
// Reserve them before assigning code, unwind and fixup addresses.
uint64_t installationCommandBytes(const NativeMachO &Object,
                                  const BinaryImage &Original) {
  const bool HasTLS = Object.Descriptors || !Object.ThreadInitializers.empty();
  const unsigned RuntimeSections =
      (HasTLS ? kTLVSectionCount : 0) +
      (Object.InitializerOffsets ? kInitializerSectionCount : 0);
  // Preparing per-thread signing can add a sealed resolver configuration and
  // ordinary mutable storage before the native descriptors and template.
  bool Relro = HasTLS, Mutable = HasTLS;
  for (const auto &S : Object.Image.Sections) {
    if (!S.IsAllocated || !S.IsInImage || !S.Size ||
        (S.Kind != Kind::WritableData && S.Kind != Kind::UninitializedData))
      continue;
    Relro |= S.ReadOnlyAfterFixups;
    Mutable |= !S.ReadOnlyAfterFixups;
  }
  // A nonempty source DICE command survives segment installation. An object
  // introducing the first data island also needs room for the new command.
  const bool NewDICE =
      !Object.Embedded.empty() && Original.MachODataInCode.empty();
  const auto SegmentBytes =
      getMachOSegmentCmdSize(Object.Image.PointerWidth == sizeof(uint64_t));
  if (RuntimeSections || Relro || Mutable) {
    const unsigned Pools = unsigned(Relro) + unsigned(Mutable);
    const auto Segments =
        Original.isArm64e() ? 1 + Pools : kRuntimeSegmentCount;
    const auto Sections = RuntimeSections + (Original.isArm64e() ? Pools : 0);
    return SegmentBytes * Segments + sizeof(section_64) * Sections +
           sizeof(linkedit_data_command) * (1 + unsigned(NewDICE));
  }
  if (!NewDICE && Original.isArm64e() &&
      Original.findSegmentByName(kDefaultNdTextSegment)) {
    const auto *Linkedit =
        Original.findSegmentByName(section_names::macho::LinkeditSeg);
    if (Linkedit && Linkedit->VA) {
      const auto *Tail = Original.getSegmentFor(Linkedit->VA - 1);
      if (!Tail || !Tail->isExecutable())
        return SegmentBytes + sizeof(linkedit_data_command);
    }
  }
  return NewDICE ? SegmentBytes + sizeof(linkedit_data_command) * 2 : 0;
}

// Count both possible sources. A frame described by both tables can reserve
// two entries. Final installation validates the relocated encoding and LSDA.
MachOUnwindFunctionCounts unwindFunctionCounts(const NativeMachO &Object) {
  uint64_t LSDAs = llvm::count_if(
      Object.OldFDEs, [](const auto &Frame) { return Frame.LSDA.has_value(); });
  if (Object.CompactSection) {
    const auto &Data =
        Object.Image.Sections[*Object.CompactSection].ExternalBytes;
    const auto Ptr = Object.Image.PointerWidth;
    const auto Stride = Ptr * kCompactPointers + kCompactFixedBytes;
    for (size_t Offset = 0; Offset + Stride <= Data.size(); Offset += Stride) {
      const auto Encoding =
          readLE<uint32_t>(Data.data() + Offset + Ptr + sizeof(uint32_t));
      LSDAs += bool(Encoding & macho_unwind::kHasLSDA);
    }
  }
  return {Object.Symbols.size() +
              unsigned(Object.Descriptors.has_value() ||
                       !Object.ThreadInitializers.empty()) +
              Object.AuthenticatedCallStubs.size(),
          LSDAs};
}

// A linked arm64e input can have only compact records while the replacement
// compiler emits an indexed DWARF function. Reserve its real FDE registry
// before any section addresses or instruction displacements are finalized.
uint64_t requiredNativeEHBytes(const NativeMachO &Object,
                               const std::vector<uint8_t> &Binary) {
  if (!Object.EH)
    return 0;
  const auto Size = Object.Image.Sections[*Object.EH].Size;
  if (!Object.Thunks.empty() || findMachOEHFrameRegion(Binary))
    return Size;
  if (Object.Image.TargetArch != Arch::AArch64 || !Object.CompactSection)
    return 0;
  const auto &Data =
      Object.Image.Sections[*Object.CompactSection].ExternalBytes;
  const auto Width = Object.Image.PointerWidth;
  const auto Stride = kCompactPointers * Width + kCompactFixedBytes;
  for (size_t Off = 0; Off + Stride <= Data.size(); Off += Stride) {
    auto Encoding =
        readLE<uint32_t>(Data.data() + Off + Width + sizeof(uint32_t));
    if ((Encoding & macho_unwind::kModeMask) == macho_unwind::kARM64ModeDwarf)
      return Size;
  }
  return 0;
}

bool isTLVLoad(unsigned Type) {
  return Type == ARM64_RELOC_TLVP_LOAD_PAGE21 ||
         Type == ARM64_RELOC_TLVP_LOAD_PAGEOFF12;
}

bool isGOTLoad(unsigned Type) {
  return Type == ARM64_RELOC_GOT_LOAD_PAGE21 ||
         Type == ARM64_RELOC_GOT_LOAD_PAGEOFF12;
}

bool isCompactPersonality(const NativeMachO &Object, const Relocation &R) {
  const unsigned Ptr = Object.Image.PointerWidth;
  return Object.CompactSection == R.Section &&
         R.Offset % (Ptr * kCompactPointers + kCompactFixedBytes) ==
             Ptr + kCompactFixedBytes;
}

llvm::Error prepareAuthenticatedCallStubs(NativeMachO &Object,
                                          std::set<unsigned> Functions);

// The arm64e unwind personality ABI uses an IA/address-diversified GOT slot;
// LSDA type information uses an ordinary data pointer. Keep these identities
// distinct even when a symbol appears in both roles.
llvm::Error prepareArm64eGOT(NativeMachO &Object, const BinaryImage &Original,
                             llvm::ArrayRef<SourcePatchDependency> Dependencies,
                             llvm::ArrayRef<SourcePatchBinding> Bindings = {}) {
  std::set<std::string> Declared;
  for (const auto &D : Dependencies)
    Declared.insert(D.Symbol);
  for (const auto &B : Bindings)
    Declared.insert(B.Symbol);
  const auto UsesGOT = [&](const Relocation &R) {
    return isCompactPersonality(Object, R) ||
           R.Type == ARM64_RELOC_POINTER_TO_GOT || isGOTLoad(R.Type) ||
           isTLVLoad(R.Type);
  };
  const auto IsImportBranch = [&](const Relocation &R) {
    if (!R.External || R.Type != ARM64_RELOC_BRANCH26)
      return false;
    const auto I = Object.Symbols.find(R.SymbolIndex);
    return I != Object.Symbols.end() && !I->second.Section &&
           !Declared.contains(I->second.Name) &&
           !Object.AuthenticatedCallStubs.contains(R.SymbolIndex);
  };
  if (Object.WeakData.empty() &&
      llvm::none_of(Object.Relocations, [&](const auto &R) {
        return UsesGOT(R) || IsImportBranch(R);
      }))
    return llvm::Error::success();
  MachOChainedFixups Chains;
  auto State = readMachODependencyState(Original.Raw, &Chains);
  if (!State)
    return State.takeError();
  const auto Providers =
      Chains.uniqueImportProviders(Object.Image.MachOImportWeakness);
  const auto MatchesProvider = [&](const std::string &Name, const auto &B) {
    auto It = Providers.find(Name);
    return It != Providers.end() && B.LibraryOrdinal &&
           It->second.Ordinal == B.LibraryOrdinal &&
           It->second.bindingStrength(Name, Object.Image.MachOImportWeakness) ==
               B.WeakImport;
  };
  std::map<std::string, va_t> Personalities, Ordinary;
  for (const auto &[Address, P] : Original.MachOAuthenticatedPointers)
    if (!P.Symbol.empty() && !P.Addend && P.Key == kPersonalityKey &&
        P.Diversity == kPersonalityDiversity &&
        P.AddressDiversity == kPersonalityAddressDiversity &&
        MatchesProvider(P.Symbol, P))
      Personalities.try_emplace(P.Symbol, Address);
  // A mutable data variable can have the same bind. Only reuse non-lazy
  // pointer storage that dyld seals after applying the ordinary binding.
  for (const auto &[Address, B] : Original.DyldBindSlots) {
    if (B.Addend || !MatchesProvider(B.Name, B))
      continue;
    const auto *Sec = Original.getSectionFor(Address);
    if (!Sec || (Sec->Type & SECTION_TYPE) != S_NON_LAZY_SYMBOL_POINTERS ||
        (Address - Sec->VA) % arm64e::kPointerBytes ||
        !rangeInBounds(Address - Sec->VA, arm64e::kPointerBytes, Sec->FileSz))
      continue;
    if (llvm::any_of(Chains.Segments, [&](const auto &S) {
          return (S.Flags & SG_READ_ONLY) && Address >= S.VMAddr &&
                 rangeInBounds(Address - S.VMAddr, arm64e::kPointerBytes,
                               S.FileSize);
        }))
      Ordinary.try_emplace(B.Name, Address);
  }
  // Keep the original weak data export as the fallback. Each new reference
  // lets dyld select the coalesced definition, including a newly allocated GOT.
  for (const auto &[Section, Index] : Object.WeakData) {
    const auto &Symbol = Object.Symbols.at(Index);
    const auto Name = Symbol.LinkerName.str();
    const auto Export = Chains.WeakDefinitions.find(Name);
    const auto Provider = Providers.find(Name);
    if (Declared.contains(Symbol.Name) ||
        Export == Chains.WeakDefinitions.end() ||
        !Original.getSectionFor(Export->second) ||
        Original.isCodeAddress(Export->second) || Provider == Providers.end() ||
        !Provider->second.LocalWeakDefinition)
      return fail(kWeakDataFallback);
    if (const auto Slot = Ordinary.find(Name); Slot != Ordinary.end())
      Object.GOT.emplace(std::pair{Index, false},
                         NativeMachO::GOTSlot{std::nullopt, Slot->second});
    auto &Discarded = Object.Image.Sections[Section];
    Discarded.IsAllocated = Discarded.IsInImage = false;
  }
  const auto WeakTarget = [&](unsigned Ref,
                              bool External) -> std::optional<unsigned> {
    if (External) {
      const auto Symbol = Object.Symbols.find(Ref);
      if (Symbol == Object.Symbols.end())
        return std::nullopt;
      Ref = Symbol->second.Section;
    }
    const auto Section = Object.Sections.find(Ref);
    if (Section == Object.Sections.end())
      return std::nullopt;
    const auto I = Object.WeakData.find(Section->second);
    return I == Object.WeakData.end() ? std::nullopt : std::optional(I->second);
  };
  llvm::DenseMap<uint64_t, uint8_t> PersonalityFields;
  std::set<unsigned> BranchImports;
  for (const auto &F : Object.OldFDEs)
    if (F.Personality)
      PersonalityFields.try_emplace(F.Personality->Offset,
                                    F.Personality->Width);
  for (const auto &R : Object.Relocations) {
    if (Object.WeakData.contains(R.Section))
      continue;
    const bool Personality =
        isCompactPersonality(Object, R) ||
        (Object.EH == R.Section && R.Type == ARM64_RELOC_POINTER_TO_GOT);
    if (!Object.WeakData.empty()) {
      const auto Target = WeakTarget(R.SymbolIndex, R.External);
      const bool BindablePointer =
          !R.PCRel && R.Width == arm64e::kPointerBytes &&
          (R.Type == ARM64_RELOC_UNSIGNED ||
           R.Type == ARM64_RELOC_AUTHENTICATED_POINTER);
      if ((R.SubtractSymbol && WeakTarget(*R.SubtractSymbol, true)) ||
          (Target && ((!UsesGOT(R) && !BindablePointer) || !R.External ||
                      R.SymbolIndex != *Target || R.SubtractSymbol ||
                      R.Scattered || Personality)))
        return fail(kWeakDataReference);
    }
    if (Personality && Object.EH == R.Section &&
        PersonalityFields.lookup(R.Offset) != R.Width)
      return fail(kLanguagePointer);
    if (IsImportBranch(R)) {
      const auto &Symbol = Object.Symbols.at(R.SymbolIndex);
      if (Providers.contains(Symbol.LinkerName.str()))
        BranchImports.insert(R.SymbolIndex);
    }
    if (!UsesGOT(R))
      continue;
    if (!R.External || R.Scattered || R.SubtractVA || R.SubtractSymbol ||
        !Object.Symbols.contains(R.SymbolIndex))
      return fail(kGOTRelocation);
    if (isTLVLoad(R.Type)) {
      // A TLVP reference loads the dyld descriptor; the descriptor's own
      // authenticated resolver chooses the instance for the calling thread.
      const auto &Symbol = Object.Symbols.at(R.SymbolIndex);
      if (Declared.contains(Symbol.Name) || R.InstructionAddend ||
          R.Width != sizeof(uint32_t) ||
          Object.Image.Sections[R.Section].Kind != Kind::Code)
        return fail(kTLVDescriptor);
      if (Object.TLV.contains(R.SymbolIndex))
        continue;
      if (Symbol.Section) {
        const auto Sec = Object.Sections.find(Symbol.Section);
        if (Sec == Object.Sections.end() || Object.Descriptors != Sec->second ||
            Symbol.Offset % kTLVDescriptorBytes)
          return fail(kTLVDescriptor);
        Object.TLV.emplace(R.SymbolIndex, NativeMachO::TLVReference{0, true});
        continue;
      }
      const auto Name = Symbol.LinkerName.str();
      const auto Export = Chains.ThreadLocalDefinitions.find(Name);
      if (Export != Chains.ThreadLocalDefinitions.end()) {
        const auto Address = Export->second;
        const auto *Sec = Original.getSectionFor(Address);
        const auto Resolver = Original.MachOAuthenticatedPointers.find(Address);
        if (Providers.contains(Name) || !Sec ||
            (Sec->Type & SECTION_TYPE) != S_THREAD_LOCAL_VARIABLES ||
            (Address - Sec->VA) % kTLVDescriptorBytes ||
            !rangeInBounds(Address - Sec->VA, kTLVDescriptorBytes,
                           Sec->FileSz) ||
            Resolver == Original.MachOAuthenticatedPointers.end())
          return fail(kTLVDescriptor);
        const auto &P = Resolver->second;
        if (P.Symbol != macho_arm64e_abi::kTLVBootstrap ||
            P.Key != kDefaultFunctionPointerKey || P.Addend ||
            P.AddressDiversity || P.Diversity || P.WeakImport ||
            !P.LibraryOrdinal || *P.LibraryOrdinal <= 0 ||
            !MatchesProvider(P.Symbol, P))
          return fail(kTLVDescriptor);
        Object.TLV.emplace(R.SymbolIndex,
                           NativeMachO::TLVReference{Address, true});
      } else {
        const auto Slot = Ordinary.find(Name);
        if (Slot == Ordinary.end())
          return fail(kTLVDescriptor);
        const auto &B = Original.DyldBindSlots.at(Slot->second);
        if (B.Name != Name || !B.LibraryOrdinal || *B.LibraryOrdinal <= 0 ||
            B.WeakImport)
          return fail(kTLVDescriptor);
        Object.TLV.emplace(R.SymbolIndex,
                           NativeMachO::TLVReference{Slot->second, false});
      }
      continue;
    }
    const auto Key = std::pair{R.SymbolIndex, Personality};
    if (Object.GOT.contains(Key))
      continue;
    const auto &Symbol = Object.Symbols.at(R.SymbolIndex);
    if (Symbol.Name.empty())
      return fail(kGOTRelocation);
    if (!Symbol.Section && !Object.Image.SymbolAddrs.contains(Symbol.Name) &&
        !Declared.contains(Symbol.Name)) {
      const auto &Candidates = Personality ? Personalities : Ordinary;
      if (auto It = Candidates.find(Symbol.LinkerName.str());
          It != Candidates.end()) {
        Object.GOT.emplace(Key, NativeMachO::GOTSlot{std::nullopt, It->second});
        continue;
      }
    }
    CompiledSection Slot;
    Slot.Name = kNativeGOT;
    Slot.Kind = Kind::WritableData;
    Slot.Alignment = Slot.Size = arm64e::kPointerBytes;
    Slot.ExternalBytes.resize(Slot.Size);
    Slot.ReadOnlyAfterFixups = true;
    CompiledFixupReference Reference;
    Reference.BitWidth = arm64e::kPointerBytes * CHAR_BIT;
    Reference.Symbol = Symbol.Name;
    Reference.IsResolved = true;
    if (Personality)
      Reference.PointerAuth = CompiledPointerAuthentication{
          kPersonalityKey, kPersonalityDiversity, kPersonalityAddressDiversity};
    else
      Reference.OrdinaryPointer = true;
    Slot.FixupReferences.push_back(std::move(Reference));
    Object.GOT.emplace(Key, NativeMachO::GOTSlot{Object.Image.Sections.size()});
    Object.Image.Sections.push_back(std::move(Slot));
  }
  if (BranchImports.empty())
    return llvm::Error::success();
  // A registered stub can serve a branch when every original import for
  // that spelling has the requested provider and strength. Otherwise the
  // branch needs its own callable, even if an ordinary GOT cell exists.
  for (const auto &[VA, Index] : Original.ImportStubIndices) {
    const auto *Stub = Original.findImportStubAt(VA);
    if (!Stub)
      continue;
    const auto P = Providers.find(Stub->Name);
    const auto N = Object.Image.MachOImportWeakness.find(Stub->Name);
    if (P != Providers.end() && P->second.Ordinal &&
        N != Object.Image.MachOImportWeakness.end() &&
        P->second.WeakImport == N->second)
      BranchImports.erase(Object.Names.at(Object.sourceName(Stub->Name)));
  }
  return prepareAuthenticatedCallStubs(Object, std::move(BranchImports));
}

bool matchesArm64eABI(const llvm::object::MachOObjectFile &Object,
                      uint32_t Subtype, const BinaryImage &Original) {
  if (Subtype == Original.MachOCPUSubtype)
    return true;
  // Darwin links classic ABI0 objects into ABI1 macOS main executables.
  // The executable ABI tag is not an instruction to compile ABI1 objects.
  if (Subtype != macho_arm64e_abi::kClassicObjectSubtype ||
      Original.MachOCPUSubtype != macho_arm64e_abi::kMacOSMainSubtype ||
      Original.DynInfo.MachOFileType != MH_EXECUTE ||
      Original.DynInfo.MachOPlatforms.size() != 1 ||
      Original.DynInfo.MachOPlatforms.front() != PLATFORM_MACOS)
    return false;
  bool HasPlatform = false;
  for (const auto &Command : Object.load_commands()) {
    auto Platform = machOCommandPlatform(Object, Command, Original.Arch);
    if (!Platform)
      continue;
    if (HasPlatform || *Platform != PLATFORM_MACOS)
      return false;
    HasPlatform = true;
  }
  return HasPlatform;
}

llvm::Expected<NativeMachO> readObject(llvm::StringRef Bytes,
                                       const BinaryImage &Original,
                                       llvm::StringRef Triple) {
  auto File = llvm::object::ObjectFile::createObjectFile(
      llvm::MemoryBufferRef(Bytes, "patch-object"));
  if (!File)
    return File.takeError();
  auto *M = llvm::dyn_cast<llvm::object::MachOObjectFile>(File->get());
  if (!M || !M->isLittleEndian() ||
      M->getBytesInAddress() != Original.getPointerSize())
    return fail("expected a matching little-endian native Mach-O object");
  const uint32_t CPU =
      M->is64Bit() ? M->getHeader64().cputype : M->getHeader().cputype;
  const uint32_t Subtype =
      (M->is64Bit() ? M->getHeader64().cpusubtype : M->getHeader().cpusubtype) &
      ~CPU_SUBTYPE_MASK;
  const uint32_t Filetype =
      M->is64Bit() ? M->getHeader64().filetype : M->getHeader().filetype;
  const uint32_t Flags =
      M->is64Bit() ? M->getHeader64().flags : M->getHeader().flags;
  const uint32_t ExpectedCPU = Original.Arch == Arch::X86   ? CPU_TYPE_X86
                               : Original.Arch == Arch::X64 ? CPU_TYPE_X86_64
                               : Original.Arch == Arch::ARM ? CPU_TYPE_ARM
                               : Original.Arch == Arch::AArch64 ? CPU_TYPE_ARM64
                                                                : 0;
  if (Filetype != MH_OBJECT || CPU != ExpectedCPU ||
      !(Flags & MH_SUBSECTIONS_VIA_SYMBOLS))
    return fail("native object ISA or linker atom contract does not match");
  const uint32_t RawSubtype =
      M->is64Bit() ? M->getHeader64().cpusubtype : M->getHeader().cpusubtype;
  if (Original.Arch == Arch::AArch64 &&
      (arm64e::isSubtype(RawSubtype) != Original.isArm64e() ||
       (Original.isArm64e() && !matchesArm64eABI(*M, RawSubtype, Original))))
    return fail(kSubtype);
  if (Original.Arch == Arch::ARM) {
    const bool Watch = llvm::Triple(Triple).isWatchOS();
    if (Watch
            ? Subtype != CPU_SUBTYPE_ARM_V7K
            : (Subtype != CPU_SUBTYPE_ARM_V7 && Subtype != CPU_SUBTYPE_ARM_V7S))
      return fail("native ARM platform ABI does not match the image");
  }
  NativeMachO Result;
  Result.SjLj = Original.Arch == Arch::ARM && !llvm::Triple(Triple).isWatchOS();
  auto &Image = Result.Image;
  Image.InputKind = RewriteInputKind::ReplacementObject;
  Image.TargetArch = Original.Arch;
  Image.Format = BinaryFormat::MachO;
  Image.PointerWidth = Original.getPointerSize();
  Image.TargetTriple = Triple.str();
  std::vector<MachODataInCodeRange> Data;
  for (const auto &LC : M->load_commands()) {
    if (!Original.isArm64e() || LC.C.cmd != LC_DATA_IN_CODE)
      continue;
    const auto D = M->getLinkeditDataLoadCommand(LC);
    if (!rangeInBounds(D.dataoff, D.datasize, Bytes.size()))
      return fail(kEmbeddedObject);
    auto Parsed = decodeMachODataInCode(
        llvm::arrayRefFromStringRef(Bytes.substr(D.dataoff, D.datasize)), 0,
        llvm::endianness::little);
    if (!Parsed)
      return Parsed.takeError();
    Data = std::move(*Parsed);
  }
  uint64_t Total = 0;
  for (const auto &S : M->sections()) {
    auto Name = S.getName();
    if (!Name)
      return Name.takeError();
    const uint32_t Number = M->getSectionID(S) + 1;
    const uint32_t Flags = M->is64Bit()
                               ? M->getSection64(S.getRawDataRefImpl()).flags
                               : M->getSection(S.getRawDataRefImpl()).flags;
    const unsigned Type = Flags & SECTION_TYPE;
    const bool Compact = *Name == "__compact_unwind";
    if ((Flags & S_ATTR_DEBUG) && !Compact)
      continue;
    if (!S.getSize())
      continue;
    const bool Initializers =
        Original.isArm64e() && Type == S_MOD_INIT_FUNC_POINTERS;
    const bool ThreadInit =
        Original.isArm64e() && Type == S_THREAD_LOCAL_INIT_FUNCTION_POINTERS;
    const bool Template =
        Original.isArm64e() &&
        (Type == S_THREAD_LOCAL_REGULAR || Type == S_THREAD_LOCAL_ZEROFILL);
    const bool Descriptors =
        Original.isArm64e() && Type == S_THREAD_LOCAL_VARIABLES;
    if ((Name->starts_with(kThreadSectionPrefix) && !Template && !Descriptors &&
         !ThreadInit) ||
        (Type == S_MOD_INIT_FUNC_POINTERS && !Initializers) ||
        Type == S_MOD_TERM_FUNC_POINTERS ||
        Type == S_THREAD_LOCAL_VARIABLE_POINTERS ||
        (Type == S_THREAD_LOCAL_INIT_FUNCTION_POINTERS && !ThreadInit) ||
        *Name == kBitcodeSegment || *Name == kModuleTermSection ||
        (*Name == kModuleInitSection && !Initializers) ||
        M->getSectionFinalSegmentName(S.getRawDataRefImpl()) == kBitcodeSegment)
      return fail(kUnsupportedRuntimeSection);
    if (Original.isArm64e() && *Name == kObjCImageInfo) {
      // The runtime reads one image-wide version/flags pair. A matching
      // object declaration is already installed in the original image.
      auto Contents = S.getContents();
      if (!Contents)
        return Contents.takeError();
      const auto InData = [](llvm::StringRef Segment) {
        return Segment == kDataSegment || Segment == kDataConstSegment ||
               Segment == kAuthConstSegment;
      };
      const Section *Info = nullptr;
      for (const auto &Candidate : Original.Sections)
        if (Candidate.Name == kObjCImageInfo && InData(Candidate.SegmentName)) {
          if (Info)
            return fail(kObjCFlags);
          Info = &Candidate;
        }
      if (!Info || Type != S_REGULAR ||
          !InData(M->getSectionFinalSegmentName(S.getRawDataRefImpl())) ||
          Contents->size() != kObjCImageInfoBytes || !S.relocations().empty() ||
          readLE<uint32_t>(Contents->bytes_begin()) != kObjCImageInfoVersion ||
          Info->Size != Contents->size() || Info->FileSz < Info->Size)
        return fail(kObjCFlags);
      const auto *Installed = Original.readVA(Info->VA, Info->Size);
      if (!Installed || std::memcmp(Installed, Contents->data(), Info->Size))
        return fail(kObjCFlags);
      continue;
    }
    const bool Code =
        Flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
    const bool Zero = Type == S_ZEROFILL || Type == S_THREAD_LOCAL_ZEROFILL;
    const bool Indirect = Type == S_NON_LAZY_SYMBOL_POINTERS;
    if (Type != S_REGULAR && Type != S_COALESCED && Type != S_ZEROFILL &&
        Type != S_CSTRING_LITERALS && Type != S_4BYTE_LITERALS &&
        Type != S_8BYTE_LITERALS && Type != S_16BYTE_LITERALS && !Indirect &&
        !Template && !Descriptors && !Initializers && !ThreadInit)
      return fail("unsupported native section kind: " + *Name);
    if (S.getSize() >
        ::neverd::limits::backend::kMachOPatchObjectMaxStorage - Total)
      return fail("native storage exceeds profile limit");
    Total += S.getSize();
    CompiledSection Item;
    Item.Name = Name->str();
    Item.Size = S.getSize();
    Item.Alignment = S.getAlignment().value();
    const auto Segment = M->getSectionFinalSegmentName(S.getRawDataRefImpl());
    Item.ReadOnlyAfterFixups =
        Segment == kDataConstSegment || Segment == kAuthConstSegment ||
        (Segment == kDataSegment && *Name == kConstSection);
    const bool Writable = Zero || Segment == kDataSegment ||
                          Segment == kAuthSegment || Item.ReadOnlyAfterFixups;
    Item.Kind = Compact    ? Kind::Other
                : Code     ? Kind::Code
                : Zero     ? Kind::UninitializedData
                : Writable ? Kind::WritableData
                           : Kind::ReadOnlyData;
    Item.IsAllocated = !Compact && !Indirect;
    Item.IsInImage = !Compact && !Indirect && *Name != "__eh_frame";
    if (!llvm::isPowerOf2_64(Item.Alignment) || Item.Alignment > 65536)
      return fail("invalid section alignment");
    if (Zero)
      Item.ExternalBytes.resize(Item.Size, 0);
    else {
      auto Data = S.getContents();
      if (!Data)
        return Data.takeError();
      if (Data->size() != Item.Size)
        return fail("truncated section");
      Item.ExternalBytes.assign(Data->begin(), Data->end());
    }
    if (Indirect) {
      const auto D = M->getDysymtabLoadCommand();
      const unsigned First =
          M->is64Bit() ? M->getSection64(S.getRawDataRefImpl()).reserved1
                       : M->getSection(S.getRawDataRefImpl()).reserved1;
      if (S.getSize() % Image.PointerWidth || First > D.nindirectsyms ||
          S.getSize() / Image.PointerWidth > D.nindirectsyms - First ||
          !rangeInBounds(D.indirectsymoff, uint64_t(D.nindirectsyms) * 4,
                         Bytes.size()) ||
          !S.relocations().empty())
        return fail("invalid native indirect-symbol storage");
      for (uint64_t N = 0; N < S.getSize() / Image.PointerWidth; ++N) {
        const unsigned Symbol = M->getIndirectSymbolTableEntry(D, First + N);
        if (Symbol & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS))
          return fail("local indirect pointer needs a native rebase contract");
        if (!Result.IndirectSymbols
                 .emplace(S.getAddress() + N * Image.PointerWidth, Symbol)
                 .second)
          return fail("overlapping native indirect pointers");
      }
    }
    const size_t Index = Image.Sections.size();
    for (const auto &R :
         machODataInCodeRanges(Data, S.getAddress(), S.getSize())) {
      if (!Code || R.Begin < S.getAddress() ||
          R.End - S.getAddress() > S.getSize())
        return fail(kEmbeddedObject);
      Result.Embedded.emplace_back(
          Index, MachODataInCodeRange{R.Begin - S.getAddress(),
                                      R.End - S.getAddress(), R.Kind});
    }
    if (Initializers || ThreadInit) {
      if (Code || (Initializers && Result.InitializerPointers) ||
          Item.Size % arm64e::kPointerBytes)
        return fail(kInitializerStorage);
      if (ThreadInit)
        Result.ThreadInitializers.push_back(Index);
      else
        Result.InitializerPointers = Index;
      Item.IsAllocated = Item.IsInImage = false;
      Item.Kind = Kind::Other;
    }
    if (Descriptors || Template) {
      if (Code || Segment != kDataSegment)
        return fail(kTLVStorage);
      if (Descriptors) {
        if (Result.Descriptors || Item.Size % kTLVDescriptorBytes)
          return fail(kTLVStorage);
        Result.Descriptors = Index;
        Item.Alignment =
            std::max<uint64_t>(Item.Alignment, arm64e::kPointerBytes);
      } else {
        Result.TemplateSections.insert(Index);
        // Coalesce zero and file-backed TLS into one contiguous file template.
        Item.Kind = Kind::WritableData;
      }
    }
    if (*Name == "__eh_frame") {
      if (Result.EH)
        return fail("duplicate native eh_frame");
      Result.EH = Index;
    }
    if (Compact) {
      if (Result.CompactSection)
        return fail("duplicate compact input");
      Result.CompactSection = Index;
    }
    Result.Sections[Number] = Index;
    Result.OldSectionVAs[Number] = S.getAddress();
    Image.Sections.push_back(std::move(Item));
    auto Range = S.relocations();
    for (auto I = Range.begin(); I != Range.end(); ++I) {
      int64_t Addend = 0;
      if (Original.Arch == Arch::AArch64 &&
          I->getType() == ARM64_RELOC_ADDEND) {
        const auto Prefix = *I;
        if (++I == Range.end())
          return fail(macho_loader::detail::kMalformedARM64Addend);
        auto Parsed =
            macho_loader::detail::decodeARM64AddendPair(*M, Prefix, *I);
        if (!Parsed)
          return fail(macho_loader::detail::kMalformedARM64Addend);
        Addend = *Parsed;
      }
      const auto Raw = M->getRelocation(I->getRawDataRefImpl());
      Relocation R{Index,
                   I->getOffset(),
                   M->getAnyRelocationType(Raw),
                   1u << M->getAnyRelocationLength(Raw),
                   M->getPlainRelocationSymbolNum(Raw),
                   M->getPlainRelocationExternal(Raw),
                   bool(M->getAnyRelocationPCRel(Raw)),
                   M->isRelocationScattered(Raw)};
      R.InstructionAddend = Addend;
      const bool Half =
          Original.Arch == Arch::ARM &&
          (R.Type == ARM_RELOC_HALF || R.Type == ARM_RELOC_HALF_SECTDIFF);
      if (Half) {
        R.HalfLength = M->getAnyRelocationLength(Raw);
        R.Width = 4;
      }
      if (R.Width != 4 && R.Width != 8)
        return fail("unsupported relocation width");
      if (Half) {
        const bool Difference = R.Type == ARM_RELOC_HALF_SECTDIFF;
        if (R.PCRel || R.Scattered != Difference || ++I == Range.end())
          return fail("malformed ARM HALF pair");
        const auto Pair = M->getRelocation(I->getRawDataRefImpl());
        if (M->isRelocationScattered(Pair) != Difference ||
            M->getAnyRelocationPCRel(Pair) ||
            M->getAnyRelocationType(Pair) != ARM_RELOC_PAIR ||
            M->getAnyRelocationLength(Pair) != R.HalfLength ||
            (!Difference && M->getPlainRelocationExternal(Pair)))
          return fail("invalid ARM HALF pair");
        R.OtherHalf = M->getAnyRelocationAddress(Pair) & 0xffff;
        if (Difference) {
          R.ScatteredVA = M->getScatteredRelocationValue(Raw);
          R.SubtractVA = M->getScatteredRelocationValue(Pair);
        }
      } else if (R.Scattered) {
        R.ScatteredVA = M->getScatteredRelocationValue(Raw);
        if (R.Type == GENERIC_RELOC_SECTDIFF ||
            R.Type == GENERIC_RELOC_LOCAL_SECTDIFF ||
            (Original.Arch == Arch::ARM &&
             (R.Type == ARM_RELOC_SECTDIFF ||
              R.Type == ARM_RELOC_LOCAL_SECTDIFF))) {
          if (++I == Range.end())
            return fail("section difference has no pair");
          const auto Pair = M->getRelocation(I->getRawDataRefImpl());
          if (!M->isRelocationScattered(Pair) ||
              M->getAnyRelocationType(Pair) != GENERIC_RELOC_PAIR ||
              (1u << M->getAnyRelocationLength(Pair)) != R.Width || R.PCRel)
            return fail("invalid section difference pair");
          R.SubtractVA = M->getScatteredRelocationValue(Pair);
        } else if (R.Type != GENERIC_RELOC_VANILLA)
          return fail("unsupported scattered native relocation");
      } else if ((Original.Arch == Arch::X64 &&
                  R.Type == X86_64_RELOC_SUBTRACTOR) ||
                 (Original.Arch == Arch::AArch64 &&
                  R.Type == ARM64_RELOC_SUBTRACTOR)) {
        if (!R.External || ++I == Range.end())
          return fail("subtractor has no symbol pair");
        const auto Pair = M->getRelocation(I->getRawDataRefImpl());
        const unsigned UnsignedType = Original.Arch == Arch::X64
                                          ? unsigned(X86_64_RELOC_UNSIGNED)
                                          : unsigned(ARM64_RELOC_UNSIGNED);
        if (M->isRelocationScattered(Pair) ||
            !M->getPlainRelocationExternal(Pair) ||
            M->getAnyRelocationType(Pair) != UnsignedType ||
            I->getOffset() != R.Offset ||
            (1u << M->getAnyRelocationLength(Pair)) != R.Width || R.PCRel)
          return fail("invalid subtractor pair");
        R.SubtractSymbol = R.SymbolIndex;
        R.SymbolIndex = M->getPlainRelocationSymbolNum(Pair);
        R.Type = UnsignedType;
      }
      Result.Relocations.push_back(R);
    }
  }
  if (Data.size() != Result.Embedded.size())
    return fail(kEmbeddedObject);
  for (const auto &S : M->symbols()) {
    auto Name = S.getName();
    if (!Name)
      return Name.takeError();
    const auto Index = M->getSymbolIndex(S.getRawDataRefImpl());
    const auto Entry = macho_loader::readObjectNList(*M, S.getRawDataRefImpl());
    const auto &[Type, Number, Desc, Value] = Entry;
    if (Type & N_STAB)
      continue;
    if ((Type & N_TYPE) != N_SECT && (Type & N_TYPE) != N_UNDF)
      continue;
    const bool PrivateWeak = Original.isArm64e() && (Type & N_TYPE) == N_SECT &&
                             (Type & N_PEXT) && (Desc & N_WEAK_DEF);
    const bool PublicWeak = Original.isArm64e() && (Type & N_TYPE) == N_SECT &&
                            (Type & N_EXT) && !(Type & N_PEXT) &&
                            (Desc & (N_WEAK_DEF | N_WEAK_REF)) == N_WEAK_DEF;
    if ((Type & N_TYPE) == N_UNDF && Original.isArm64e()) {
      const auto Weak = Entry.importWeakness();
      // N_REF_TO_WEAK describes the definition. Only N_WEAK_REF allows
      // this object reference to be absent when dyld resolves the symbol.
      if (!Weak.has_value())
        return fail(kUnsupportedNativeSymbol);
      Image.MachOImportWeakness.emplace(Name->str(), *Weak);
    } else if (((Desc & (N_WEAK_REF | N_WEAK_DEF)) && !PrivateWeak &&
                !PublicWeak) ||
               ((Type & N_TYPE) == N_UNDF && Value)) {
      return fail(kUnsupportedNativeSymbol);
    }
    const bool Prefixed = Name->starts_with(macho_arm64e_abi::kSymbolPrefix);
    Symbol Item{{},
                *Name,
                Number,
                0,
                0,
                false,
                bool(Desc & N_ARM_THUMB_DEF),
                bool(Type & N_EXT)};
    if ((Type & N_TYPE) == N_SECT) {
      if (!Result.Sections.contains(Number))
        continue;
      const auto &Section = Image.Sections[Result.Sections.at(Number)];
      if (Value < Result.OldSectionVAs.at(Number) ||
          Value - Result.OldSectionVAs.at(Number) >= Section.Size)
        return fail("symbol exceeds native section");
      Item.Offset = Value - Result.OldSectionVAs.at(Number);
      Item.Function = Section.Kind == Kind::Code && Prefixed &&
                      machODataInCodeRanges(Data, Value, 1).empty();
      // Hidden compiler helpers (e.g. terminate landing pads) bind within this
      // object. Staging adds no exported weak definition or coalescing promise.
      if (PublicWeak) {
        if (Item.Function || Item.Offset ||
            Section.Name != kMutableDataSection ||
            Section.Kind != Kind::WritableData ||
            !Result.WeakData.emplace(Result.Sections.at(Number), Index).second)
          return fail(kWeakDataAtom);
        Image.MachOImportWeakness.emplace(Name->str(), false);
      }
      if (Result.Descriptors == Result.Sections.at(Number) &&
          !(Type & N_PEXT) && (Type & N_EXT) && !PublicWeak) {
        if (Item.Offset % kTLVDescriptorBytes)
          return fail(kTLVStorage);
        Result.TLSExports.emplace(Name->str(), Item.Offset);
      }
      if (PrivateWeak && !Item.Function)
        return fail(kPrivateWeak);
    } else
      Item.Section = 0;
    if (!Name->empty() && (!Original.isArm64e() || Prefixed)) {
      Item.Name =
          Original.isArm64e() ? Result.sourceName(*Name) : canonical(*Name);
      if (!Result.Names.emplace(Item.Name, Index).second)
        return fail(kDuplicateNativeSymbol);
    }
    Result.Symbols.emplace(Index, std::move(Item));
  }
  for (auto &[Index, S] : Result.Symbols) {
    if (Original.isArm64e() && !S.LinkerName.empty() &&
        !S.LinkerName.starts_with(macho_arm64e_abi::kSymbolPrefix)) {
      S.Name = Result.sourceName(S.LinkerName);
      if (!Result.Names.emplace(S.Name, Index).second)
        return fail(kDuplicateNativeSymbol);
    }
    if (!S.Section)
      continue;
    auto It = Result.WeakData.find(Result.Sections.at(S.Section));
    if (It != Result.WeakData.end() &&
        (S.Offset || (S.External && Index != It->second)))
      return fail(kWeakDataAtom);
  }
  if (Result.EH) {
    const auto &EH = Image.Sections[*Result.EH];
    auto Logical = EH.ExternalBytes;
    llvm::SmallVector<const Relocation *> ExternalLSDAs;
    // Native assemblers can leave FDE/LSDA section differences unresolved.
    // Decode a logical object-address view, preserving raw bytes for the final
    // placement relocation. Otherwise two placeholder FDEs look overlapping.
    for (const auto &R : Result.Relocations) {
      if (R.Section != *Result.EH)
        continue;
      // PTR_TO_GOT has no addend. Its assembler placeholder is not a linked
      // personality address and may underflow the logical object address.
      // Decode the field shape now; prepareArm64eGOT and relocate require the
      // exact personality field and install its final authenticated GOT slot.
      if (Original.isArm64e() && R.Type == ARM64_RELOC_POINTER_TO_GOT) {
        if (!R.External || !R.PCRel || R.Scattered || R.SubtractSymbol ||
            R.SubtractVA || R.Width != sizeof(uint32_t) ||
            !rangeInBounds(R.Offset, R.Width, Logical.size()))
          return fail(kLanguagePointer);
        writeLE<uint32_t>(Logical.data() + R.Offset, 0);
        continue;
      }
      if (!R.SubtractSymbol)
        continue;
      if (!rangeInBounds(R.Offset, R.Width, Logical.size()) ||
          !Result.Symbols.contains(R.SymbolIndex) ||
          !Result.Symbols.contains(*R.SubtractSymbol))
        return fail(kLanguagePointer);
      const auto &Target = Result.Symbols.at(R.SymbolIndex);
      const auto &Subtract = Result.Symbols.at(*R.SubtractSymbol);
      if (!Subtract.Section)
        return fail(kLanguagePointer);
      if (!Target.Section) {
        // An explicitly bound LSDA may remain in the original image. Decode
        // only the field shape here; the binding and final relocation supply
        // its address. Never apply this to an FDE start or an arbitrary field.
        if (!Original.isArm64e() || !R.External || R.PCRel || R.Scattered ||
            Result.Sections.at(Subtract.Section) != *Result.EH ||
            (R.Width != sizeof(uint32_t) && R.Width != sizeof(uint64_t)))
          return fail(kLanguagePointer);
        std::fill_n(Logical.data() + R.Offset, R.Width, 0);
        ExternalLSDAs.push_back(&R);
        continue;
      }
      auto *Field = Logical.data() + R.Offset;
      const uint64_t Raw = R.Width == sizeof(uint64_t)
                               ? readLE<uint64_t>(Field)
                               : readLE<uint32_t>(Field);
      const auto Value =
          llvm::APInt(kRelocationBits,
                      Result.OldSectionVAs.at(Target.Section) + Target.Offset) -
          llvm::APInt(kRelocationBits,
                      Result.OldSectionVAs.at(Subtract.Section) +
                          Subtract.Offset) +
          llvm::APInt(R.Width * CHAR_BIT, Raw).sext(kRelocationBits);
      if (!Value.isSignedIntN(R.Width * CHAR_BIT))
        return fail(kLanguagePointer);
      if (R.Width == sizeof(uint64_t))
        writeLE<uint64_t>(Field, Value.getLoBits(64).getZExtValue());
      else
        writeLE<uint32_t>(Field, Value.getLoBits(32).getZExtValue());
    }
    const auto Number = llvm::find_if(Result.Sections, [&](const auto &S) {
                          return S.second == *Result.EH;
                        })->first;
    auto Records = decodeDwarfEHFrameRecords(
        Logical, Result.OldSectionVAs.at(Number), Image.PointerWidth == 8);
    if (!Records)
      return Records.takeError();
    for (const auto *R : ExternalLSDAs)
      if (llvm::none_of(*Records, [&](const auto &F) {
            return F.LSDA && F.LSDA->Offset == R->Offset &&
                   F.LSDA->Width == R->Width &&
                   dweh::getApplication(F.LSDA->Encoding) == dweh::PCRel &&
                   !(F.LSDA->Encoding & dweh::Indirect);
          }))
        return fail(kLanguagePointer);
    Result.OldFDEs = std::move(*Records);
  }
  if (Result.InitializerPointers) {
    CompiledSection Offsets;
    Offsets.Name = kInitializerOffsets;
    Offsets.Kind = Kind::ReadOnlyData;
    Offsets.Alignment = sizeof(uint32_t);
    Offsets.Size = Image.Sections[*Result.InitializerPointers].Size /
                   arm64e::kPointerBytes * sizeof(uint32_t);
    Offsets.ExternalBytes.resize(Offsets.Size);
    Result.InitializerOffsets = Image.Sections.size();
    Image.Sections.push_back(std::move(Offsets));
  }
  return Result;
}

llvm::Error prepareImports(NativeMachO &Object,
                           llvm::ArrayRef<SourcePatchImport> Imports) {
  auto &Image = Object.Image;
  for (const auto &I : Imports) {
    const auto N = Object.Names.find(I.Request.Symbol);
    if (N == Object.Names.end() || Object.Symbols.at(N->second).Section)
      return fail("new dependency requires a unique undefined symbol: " +
                  I.Request.Symbol);
    Object.Imports.emplace(N->second, I);
    if (I.Request.Kind != SourcePatchDependencyKind::Function)
      continue;
    auto Code = buildSourceImportThunk(Image.TargetArch,
                                       Image.TargetArch == Arch::ARM
                                           ? InstructionMode::Thumb
                                           : InstructionMode::Default,
                                       0x10000, 0x20000);
    if (!Code)
      return Code.takeError();
    CompiledSection S;
    S.Name = kImportStubPrefix + std::to_string(N->second);
    S.Kind = Kind::Code;
    S.Alignment = 4;
    S.Size = Code->size();
    S.ExternalBytes = std::move(*Code);
    S.IsAllocated = S.IsInImage = true;
    Object.Thunks.emplace(
        N->second, NativeMachO::Thunk{Image.Sections.size(), I.SlotVA, 0, 0});
    Image.Sections.push_back(std::move(S));
  }
  for (const auto &[VA, Index] : Object.IndirectSymbols) {
    const auto Import = Object.Imports.find(Index);
    if (Import == Object.Imports.end())
      return fail("native indirect pointer has no explicit dependency");
    Object.IndirectSlots.emplace(VA, Import->second.SlotVA);
  }
  if (!Object.Thunks.empty()) {
    if (!Object.EH) {
      Object.EH = Image.Sections.size();
      CompiledSection F;
      F.Name = "__eh_frame";
      F.Kind = Kind::ReadOnlyData;
      F.Alignment = 8;
      F.IsAllocated = true;
      F.IsInImage = false;
      Image.Sections.push_back(std::move(F));
    }
    // Reserve the size now, append records only after producer relocations and
    // exact producer function ranges have been checked independently.
    auto &F = Image.Sections[*Object.EH];
    F.Size = llvm::alignTo(F.Size, 8);
    for (auto &[Index, T] : Object.Thunks) {
      auto Frame = buildSourceImportThunkFrame(
          Image.TargetArch, 0x10000, Image.Sections[T.Section].Size, 0x20000);
      if (!Frame)
        return Frame.takeError();
      T.FrameSize = Frame->size();
      F.Size += T.FrameSize;
    }
  }
  return llvm::Error::success();
}

llvm::Error finalizeImports(NativeMachO &Object,
                            const MachOEHFrameRegion &Region) {
  if (Object.Thunks.empty())
    return llvm::Error::success();
  auto &Image = Object.Image;
  auto &F = Image.Sections[*Object.EH];
  if (F.ExternalBytes.size() % 8) {
    // Pad the last authenticated record with DW_CFA_nop, not a zero-length
    // record (which would terminate the entire table before our new CIE).
    size_t At = 0, Last = 0;
    while (At < F.ExternalBytes.size()) {
      if (!rangeInBounds(At, 4, F.ExternalBytes.size()))
        return fail("truncated producer frame");
      const uint32_t Size = readLE<uint32_t>(F.ExternalBytes.data() + At);
      if (Size < 4 || !rangeInBounds(At + 4, Size, F.ExternalBytes.size()))
        return fail("unmodeled producer frame terminator");
      Last = At;
      At += 4 + Size;
    }
    const auto Padding = llvm::alignTo(At, 8) - At;
    writeLE<uint32_t>(F.ExternalBytes.data() + Last,
                      readLE<uint32_t>(F.ExternalBytes.data() + Last) +
                          Padding);
    F.ExternalBytes.resize(At + Padding, 0);
  }
  for (auto &[Index, T] : Object.Thunks) {
    auto &S = Image.Sections[T.Section];
    auto Code = buildSourceImportThunk(Image.TargetArch,
                                       Image.TargetArch == Arch::ARM
                                           ? InstructionMode::Thumb
                                           : InstructionMode::Default,
                                       S.VA, T.Slot);
    if (!Code)
      return Code.takeError();
    std::copy(Code->begin(), Code->end(), Image.Bytes.begin() + S.Offset);
    // Mach-O FDEs must start on 8-byte boundaries; the producer's records
    // already have this padding. Reject rather than insert a zero terminator.
    if (F.ExternalBytes.size() % 8)
      return fail("producer frame alignment cannot host an import thunk");
    T.FrameOffset = F.ExternalBytes.size();
    auto Frame = buildSourceImportThunkFrame(Image.TargetArch, S.VA, S.Size,
                                             F.VA + T.FrameOffset);
    if (!Frame)
      return Frame.takeError();
    if (Frame->size() != T.FrameSize)
      return fail("import frame sizing changed");
    auto Records = decodeDwarfEHFrameRecords(*Frame, F.VA + T.FrameOffset,
                                             Image.PointerWidth == 8);
    if (!Records)
      return Records.takeError();
    if (Records->size() != 1 || Records->front().BeginVA != S.VA ||
        Records->front().EndVA != S.VA + S.Size)
      return fail("import frame has no exact thunk owner");
    const uint64_t FDE = Records->front().RecordVA - Region.SectionVA;
    if (FDE > 0xffffff)
      return fail("import FDE exceeds compact offset encoding");
    Object.Compact.push_back(
        {S.VA, S.VA + S.Size,
         (Image.TargetArch == Arch::AArch64 ? 0x03000000u : 0x04000000u) |
             uint32_t(FDE)});
    F.ExternalBytes.insert(F.ExternalBytes.end(), Frame->begin(), Frame->end());
  }
  if (F.ExternalBytes.size() != F.Size)
    return fail("native import frame reservation changed");
  return llvm::Error::success();
}

llvm::Error prepareMainRelro(NativeMachO &Object, std::vector<uint8_t> &Binary,
                             const BinaryImage &Original) {
  if (!Original.isArm64e() || Original.DynInfo.MachOFileType != MH_EXECUTE)
    return llvm::Error::success();
  unsigned ReadOnlySegments = 0;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t, bool) {
        if (ID == LC_SEGMENT_64)
          ReadOnlySegments +=
              (reinterpret_cast<const segment_command_64 *>(Command)->flags &
               SG_READ_ONLY) != 0;
      });
  if (!ReadOnlySegments)
    return llvm::Error::success();
  if (ReadOnlySegments != 1)
    return fail(kMainRelro);
  uint64_t Size = 0, Alignment = 1;
  constexpr auto Limit = ::neverd::limits::backend::kMachOPatchObjectMaxStorage;
  for (const auto &Section : Object.Image.Sections)
    if (Section.ReadOnlyAfterFixups && Section.IsInImage) {
      if (Section.Alignment > Limit)
        return fail(kMainRelroExtent);
      Alignment = std::max(Alignment, Section.Alignment);
      Size = llvm::alignTo(Size, Section.Alignment);
      if (Size > Limit || Section.Size > Limit - Size)
        return fail(kMainRelroExtent);
      Size += Section.Size;
    }
  if (!Size)
    return llvm::Error::success();
  if (Size > Limit - Alignment)
    return fail(kMainRelroExtent);
  auto Storage = declareMachOReadOnlyData(
      Binary, Original.Arch, kNativeRelroSection, Size + Alignment);
  if (!Storage)
    return Storage.takeError();
  uint64_t Cursor = *Storage;
  for (auto &Section : Object.Image.Sections)
    if (Section.ReadOnlyAfterFixups && Section.IsInImage) {
      Cursor = llvm::alignTo(Cursor, Section.Alignment);
      Section.VA = Cursor;
      Section.Offset = 0;
      Section.IsInImage = false;
      Cursor += Section.Size;
    }
  return llvm::Error::success();
}

bool needsThreadLocalTerminator(const NativeMachO &Object) {
  const auto I = Object.Names.find(kTLVAtExit);
  return I != Object.Names.end() && !Object.Symbols.at(I->second).Section &&
         llvm::any_of(Object.Relocations, [&](const auto &R) {
           return R.External && R.SymbolIndex == I->second &&
                  R.Type == ARM64_RELOC_BRANCH26;
         });
}

static CompiledFixupReference signedFunctionImport(llvm::StringRef Name) {
  CompiledFixupReference Ref;
  Ref.BitWidth = arm64e::kPointerBytes * CHAR_BIT;
  Ref.Symbol = Name.str();
  Ref.IsResolved = true;
  Ref.PointerAuth =
      CompiledPointerAuthentication{kDefaultFunctionPointerKey, 0, false};
  return Ref;
}

// A native B/BL to an imported function needs a local callable. Keep the
// slot in RELRO and describe the leaf stub to the system unwinder.
llvm::Error prepareAuthenticatedCallStubs(NativeMachO &Object,
                                          std::set<unsigned> Functions) {
  if (Functions.empty())
    return llvm::Error::success();
  for (const auto &R : Object.Relocations) {
    if (!R.External || R.Type != ARM64_RELOC_BRANCH26 ||
        !Functions.erase(R.SymbolIndex))
      continue;
    const auto &Name = Object.Symbols.at(R.SymbolIndex).Name;
    CompiledSection Slot;
    Slot.Name = kNativeGOT;
    Slot.Kind = Kind::WritableData;
    Slot.Size = Slot.Alignment = arm64e::kPointerBytes;
    Slot.ExternalBytes.resize(Slot.Size);
    Slot.ReadOnlyAfterFixups = true;
    Slot.FixupReferences.push_back(signedFunctionImport(Name));
    auto &Sections = Object.Image.Sections;
    const auto SlotIndex = Sections.size();
    Sections.push_back(std::move(Slot));
    Object.AuthenticatedCallStubs.emplace(
        R.SymbolIndex,
        NativeMachO::AuthenticatedCallStub{Sections.size(), SlotIndex});
    CompiledSection Code;
    Code.Name = Name == kTLVAtExit
                    ? std::string(kTLVAtExitThunk)
                    : kImportStubPrefix + std::to_string(R.SymbolIndex);
    Code.Kind = Kind::Code;
    Code.Alignment = neverd::aarch64::kInsnSize;
    auto Stub =
        buildSourceImportThunk(Arch::AArch64, InstructionMode::Default, 0, 0);
    if (!Stub)
      return Stub.takeError();
    Code.Size = Stub->size();
    Code.ExternalBytes = std::move(*Stub);
    Sections.push_back(std::move(Code));
  }
  return llvm::Error::success();
}

llvm::Error
prepareRuntimeDependencies(NativeMachO &Object, const BinaryImage &Original,
                           std::vector<SourcePatchDependency> &Dependencies,
                           llvm::ArrayRef<SourcePatchBinding> Bindings = {}) {
  std::set<unsigned> Functions;
  if (Original.isArm64e()) {
    for (auto &Dependency : Dependencies) {
      llvm::StringRef Spelling = Dependency.Symbol;
      if (!Spelling.consume_front(macho_arm64e_abi::kVerbatimPrefix))
        continue;
      auto Name = Object.sourceName(Spelling);
      const auto I = Object.Names.find(Name);
      if (I != Object.Names.end() &&
          Object.Symbols.at(I->second).LinkerName == Spelling)
        Dependency.Symbol = std::move(Name);
    }
    const auto AdmitRuntime = [&](const SourcePatchDependency &Runtime) {
      const auto I = Object.Names.find(Runtime.Symbol);
      if (I == Object.Names.end() || Object.Symbols.at(I->second).Section ||
          llvm::none_of(Object.Relocations,
                        [&](const auto &R) {
                          return R.External && R.SymbolIndex == I->second;
                        }) ||
          llvm::any_of(
              Bindings,
              [&](const auto &B) { return B.Symbol == Runtime.Symbol; }) ||
          llvm::any_of(Dependencies, [&](const auto &D) {
            return D.Symbol == Runtime.Symbol;
          }))
        return;
      // Keep the original dyld provider. A direct branch can still need a
      // callable when the original only stored the imported pointer.
      if (Original.findImport(Object.Symbols.at(I->second).LinkerName)) {
        if (Runtime.Kind == SourcePatchDependencyKind::Function)
          Functions.insert(I->second);
      } else {
        Dependencies.push_back(Runtime);
      }
    };
    for (const auto &Runtime : machODarwinStackImports())
      AdmitRuntime(Runtime);
    AdmitRuntime({macho_arm64e_abi::kLibSystem, macho_arm64e_abi::kCxaAtExit,
                  SourcePatchDependencyKind::Function});
  }
  const auto Add = [&](llvm::StringRef Name) -> llvm::Error {
    const auto Old = llvm::find_if(
        Dependencies, [&](const auto &D) { return D.Symbol == Name; });
    if (Old != Dependencies.end())
      return Old->Library == macho_arm64e_abi::kLibSystem &&
                     Old->Kind == SourcePatchDependencyKind::Function
                 ? llvm::Error::success()
                 : fail(kTLVStorage);
    Dependencies.push_back({macho_arm64e_abi::kLibSystem, Name.str(),
                            SourcePatchDependencyKind::Function});
    return llvm::Error::success();
  };
  if (Object.Descriptors || !Object.ThreadInitializers.empty()) {
    if (auto E = Add(canonical(macho_arm64e_abi::kTLVBootstrap)))
      return E;
    if (needsThreadLocalTerminator(Object))
      if (auto E = Add(kTLVAtExit))
        return E;
  }
  if (!Original.isArm64e())
    return llvm::Error::success();
  for (const auto &D : Dependencies) {
    const auto I = Object.Names.find(D.Symbol);
    if (D.Kind == SourcePatchDependencyKind::Function &&
        I != Object.Names.end() && !Object.Symbols.at(I->second).Section)
      Functions.insert(I->second);
  }
  return prepareAuthenticatedCallStubs(Object, std::move(Functions));
}

// Existing descriptors and public TLS exports retain their address and byte
// offset. Move only the loader's template view, keeping the original span as
// a prefix. Its new address follows every descriptor, as dyld's v2 thunk uses
// a forward 32-bit delta to the initialized bytes.
llvm::Error prepareThreadLocals(NativeMachO &Object,
                                const BinaryImage &Original) {
  if (!Object.Descriptors && Object.TemplateSections.empty() &&
      Object.ThreadInitializers.empty())
    return llvm::Error::success();
  if (Object.Descriptors.has_value() == Object.TemplateSections.empty())
    return fail(kTLVStorage);
  CompiledSection Prefix;
  Prefix.Name = kTLVTemplatePrefix;
  Prefix.Kind = Kind::WritableData;
  std::optional<va_t> Start;
  std::vector<va_t> OldInitializers, OldDescriptors;
  std::vector<NativeMachO::ThreadRuntime::Field> Fields;
  const size_t PrefixIndex = Object.Image.Sections.size();
  for (const auto &Section : Original.Sections) {
    const auto Type = Section.Type & SECTION_TYPE;
    if (Type == S_THREAD_LOCAL_INIT_FUNCTION_POINTERS) {
      if (Section.Size != Section.FileSz ||
          Section.Size % arm64e::kPointerBytes)
        return fail(kThreadInitPointers);
      // dyld visits sections in load-command order and entries in reverse.
      for (auto End = Section.Size; End; End -= arm64e::kPointerBytes) {
        const auto Slot = Section.VA + End - arm64e::kPointerBytes;
        auto P = Original.MachOAuthenticatedPointers.find(Slot);
        va_t Target;
        if (P != Original.MachOAuthenticatedPointers.end()) {
          const auto &Recipe = P->second;
          if (Recipe.Key != kDefaultFunctionPointerKey || Recipe.Addend ||
              Recipe.AddressDiversity || Recipe.Diversity ||
              !Recipe.Symbol.empty())
            return fail(kThreadInitPointers);
          Target = Recipe.Target;
        } else {
          const auto *Value = Original.readVA(Slot, arm64e::kPointerBytes);
          if (!Value || Original.DyldBindSlots.contains(Slot) ||
              !Original.MachOResolvedChainedPointerSlots.contains(Slot))
            return fail(kThreadInitPointers);
          Target = readLE<uint64_t>(Value);
        }
        if (!Original.hasAuthenticatedFunctionEntryAt(Target) ||
            Target % neverd::aarch64::kInsnSize)
          return fail(kThreadInitPointers);
        OldInitializers.push_back(Target);
      }
      continue;
    }
    if (Type != S_THREAD_LOCAL_REGULAR && Type != S_THREAD_LOCAL_ZEROFILL)
      continue;
    if (!Section.Size)
      continue;
    if (!Start)
      Start = Section.VA;
    if (Section.VA < *Start || Section.VA - *Start < Prefix.Size ||
        !rangeInBounds(Section.VA - *Start, Section.Size,
                       limits::backend::kMachOPatchObjectMaxStorage))
      return fail(kTLVStorage);
    const auto Offset = Section.VA - *Start;
    // A gap can contain only alignment padding. Copying unrelated mapped
    // storage would make its contents part of every new thread's initial state.
    if (!llvm::isPowerOf2_64(Section.Alignment) ||
        Offset != llvm::alignTo(Prefix.Size, Section.Alignment))
      return fail(kTLVStorage);
    Prefix.Alignment = std::max<uint64_t>(Prefix.Alignment, Section.Alignment);
    Prefix.Size = Offset + Section.Size;
    Prefix.ExternalBytes.resize(Prefix.Size);
    if (Type == S_THREAD_LOCAL_REGULAR) {
      const auto *Data = Original.readVA(Section.VA, Section.Size);
      if (!Data || Section.FileSz != Section.Size)
        return fail(kTLVStorage);
      llvm::copy(llvm::ArrayRef(Data, Section.Size),
                 Prefix.ExternalBytes.begin() + Offset);
    }
    const auto AddFixup = [&](va_t Address, MachOAuthenticatedPointer Recipe,
                              bool Authenticated) -> llvm::Error {
      if (Type != S_THREAD_LOCAL_REGULAR ||
          !rangeInBounds(Address - Section.VA, arm64e::kPointerBytes,
                         Section.Size))
        return fail(kTLVStorage);
      // The source word encodes its old chain successor. Only the decoded
      // bind/rebase and PAC schema describe the new template slot.
      Recipe.EncodedValue.reset();
      const auto At = Address - *Start;
      if (Authenticated && Recipe.AddressDiversity) {
        Fields.push_back(
            {PrefixIndex, At, {Recipe.Key, Recipe.Diversity, true}});
      }
      writeLE<uint64_t>(Prefix.ExternalBytes.data() + At, 0);
      Object.TemplateFixups.push_back(
          {At, std::move(Recipe), false, Authenticated});
      return llvm::Error::success();
    };
    for (auto I = Original.MachOAuthenticatedPointers.lower_bound(Section.VA);
         I != Original.MachOAuthenticatedPointers.end() &&
         Section.contains(I->first);
         ++I)
      if (auto E = AddFixup(I->first, I->second, true))
        return E;
    for (auto I = Original.DyldBindSlots.lower_bound(Section.VA);
         I != Original.DyldBindSlots.end() && Section.contains(I->first); ++I) {
      if (Original.MachOAuthenticatedPointers.contains(I->first))
        continue;
      MachOAuthenticatedPointer Recipe;
      Recipe.Symbol = I->second.Name;
      Recipe.LibraryOrdinal = I->second.LibraryOrdinal;
      Recipe.Module = I->second.Module;
      Recipe.Addend = I->second.Addend;
      Recipe.WeakImport = I->second.WeakImport;
      if (auto E = AddFixup(I->first, std::move(Recipe), false))
        return E;
    }
    for (auto I =
             Original.MachOResolvedChainedPointerSlots.lower_bound(Section.VA);
         I != Original.MachOResolvedChainedPointerSlots.end() &&
         Section.contains(*I);
         ++I) {
      if (Original.MachOAuthenticatedPointers.contains(*I) ||
          Original.DyldBindSlots.contains(*I))
        continue;
      const auto *Value = Original.readVA(*I, arm64e::kPointerBytes);
      if (!Value)
        return fail(kTLVStorage);
      MachOAuthenticatedPointer Recipe;
      Recipe.Target = readLE<uint64_t>(Value);
      if (auto E = AddFixup(*I, std::move(Recipe), false))
        return E;
    }
  }
  for (const auto &Section : Original.Sections) {
    if ((Section.Type & SECTION_TYPE) != S_THREAD_LOCAL_VARIABLES)
      continue;
    if (!Start || Section.Size % kTLVDescriptorBytes)
      return fail(kTLVStorage);
    for (uint64_t At = 0; At < Section.Size; At += kTLVDescriptorBytes) {
      const auto *Field = Original.readVA(Section.VA + At + kTLVOffsetField,
                                          arm64e::kPointerBytes);
      if (!Field || readLE<uint64_t>(Field) >= Prefix.Size)
        return fail(kTLVStorage);
      OldDescriptors.push_back(Section.VA + At);
    }
  }
  if (Start) {
    Object.TemplatePrefix = Object.Image.Sections.size();
    Object.Image.Sections.push_back(std::move(Prefix));
  }
  for (const auto &R : Object.Relocations) {
    if (!Object.TemplateSections.contains(R.Section) ||
        R.Type != ARM64_RELOC_AUTHENTICATED_POINTER)
      continue;
    const auto &S = Object.Image.Sections[R.Section];
    if (!rangeInBounds(R.Offset, arm64e::kPointerBytes, S.ExternalBytes.size()))
      return fail(kTLVStorage);
    const auto Word = arm64e::decodeObjectPointerWord(
        readLE<uint64_t>(S.ExternalBytes.data() + R.Offset));
    if (!Word)
      return fail(kAuthMetadata);
    if (Word->AddressDiversity) {
      Fields.push_back(
          {R.Section, R.Offset, {Word->Key, Word->Diversity, true}});
    }
  }
  if (Fields.empty() && OldInitializers.empty() &&
      Object.ThreadInitializers.empty())
    return llvm::Error::success();
  if (!Object.Descriptors && OldDescriptors.empty())
    return fail(kTLVStorage);
  size_t Count = OldInitializers.size();
  for (size_t I : Object.ThreadInitializers)
    Count += Object.Image.Sections[I].Size / arm64e::kPointerBytes;
  const auto Words =
      kThreadConfigWords + kThreadFieldWords * Fields.size() + Count;
  if (Words >
      limits::backend::kMachOPatchObjectMaxStorage / arm64e::kPointerBytes)
    return fail(kTLVStorage);
  auto Resolver = buildMachOTLSResolver();
  if (!Resolver)
    return Resolver.takeError();
  auto &Sections = Object.Image.Sections;
  const auto Append = [&](llvm::StringRef Name, Kind K, size_t Size,
                          uint64_t Align) {
    CompiledSection S;
    S.Name = Name.str();
    S.Kind = K;
    S.Alignment = Align;
    S.Size = Size;
    S.ExternalBytes.resize(Size);
    Sections.push_back(std::move(S));
    return Sections.size() - 1;
  };
  const size_t Code = Append(kThreadResolver, Kind::Code, Resolver->Code.size(),
                             arm64e::kPointerBytes);
  Sections[Code].ExternalBytes = std::move(Resolver->Code);
  const size_t Config =
      Append(kThreadConfig, Kind::WritableData, Words * arm64e::kPointerBytes,
             arm64e::kPointerBytes);
  Sections[Config].ReadOnlyAfterFixups = true;
  const size_t Anchor = Append(kThreadAnchor, Kind::WritableData,
                               kTLVDescriptorBytes, arm64e::kPointerBytes);
  Sections[Anchor].FixupReferences.push_back(
      signedFunctionImport(canonical(macho_arm64e_abi::kTLVBootstrap)));
  const size_t Guard = Append(kThreadGuard, Kind::WritableData, 1, 1);
  Object.TemplateSections.insert(Guard);
  Object.PerThread.emplace(NativeMachO::ThreadRuntime{
      Code, Config, Anchor, Guard, Resolver->ConfigDeltaOffset, Count,
      std::move(Fields), std::move(OldInitializers),
      std::move(OldDescriptors)});
  Object.Runtime.UsesThreadResolver = true;
  return llvm::Error::success();
}

llvm::Error place(NativeMachO &Object, uint64_t Base, uint64_t OriginalSlide,
                  const std::optional<MachOEHFrameRegion> &EH,
                  bool PrivateFrames = false) {
  auto &Image = Object.Image;
  Image.BaseVA = Base;
  uint64_t Cursor = Base;
  enum class Group {
    Code,
    ReadOnly,
    Relro,
    Mutable,
    Descriptors,
    Prefix,
    Template
  };
  auto GroupOf = [&](size_t I, const CompiledSection &S) {
    if (Object.PerThread ? Object.PerThread->Anchor == I
                         : Object.Descriptors == I)
      return Group::Descriptors;
    if (Object.TemplatePrefix == I)
      return Group::Prefix;
    if (Object.TemplateSections.contains(I))
      return Group::Template;
    if (S.Kind == Kind::Code)
      return Group::Code;
    if (S.Kind == Kind::ReadOnlyData)
      return Group::ReadOnly;
    return S.ReadOnlyAfterFixups ? Group::Relro : Group::Mutable;
  };
  uint64_t TemplateAlignment = 1;
  for (auto Index : Object.TemplateSections)
    TemplateAlignment =
        std::max(TemplateAlignment, Image.Sections[Index].Alignment);
  if (Object.TemplatePrefix)
    TemplateAlignment = std::max(
        TemplateAlignment, Image.Sections[*Object.TemplatePrefix].Alignment);
  for (auto G : {Group::Code, Group::ReadOnly, Group::Relro, Group::Mutable,
                 Group::Descriptors, Group::Prefix, Group::Template}) {
    if (G == Group::Relro || G == Group::Mutable)
      Cursor = llvm::alignTo(
          Cursor, sourcePatchStorageAlignment(Image.TargetArch, Image.Format));
    if (G == Group::Prefix && (Object.Descriptors || Object.PerThread)) {
      Cursor = llvm::alignTo(Cursor, TemplateAlignment);
      Object.Runtime.Template = {Cursor, 0, TemplateAlignment};
    }
    for (auto &&[I, S] : llvm::enumerate(Image.Sections)) {
      if (!S.IsAllocated || !S.IsInImage || G != GroupOf(I, S))
        continue;
      Cursor = llvm::alignTo(Cursor, S.Alignment);
      if (Cursor < Base ||
          !rangeInBounds(Cursor - Base, S.Size,
                         limits::backend::kMachOPatchObjectMaxStorage))
        return fail(kPlacementOverflow);
      S.VA = Cursor;
      S.Offset = Cursor - Base;
      Cursor += S.Size;
      if (G == Group::Descriptors)
        Object.Runtime.Descriptors = {S.VA, S.Size, S.Alignment};
      if (G == Group::Template)
        Object.Runtime.Template.Size = Cursor - Object.Runtime.Template.VA;
    }
  }
  if (Cursor - Base > ::neverd::limits::backend::kMachOPatchObjectMaxStorage)
    return fail("native placement exceeds limit");
  Image.Bytes.resize(Cursor - Base, 0);
  for (auto &S : Image.Sections)
    if (S.IsInImage) {
      std::copy(S.ExternalBytes.begin(), S.ExternalBytes.end(),
                Image.Bytes.begin() + S.Offset);
      S.ExternalBytes.clear();
    }
  if (Object.EH) {
    // Darwin commonly links away redundant DWARF when every function has a
    // compact encoding. In that image, retain native compact evidence for
    // every replacement instead of requiring an unused DWARF registration.
    Object.CompactOnly = !PrivateFrames && !EH;
    if (!Object.CompactOnly) {
      Image.Sections[*Object.EH].VA = PrivateFrames ? Cursor : EH->AppendVA;
      Object.EHSectionVA = PrivateFrames ? Cursor : EH->SectionVA;
    }
  }
  for (const auto &[Index, S] : Object.Symbols)
    if (S.Section && Object.Sections.contains(S.Section) &&
        !Object.WeakData.contains(Object.Sections.at(S.Section)))
      Image.SymbolAddrs[S.Name] =
          Image.Sections[Object.Sections.at(S.Section)].VA + S.Offset;
  for (auto &[Key, Slot] : Object.GOT) {
    if (!Slot.Section) {
      Slot.Address += OriginalSlide;
      continue;
    }
    auto &S = Image.Sections[*Slot.Section];
    Slot.Address = S.VA;
    auto &Reference = S.FixupReferences.front();
    if (auto It = Image.SymbolAddrs.find(Reference.Symbol);
        It != Image.SymbolAddrs.end())
      Reference.ResolvedValue = It->second;
  }
  if (Object.InitializerOffsets) {
    const auto &S = Image.Sections[*Object.InitializerOffsets];
    Object.Runtime.InitializerOffsets = {S.VA, S.Size, S.Alignment};
  }
  for (auto &Fixup : Object.TemplateFixups)
    Fixup.Address += Object.Runtime.Template.VA;
  for (auto &[Name, Address] : Object.TLSExports)
    Address += Object.Image.Sections[*Object.Descriptors].VA;
  if (Object.PerThread) {
    const auto &T = *Object.PerThread;
    const auto &Resolver = Image.Sections[T.Code];
    auto &Config = Image.Sections[T.Config];
    auto *Data = Config.IsInImage ? Image.Bytes.data() + Config.Offset
                                  : Config.ExternalBytes.data();
    const auto SetRebase = [&](uint64_t Slot, uint64_t Target, bool Auth) {
      MachOAuthenticatedPointer Recipe;
      Recipe.Target = Target;
      Object.TemplateFixups.push_back({Slot, std::move(Recipe), true, Auth});
    };
    // All table pointers are dyld rebases in RELRO. The text contains only
    // the signed slide-independent displacement to that table.
    writeLE<uint64_t>(Image.Bytes.data() + Resolver.Offset + T.DeltaOffset,
                      Config.VA - (Resolver.VA + T.DeltaOffset));
    SetRebase(Config.VA, Image.Sections[T.Anchor].VA, false);
    writeLE<uint64_t>(Data + arm64e::kPointerBytes,
                      Image.Sections[T.Guard].VA - Object.Runtime.Template.VA);
    SetRebase(Config.VA + kTLVOffsetField, Object.Runtime.Template.VA, false);
    uint64_t At = kThreadConfigWords * arm64e::kPointerBytes;
    writeLE<uint64_t>(Data + kThreadFieldCountOffset, T.SignedFields.size());
    writeLE<uint64_t>(Data + kThreadCallbackCountOffset, T.CallbackCount);
    for (const auto &[Section, Offset, Schema] : T.SignedFields) {
      writeLE<uint64_t>(Data + At, Image.Sections[Section].VA + Offset -
                                       Object.Runtime.Template.VA);
      writeLE<uint64_t>(Data + At + arm64e::kPointerBytes,
                        (uint64_t(Schema.Key) << kThreadKeyShift) |
                            Schema.Diversity);
      At += kThreadFieldWords * arm64e::kPointerBytes;
    }
    for (va_t Target : T.OldInitializers) {
      SetRebase(Config.VA + At, Target, true);
      At += arm64e::kPointerBytes;
    }
    for (va_t Public : T.OldDescriptors) {
      MachOAuthenticatedPointer Recipe;
      Recipe.Target = Resolver.VA;
      Object.TemplateFixups.push_back(
          {Public, std::move(Recipe), false, true, true});
    }
    Object.Compact.push_back({Resolver.VA, Resolver.VA + T.DeltaOffset,
                              macho_unwind::kARM64ModeFrame, std::nullopt,
                              std::nullopt});
  }
  for (const auto &[Index, CallStub] : Object.AuthenticatedCallStubs) {
    const auto &Code = Image.Sections[CallStub.Code];
    const auto &Slot = Image.Sections[CallStub.Slot];
    auto Stub = buildSourceImportThunk(Arch::AArch64, InstructionMode::Default,
                                       Code.VA, Slot.VA);
    if (!Stub)
      return Stub.takeError();
    writeLE<uint32_t>(Stub->data() + Stub->size() - sizeof(uint32_t),
                      kBranchAuthZeroX16);
    llvm::copy(*Stub, Image.Bytes.begin() + Code.Offset);
    Object.Compact.push_back({Code.VA, Code.VA + Code.Size,
                              macho_unwind::kARM64ModeFrameless, std::nullopt,
                              std::nullopt});
  }
  for (auto &[Index, Reference] : Object.TLV) {
    const auto &S = Object.Symbols.at(Index);
    if (S.Section)
      Reference.Address = Image.SymbolAddrs.at(S.Name);
    else
      Reference.Address += OriginalSlide;
  }
  Image.Success = true;
  return llvm::Error::success();
}
llvm::Expected<uint64_t> mapOldVA(const NativeMachO &Object, uint64_t VA) {
  if (auto It = Object.IndirectSlots.upper_bound(VA);
      It != Object.IndirectSlots.begin()) {
    --It;
    if (VA - It->first < Object.Image.PointerWidth)
      return It->second + VA - It->first;
  }
  std::optional<uint64_t> Value;
  for (const auto &[Number, Index] : Object.Sections) {
    const auto &S = Object.Image.Sections[Index];
    const uint64_t Old = Object.OldSectionVAs.at(Number);
    if (Object.CompactOnly && Object.EH && Index == *Object.EH && VA >= Old &&
        VA - Old < S.Size)
      return fail("reference to unregistered native DWARF storage");
    if (!S.IsAllocated || VA < Old || VA - Old >= S.Size)
      continue;
    if (Value)
      return fail("ambiguous object address");
    Value = S.VA + VA - Old;
  }
  if (!Value)
    return fail("object address does not name allocated storage");
  return *Value;
}
llvm::Expected<uint64_t>
resolve(const NativeMachO &Object, unsigned Index, const BinaryImage &Original,
        const std::map<std::string, SourcePatchBinding> &Bindings,
        bool DirectBranch = false) {
  if (!Object.Symbols.contains(Index))
    return fail("relocation symbol is invalid");
  const auto &S = Object.Symbols.at(Index);
  if (S.Section) {
    if (!Object.Sections.contains(S.Section))
      return fail("nonallocated native symbol");
    const auto Section = Object.Sections.at(S.Section);
    if (Object.InitializerPointers == Section ||
        llvm::is_contained(Object.ThreadInitializers, Section))
      return fail(kInitializerStorage);
    if (Object.CompactOnly && Object.EH && Section == *Object.EH)
      return fail("reference to unregistered native DWARF storage");
    return Object.Image.Sections[Object.Sections.at(S.Section)].VA + S.Offset +
           (S.Function && S.Thumb ? 1 : 0);
  }
  if (DirectBranch || S.Name == kTLVAtExit)
    if (auto Stub = Object.AuthenticatedCallStubs.find(Index);
        Stub != Object.AuthenticatedCallStubs.end())
      return Object.Image.Sections[Stub->second.Code].VA;
  // Darwin passes the address of the final Mach header to libSystem when
  // registering image and per-thread destructors.
  if (Original.isArm64e() && !DirectBranch &&
      S.Name == macho_arm64e_abi::kDSOHandle && !Bindings.contains(S.Name)) {
    const auto Header = Original.fileOffsetToVA(0);
    if (Header == InvalidVA)
      return fail(kMissingDSOHeader);
    return Header;
  }
  if (auto T = Object.Thunks.find(Index); T != Object.Thunks.end())
    return Object.Image.Sections[T->second.Section].VA +
           (Object.Image.TargetArch == Arch::ARM ? 1 : 0);
  if (Object.Imports.contains(Index))
    return fail("data dependency needs a loader pointer-slot relocation");
  if (auto B = Bindings.find(S.Name); B != Bindings.end())
    return B->second.OriginalVA;
  std::set<uint64_t> VAs;
  const bool ExactSpelling = Original.isArm64e();
  const llvm::StringRef LinkerName =
      ExactSpelling ? S.LinkerName : llvm::StringRef(S.Name);
  for (const auto &S0 : Original.Symbols)
    if ((ExactSpelling ? S0.Name == LinkerName
                       : canonical(S0.Name) == S.Name) &&
        S0.IsFunc && Original.hasAuthenticatedFunctionEntryAt(S0.Addr))
      VAs.insert(S0.Addr);
  for (const auto &[VA, Import] : Original.ImportStubIndices)
    if (Import < Original.Imports.size() &&
        Original.Imports[Import].Name == LinkerName &&
        Original.findImportStubAt(VA))
      VAs.insert(VA);
  if (VAs.size() != 1)
    return fail(
        "external native symbol needs a binding or authenticated callable: " +
        S.Name);
  return *VAs.begin();
}

/// Darwin's classic unwinder implicitly authenticates a saved LR with IB and
/// the incoming SP. Older LLVM emits an additional generic signing rule for
/// this same prologue. Record each translation for verification in final code.
void normalizeClassicReturnCFI(const BinaryImage &Original, NativeMachO &Object,
                               llvm::ArrayRef<DwarfEHFrameRecord> Records) {
  auto &Image = Object.Image;
  auto &EH = Image.Sections[*Object.EH];
  if (!llvm::is_contained(EH.ExternalBytes, dwarf_eh::aarch64::kNegateRAState))
    return;

  // A relocated personality can refer to a newly allocated GOT slot. Decode
  // just the unwind rules here. Final replay checks the actual personality.
  auto ProgramBytes = EH.ExternalBytes;
  for (const auto &R : Records)
    if (R.Personality)
      std::fill_n(ProgramBytes.begin() + R.Personality->Offset,
                  R.Personality->Width, 0);
  llvm::DenseMap<uint64_t, llvm::SmallVector<size_t, 1>> Opcodes;
  auto Frames = dwarf_eh::parseEHFrame(
      Original, {ProgramBytes.data(), ProgramBytes.size(), EH.VA}, {}, {},
      [&](uint64_t FDEOffset, size_t Offset, uint8_t Opcode) {
        if (Opcode == dwarf_eh::aarch64::kNegateRAState)
          Opcodes[FDEOffset].push_back(Offset);
      });
  if (Opcodes.empty())
    return;
  llvm::DenseMap<uint64_t, const DwarfCIE *> ClassicCIEs;
  for (const auto &CIE : Frames.CIEs) {
    bool HasEntryCFA = false;
    for (const auto &I : CIE.InitialInstructions) {
      if (I.Kind == CFIOpKind::Nop)
        continue;
      HasEntryCFA = I.Kind == CFIOpKind::DefCFA &&
                    I.Register == aarch64_unwind::kSP && !I.Offset;
      if (!HasEntryCFA)
        break;
    }
    if (HasEntryCFA && !CIE.IsSignalFrame && !CIE.UsesNonHintPAuthLR &&
        CIE.ReturnAddressRegister == aarch64_unwind::kLR)
      ClassicCIEs.try_emplace(CIE.SectionOffset, &CIE);
  }
  llvm::DenseSet<va_t> CompactFrames;
  for (const auto &Range : Object.Compact)
    if ((Range.Encoding & macho_unwind::kModeMask) !=
        macho_unwind::kARM64ModeDwarf)
      CompactFrames.insert(Range.BeginVA);
  constexpr auto Word = aarch64::kInsnSize;
  for (const auto &FDE : Frames.FDEs) {
    const auto *CIE = ClassicCIEs.lookup(FDE.CIESectionOffset);
    auto Found = Opcodes.find(FDE.SectionOffset);
    if (!CIE || CompactFrames.contains(FDE.InitialLocation) ||
        FDE.ParseStatus != ExceptionParseStatus::Complete ||
        Found == Opcodes.end() || FDE.AddressRange <= Word ||
        FDE.AddressRange % Word)
      continue;
    auto IsToggle = [](const auto &I) {
      return I.Kind == CFIOpKind::NegateRAState;
    };
    auto Entry = llvm::find_if(FDE.Instructions, IsToggle);
    if (Entry == FDE.Instructions.end() || Entry->CodeOffset != Word ||
        !std::all_of(FDE.Instructions.begin(), Entry, [](const auto &I) {
          return I.Kind == CFIOpKind::Nop || I.Kind == CFIOpKind::AdvanceLoc;
        }))
      continue;
    // Generic CFI toggles depend on the remembered state at each exit. Use
    // the DWARF evaluator once, then compare every transition to final code.
    using namespace dwarf_eh;
    AArch64ClassicReturnCFI Source;
    auto &Epilogues = Source.Epilogues;
    bool Valid = true, Explicit;
    auto Rule = visitAArch64ReturnAddressTransitions(
        Arch::AArch64, *CIE, FDE, FDE.InitialLocation, {}, Explicit,
        [&](const CFIInstruction &I, const AArch64ReturnAddressState &State) {
          Source.SigningRows.push_back(
              {I.CodeOffset, State.Kind == AArch64RASigningKind::SP});
          if (I.Kind == CFIOpKind::RestoreState)
            return;
          if (I.Kind != CFIOpKind::NegateRAState) {
            Valid = false;
            return;
          }
          if (&I == &*Entry) {
            Valid &= State.Kind == AArch64RASigningKind::SP &&
                     State.Key == AArch64RAKey::B;
            return;
          }
          Valid &= I.CodeOffset > Word && I.CodeOffset < FDE.AddressRange &&
                   I.CodeOffset % Word == 0 &&
                   (Epilogues.empty() || I.CodeOffset > Epilogues.back()) &&
                   State.Kind == AArch64RASigningKind::Unsigned;
          Epilogues.push_back(I.CodeOffset);
        });
    if (!Valid || !Explicit || Rule.Status != AArch64RAQueryStatus::Known ||
        Epilogues.size() + 1 != Found->second.size())
      continue;
    for (size_t Offset : Found->second)
      EH.ExternalBytes[Offset] = llvm::dwarf::DW_CFA_nop;
    Object.ClassicReturnFrames.try_emplace(FDE.InitialLocation,
                                           std::move(Source));
  }
}

llvm::Error
relocate(NativeMachO &Object, const BinaryImage &Original,
         const std::map<std::string, SourcePatchBinding> &Bindings) {
  const Arch A = Original.Arch;
  std::map<std::pair<size_t, uint64_t>, unsigned> Written;
  for (const auto &R : Object.Relocations) {
    const auto &S = Object.Image.Sections[R.Section];
    if (!rangeInBounds(R.Offset, R.Width, S.Size))
      return fail("relocation exceeds native section");
    auto Key = std::pair{R.Section, R.Offset};
    auto Next = Written.lower_bound(Key);
    if ((Next != Written.end() && Next->first.first == R.Section &&
         Next->first.second - R.Offset < R.Width) ||
        (Next != Written.begin() && std::prev(Next)->first.first == R.Section &&
         R.Offset - std::prev(Next)->first.second < std::prev(Next)->second))
      return fail("overlapping native relocation writers");
    Written.emplace(Key, R.Width);
    if (Object.WeakData.contains(R.Section))
      continue;
    if (Object.CompactOnly && Object.EH && R.Section == *Object.EH) {
      if ((R.External && !Object.Symbols.contains(R.SymbolIndex)) ||
          (R.SubtractSymbol && !Object.Symbols.contains(*R.SubtractSymbol)))
        return fail("unregistered DWARF relocation names an invalid symbol");
      continue;
    }
    uint8_t *Out =
        S.IsInImage
            ? Object.Image.Bytes.data() + S.Offset + R.Offset
            : Object.Image.Sections[R.Section].ExternalBytes.data() + R.Offset;
    const uint64_t Raw = R.Width == 8 ? readLE<uint64_t>(Out)
                                      : readLE<uint32_t>(Out),
                   P = S.VA + R.Offset;
    const auto ThreadInit = llvm::find(Object.ThreadInitializers, R.Section);
    if (Object.InitializerPointers == R.Section ||
        ThreadInit != Object.ThreadInitializers.end()) {
      if (R.Width != arm64e::kPointerBytes || R.Offset % R.Width || R.PCRel ||
          R.Scattered || R.SubtractSymbol || R.SubtractVA ||
          (R.Type != ARM64_RELOC_UNSIGNED &&
           R.Type != ARM64_RELOC_AUTHENTICATED_POINTER))
        return fail(kInitializerStorage);
      uint64_t Address;
      if (R.External) {
        auto Sym = Object.Symbols.find(R.SymbolIndex);
        if (Sym == Object.Symbols.end() || !Sym->second.Section ||
            !Sym->second.Function)
          return fail(kInitializerStorage);
        if (R.Type == ARM64_RELOC_AUTHENTICATED_POINTER) {
          const auto Word = arm64e::decodeObjectPointerWord(Raw);
          if (!Word || Word->Key != kDefaultFunctionPointerKey ||
              Word->Addend || Word->Diversity || Word->AddressDiversity)
            return fail(kInitializerStorage);
        } else if (Raw) {
          return fail(kInitializerStorage);
        }
        Address = Object.Image.SymbolAddrs.at(Sym->second.Name);
      } else {
        if (R.Type != ARM64_RELOC_UNSIGNED ||
            !Object.Sections.contains(R.SymbolIndex))
          return fail(kInitializerStorage);
        const auto &Code =
            Object.Image.Sections[Object.Sections.at(R.SymbolIndex)];
        const auto Old = Object.OldSectionVAs.at(R.SymbolIndex);
        if (Code.Kind != Kind::Code || Raw < Old || Raw - Old >= Code.Size ||
            llvm::none_of(
                Object.Symbols,
                [&](const auto &P) {
                  return P.second.Function &&
                         P.second.Section == R.SymbolIndex &&
                         P.second.Offset == Raw - Old;
                }))
          return fail(kInitializerStorage);
        Address = Code.VA + Raw - Old;
      }
      if (ThreadInit != Object.ThreadInitializers.end()) {
        const auto &T = *Object.PerThread;
        auto At =
            (kThreadConfigWords + kThreadFieldWords * T.SignedFields.size() +
             T.OldInitializers.size()) *
            arm64e::kPointerBytes;
        for (auto I = Object.ThreadInitializers.begin(); I != ThreadInit; ++I)
          At += Object.Image.Sections[*I].Size;
        At += S.Size - R.Offset - R.Width;
        MachOAuthenticatedPointer Recipe;
        Recipe.Target = Address;
        Object.TemplateFixups.push_back(
            {Object.Image.Sections[T.Config].VA + At, std::move(Recipe), true,
             true});
        continue;
      }
      auto Header = Original.fileOffsetToVA(0);
      if (Address < Header || Address - Header > UINT32_MAX)
        return fail(kInitializerStorage);
      const auto &Offsets = Object.Image.Sections[*Object.InitializerOffsets];
      writeLE<uint32_t>(Object.Image.Bytes.data() + Offsets.Offset +
                            R.Offset / R.Width * sizeof(uint32_t),
                        Address - Header);
      continue;
    }
    if (Object.Descriptors == R.Section) {
      if (R.Width != arm64e::kPointerBytes || R.PCRel || R.Scattered ||
          !R.External || R.SubtractVA || R.SubtractSymbol ||
          !Object.Symbols.contains(R.SymbolIndex))
        return fail(kTLVStorage);
      const auto Field = R.Offset % kTLVDescriptorBytes;
      const auto &Sym = Object.Symbols.at(R.SymbolIndex);
      if (Field == kTLVOffsetField) {
        if (R.Type != ARM64_RELOC_UNSIGNED || !Sym.Section ||
            !Object.Sections.contains(Sym.Section) ||
            !Object.TemplateSections.contains(Object.Sections.at(Sym.Section)))
          return fail(kTLVStorage);
        const auto &Target =
            Object.Image.Sections[Object.Sections.at(Sym.Section)];
        if (Raw >= Target.Size - Sym.Offset)
          return fail(kTLVStorage);
        writeLE<uint64_t>(Out, Target.VA - Object.Runtime.Template.VA +
                                   Sym.Offset + Raw);
      } else if (!Field) {
        if (Sym.Section ||
            Sym.Name != canonical(macho_arm64e_abi::kTLVBootstrap))
          return fail(kTLVStorage);
        if (R.Type == ARM64_RELOC_AUTHENTICATED_POINTER) {
          const auto Word = arm64e::decodeObjectPointerWord(Raw);
          if (!Word || Word->Key != kDefaultFunctionPointerKey ||
              Word->Diversity || Word->AddressDiversity || Word->Addend)
            return fail(kTLVStorage);
        } else if (R.Type != ARM64_RELOC_UNSIGNED || Raw) {
          return fail(kTLVStorage);
        }
        if (Object.PerThread) {
          MachOAuthenticatedPointer Recipe;
          Recipe.Target = Object.Image.Sections[Object.PerThread->Code].VA;
          Object.TemplateFixups.push_back({P, std::move(Recipe), false, true});
        } else {
          auto Ref =
              signedFunctionImport(canonical(macho_arm64e_abi::kTLVBootstrap));
          Ref.Offset = R.Offset;
          Object.Image.Sections[R.Section].FixupReferences.push_back(
              std::move(Ref));
        }
        writeLE<uint64_t>(Out, 0);
      } else {
        return fail(kTLVStorage);
      }
      continue;
    }
    if (A == Arch::AArch64 && R.Type == ARM64_RELOC_AUTHENTICATED_POINTER) {
      if (!Original.isArm64e() || R.Width != arm64e::kPointerBytes || R.PCRel ||
          R.Scattered || !R.External || R.SubtractVA || R.SubtractSymbol ||
          !S.IsAllocated ||
          (!S.IsInImage && !isExternalMachORelro(Object.Image, S)) ||
          S.Kind != Kind::WritableData)
        return fail(kAuthMetadata);
      const auto Word = arm64e::decodeObjectPointerWord(Raw);
      if (!Word)
        return fail(arm64e::kInvalidObjectPointerBits);
      const auto Symbol = Object.Symbols.find(R.SymbolIndex);
      if (Symbol == Object.Symbols.end() || Symbol->second.Name.empty())
        return fail(kAuthSymbol);
      CompiledFixupReference Reference;
      Reference.Offset = R.Offset;
      Reference.Kind = R.Type;
      Reference.BitWidth = arm64e::kPointerBytes * CHAR_BIT;
      Reference.Symbol = Symbol->second.Name;
      Reference.Addend = Word->Addend;
      Reference.IsResolved = true;
      Reference.PointerAuth = CompiledPointerAuthentication{
          Word->Key, Word->Diversity, Word->AddressDiversity};
      if (Object.Image.SymbolAddrs.contains(Reference.Symbol)) {
        auto Target = resolve(Object, R.SymbolIndex, Original, Bindings);
        if (!Target)
          return Target.takeError();
        constexpr unsigned WideBits = 2 * arm64e::kPointerBytes * CHAR_BIT;
        const auto Value = llvm::APInt(WideBits, *Target) +
                           llvm::APInt(WideBits, Word->Addend, true);
        if (!Value.isIntN(arm64e::kPointerBytes * CHAR_BIT))
          return fail(kAuthTarget);
        Reference.ResolvedValue = Value.getZExtValue();
      }
      Object.Image.Sections[R.Section].FixupReferences.push_back(
          std::move(Reference));
      writeLE<uint64_t>(Out, 0);
      continue;
    }
    const bool CompactPersonality = isCompactPersonality(Object, R);
    const bool PointerToGOT =
        A == Arch::AArch64 && R.Type == ARM64_RELOC_POINTER_TO_GOT;
    const bool AbsoluteGOT = PointerToGOT && !R.PCRel;
    if (Original.isArm64e() &&
        (CompactPersonality || (PointerToGOT && !AbsoluteGOT))) {
      const bool Personality = CompactPersonality || Object.EH == R.Section;
      auto Slot = Object.GOT.find({R.SymbolIndex, Personality});
      if (Slot == Object.GOT.end() ||
          (CompactPersonality &&
           (R.Type != ARM64_RELOC_UNSIGNED ||
            R.Width != arm64e::kPointerBytes || R.PCRel || Raw)) ||
          (PointerToGOT && R.Width != sizeof(uint32_t)))
        return fail(kGOTRelocation);
      if (CompactPersonality)
        writeLE<uint64_t>(Out, Slot->second.Address);
      else {
        // Mach-O PTR_TO_GOT carries no addend. The assembler's in-place
        // section offset is discarded, as by Apple's native linker.
        const auto Delta = llvm::APInt(kRelocationBits, Slot->second.Address) -
                           llvm::APInt(kRelocationBits, P);
        if (!Delta.isSignedIntN(sizeof(uint32_t) * CHAR_BIT))
          return fail(kGOTRange);
        writeLE<uint32_t>(Out, Delta.getLoBits(32).getZExtValue());
      }
      continue;
    }
    if (Original.isArm64e() &&
        (R.Type == ARM64_RELOC_UNSIGNED || AbsoluteGOT) && !R.SubtractSymbol &&
        !R.SubtractVA && S.IsAllocated &&
        (S.IsInImage || isExternalMachORelro(Object.Image, S))) {
      if (R.PCRel || R.Scattered || R.Width != arm64e::kPointerBytes ||
          S.Kind != Kind::WritableData)
        return fail(kOrdinaryStorage);
      CompiledFixupReference Reference;
      Reference.Offset = R.Offset;
      Reference.Kind = R.Type;
      Reference.BitWidth = R.Width * CHAR_BIT;
      Reference.IsResolved = Reference.OrdinaryPointer = true;
      if (AbsoluteGOT) {
        const auto Slot = Object.GOT.find({R.SymbolIndex, false});
        if (Raw || Slot == Object.GOT.end())
          return fail(kGOTRelocation);
        // This field rebases to the GOT cell. Only the cell binds the symbol.
        Reference.ResolvedValue = Slot->second.Address;
      } else if (R.External) {
        auto Symbol = Object.Symbols.find(R.SymbolIndex);
        if (Symbol == Object.Symbols.end() || Symbol->second.Name.empty())
          return fail(kOrdinarySymbol);
        Reference.Symbol = Symbol->second.Name;
        Reference.Addend = int64_t(Raw);
        if (Object.Image.SymbolAddrs.contains(Reference.Symbol)) {
          auto Target = resolve(Object, R.SymbolIndex, Original, Bindings);
          if (!Target)
            return Target.takeError();
          auto Value = llvm::APInt(kRelocationBits, *Target) +
                       llvm::APInt(kRelocationBits, Raw, true);
          if (!Value.isIntN(R.Width * CHAR_BIT))
            return fail(kOrdinaryTarget);
          Reference.ResolvedValue = Value.getZExtValue();
        }
      } else {
        if (!Object.Sections.contains(R.SymbolIndex))
          return fail(kOrdinarySymbol);
        const auto &Target =
            Object.Image.Sections[Object.Sections.at(R.SymbolIndex)];
        const auto Old = Object.OldSectionVAs.at(R.SymbolIndex);
        if (!Target.IsAllocated ||
            (!Target.IsInImage &&
             !isExternalMachORelro(Object.Image, Target)) ||
            Raw < Old || Raw - Old > Target.Size)
          return fail(kOrdinaryTarget);
        Reference.ResolvedValue = Target.VA + Raw - Old;
      }
      Object.Image.Sections[R.Section].FixupReferences.push_back(
          std::move(Reference));
      writeLE<uint64_t>(Out, 0);
      continue;
    }
    const auto Number = llvm::find_if(Object.Sections, [&](const auto &X) {
                          return X.second == R.Section;
                        })->first;
    const uint64_t OldP = Object.OldSectionVAs.at(Number) + R.Offset;
    const auto TLV = A == Arch::AArch64 && isTLVLoad(R.Type)
                         ? Object.TLV.find(R.SymbolIndex)
                         : Object.TLV.end();
    const bool GOT = R.External &&
                     (Object.Imports.contains(R.SymbolIndex) ||
                      Object.GOT.contains({R.SymbolIndex, false})) &&
                     ((A == Arch::X64 && (R.Type == X86_64_RELOC_GOT ||
                                          R.Type == X86_64_RELOC_GOT_LOAD)) ||
                      (A == Arch::AArch64 && isGOTLoad(R.Type)));
    llvm::Expected<uint64_t> Target =
        TLV != Object.TLV.end() ? llvm::Expected<uint64_t>(TLV->second.Address)
        : GOT         ? llvm::Expected<uint64_t>(
                            Object.GOT.contains({R.SymbolIndex, false})
                                ? Object.GOT.at({R.SymbolIndex, false}).Address
                                : Object.Imports.at(R.SymbolIndex).SlotVA)
        : R.Scattered ? mapOldVA(Object, R.ScatteredVA)
        : R.External
            ? resolve(Object, R.SymbolIndex, Original, Bindings,
                      A == Arch::AArch64 && R.Type == ARM64_RELOC_BRANCH26)
        : Object.Sections.contains(R.SymbolIndex)
            ? llvm::Expected<uint64_t>(
                  Object.Image.Sections[Object.Sections.at(R.SymbolIndex)].VA)
            : llvm::Expected<uint64_t>(
                  fail("invalid relocation section ordinal"));
    if (!Target)
      return Target.takeError();
    llvm::APInt V(128, *Target);
    V += llvm::APInt(V.getBitWidth(), R.InstructionAddend, true);
    if (!V.isIntN(sizeof(va_t) * CHAR_BIT))
      return fail(macho_loader::detail::kARM64AddendOverflow);
    const bool Vanilla =
        (A == Arch::X86 && R.Type == GENERIC_RELOC_VANILLA) ||
        (A == Arch::ARM && R.Type == ARM_RELOC_VANILLA) ||
        (A == Arch::X64 &&
         (GOT || R.Type == X86_64_RELOC_UNSIGNED ||
          R.Type == X86_64_RELOC_BRANCH || R.Type == X86_64_RELOC_SIGNED ||
          R.Type == X86_64_RELOC_SIGNED_1 || R.Type == X86_64_RELOC_SIGNED_2 ||
          R.Type == X86_64_RELOC_SIGNED_4)) ||
        (A == Arch::AArch64 && R.Type == ARM64_RELOC_UNSIGNED);
    const bool Half = A == Arch::ARM && (R.Type == ARM_RELOC_HALF ||
                                         R.Type == ARM_RELOC_HALF_SECTDIFF);
    if (Half) {
      const bool High = R.HalfLength & 1, Thumb = R.HalfLength & 2;
      uint16_t EncodedHalf;
      if (Thumb) {
        const uint16_t H = Raw, L = Raw >> 16;
        if ((H & 0xfbf0) != (High ? 0xf2c0 : 0xf240) || (L & 0x8000))
          return fail("ARM HALF does not name the selected Thumb MOV");
        EncodedHalf = ((H & 0xf) << 12) | ((H & 0x400) << 1) |
                      ((L & 0x7000) >> 4) | (L & 0xff);
      } else {
        if ((uint32_t(Raw) & 0x0ff00000) != (High ? 0x03400000 : 0x03000000))
          return fail("ARM HALF does not name the selected ARM MOV");
        EncodedHalf = ((Raw >> 4) & 0xf000) | (Raw & 0xfff);
      }
      const uint32_t Encoded =
          High ? (uint32_t(EncodedHalf) << 16) | R.OtherHalf
               : (uint32_t(R.OtherHalf) << 16) | EncodedHalf;
      if (!R.SubtractVA)
        return fail(
            "absolute ARM HALF instructions need dyld address registration");
      auto Sub = mapOldVA(Object, *R.SubtractVA);
      if (!Sub)
        return Sub.takeError();
      V -= llvm::APInt(128, *Sub);
      const uint32_t Addend = Encoded - uint32_t(R.ScatteredVA - *R.SubtractVA);
      V += llvm::APInt(32, Addend).sext(128);
      if (!V.isSignedIntN(32) && !V.isIntN(32))
        return fail("ARM HALF difference overflows");
      const uint32_t Value = V.getLoBits(32).getZExtValue();
      const uint16_t Part = High ? Value >> 16 : Value;
      if (Thumb) {
        const uint16_t H = Raw, L = Raw >> 16;
        writeLE<uint16_t>(Out, (H & ~0x040f) | ((Part >> 12) & 0xf) |
                                   ((Part >> 1) & 0x0400));
        writeLE<uint16_t>(Out + 2, (L & ~0x70ff) | ((Part << 4) & 0x7000) |
                                       (Part & 0xff));
      } else
        writeLE<uint32_t>(Out, (uint32_t(Raw) & ~0x000f0fff) |
                                   ((uint32_t(Part) & 0xf000) << 4) |
                                   (Part & 0xfff));
    } else if (R.SubtractVA || R.SubtractSymbol) {
      auto Sub = R.SubtractVA
                     ? mapOldVA(Object, *R.SubtractVA)
                     : resolve(Object, *R.SubtractSymbol, Original, Bindings);
      if (!Sub)
        return Sub.takeError();
      V -= llvm::APInt(128, *Sub);
      V += llvm::APInt(R.Width * 8, Raw).sext(128);
      if (R.SubtractVA) {
        V -= llvm::APInt(128, R.ScatteredVA);
        V += llvm::APInt(128, *R.SubtractVA);
      }
      if (!V.isSignedIntN(R.Width * 8))
        return fail("native section difference overflows");
      if (R.Width == 8)
        writeLE<uint64_t>(Out, V.getLoBits(64).getZExtValue());
      else
        writeLE<uint32_t>(Out, V.getLoBits(32).getZExtValue());
    } else if (Vanilla) {
      if (R.PCRel) {
        V += llvm::APInt(128, uint64_t(int64_t(int32_t(Raw))), true);
        unsigned Bias = R.Type == X86_64_RELOC_SIGNED_1   ? 1
                        : R.Type == X86_64_RELOC_SIGNED_2 ? 2
                        : R.Type == X86_64_RELOC_SIGNED_4 ? 4
                                                          : 0;
        if (!R.External || A == Arch::X86) {
          V += llvm::APInt(128, OldP + R.Width);
          if (!R.External)
            V -= llvm::APInt(128, R.Scattered
                                      ? R.ScatteredVA
                                      : Object.OldSectionVAs.at(R.SymbolIndex));
        }
        V -= llvm::APInt(128, P + R.Width + Bias);
        if (!V.isSignedIntN(32))
          return fail("native PC-relative relocation overflows");
        writeLE<uint32_t>(Out, V.getLoBits(32).getZExtValue());
      } else {
        V += llvm::APInt(128, Raw);
        if (!R.External)
          V -= llvm::APInt(128, R.Scattered
                                    ? R.ScatteredVA
                                    : Object.OldSectionVAs.at(R.SymbolIndex));
        if (!V.isIntN(R.Width * 8))
          return fail("native pointer relocation overflows");
        if (R.Width == 8)
          writeLE<uint64_t>(Out, V.getLoBits(64).getZExtValue());
        else
          writeLE<uint32_t>(Out, V.getLoBits(32).getZExtValue());
        if (S.IsAllocated && S.IsInImage)
          return fail(
              "native absolute pointer needs dyld relocation registration");
      }
    } else if (A == Arch::AArch64 &&
               (R.Type == ARM64_RELOC_PAGE21 ||
                R.Type == ARM64_RELOC_PAGEOFF12 || GOT ||
                TLV != Object.TLV.end()) &&
               R.Width == 4) {
      uint32_t Old = Raw;
      if (isTLVLoad(R.Type)) {
        const bool Page = R.Type == ARM64_RELOC_TLVP_LOAD_PAGE21;
        const auto RegisterFields = Page ? aarch64::kRdMask : kTLVLoadRegisters;
        const auto Op = Page ? aarch64::kADRP_Op : kTLVDescriptorLoad;
        if (TLV == Object.TLV.end() || (Old & ~RegisterFields) != Op)
          return fail(kTLVInstruction);
        // dyld owns the local descriptor. Relax its indirect load to the
        // address calculation that the Darwin linker uses for this export.
        if (!Page && TLV->second.Direct) {
          if ((Old & aarch64::kRdMask) == aarch64::kRdMask)
            return fail(kTLVInstruction);
          Old = kAddImmediate64 | (Old & RegisterFields);
        }
      }
      if (R.Type == ARM64_RELOC_PAGE21 ||
          R.Type == ARM64_RELOC_GOT_LOAD_PAGE21 ||
          R.Type == ARM64_RELOC_TLVP_LOAD_PAGE21) {
        if (!R.PCRel || (Old & 0x9f000000) != 0x90000000 || !R.External)
          return fail("PAGE21 requires a symbolic ADRP");
        V += llvm::APInt(
            128,
            uint64_t(llvm::SignExtend64<21>((((Old >> 5) & 0x7ffff) << 2) |
                                            ((Old >> 29) & 3)) *
                     4096),
            true);
        V &= ~llvm::APInt(128, 4095);
        V -= llvm::APInt(128, P & ~uint64_t(4095));
        if (!V.isSignedIntN(33))
          return fail("native PAGE21 overflows");
        const uint32_t I = V.getLoBits(33).getZExtValue() >> 12;
        writeLE<uint32_t>(Out, (Old & 0x9f00001f) | ((I & 3) << 29) |
                                   (((I >> 2) & 0x7ffff) << 5));
      } else {
        if (R.PCRel || !R.External)
          return fail("PAGEOFF12 requires a symbolic unshifted address");
        unsigned Scale = 0;
        if ((Old & 0x3b000000) == 0x39000000) {
          Scale = Old >> 30;
          if (!Scale && (Old & 0x04800000) == 0x04800000)
            Scale = 4;
        } else if ((Old & 0x7fc00000) != 0x11000000)
          return fail("PAGEOFF12 does not name an unsigned load/store or "
                      "unshifted ADD");
        V += llvm::APInt(128, uint64_t((Old >> 10) & 0xfff) << Scale);
        const uint32_t I = V.getLoBits(12).getZExtValue();
        if (I & ((1u << Scale) - 1))
          return fail("unaligned PAGEOFF12 access");
        writeLE<uint32_t>(Out, (Old & ~0x003ffc00) | ((I >> Scale) << 10));
      }
    } else if (A == Arch::ARM && R.Type == ARM_THUMB_RELOC_BR22 && R.PCRel &&
               R.Width == 4) {
      const uint16_t H = readLE<uint16_t>(Out), L = readLE<uint16_t>(Out + 2);
      if ((H & 0xf800) != 0xf000 || (L & 0xf800) != 0xf800)
        return fail("BR22 does not name the Darwin Thumb branch pair");
      const int64_t Addend =
          llvm::SignExtend64<23>(((H & 0x7ff) << 12) | ((L & 0x7ff) << 1));
      V += llvm::APInt(128, uint64_t(Addend), true);
      if (!R.External) {
        V += llvm::APInt(128, OldP + 4);
        V -= llvm::APInt(128, Object.OldSectionVAs.at(R.SymbolIndex));
      } else
        V += llvm::APInt(128, OldP + 4);
      V.clearBit(0);
      const uint64_t TargetVA = V.getLoBits(64).getZExtValue();
      const bool Thumb =
          llvm::any_of(Object.Symbols,
                       [&](const auto &S0) {
                         const auto &S = S0.second;
                         return S.Function && S.Thumb &&
                                Object.Image.SymbolAddrs.at(S.Name) == TargetVA;
                       }) ||
          (Original.instructionModeAt(TargetVA) == InstructionMode::Thumb) ||
          llvm::any_of(Object.Thunks, [&](const auto &T) {
            return Object.Image.Sections[T.second.Section].VA == TargetVA;
          });
      if (!Thumb)
        return fail("BR22 requires a verified Thumb target");
      V -= llvm::APInt(128, P + 4);
      if (!V.isSignedIntN(23) || V.getLoBits(1).getZExtValue())
        return fail("BR22 displacement overflows");
      const uint32_t D = V.getLoBits(23).getZExtValue();
      writeLE<uint16_t>(Out, (H & 0xf800) | ((D >> 12) & 0x7ff));
      writeLE<uint16_t>(Out + 2, (L & 0xf800) | ((D >> 1) & 0x7ff));
    } else if (A == Arch::AArch64 && R.Type == ARM64_RELOC_BRANCH26 &&
               R.PCRel && R.Width == 4) {
      const uint32_t Old = Raw;
      if (!branch::A64BranchOrLink.matches(Old))
        return fail("BRANCH26 does not name B/BL");
      const auto Addend = branch::a64BranchDisplacement(Old);
      if (R.External && Object.AuthenticatedCallStubs.contains(R.SymbolIndex) &&
          (R.InstructionAddend || Addend))
        return fail(kImportStubAddend);
      V += llvm::APInt(kRelocationBits, uint64_t(Addend), true);
      V -= llvm::APInt(128, P);
      if (!V.isSignedIntN(28) || (V.getLoBits(2).getZExtValue()))
        return fail("native branch overflows");
      writeLE<uint32_t>(
          Out, (Old & 0xfc000000) |
                   ((V.getLoBits(28).getZExtValue() >> 2) & 0x3ffffff));
    } else
      return fail("unsupported native relocation " + llvm::Twine(R.Type));
  }
  for (size_t I : Object.ThreadInitializers)
    for (uint64_t Off = 0; Off < Object.Image.Sections[I].Size;
         Off += arm64e::kPointerBytes)
      if (!Written.contains({I, Off}))
        return fail(kThreadInitPointers);
  if (Object.InitializerPointers)
    for (uint64_t Off = 0;
         Off < Object.Image.Sections[*Object.InitializerPointers].Size;
         Off += arm64e::kPointerBytes)
      if (!Written.contains({*Object.InitializerPointers, Off}))
        return fail(kInitializerStorage);
  if (Object.Descriptors) {
    const auto &S = Object.Image.Sections[*Object.Descriptors];
    for (uint64_t Off = 0; Off < S.Size; Off += kTLVDescriptorBytes)
      if (!Written.contains({*Object.Descriptors, Off}) ||
          !Written.contains({*Object.Descriptors, Off + kTLVOffsetField}) ||
          readLE<uint64_t>(Object.Image.Bytes.data() + S.Offset + Off +
                           arm64e::kPointerBytes))
        return fail(kTLVStorage);
    for (const auto &Sec : Original.Sections)
      if ((Sec.Type & SECTION_TYPE) == S_THREAD_LOCAL_VARIABLES &&
          (Sec.VA > Object.Runtime.Template.VA ||
           Object.Runtime.Template.VA - Sec.VA > UINT32_MAX))
        return fail(kTLVStorage);
  }
  if (Object.EH && Object.CompactOnly)
    for (const auto &Record : Object.OldFDEs)
      if (Record.HasLanguageAugmentation && !Original.isArm64e())
        return fail(
            "native DWARF language metadata requires a separate contract");
  if (Object.EH && !Object.CompactOnly) {
    auto &EH = Object.Image.Sections[*Object.EH];
    for (const auto &R : Object.OldFDEs) {
      if ((R.HasLanguageAugmentation && !Original.isArm64e()) ||
          ((R.InitialLocationEncoding & 0x70) != 0x10) ||
          (R.InitialLocationEncoding & 0x80) ||
          (R.InitialLocationWidth != 4 && R.InitialLocationWidth != 8))
        return fail("native DWARF language or address encoding requires a "
                    "separate contract");
      if (R.Personality) {
        const auto &P = *R.Personality;
        auto Writer = Written.find({*Object.EH, P.Offset});
        if (P.Encoding != (dweh::Indirect | dweh::PCRel | dweh::Sdata4) ||
            Writer == Written.end() || Writer->second != P.Width)
          return fail(kLanguagePointer);
      }
      if (R.LSDA && R.LSDA->Value) {
        const auto &L = *R.LSDA;
        if (dweh::getApplication(L.Encoding) != dweh::PCRel ||
            (L.Encoding & dweh::Indirect) ||
            (L.Width != sizeof(uint32_t) && L.Width != sizeof(uint64_t)))
          return fail(kLanguagePointer);
        if (!Written.contains({*Object.EH, L.Offset})) {
          auto Address = mapOldVA(Object, L.Value);
          if (!Address)
            return Address.takeError();
          const auto Delta = llvm::APInt(kRelocationBits, *Address) -
                             llvm::APInt(kRelocationBits, EH.VA + L.Offset);
          if (!Delta.isSignedIntN(L.Width * CHAR_BIT))
            return fail(kLanguagePointer);
          auto *Field = EH.ExternalBytes.data() + L.Offset;
          if (L.Width == sizeof(uint64_t))
            writeLE<uint64_t>(Field, Delta.getLoBits(64).getZExtValue());
          else
            writeLE<uint32_t>(Field, Delta.getLoBits(32).getZExtValue());
        }
      }
      auto New = mapOldVA(Object, R.BeginVA);
      if (!New)
        return New.takeError();
      llvm::APInt D(128, *New);
      D -= llvm::APInt(128, EH.VA + R.InitialLocationOffset);
      if (!D.isSignedIntN(R.InitialLocationWidth * 8))
        return fail("native FDE initial location overflows");
      auto *Field = EH.ExternalBytes.data() + R.InitialLocationOffset;
      if (R.InitialLocationWidth == 8)
        writeLE<uint64_t>(Field, D.getLoBits(64).getZExtValue());
      else
        writeLE<uint32_t>(Field, D.getLoBits(32).getZExtValue());
    }
  }
  std::map<uint64_t, uint64_t> Extents;
  if (Object.CompactSection) {
    const auto &Data =
        Object.Image.Sections[*Object.CompactSection].ExternalBytes;
    const unsigned Ptr = Object.Image.PointerWidth,
                   Stride = Ptr * kCompactPointers + kCompactFixedBytes;
    if (Data.size() % Stride)
      return fail("truncated native compact records");
    for (size_t O = 0; O < Data.size(); O += Stride) {
      const uint64_t Begin = Ptr == 8 ? readLE<uint64_t>(Data.data() + O)
                                      : readLE<uint32_t>(Data.data() + O);
      const uint32_t Length = readLE<uint32_t>(Data.data() + O + Ptr),
                     Encoding = readLE<uint32_t>(Data.data() + O + Ptr + 4);
      const uint64_t Personality =
          Ptr == 8 ? readLE<uint64_t>(Data.data() + O + Ptr + 8)
                   : readLE<uint32_t>(Data.data() + O + Ptr + 8);
      const uint64_t LSDA =
          Ptr == 8 ? readLE<uint64_t>(Data.data() + O + 2 * Ptr + 8)
                   : readLE<uint32_t>(Data.data() + O + 2 * Ptr + 8);
      const auto HasPointerWriter = [&](uint64_t Offset) {
        auto It = Written.find({*Object.CompactSection, Offset});
        return It != Written.end() && It->second == Ptr;
      };
      if (!Length || Begin > UINT64_MAX - Length ||
          ((Personality || LSDA) && !Original.isArm64e()) ||
          (Personality && !HasPointerWriter(O + Ptr + 8)) ||
          (LSDA && !HasPointerWriter(O + 2 * Ptr + 8)) ||
          (Encoding & macho_unwind::kPersonalityMask) ||
          bool(LSDA) != bool(Encoding & macho_unwind::kHasLSDA) ||
          (LSDA && !Personality) ||
          !Extents.emplace(Begin, Begin + Length).second)
        return fail(kCompactLanguage);
      NativeMachOCompactRange Range{Begin, Begin + Length, Encoding};
      if (Personality)
        Range.PersonalitySlotVA = Personality;
      if (LSDA)
        Range.LSDAVA = LSDA;
      const uint32_t Mode = Encoding & macho_unwind::kModeMask;
      const bool Dwarf =
          (A == Arch::AArch64 ? Mode == macho_unwind::kARM64ModeDwarf
                              : Mode == macho_unwind::kX86ModeDwarf);
      if (Dwarf && Object.CompactOnly)
        return fail(
            "native DWARF requires a registered original eh_frame section");
      Object.Compact.push_back(Range);
    }
  }
  if (Object.EH && !Object.CompactOnly) {
    const auto &EH = Object.Image.Sections[*Object.EH];
    auto Records = decodeDwarfEHFrameRecords(EH.ExternalBytes, EH.VA,
                                             Object.Image.PointerWidth == 8);
    if (!Records)
      return Records.takeError();
    if (Original.Arch == Arch::AArch64 &&
        (Original.MachOCPUSubtype & ~CPU_SUBTYPE_MASK) == arm64e::kCPUSubtype)
      normalizeClassicReturnCFI(Original, Object, *Records);
    for (const auto &R : *Records) {
      auto [It, Added] = Extents.emplace(R.BeginVA, R.EndVA);
      if (!Added && It->second != R.EndVA)
        return fail("native DWARF and compact extents disagree");
    }
    // Placement owns the index base for both registered and private frames.
    const va_t SectionVA = Object.EHSectionVA;
    for (const auto &R : *Records) {
      auto Compact = llvm::find_if(Object.Compact, [&](const auto &C) {
        return C.BeginVA == R.BeginVA;
      });
      if (Compact != Object.Compact.end() &&
          (Compact->Encoding & macho_unwind::kModeMask) !=
              (A == Arch::AArch64 ? macho_unwind::kARM64ModeDwarf
                                  : macho_unwind::kX86ModeDwarf))
        continue;
      if (R.RecordVA < SectionVA ||
          R.RecordVA - SectionVA > macho_unwind::kDwarfSectionOffsetMask)
        return fail(kFDEIndexOverflow);
      NativeMachOCompactRange Range{R.BeginVA, R.EndVA,
                                    (A == Arch::AArch64
                                         ? macho_unwind::kARM64ModeDwarf
                                         : macho_unwind::kX86ModeDwarf) |
                                        uint32_t(R.RecordVA - SectionVA)};
      if (R.Personality && R.Personality->Value)
        Range.PersonalitySlotVA = R.Personality->Value;
      if (R.LSDA && R.LSDA->Value) {
        Range.LSDAVA = R.LSDA->Value;
        Range.Encoding |= macho_unwind::kHasLSDA;
      }
      if (Compact == Object.Compact.end())
        Object.Compact.push_back(Range);
      else {
        // Native DWARF fallback rows may omit language fields entirely;
        // their CIE/FDE owns those pointers. Any duplicated field must agree.
        if ((Compact->PersonalitySlotVA &&
             Compact->PersonalitySlotVA != Range.PersonalitySlotVA) ||
            (Compact->LSDAVA && Compact->LSDAVA != Range.LSDAVA))
          return fail(kLanguagePointer);
        *Compact = Range;
      }
    }
  }
  std::map<uint64_t, Symbol *> Functions;
  llvm::SmallVector<std::pair<Symbol *, Symbol *>> Aliases;
  for (auto &[I, S] : Object.Symbols)
    if (S.Function) {
      const auto &Section =
          Object.Image.Sections[Object.Sections.at(S.Section)];
      const uint64_t Begin = Section.VA + S.Offset;
      auto [Entry, Inserted] = Functions.emplace(Begin, &S);
      if (!Inserted) {
        // Clang gives its C++ TLS initializer and the per-variable init
        // entry the same body. Both names must share its actual unwind range.
        auto *Owner = Entry->second;
        if (!Original.isArm64e() || Owner->Section != S.Section ||
            Owner->Thumb != S.Thumb || !Extents.contains(Begin))
          return fail(kFunctionAlias);
        Aliases.emplace_back(&S, Owner);
      }
    }
  for (auto I = Functions.begin(); I != Functions.end(); ++I) {
    auto &S = *I->second;
    const auto &Section = Object.Image.Sections[Object.Sections.at(S.Section)];
    if (Extents.contains(I->first))
      S.Size = Extents.at(I->first) - I->first;
    else if (Object.SjLj) {
      auto Next = std::next(I);
      S.Size = (Next != Functions.end() && Next->second->Section == S.Section
                    ? Next->first
                    : Section.VA + Section.Size) -
               I->first;
    } else
      return fail("native function lacks registered unwind coverage");
    if (!S.Size || !rangeInBounds(S.Offset, S.Size, Section.Size))
      return fail("native function extent exceeds code section");
  }
  for (auto [Alias, Owner] : Aliases)
    Alias->Size = Owner->Size;
  for (const auto &[Begin, End] : Extents)
    if (!Functions.contains(Begin) || Functions.at(Begin)->Size != End - Begin)
      return fail("native unwind has no exact function symbol");
  return llvm::Error::success();
}

llvm::Error verifyClassicReturnCFI(const BinaryImage &Image,
                                   const ExceptionFunction &Function,
                                   const AArch64ClassicReturnCFI &Source) {
  constexpr auto Word = aarch64::kInsnSize;
  const auto CodeWord = [&](va_t VA) -> std::optional<uint32_t> {
    const auto *Bytes = Image.readVA(VA, Word);
    if (VA % Word || !Bytes || !Image.isCodeRange(VA, Word))
      return std::nullopt;
    return readLE<uint32_t>(Bytes);
  };
  const va_t Start = Function.CodeRange.Begin;
  if (CodeWord(Start) != static_cast<uint32_t>(PAuthEncoding::PACIBSP))
    return fail(kClassicReturnCFI);
  if (Source.Epilogues.empty())
    return llvm::Error::success();
  auto Rows = recoverDarwinAArch64ObjectUnwind(Image, Function, Source);
  if (!Rows)
    return llvm::joinErrors(fail(kClassicReturnCFI), Rows.takeError());
  for (uint64_t Offset : Source.Epilogues) {
    const auto PC = Start + Offset;
    const auto Exit = CodeWord(PC);
    if (CodeWord(PC - Word) != static_cast<uint32_t>(PAuthEncoding::AUTIBSP) ||
        !Exit)
      return fail(kClassicReturnCFI);
    // Recovery follows local register targets and checks dynamic re-entry.
    if (*Exit != macho_arm64e_abi::kReturnToLR &&
        !branch::A64BranchRegister.matches(*Exit) &&
        (!branch::A64Branch.matches(*Exit) ||
         !branch::a64BranchTarget(*Exit, PC)))
      return fail(kClassicReturnCFI);
    const auto &Signed = aarch64UnwindRowAt(*Rows, Offset - Word);
    const auto &Caller = aarch64UnwindRowAt(*Rows, Offset);
    using namespace dwarf_eh;
    if (Signed.StackOffset != 0 || Caller.StackOffset != 0 ||
        !Signed.Signing.State || !Caller.Signing.State ||
        Signed.Signing.State->Kind != AArch64RASigningKind::SP ||
        Signed.Signing.State->Key != AArch64RAKey::B ||
        Caller.Signing.State->Kind != AArch64RASigningKind::Unsigned ||
        Signed.Registers[aarch64_unwind::kLR].Location !=
            AArch64UnwindRegister::SameValue ||
        Caller.Registers[aarch64_unwind::kLR].Location !=
            AArch64UnwindRegister::SameValue)
      return fail(kClassicReturnCFI);
  }
  return llvm::Error::success();
}

llvm::Error verifyNativeObjectUnwind(const BinaryImage &Image,
                                     const NativeMachO &Object) {
  const auto Native = macho_unwind::parseCompactUnwind(Image);
  if (Native.ParseStatus != ExceptionParseStatus::Complete)
    return fail(kPublishedCompact);
  for (const auto &Range : Object.Compact) {
    const auto It = llvm::find_if(Native.Entries, [&](const auto &Entry) {
      return Entry.CodeRange.Begin == Range.BeginVA &&
             Entry.CodeRange.End == Range.EndVA;
    });
    if (It == Native.Entries.end() ||
        It->SemanticStatus != CompactUnwindSemanticStatus::Complete ||
        (It->NativeEncoding & ~macho_unwind::kPersonalityMask) !=
            (Range.Encoding & ~macho_unwind::kPersonalityMask))
      return fail(kPublishedRange);
    if (Range.PersonalitySlotVA.has_value() !=
            bool(It->NativeEncoding & macho_unwind::kPersonalityMask) ||
        It->PersonalitySlotVA != Range.PersonalitySlotVA.value_or(0))
      return fail(kLanguageReplay);
    const bool Arm64eDwarf =
        Image.isArm64e() && (Range.Encoding & macho_unwind::kModeMask) ==
                                macho_unwind::kARM64ModeDwarf;
    if (!Range.PersonalitySlotVA && !Range.LSDAVA && !Arm64eDwarf)
      continue;
    const auto *Function = Image.ExceptionMetadata.findFunction(Range.BeginVA);
    if (!Function || Function->CodeRange.Begin != Range.BeginVA ||
        Function->CodeRange.End != Range.EndVA ||
        Function->ParseStatus != ExceptionParseStatus::Complete ||
        (Range.LSDAVA &&
         (Function->HandlerDataVA != *Range.LSDAVA || !Function->Itanium)))
      return fail(kLanguageReplay);
    if (Arm64eDwarf && !Function->Dwarf)
      return fail(kPublishedRange);
    if (Function->Dwarf) {
      const auto *CIE =
          Image.ExceptionMetadata.findCIE(Function->Dwarf->CIESectionOffset);
      if (!CIE || (Range.PersonalitySlotVA &&
                   CIE->PersonalitySlotVA != *Range.PersonalitySlotVA))
        return fail(kLanguageReplay);
      if (Arm64eDwarf) {
        const auto HasClassicToggle = [](const auto &Program) {
          return llvm::any_of(Program, [](const auto &Instruction) {
            return Instruction.Kind == CFIOpKind::NegateRAState;
          });
        };
        // Classic Darwin already authenticates saved LR with the B key.
        // Any generic toggle still present lacks an equivalent implicit rule.
        if (HasClassicToggle(CIE->InitialInstructions) ||
            HasClassicToggle(Function->Dwarf->Instructions))
          return fail(kClassicReturnCFI);
        auto Translated = Object.ClassicReturnFrames.find(Range.BeginVA);
        if (Translated != Object.ClassicReturnFrames.end())
          if (auto E =
                  verifyClassicReturnCFI(Image, *Function, Translated->second))
            return E;
      }
    }
  }
  return llvm::Error::success();
}
} // namespace

llvm::Expected<CompiledImage>
appendMachOObjectImage(std::vector<uint8_t> &Binary, llvm::StringRef Bytes,
                       llvm::ArrayRef<SourcePatchDependency> Dependencies) {
  auto Before = readMachODependencyState(Binary);
  if (!Before)
    return Before.takeError();
  auto Reader = Loader::create(BinaryFormat::MachO);
  const auto Load = [&](llvm::ArrayRef<uint8_t> Data) {
    return Reader->loadBuffer(llvm::MemoryBufferRef(
        llvm::StringRef(reinterpret_cast<const char *>(Data.data()),
                        Data.size()),
        kNativeImage));
  };
  auto Original = Load(Binary);
  if (!Original)
    return Original.takeError();
  if (!Original->isArm64e() || Original->MachOSharedCache)
    return fail(kNativeStorage);
  auto Object = readObject(Bytes, *Original, macho_arm64e_abi::kTriple);
  if (!Object)
    return Object.takeError();
  std::vector<SourcePatchDependency> RuntimeDependencies(Dependencies.begin(),
                                                         Dependencies.end());
  if (auto E =
          prepareRuntimeDependencies(*Object, *Original, RuntimeDependencies))
    return E;
  Dependencies = RuntimeDependencies;
  // Newly allocated GOT slots need segment and section commands. Resolve
  // reuse against the input image before reserving the publication header.
  if (auto Error = prepareArm64eGOT(*Object, *Original, Dependencies))
    return Error;
  auto Candidate = Binary;
  const uint64_t ExtraEH = requiredNativeEHBytes(*Object, Candidate);
  std::optional<MachODependencyState> SlidDependencies;
  const auto CommandBytes =
      installationCommandBytes(*Object, *Original) +
      machOSourceLibraryCommandBytes(*Original, Dependencies);
  auto Slide = reserveMachOPublishedUnwind(
      Candidate, Original->Arch, ExtraEH, unwindFunctionCounts(*Object),
      CommandBytes != 0, SlidDependencies, CommandBytes);
  if (!Slide)
    return Slide.takeError();
  MachOPatcher Patcher;
  Patcher.setInputKind(RewriteInputKind::ReplacementObject);
  const auto Base = Patcher.plannedExecSegmentVA(Candidate, Original->Arch);
  if (!Base)
    return fail(kNativePlacement);
  const auto EH = findMachOEHFrameRegion(Candidate);
  auto Layout = Load(Candidate);
  if (!Layout)
    return Layout.takeError();
  scanImportThunks(*Layout);
  if (auto Error = prepareThreadLocals(*Object, *Layout))
    return Error;
  if (auto Error = prepareMainRelro(*Object, Candidate, *Layout))
    return Error;
  if (auto Error = place(*Object, Base, *Slide, EH))
    return Error;
  if (auto Error = relocate(*Object, *Layout, {}))
    return Error;
  if (Object->EH && !Object->CompactOnly)
    if (auto Error = installNativeMachOEHFrame(
            Candidate, *EH, Object->Image.Sections[*Object->EH],
            Object->Compact))
      return Error;
  if (auto Error = installNativeMachOCompactUnwind(Candidate, Original->Arch,
                                                   Object->Compact))
    return Error;
  if (auto Error = Patcher.appendReplacementImage(
          Candidate, Object->Image, Object->TemplateFixups, Dependencies,
          (Object->Descriptors || Object->PerThread ||
           Object->InitializerOffsets)
              ? &Object->Runtime
              : nullptr))
    return Error;
  if (auto Error =
          installMachOPublishedData(Candidate, Object->Image, Object->Embedded))
    return Error;
  if (auto Error =
          installMachOThreadLocalExports(Candidate, Object->TLSExports))
    return Error;
  auto After = readMachODependencyState(Candidate);
  if (!After)
    return After.takeError();
  if (auto Error = preserveMachODependencyState(
          SlidDependencies ? *SlidDependencies : *Before, *After,
          Object->TLSExports, Object->TemplateFixups))
    return Error;
  auto Installed = Load(Candidate);
  if (!Installed)
    return Installed.takeError();
  if (auto Error = verifyNativeObjectUnwind(*Installed, *Object))
    return Error;
  Binary.swap(Candidate);
  return std::move(Object->Image);
}

namespace {
// Keep each site at its verified instruction offset. One-word sites may use a
// dyld pointer cell. Page pairs carry both original register operands.
struct LiftedAddressPlacement {
  const Symbol *Owner;
  uint64_t Offset;
  uint32_t SourceWord;
  uint32_t Word;
  std::optional<uint64_t> PoolOffset;
};

struct LiftedAddressPlan {
  std::vector<LiftedAddressPlacement> Sites;
  std::vector<va_t> PoolInstructions;
  size_t TextSection = 0;
  va_t TextVA = 0;
  std::optional<size_t> PoolSection;
};

llvm::Expected<LiftedAddressPlan>
prepareLiftedAddresses(NativeMachO &Object, const BinaryImage &Original,
                       va_t Base, va_t Slide,
                       llvm::ArrayRef<SourcePatchMapping> Mappings,
                       llvm::ArrayRef<MachOLiftedSourceAddress> Addresses) {
  using namespace aarch64;
  LiftedAddressPlan Plan;
  if (Addresses.empty())
    return Plan;
  Plan.Sites.reserve(Addresses.size());
  // The lifted-image verifier accepts a single contiguous text section.
  // place() lays code first, before any RELRO or external unwind storage.
  const auto &Sections = Object.Image.Sections;
  auto Text = llvm::find_if(Sections, [](const auto &S) {
    return S.Kind == Kind::Code && S.IsAllocated && S.IsInImage;
  });
  if (Text == Sections.end())
    return fail(kAddressSourceWord);
  Plan.TextSection = size_t(Text - Sections.begin());
  Plan.TextVA = llvm::alignTo(Base, Text->Alignment);
  CompiledSection Pool;
  Pool.Name = macho_arm64e_abi::kSourceAddressPool;
  Pool.Kind = Kind::WritableData;
  Pool.Alignment = arm64e::kPointerBytes;
  Pool.ReadOnlyAfterFixups = true;
  std::map<va_t, uint64_t> Slots;
  uint64_t AddedBytes = 0;
  for (const auto &Fixup : Addresses) {
    while (Mappings.size() > 1 && Mappings[1].OriginalVA <= Fixup.Instruction) {
      Mappings = Mappings.drop_front();
      AddedBytes = 0;
    }
    if (Mappings.empty() || Fixup.Instruction < Mappings.front().OriginalVA)
      return fail(kAddressSourceWord);
    const auto &Entry = Mappings.front();
    const auto &Sym = Object.Symbols.at(Object.Names.at(Entry.Symbol));
    const uint64_t Offset = Fixup.Instruction - Entry.OriginalVA + AddedBytes;
    const bool Pair = Fixup.Form == MachOAddressForm::PagePair;
    const bool Expanded = Fixup.Form == MachOAddressForm::Expanded;
    if (Expanded)
      AddedBytes += kInsnSize;
    const auto *Before =
        Original.readVA(Fixup.Instruction, Pair ? 2 * kInsnSize : kInsnSize);
    if (!Before || Object.Sections.at(Sym.Section) != Plan.TextSection ||
        !rangeInBounds(Offset, Pair || Expanded ? 2 * kInsnSize : kInsnSize,
                       Text->Size - Sym.Offset) ||
        Fixup.Target > InvalidVA - Slide)
      return fail(kAddressSourceWord);
    const uint32_t Word = readLE<uint32_t>(Before);
    const uint64_t At = Sym.Offset + Offset;
    if (Pair ? (Word & kADRP_Mask) != kADRP_Op
             : (Word & kADRP_Mask) != kADR_Op &&
                   machORebasedLiteralAddress(
                       Original, Word, Fixup.Instruction) != Fixup.Target)
      return fail(kAddressSourceWord);
    const va_t PC = Plan.TextVA + At;
    const va_t Target = Fixup.Target + Slide;
    const uint32_t Rd = Word & kRdMask;
    if (Pair || Expanded) {
      const auto Page =
          encodePCRelativeAddress(AddressInstruction::ADRP, Rd, PC, Target);
      if (!Page)
        return fail(kAddressPageReach);
      const uint32_t OriginalAdd =
          Pair ? readLE<uint32_t>(Before + kInsnSize) : Word;
      const auto Imm = branch::A64AddImm12;
      // An existing pair may use different page and result registers. A
      // widened single instruction must write only its original destination.
      const uint32_t Add =
          Pair ? OriginalAdd & ~Imm.mask()
               : kAddImmediate64 | (Rd << kAddressBaseRegisterShift) | Rd;
      Plan.Sites.push_back({&Sym, At, Expanded ? kNop : Word, *Page, {}});
      Plan.Sites.push_back({&Sym,
                            At + kInsnSize,
                            OriginalAdd,
                            Add | uint32_t((Target & ~kPageMask) << Imm.Low),
                            {}});
      continue;
    }
    auto &Site = Plan.Sites.emplace_back(
        LiftedAddressPlacement{&Sym, At, Word, Rd, std::nullopt});
    if (auto ADR =
            encodePCRelativeAddress(AddressInstruction::ADR, Rd, PC, Target)) {
      Site.Word = *ADR;
      continue;
    }
    if (isPointerLiteralLoad(Word)) {
      // On a later publication dyld already owns this immutable cell.
      const auto Old = decodeLiteralLoad(Word, Fixup.Instruction)->Address;
      if (Old <= InvalidVA - Slide)
        if (const auto Load = encodePointerLiteralLoad(Rd, PC, Old + Slide)) {
          Site.Word = *Load;
          continue;
        }
    }
    const auto [It, Added] = Slots.try_emplace(Target, Pool.Size);
    if (Added) {
      CompiledFixupReference Ref;
      Ref.Offset = It->second;
      Ref.BitWidth = arm64e::kPointerBytes * CHAR_BIT;
      Ref.IsResolved = Ref.OrdinaryPointer = true;
      Ref.ResolvedValue = Target;
      Pool.FixupReferences.push_back(std::move(Ref));
      Pool.Size += arm64e::kPointerBytes;
    }
    Site.PoolOffset = It->second;
    Plan.PoolInstructions.push_back(Fixup.Instruction);
  }
  if (Pool.Size) {
    Pool.ExternalBytes.resize(Pool.Size);
    Plan.PoolSection = Sections.size();
    Object.Image.Sections.push_back(std::move(Pool));
  }
  return Plan;
}

llvm::Error relocateLiftedAddresses(NativeMachO &Object,
                                    const LiftedAddressPlan &Plan) {
  using namespace aarch64;
  if (Plan.Sites.empty())
    return llvm::Error::success();
  const auto &Text = Object.Image.Sections[Plan.TextSection];
  if (Text.VA != Plan.TextVA)
    return fail(kAddressSourceWord);
  for (const auto &Site : Plan.Sites) {
    // Object relocation establishes the native symbol's unwind extent.
    auto *After = Object.Image.Bytes.data() + Text.Offset + Site.Offset;
    if (!rangeInBounds(Site.Offset - Site.Owner->Offset, kInsnSize,
                       Site.Owner->Size) ||
        readLE<uint32_t>(After) != Site.SourceWord)
      return fail(kAddressSourceWord);
    uint32_t Word = Site.Word;
    if (Site.PoolOffset) {
      const va_t Slot =
          Object.Image.Sections[*Plan.PoolSection].VA + *Site.PoolOffset;
      const auto Load =
          encodePointerLiteralLoad(Word, Text.VA + Site.Offset, Slot);
      if (!Load)
        return llvm::make_error<MachOLiteralPoolOutOfRange>(
            Plan.PoolInstructions);
      Word = *Load;
    }
    writeLE<uint32_t>(After, Word);
  }
  return llvm::Error::success();
}
} // namespace

char MachOLiteralPoolOutOfRange::ID;
void MachOLiteralPoolOutOfRange::log(llvm::raw_ostream &OS) const {
  OS << kObjectErrorPrefix << kAddressReach;
}
std::error_code MachOLiteralPoolOutOfRange::convertToErrorCode() const {
  return llvm::errc::not_supported;
}

static llvm::Expected<PatchResult> installObject(
    llvm::StringRef Bytes, const BinaryImage &OriginalInput,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<SourcePatchBinding> RequestedBindings,
    const std::filesystem::path &Output,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt,
    const DynCodePatchEvidence *DynCode,
    const SourcePatchPreparation *Preparation,
    llvm::ArrayRef<SourcePatchDependency> Dependencies,
    llvm::function_ref<llvm::Error(const CompiledImage &,
                                   llvm::ArrayRef<NativeMachOCompactRange>)>
        VerifyGenerated,
    MachOLiftedObjectVerifier VerifyLiftedImage,
    llvm::ArrayRef<MachODataInCodeRange> RecoveredData,
    llvm::ArrayRef<MachOLiftedSourceAddress> AddressesToRelocate) {
  const BinaryImage &Original =
      Preparation ? Preparation->Image : OriginalInput;
  if (Original.MachOSharedCache)
    return fail(kSharedCache);
  if (!Dependencies.empty() && !Original.isArm64e())
    return fail(kDependencyProfile);
  auto Triple = Original.isArm64e()
                    ? llvm::Expected<std::string>(macho_arm64e_abi::kTriple)
                    : sourcePatchTargetTriple(Original);
  if (!Triple)
    return Triple.takeError();
  auto Object = readObject(Bytes, Original, *Triple);
  if (!Object)
    return Object.takeError();
  std::vector<SourcePatchDependency> RuntimeDependencies(Dependencies.begin(),
                                                         Dependencies.end());
  if (auto E = prepareRuntimeDependencies(
          *Object, Original, RuntimeDependencies, RequestedBindings))
    return E;
  Dependencies = RuntimeDependencies;
  std::set<std::string> Entries;
  std::set<uint64_t> Addresses;
  llvm::DenseMap<va_t, const ExceptionFunction *> SigningFrames;
  for (const auto &M : Mappings) {
    if (!Object->Names.contains(M.Symbol) || !Entries.insert(M.Symbol).second ||
        !Addresses.insert(M.OriginalVA).second ||
        !Original.hasAuthenticatedFunctionEntryAt(M.OriginalVA) ||
        !sourceEntryCanHostTrampoline(Original, M.OriginalVA))
      return fail("invalid native entry mapping");
    const auto &S = Object->Symbols.at(Object->Names.at(M.Symbol));
    if (!S.Function || !S.Section)
      return fail("entry needs a native function definition");
    if (VerifyLiftedImage && !Object->OldFDEs.empty()) {
      const auto *Source =
          Original.ExceptionMetadata.findFunction(M.OriginalVA);
      if (Source && Source->Compact &&
          Source->Compact->ReturnAddressSigningOffset &&
          Source->CodeRange.Begin == M.OriginalVA)
        SigningFrames.try_emplace(
            Object->OldSectionVAs.at(S.Section) + S.Offset, Source);
    }
  }
  BinaryImage References = Original;
  scanImportThunks(References);
  std::map<std::string, SourcePatchBinding> Bindings;
  for (const auto &B : RequestedBindings) {
    if (!Object->Names.contains(B.Symbol) ||
        Object->Symbols.at(Object->Names.at(B.Symbol)).Section ||
        !Bindings.emplace(B.Symbol, B).second)
      return fail("binding needs a unique undefined native symbol");
    if (auto E = validateSourcePatchBinding(B, References, Mappings))
      return std::move(E);
  }
  MachOPatcher Patcher;
  Patcher.setInputKind(RewriteInputKind::ReplacementObject);
  auto LayoutBytes = Original.Raw;
  if (Preparation)
    if (auto E = prepareImports(*Object, Preparation->Imports))
      return E;
  if (Original.isArm64e())
    if (auto E = prepareArm64eGOT(*Object, Original, Dependencies,
                                  RequestedBindings))
      return E;
  // If every FDE has a source PAuth_LR compact owner, verify it in private
  // storage. A compact-only image then needs no DWARF section or header slide.
  const bool PrivateFrames =
      !SigningFrames.empty() && Object->Thunks.empty() &&
      llvm::all_of(Object->OldFDEs, [&](const auto &FDE) {
        return SigningFrames.contains(FDE.BeginVA);
      });
  const uint64_t ExtraEH =
      PrivateFrames ? 0 : requiredNativeEHBytes(*Object, LayoutBytes);
  std::optional<MachODependencyState> SlidDependencies;
  const auto LibraryCommands =
      machOSourceLibraryCommandBytes(Original, Dependencies);
  const auto Reserve = [&](uint64_t Commands) {
    return reserveMachOPublishedUnwind(
        LayoutBytes, Original.Arch, ExtraEH, unwindFunctionCounts(*Object),
        Original.isArm64e() && !Preparation && !DynCode, SlidDependencies,
        Commands + LibraryCommands, RecoveredData);
  };
  const auto InitialCommands = installationCommandBytes(*Object, Original);
  auto Slide = Reserve(InitialCommands);
  if (!Slide)
    return Slide.takeError();
  auto Base = Patcher.plannedExecSegmentVA(LayoutBytes, Original.Arch);
  auto AddressPlan = prepareLiftedAddresses(*Object, Original, Base, *Slide,
                                            Mappings, AddressesToRelocate);
  if (!AddressPlan)
    return AddressPlan.takeError();
  if (AddressPlan->PoolSection) {
    const auto Commands = installationCommandBytes(*Object, Original);
    if (Commands > InitialCommands) {
      // Only a distant operand calls for extra segment commands. Header
      // growth can move both ends, so plan against the actual new layout.
      Object->Image.Sections.pop_back();
      LayoutBytes = Original.Raw;
      SlidDependencies.reset();
      Slide = Reserve(Commands);
      if (!Slide)
        return Slide.takeError();
      Base = Patcher.plannedExecSegmentVA(LayoutBytes, Original.Arch);
      AddressPlan = prepareLiftedAddresses(*Object, Original, Base, *Slide,
                                           Mappings, AddressesToRelocate);
      if (!AddressPlan)
        return AddressPlan.takeError();
    }
  }
  for (auto &[Name, Binding] : Bindings) {
    Binding.OriginalVA += *Slide;
    // Explicitly bound symbols are defined by the input image. GOT slots
    // and pointer initializers for them require rebases to its final layout.
    if (Original.isArm64e())
      Object->Image.SymbolAddrs.emplace(Name, Binding.OriginalVA);
  }
  const auto EH = findMachOEHFrameRegion(LayoutBytes);
  if (!Base)
    return fail(kNoPlacement);
  auto LayoutReader = Loader::create(BinaryFormat::MachO);
  auto LayoutImage = LayoutReader->loadBuffer(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(LayoutBytes.data()),
                      LayoutBytes.size()),
      "object-layout"));
  if (!LayoutImage)
    return LayoutImage.takeError();
  scanImportThunks(*LayoutImage);
  if (auto E = prepareThreadLocals(*Object, *LayoutImage))
    return E;
  if (auto E = prepareMainRelro(*Object, LayoutBytes, *LayoutImage))
    return E;
  if (auto E = place(*Object, Base, *Slide, EH, PrivateFrames))
    return std::move(E);
  if (auto E = relocate(*Object, *LayoutImage, Bindings))
    return std::move(E);
  // A repeated code-only installation can extend the old RX segment. Adding
  // another in-image pool would also require a new RELRO segment. Return the
  // affected sites to the layout planner before publishing either mapping.
  if (AddressPlan->PoolSection &&
      Object->Image.Sections[*AddressPlan->PoolSection].IsInImage &&
      Original.findSegmentByName(kDefaultNdTextSegment))
    return llvm::make_error<MachOLiteralPoolOutOfRange>(
        AddressPlan->PoolInstructions);
  if (auto E = relocateLiftedAddresses(*Object, *AddressPlan))
    return E;
  if (!Object->Thunks.empty()) {
    if (!EH)
      return fail("import thunks need registered native DWARF storage");
    if (auto E = finalizeImports(*Object, *EH))
      return E;
  }
  if (!SigningFrames.empty() && !Object->CompactOnly) {
    llvm::DenseMap<va_t, const ExceptionFunction *> Placed;
    for (const auto &[OldVA, Source] : SigningFrames) {
      auto VA = mapOldVA(*Object, OldVA);
      if (!VA)
        return VA.takeError();
      Placed.try_emplace(*VA, Source);
    }
    auto &Frames = Object->Image.Sections[*Object->EH];
    if (auto E = restoreMachOLiftedCompactFrames(
            Original, *LayoutImage, Placed, Object->Image, *Slide,
            Object->EHSectionVA, Frames, Object->Compact))
      return E;
    Object->CompactOnly = Frames.ExternalBytes.empty();
    if (PrivateFrames && !Object->CompactOnly)
      return fail(kPrivateSigningFrames);
  }
  if (VerifyGenerated)
    if (auto E = VerifyGenerated(Object->Image, Object->Compact))
      return E;
  if (DynCode)
    if (auto E =
            validateDynCodePatchImage(*DynCode, Bytes, Original, Object->Image,
                                      Mappings, RequestedBindings))
      return std::move(E);
  std::vector<MachOAuthenticatedFixup> Fixups;
  return publishSourcePatch(
      OriginalInput, Mappings, Output,
      [&](const std::filesystem::path &Input,
          const std::filesystem::path &Candidate)
          -> llvm::Expected<PatchResult> {
        std::string Failure;
        auto R = BinaryPatcher::readPatchWrite(
            Input, Candidate, true, "object_patch",
            [&](std::vector<uint8_t> &Binary, PatchResult &Result) {
              auto Reject = [&](llvm::Error E) {
                Failure = llvm::toString(std::move(E));
                return false;
              };
              Binary = LayoutBytes;
              if (Object->EH && !Object->CompactOnly)
                if (auto E = installNativeMachOEHFrame(
                        Binary, *EH, Object->Image.Sections[*Object->EH],
                        Object->Compact))
                  return Reject(std::move(E));
              if (auto E = installNativeMachOCompactUnwind(
                      Binary, Original.Arch, Object->Compact))
                return Reject(std::move(E));
              if (auto E = Patcher.appendReplacementImage(
                      Binary, Object->Image, Object->TemplateFixups,
                      Dependencies,
                      (Object->Descriptors || Object->PerThread ||
                       Object->InitializerOffsets)
                          ? &Object->Runtime
                          : nullptr))
                return Reject(std::move(E));
              if (auto E = installMachOPublishedData(Binary, Object->Image,
                                                     Object->Embedded,
                                                     RecoveredData, *Slide))
                return Reject(std::move(E));
              if (auto E = installMachOThreadLocalExports(Binary,
                                                          Object->TLSExports))
                return Reject(std::move(E));
              if (Original.isArm64e()) {
                auto Collected = collectMachOAuthenticatedFixups(
                    Binary, Object->Image, Dependencies);
                if (!Collected)
                  return Reject(Collected.takeError());
                Fixups = std::move(*Collected);
                llvm::append_range(Fixups, Object->TemplateFixups);
              }
              for (const auto &M : Mappings) {
                const auto &S = Object->Symbols.at(Object->Names.at(M.Symbol));
                const auto Mode = Original.instructionModeAt(M.OriginalVA);
                const uint64_t EntryVA = M.OriginalVA + *Slide;
                const uint64_t Owner = Object->Image.SymbolAddrs.at(M.Symbol);
                if (!Mode ||
                    (Original.Arch == Arch::ARM &&
                     (S.Thumb ? InstructionMode::Thumb
                              : InstructionMode::ARM) != *Mode) ||
                    !getTargetCodegenInfo(Original.Arch, *Mode)
                         .writeSourceTrampoline(
                             Binary, LayoutImage->vaToFileOffset(EntryVA),
                             Owner, EntryVA,
                             getTargetCodegenInfo(Original.Arch, *Mode)
                                 .sourceTrampolineSize()))
                  return Reject(
                      fail("native entry mode or branch is incompatible"));
                Result.PatchedFunctions.push_back(
                    {M.Symbol, M.OriginalVA, Owner, M.Symbol});
                Result.PatchedOriginalEntries.push_back(M.OriginalVA);
              }
              llvm::sort(Result.PatchedOriginalEntries);
              Result.CodeSize = Object->Image.Bytes.size();
              Result.OriginalSectionSlide = *Slide;
              Result.TrampolineCount = Mappings.size();
              return true;
            });
        if (!R.Success)
          return fail(Failure.empty() ? "candidate write failed" : Failure);
        return R;
      },
      PrepareReceipt, Preparation,
      [&](const BinaryImage &Final) -> llvm::Error {
        if (auto E = verifyMachOAuthenticatedFixups(Final.Raw, Fixups))
          return E;
        // Dyld recipes replace producer-owned pointer placeholders. Only
        // these replayed slots may differ from the relocated object bytes.
        auto Expected = Object->Image;
        for (auto &S : Expected.Sections) {
          if (!S.IsAllocated)
            continue;
          for (const auto &F : Fixups) {
            if (F.Address < S.VA ||
                !rangeInBounds(F.Address - S.VA, arm64e::kPointerBytes, S.Size))
              continue;
            const auto *Bytes = Final.readVA(F.Address, arm64e::kPointerBytes);
            if (!Bytes)
              return fail(kPublishedPointer);
            auto *Storage = S.IsInImage ? Expected.Bytes.data() + S.Offset
                                        : S.ExternalBytes.data();
            std::copy_n(Bytes, arm64e::kPointerBytes,
                        Storage + F.Address - S.VA);
          }
        }
        for (const auto &S : Expected.Sections) {
          if (!S.IsAllocated || !S.Size ||
              (Object->CompactOnly && Object->EH &&
               &S == &Expected.Sections[*Object->EH]))
            continue;
          if (auto E = validateInstalledSourcePatchSection(Final, Expected, S))
            return E;
        }
        if (auto E = verifyNativeObjectUnwind(Final, *Object))
          return E;
        // LSDA references must be read from the installed candidate, including
        // newly generated tables. This still precedes atomic publication.
        if (VerifyLiftedImage) {
          const auto *Pool =
              AddressPlan->PoolSection
                  ? &Object->Image.Sections[*AddressPlan->PoolSection]
                  : nullptr;
          if (auto E = VerifyLiftedImage(Object->Image, Final, *Slide,
                                         Object->Compact, Pool))
            return E;
        }
        if (Original.isArm64e()) {
          auto After = readMachODependencyState(Final.Raw);
          if (!After)
            return After.takeError();
          if (SlidDependencies)
            return preserveMachODependencyState(*SlidDependencies, *After,
                                                Object->TLSExports,
                                                Object->TemplateFixups);
          auto Before = readMachODependencyState(Original.Raw);
          if (!Before)
            return Before.takeError();
          if (auto E = preserveMachODependencyState(
                  *Before, *After, Object->TLSExports, Object->TemplateFixups))
            return E;
        }
        return llvm::Error::success();
      });
}
llvm::Expected<PatchResult> installMachOObjectPatch(
    llvm::StringRef Object, const BinaryImage &Original,
    llvm::ArrayRef<SourcePatchMapping> Mappings,
    llvm::ArrayRef<SourcePatchBinding> Bindings,
    const std::filesystem::path &Output,
    llvm::function_ref<llvm::Error(const PatchResult &)> PrepareReceipt,
    const DynCodePatchEvidence *DynCode,
    const SourcePatchPreparation *Preparation,
    llvm::ArrayRef<SourcePatchDependency> Dependencies,
    llvm::function_ref<llvm::Error(const CompiledImage &,
                                   llvm::ArrayRef<NativeMachOCompactRange>)>
        VerifyGenerated) {
  return installObject(Object, Original, Mappings, Bindings, Output,
                       PrepareReceipt, DynCode, Preparation, Dependencies,
                       VerifyGenerated, {}, {}, {});
}

llvm::Expected<PatchResult>
installMachOLiftedObject(llvm::StringRef Object, const BinaryImage &Original,
                         llvm::ArrayRef<SourcePatchMapping> Mappings,
                         llvm::ArrayRef<SourcePatchBinding> Bindings,
                         const std::filesystem::path &Output,
                         llvm::ArrayRef<MachODataInCodeRange> RecoveredData,
                         llvm::ArrayRef<MachOLiftedSourceAddress> Addresses,
                         MachOLiftedObjectVerifier Verify) {
  return installObject(Object, Original, Mappings, Bindings, Output, {},
                       nullptr, nullptr, {}, {}, Verify, RecoveredData,
                       Addresses);
}
} // namespace neverd
