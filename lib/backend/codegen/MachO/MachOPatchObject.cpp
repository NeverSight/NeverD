//===- MachOPatchObject.cpp - Native Mach-O replacement installation -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachOArm64eABI.h"
#include "MachODependencyState.h"
#include "MachOLiftedObject.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/MachO/MachOAuthenticatedFixups.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/backend/codegen/MachO/MachOLayoutGrowth.h"
#include "neverd/backend/codegen/MachO/MachOPatch.h"
#include "neverd/backend/codegen/PatchObject.h"
#include "neverd/backend/codegen/PatchStorage.h"
#include "neverd/backend/codegen/SourcePatchPreparation.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/loader/MachO/MachOPlatform.h"
#include "neverd/loader/MachO/MachORelocations.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/support/DwarfEH.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/TargetCodegenInfo.h"

#include "llvm/ADT/DenseMap.h"
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
llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "Mach-O patch object: " + Message);
}
std::string canonical(llvm::StringRef Name) {
  return (Name.starts_with("_") ? Name.drop_front() : Name).str();
}
struct Symbol {
  std::string Name;
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
  std::vector<NativeMachOCompactRange> Compact;
  std::vector<DwarfEHFrameRecord> OldFDEs;
  std::optional<size_t> EH, CompactSection;
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
  // Section index to the sole public weak definition that owns its storage.
  std::map<size_t, unsigned> WeakData;
};

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
  return {Object.Symbols.size(), LSDAs};
}

bool isTLVLoad(unsigned Type) {
  return Type == ARM64_RELOC_TLVP_LOAD_PAGE21 ||
         Type == ARM64_RELOC_TLVP_LOAD_PAGEOFF12;
}

bool isGOTLoad(unsigned Type) {
  return Type == ARM64_RELOC_GOT_LOAD_PAGE21 ||
         Type == ARM64_RELOC_GOT_LOAD_PAGEOFF12 || isTLVLoad(Type);
}

bool isCompactPersonality(const NativeMachO &Object, const Relocation &R) {
  const unsigned Ptr = Object.Image.PointerWidth;
  return Object.CompactSection == R.Section &&
         R.Offset % (Ptr * kCompactPointers + kCompactFixedBytes) ==
             Ptr + kCompactFixedBytes;
}

// The arm64e unwind personality ABI uses an IA/address-diversified GOT slot;
// LSDA type information uses an ordinary data pointer. Keep these identities
// distinct even when a symbol appears in both roles.
llvm::Error
prepareArm64eGOT(NativeMachO &Object, const BinaryImage &Original,
                 llvm::ArrayRef<SourcePatchDependency> Dependencies) {
  std::set<std::string> Declared;
  for (const auto &D : Dependencies)
    Declared.insert(D.Symbol);
  const auto UsesGOT = [&](const Relocation &R) {
    return isCompactPersonality(Object, R) ||
           R.Type == ARM64_RELOC_POINTER_TO_GOT || isGOTLoad(R.Type);
  };
  if (Object.WeakData.empty() && llvm::none_of(Object.Relocations, UsesGOT))
    return llvm::Error::success();
  MachOChainedFixups Chains;
  auto State = readMachODependencyState(Original.Raw, &Chains);
  if (!State)
    return State.takeError();
  const auto Providers = Chains.uniqueImportProviders();
  const auto MatchesProvider = [&](const std::string &Name, const auto &B) {
    auto It = Providers.find(Name);
    return It != Providers.end() && It->second && B.LibraryOrdinal &&
           *It->second ==
               MachOChainedFixups::Provider{*B.LibraryOrdinal, B.WeakImport};
  };
  std::map<std::string, va_t> Personalities, Ordinary;
  for (const auto &[Address, P] : Original.MachOAuthenticatedPointers)
    if (!P.Symbol.empty() && !P.Addend && P.Key == kPersonalityKey &&
        P.Diversity == kPersonalityDiversity &&
        P.AddressDiversity == kPersonalityAddressDiversity &&
        MatchesProvider(P.Symbol, P))
      Personalities.try_emplace(canonical(P.Symbol), Address);
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
      Ordinary.try_emplace(canonical(B.Name), Address);
  }
  // A public weak fallback can be discarded only when all its users go
  // through the original coalescing GOT. The original definition and dyld
  // lookup scope then continue to choose the runtime address together.
  for (const auto &[Section, Index] : Object.WeakData) {
    const auto &Symbol = Object.Symbols.at(Index);
    const auto Slot = Ordinary.find(Symbol.Name);
    if (Slot == Ordinary.end() || Declared.contains(Symbol.Name))
      return fail(kWeakDataFallback);
    const auto &Bind = Original.DyldBindSlots.at(Slot->second);
    const auto Export = Chains.WeakDefinitions.find(Bind.Name);
    if (Bind.LibraryOrdinal != BIND_SPECIAL_DYLIB_WEAK_LOOKUP ||
        Bind.WeakImport || Export == Chains.WeakDefinitions.end() ||
        !Original.getSectionFor(Export->second) ||
        Original.isCodeAddress(Export->second))
      return fail(kWeakDataFallback);
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
      if ((R.SubtractSymbol && WeakTarget(*R.SubtractSymbol, true)) ||
          (Target && (!UsesGOT(R) || !R.External || R.SymbolIndex != *Target ||
                      R.SubtractSymbol || R.Scattered || Personality)))
        return fail(kWeakDataReference);
    }
    if (Personality && Object.EH == R.Section &&
        PersonalityFields.lookup(R.Offset) != R.Width)
      return fail(kLanguagePointer);
    if (!UsesGOT(R))
      continue;
    if (!R.External || R.Scattered || R.SubtractVA || R.SubtractSymbol ||
        !Object.Symbols.contains(R.SymbolIndex))
      return fail(kGOTRelocation);
    if (isTLVLoad(R.Type)) {
      // A TLVP reference loads the dyld descriptor; the descriptor's own
      // authenticated resolver chooses the instance for the calling thread.
      const auto &Symbol = Object.Symbols.at(R.SymbolIndex);
      const auto Slot = Ordinary.find(Symbol.Name);
      if (Symbol.Section || Slot == Ordinary.end() ||
          Declared.contains(Symbol.Name) || R.InstructionAddend ||
          R.Width != sizeof(uint32_t) ||
          Object.Image.Sections[R.Section].Kind != Kind::Code)
        return fail(kTLVDescriptor);
      const auto &B = Original.DyldBindSlots.at(Slot->second);
      if (!B.LibraryOrdinal || *B.LibraryOrdinal <= 0 || B.WeakImport)
        return fail(kTLVDescriptor);
      Object.GOT.try_emplace(std::pair{R.SymbolIndex, false},
                             NativeMachO::GOTSlot{std::nullopt, Slot->second});
      continue;
    }
    const auto Key = std::pair{R.SymbolIndex, Personality};
    if (Object.GOT.contains(Key))
      continue;
    const auto &Symbol = Object.Symbols.at(R.SymbolIndex);
    if (Symbol.Name.empty())
      return fail(kGOTRelocation);
    if (!Symbol.Section && !Declared.contains(Symbol.Name)) {
      const auto &Candidates = Personality ? Personalities : Ordinary;
      if (auto It = Candidates.find(Symbol.Name); It != Candidates.end()) {
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
  return llvm::Error::success();
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
    if (Name->starts_with("__thread") || *Name == "__mod_init_func" ||
        *Name == "__mod_term_func" || *Name == "__LLVM" ||
        M->getSectionFinalSegmentName(S.getRawDataRefImpl()) == "__LLVM")
      return fail("unsupported runtime initialization, TLS or bitcode section");
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
    const bool Zero = Type == S_ZEROFILL;
    const bool Indirect = Type == S_NON_LAZY_SYMBOL_POINTERS;
    if (Type != S_REGULAR && Type != S_COALESCED && Type != S_ZEROFILL &&
        Type != S_CSTRING_LITERALS && Type != S_4BYTE_LITERALS &&
        Type != S_8BYTE_LITERALS && Type != S_16BYTE_LITERALS && !Indirect)
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
  for (const auto &S : M->symbols()) {
    auto Name = S.getName();
    if (!Name)
      return Name.takeError();
    const auto Index = M->getSymbolIndex(S.getRawDataRefImpl());
    const uint8_t Type =
        M->is64Bit() ? M->getSymbol64TableEntry(S.getRawDataRefImpl()).n_type
                     : M->getSymbolTableEntry(S.getRawDataRefImpl()).n_type;
    const uint16_t Desc =
        M->is64Bit() ? M->getSymbol64TableEntry(S.getRawDataRefImpl()).n_desc
                     : M->getSymbolTableEntry(S.getRawDataRefImpl()).n_desc;
    const uint32_t Number = M->getSymbolSectionID(S) + 1;
    const uint64_t Value =
        M->is64Bit() ? M->getSymbol64TableEntry(S.getRawDataRefImpl()).n_value
                     : M->getSymbolTableEntry(S.getRawDataRefImpl()).n_value;
    if (Type & N_STAB)
      continue;
    if ((Type & N_TYPE) != N_SECT && (Type & N_TYPE) != N_UNDF)
      continue;
    const bool PrivateWeak = Original.isArm64e() && (Type & N_TYPE) == N_SECT &&
                             (Type & N_PEXT) && (Desc & N_WEAK_DEF);
    const bool PublicWeak = Original.isArm64e() && (Type & N_TYPE) == N_SECT &&
                            (Type & N_EXT) && !(Type & N_PEXT) &&
                            (Desc & (N_WEAK_DEF | N_WEAK_REF)) == N_WEAK_DEF;
    if (((Desc & (N_WEAK_REF | N_WEAK_DEF)) && !PrivateWeak && !PublicWeak) ||
        ((Type & N_TYPE) == N_UNDF && Value))
      return fail("weak/common native definitions are unsupported");
    Symbol Item{
        canonical(*Name),  Number, 0, 0, false, bool(Desc & N_ARM_THUMB_DEF),
        bool(Type & N_EXT)};
    if ((Type & N_TYPE) == N_SECT) {
      if (!Result.Sections.contains(Number))
        continue;
      const auto &Section = Image.Sections[Result.Sections.at(Number)];
      if (Value < Result.OldSectionVAs.at(Number) ||
          Value - Result.OldSectionVAs.at(Number) >= Section.Size)
        return fail("symbol exceeds native section");
      Item.Offset = Value - Result.OldSectionVAs.at(Number);
      Item.Function = Section.Kind == Kind::Code && Name->starts_with("_");
      // Hidden compiler helpers (e.g. terminate landing pads) bind within this
      // object. Staging adds no exported weak definition or coalescing promise.
      if (PublicWeak) {
        if (Item.Function || Item.Offset ||
            Section.Name != kMutableDataSection ||
            Section.Kind != Kind::WritableData ||
            !Result.WeakData.emplace(Result.Sections.at(Number), Index).second)
          return fail(kWeakDataAtom);
      }
      if (PrivateWeak && !Item.Function)
        return fail(kPrivateWeak);
    } else
      Item.Section = 0;
    if (!Name->empty() && !Result.Names.emplace(Item.Name, Index).second)
      return fail("duplicate native symbol");
    Result.Symbols.emplace(Index, Item);
  }
  for (const auto &[Index, S] : Result.Symbols) {
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
      if (!Target.Section || !Subtract.Section)
        return fail(kLanguagePointer);
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
    Result.OldFDEs = std::move(*Records);
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
    S.Name = "__nd_import_" + std::to_string(N->second);
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

llvm::Error place(NativeMachO &Object, uint64_t Base,
                  const std::optional<MachOEHFrameRegion> &EH) {
  auto &Image = Object.Image;
  Image.BaseVA = Base;
  uint64_t Cursor = Base;
  // Two writable groups: loader-only mutation, then ordinary mutable data.
  enum class PlacementGroup { Code, ReadOnly, Relro, Mutable };
  for (const auto Group : {PlacementGroup::Code, PlacementGroup::ReadOnly,
                           PlacementGroup::Relro, PlacementGroup::Mutable}) {
    if (Group == PlacementGroup::Relro || Group == PlacementGroup::Mutable)
      Cursor = llvm::alignTo(
          Cursor, sourcePatchStorageAlignment(Image.TargetArch, Image.Format));
    for (auto &S : Image.Sections)
      if (S.IsAllocated && S.IsInImage &&
          (Group == PlacementGroup::Code ? S.Kind == Kind::Code
           : Group == PlacementGroup::ReadOnly
               ? S.Kind == Kind::ReadOnlyData
               : (S.Kind == Kind::WritableData ||
                  S.Kind == Kind::UninitializedData) &&
                     S.ReadOnlyAfterFixups ==
                         (Group == PlacementGroup::Relro))) {
        Cursor = llvm::alignTo(Cursor, S.Alignment);
        if (Cursor < Base ||
            Cursor - Base >
                ::neverd::limits::backend::kMachOPatchObjectMaxStorage ||
            S.Size > ::neverd::limits::backend::kMachOPatchObjectMaxStorage -
                         (Cursor - Base))
          return fail("native placement overflows");
        S.VA = Cursor;
        S.Offset = Cursor - Base;
        Cursor += S.Size;
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
    Object.CompactOnly = !EH;
    if (EH)
      Image.Sections[*Object.EH].VA = EH->AppendVA;
  }
  for (const auto &[Index, S] : Object.Symbols)
    if (S.Section && Object.Sections.contains(S.Section) &&
        !Object.WeakData.contains(Object.Sections.at(S.Section)))
      Image.SymbolAddrs[S.Name] =
          Image.Sections[Object.Sections.at(S.Section)].VA + S.Offset;
  for (auto &[Key, Slot] : Object.GOT) {
    if (!Slot.Section)
      continue;
    auto &S = Image.Sections[*Slot.Section];
    Slot.Address = S.VA;
    auto &Reference = S.FixupReferences.front();
    if (auto It = Image.SymbolAddrs.find(Reference.Symbol);
        It != Image.SymbolAddrs.end())
      Reference.ResolvedValue = It->second;
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
        const std::map<std::string, SourcePatchBinding> &Bindings) {
  if (!Object.Symbols.contains(Index))
    return fail("relocation symbol is invalid");
  const auto &S = Object.Symbols.at(Index);
  if (S.Section) {
    if (!Object.Sections.contains(S.Section))
      return fail("nonallocated native symbol");
    if (Object.CompactOnly && Object.EH &&
        Object.Sections.at(S.Section) == *Object.EH)
      return fail("reference to unregistered native DWARF storage");
    return Object.Image.Sections[Object.Sections.at(S.Section)].VA + S.Offset +
           (S.Function && S.Thumb ? 1 : 0);
  }
  if (auto T = Object.Thunks.find(Index); T != Object.Thunks.end())
    return Object.Image.Sections[T->second.Section].VA +
           (Object.Image.TargetArch == Arch::ARM ? 1 : 0);
  if (Object.Imports.contains(Index))
    return fail("data dependency needs a loader pointer-slot relocation");
  if (auto B = Bindings.find(S.Name); B != Bindings.end())
    return B->second.OriginalVA;
  std::set<uint64_t> VAs;
  for (const auto &S0 : Original.Symbols)
    if (canonical(S0.Name) == S.Name && S0.IsFunc &&
        Original.hasAuthenticatedFunctionEntryAt(S0.Addr))
      VAs.insert(S0.Addr);
  for (const auto &[VA, Import] : Original.ImportStubIndices)
    if (Import < Original.Imports.size() &&
        Original.Imports[Import].Name == S.Name &&
        Original.findImportStubAt(VA))
      VAs.insert(VA);
  if (VAs.size() != 1)
    return fail(
        "external native symbol needs a binding or authenticated callable: " +
        S.Name);
  return *VAs.begin();
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
      if (Symbol == Object.Symbols.end() || Symbol->second.Name.empty() ||
          Bindings.contains(Symbol->second.Name))
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
      if (Symbol->second.Section) {
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
    if (Original.isArm64e() && (CompactPersonality || PointerToGOT)) {
      const bool Personality = CompactPersonality || Object.EH == R.Section;
      auto Slot = Object.GOT.find({R.SymbolIndex, Personality});
      if (Slot == Object.GOT.end() ||
          (CompactPersonality &&
           (R.Type != ARM64_RELOC_UNSIGNED ||
            R.Width != arm64e::kPointerBytes || R.PCRel || Raw)) ||
          (PointerToGOT && (!R.PCRel || R.Width != sizeof(uint32_t))))
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
    if (Original.isArm64e() && R.Type == ARM64_RELOC_UNSIGNED &&
        !R.SubtractSymbol && !R.SubtractVA && S.IsAllocated &&
        (S.IsInImage || isExternalMachORelro(Object.Image, S))) {
      if (R.PCRel || R.Scattered || R.Width != arm64e::kPointerBytes ||
          S.Kind != Kind::WritableData)
        return fail(kOrdinaryStorage);
      CompiledFixupReference Reference;
      Reference.Offset = R.Offset;
      Reference.Kind = R.Type;
      Reference.BitWidth = R.Width * CHAR_BIT;
      Reference.IsResolved = Reference.OrdinaryPointer = true;
      if (R.External) {
        auto Symbol = Object.Symbols.find(R.SymbolIndex);
        if (Symbol == Object.Symbols.end() || Symbol->second.Name.empty() ||
            Bindings.contains(Symbol->second.Name))
          return fail(kOrdinarySymbol);
        Reference.Symbol = Symbol->second.Name;
        Reference.Addend = int64_t(Raw);
        if (Symbol->second.Section) {
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
    const bool GOT = R.External &&
                     (Object.Imports.contains(R.SymbolIndex) ||
                      Object.GOT.contains({R.SymbolIndex, false})) &&
                     ((A == Arch::X64 && (R.Type == X86_64_RELOC_GOT ||
                                          R.Type == X86_64_RELOC_GOT_LOAD)) ||
                      (A == Arch::AArch64 && isGOTLoad(R.Type) &&
                       (!isTLVLoad(R.Type) || Original.isArm64e())));
    llvm::Expected<uint64_t> Target =
        GOT           ? llvm::Expected<uint64_t>(
                            Object.GOT.contains({R.SymbolIndex, false})
                                ? Object.GOT.at({R.SymbolIndex, false}).Address
                                : Object.Imports.at(R.SymbolIndex).SlotVA)
        : R.Scattered ? mapOldVA(Object, R.ScatteredVA)
        : R.External  ? resolve(Object, R.SymbolIndex, Original, Bindings)
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
                R.Type == ARM64_RELOC_PAGEOFF12 || GOT) &&
               R.Width == 4) {
      const uint32_t Old = Raw;
      if (isTLVLoad(R.Type)) {
        const bool Page = R.Type == ARM64_RELOC_TLVP_LOAD_PAGE21;
        const auto RegisterFields = Page ? aarch64::kRdMask : kTLVLoadRegisters;
        const auto Op = Page ? aarch64::kADRP_Op : kTLVDescriptorLoad;
        if ((Old & ~RegisterFields) != Op)
          return fail(kTLVInstruction);
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
      if ((Old & 0x7c000000) != 0x14000000)
        return fail("BRANCH26 does not name B/BL");
      V += llvm::APInt(
          128, uint64_t(llvm::SignExtend64<26>(Old & 0x3ffffff) * 4), true);
      V -= llvm::APInt(128, P);
      if (!V.isSignedIntN(28) || (V.getLoBits(2).getZExtValue()))
        return fail("native branch overflows");
      writeLE<uint32_t>(
          Out, (Old & 0xfc000000) |
                   ((V.getLoBits(28).getZExtValue() >> 2) & 0x3ffffff));
    } else
      return fail("unsupported native relocation " + llvm::Twine(R.Type));
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
    for (const auto &R : *Records) {
      auto [It, Added] = Extents.emplace(R.BeginVA, R.EndVA);
      if (!Added && It->second != R.EndVA)
        return fail("native DWARF and compact extents disagree");
    }
  }
  if (Object.EH && !Object.CompactOnly) {
    const auto Region = findMachOEHFrameRegion(Original.Raw);
    // Actual section identity is supplied by placement when storage has grown.
    const auto &EH = Object.Image.Sections[*Object.EH];
    auto Records = decodeDwarfEHFrameRecords(EH.ExternalBytes, EH.VA,
                                             Object.Image.PointerWidth == 8);
    if (!Records)
      return Records.takeError();
    if (!Region)
      return fail("producer FDEs need a registered frame section");
    for (const auto &R : *Records) {
      auto Compact = llvm::find_if(Object.Compact, [&](const auto &C) {
        return C.BeginVA == R.BeginVA;
      });
      if (Compact != Object.Compact.end() &&
          (Compact->Encoding & macho_unwind::kModeMask) !=
              (A == Arch::AArch64 ? macho_unwind::kARM64ModeDwarf
                                  : macho_unwind::kX86ModeDwarf))
        continue;
      if (R.RecordVA < Region->SectionVA ||
          R.RecordVA - Region->SectionVA > 0xffffff)
        return fail("producer FDE exceeds compact encoding");
      NativeMachOCompactRange Range{
          R.BeginVA, R.EndVA,
          (A == Arch::AArch64 ? macho_unwind::kARM64ModeDwarf
                              : macho_unwind::kX86ModeDwarf) |
              uint32_t(R.RecordVA - Region->SectionVA)};
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
  for (auto &[I, S] : Object.Symbols)
    if (S.Function) {
      const auto &Section =
          Object.Image.Sections[Object.Sections.at(S.Section)];
      const uint64_t Begin = Section.VA + S.Offset;
      if (!Functions.emplace(Begin, &S).second)
        return fail("aliased native function entries are ambiguous");
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
  for (const auto &[Begin, End] : Extents)
    if (!Functions.contains(Begin) || Functions.at(Begin)->Size != End - Begin)
      return fail("native unwind has no exact function symbol");
  return llvm::Error::success();
}
llvm::Error
verifyNativeObjectUnwind(const BinaryImage &Image,
                         llvm::ArrayRef<NativeMachOCompactRange> Ranges) {
  const auto Native = macho_unwind::parseCompactUnwind(Image);
  if (Native.ParseStatus != ExceptionParseStatus::Complete)
    return fail(kPublishedCompact);
  for (const auto &Range : Ranges) {
    const auto It = llvm::find_if(Native.Entries, [&](const auto &Entry) {
      return Entry.CodeRange.Begin == Range.BeginVA &&
             Entry.CodeRange.End == Range.EndVA;
    });
    if (It == Native.Entries.end() ||
        It->SemanticStatus != CompactUnwindSemanticStatus::Complete ||
        (It->NativeEncoding & ~macho_unwind::kPersonalityMask) !=
            (Range.Encoding & ~macho_unwind::kPersonalityMask))
      return fail(kPublishedRange);
    const auto Index = (It->NativeEncoding & macho_unwind::kPersonalityMask) >>
                       macho_unwind::kPersonalityShift;
    if (Range.PersonalitySlotVA
            ? (!Index || Index > Native.PersonalitySlots.size() ||
               Native.PersonalitySlots[Index - 1] != *Range.PersonalitySlotVA)
            : Index != 0)
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
        // Explicit generic RA toggles break older system unwinders. Preserve
        // the producer bytes and reject the incompatible ABI at publication.
        if (HasClassicToggle(CIE->InitialInstructions) ||
            HasClassicToggle(Function->Dwarf->Instructions))
          return fail(kClassicReturnCFI);
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
  auto Candidate = Binary;
  const uint64_t ExtraEH = Object->EH && findMachOEHFrameRegion(Candidate)
                               ? Object->Image.Sections[*Object->EH].Size
                               : 0;
  auto Grown = reserveMachOSourceUnwind(Candidate, Original->Arch, ExtraEH,
                                        unwindFunctionCounts(*Object));
  if (!Grown)
    return Grown.takeError();
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
  if (auto Error = prepareArm64eGOT(*Object, *Layout, Dependencies))
    return Error;
  if (auto Error = prepareMainRelro(*Object, Candidate, *Layout))
    return Error;
  if (auto Error = place(*Object, Base, EH))
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
  if (auto Error = Patcher.appendReplacementImage(Candidate, Object->Image, {},
                                                  Dependencies))
    return Error;
  auto After = readMachODependencyState(Candidate);
  if (!After)
    return After.takeError();
  if (auto Error = preserveMachODependencyState(*Before, *After))
    return Error;
  auto Installed = Load(Candidate);
  if (!Installed)
    return Installed.takeError();
  if (auto Error = verifyNativeObjectUnwind(*Installed, Object->Compact))
    return Error;
  Binary.swap(Candidate);
  return std::move(Object->Image);
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
    MachOLiftedObjectVerifier VerifyGenerated) {
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
  std::set<std::string> Entries;
  std::set<uint64_t> Addresses;
  for (const auto &M : Mappings) {
    if (!Object->Names.contains(M.Symbol) || !Entries.insert(M.Symbol).second ||
        !Addresses.insert(M.OriginalVA).second ||
        !Original.hasAuthenticatedFunctionEntryAt(M.OriginalVA) ||
        !sourceEntryCanHostTrampoline(Original, M.OriginalVA))
      return fail("invalid native entry mapping");
    const auto &S = Object->Symbols.at(Object->Names.at(M.Symbol));
    if (!S.Function || !S.Section)
      return fail("entry needs a native function definition");
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
  const uint64_t ExtraEH = Object->EH && (findMachOEHFrameRegion(LayoutBytes) ||
                                          !Object->Thunks.empty())
                               ? Object->Image.Sections[*Object->EH].Size
                               : 0;
  std::optional<MachODependencyState> SlidDependencies;
  auto Slide = reserveMachOPublishedUnwind(
      LayoutBytes, Original.Arch, ExtraEH, unwindFunctionCounts(*Object),
      Original.isArm64e() && !Preparation && !DynCode, SlidDependencies);
  if (!Slide)
    return Slide.takeError();
  for (auto &[Name, Binding] : Bindings)
    Binding.OriginalVA += *Slide;
  const uint64_t Base =
      Patcher.plannedExecSegmentVA(LayoutBytes, Original.Arch);
  const auto EH = findMachOEHFrameRegion(LayoutBytes);
  if (!Base)
    return fail("original Mach-O has no safe placement");
  auto LayoutReader = Loader::create(BinaryFormat::MachO);
  auto LayoutImage = LayoutReader->loadBuffer(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(LayoutBytes.data()),
                      LayoutBytes.size()),
      "object-layout"));
  if (!LayoutImage)
    return LayoutImage.takeError();
  scanImportThunks(*LayoutImage);
  if (Original.isArm64e())
    if (auto E = prepareArm64eGOT(*Object, *LayoutImage, Dependencies))
      return E;
  if (auto E = prepareMainRelro(*Object, LayoutBytes, *LayoutImage))
    return E;
  if (auto E = place(*Object, Base, EH))
    return std::move(E);
  if (auto E = relocate(*Object, *LayoutImage, Bindings))
    return std::move(E);
  if (!Object->Thunks.empty()) {
    if (!EH)
      return fail("import thunks need registered native DWARF storage");
    if (auto E = finalizeImports(*Object, *EH))
      return E;
  }
  if (VerifyGenerated)
    if (auto E = VerifyGenerated(Object->Image, *Slide, Object->Compact))
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
              if (auto E = Patcher.appendReplacementImage(Binary, Object->Image,
                                                          {}, Dependencies))
                return Reject(std::move(E));
              if (Original.isArm64e()) {
                auto Collected = collectMachOAuthenticatedFixups(
                    Binary, Object->Image, Dependencies);
                if (!Collected)
                  return Reject(Collected.takeError());
                Fixups = std::move(*Collected);
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
        if (auto E = verifyNativeObjectUnwind(Final, Object->Compact))
          return E;
        if (Original.isArm64e()) {
          auto After = readMachODependencyState(Final.Raw);
          if (!After)
            return After.takeError();
          if (SlidDependencies)
            return preserveMachODependencyState(*SlidDependencies, *After);
          auto Before = readMachODependencyState(Original.Raw);
          if (!Before)
            return Before.takeError();
          if (auto E = preserveMachODependencyState(*Before, *After))
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
                       [&](const CompiledImage &Image, va_t,
                           llvm::ArrayRef<NativeMachOCompactRange> Ranges) {
                         return VerifyGenerated ? VerifyGenerated(Image, Ranges)
                                                : llvm::Error::success();
                       });
}

llvm::Expected<PatchResult>
installMachOLiftedObject(llvm::StringRef Object, const BinaryImage &Original,
                         llvm::ArrayRef<SourcePatchMapping> Mappings,
                         llvm::ArrayRef<SourcePatchBinding> Bindings,
                         const std::filesystem::path &Output,
                         MachOLiftedObjectVerifier Verify) {
  return installObject(Object, Original, Mappings, Bindings, Output, {},
                       nullptr, nullptr, {}, Verify);
}
} // namespace neverd
