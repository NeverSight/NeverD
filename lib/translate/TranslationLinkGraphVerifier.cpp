//===- TranslationLinkGraphVerifier.cpp - Preallocation graph audit ------===//

#include "neverd/translate/TranslationLinkGraphVerifier.h"

#include "neverd/translate/RuntimeSymbolRegistry.h"
#include "TranslationLinkGraphVerifierInternal.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::translate {

char TranslationLinkGraphError::ID;

TranslationLinkGraphError::TranslationLinkGraphError(
    TranslationLinkGraphErrorCode Code, std::string Detail)
    : Code(Code), Detail(std::move(Detail)) {}

void TranslationLinkGraphError::log(llvm::raw_ostream &OS) const {
  OS << "translation LinkGraph audit: ";
  switch (Code) {
  case TranslationLinkGraphErrorCode::InvalidInput:
    OS << "invalid input";
    break;
  case TranslationLinkGraphErrorCode::InvalidManifest:
    OS << "invalid manifest";
    break;
  case TranslationLinkGraphErrorCode::ObjectGraphCreationFailed:
    OS << "object-to-graph conversion failed";
    break;
  case TranslationLinkGraphErrorCode::GraphTargetMismatch:
    OS << "graph target mismatch";
    break;
  case TranslationLinkGraphErrorCode::SectionPolicyViolation:
    OS << "section policy violation";
    break;
  case TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch:
    OS << "block symbol manifest mismatch";
    break;
  case TranslationLinkGraphErrorCode::RuntimeSymbolManifestMismatch:
    OS << "runtime symbol manifest mismatch";
    break;
  case TranslationLinkGraphErrorCode::AbsoluteSymbolRejected:
    OS << "absolute symbol rejected";
    break;
  case TranslationLinkGraphErrorCode::EdgePolicyViolation:
    OS << "edge policy violation";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code TranslationLinkGraphError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}

namespace {

using llvm::jitlink::Block;
using llvm::jitlink::Edge;
using llvm::jitlink::Linkage;
using llvm::jitlink::LinkGraph;
using llvm::jitlink::Scope;
using llvm::jitlink::Section;
using llvm::jitlink::Symbol;

llvm::Error failure(TranslationLinkGraphErrorCode Code,
                    const llvm::Twine &Detail = {}) {
  return llvm::make_error<TranslationLinkGraphError>(Code, Detail.str());
}

enum class ObjectFormat : uint8_t { ELF, MachO };

llvm::Expected<ObjectFormat>
validateHostTarget(const ResolvedHostTarget &HostTarget) {
  const llvm::Triple Triple(HostTarget.triple());
  if (HostTarget.architecture() != GuestArchitecture::AArch64 ||
      Triple.getArch() != llvm::Triple::aarch64)
    return failure(TranslationLinkGraphErrorCode::GraphTargetMismatch,
                   "v1 requires an AArch64 host target");
  if (!Triple.isLittleEndian() || Triple.getArchPointerBitWidth() != 64)
    return failure(TranslationLinkGraphErrorCode::GraphTargetMismatch,
                   "v1 requires a little-endian 64-bit target");
  if (Triple.isOSBinFormatELF())
    return ObjectFormat::ELF;
  if (Triple.isOSBinFormatMachO())
    return ObjectFormat::MachO;
  return failure(TranslationLinkGraphErrorCode::GraphTargetMismatch,
                 "v1 accepts only ELF and Mach-O targets");
}

llvm::StringRef objectPrefix(ObjectFormat Format) {
  return Format == ObjectFormat::MachO ? "_" : "";
}

llvm::Expected<std::pair<llvm::StringSet<>, llvm::StringSet<>>>
validateManifest(ObjectFormat Format,
                 llvm::ArrayRef<TranslationObjectSymbolV1> ExpectedBlockSymbols,
                 llvm::ArrayRef<TranslationObjectSymbolV1> SealedRuntimeSymbols,
                 llvm::StringRef SealedRuntimeRegistryIdentity) {
  if (ExpectedBlockSymbols.empty())
    return failure(TranslationLinkGraphErrorCode::InvalidManifest,
                   "block manifest is empty");

  llvm::Expected<RuntimeSymbolRegistryV1> RegistryOrErr =
      RuntimeSymbolRegistryV1::create();
  if (!RegistryOrErr)
    return failure(TranslationLinkGraphErrorCode::InvalidManifest,
                   llvm::toString(RegistryOrErr.takeError()));
  const RuntimeSymbolRegistryV1 &Registry = *RegistryOrErr;
  if (SealedRuntimeRegistryIdentity.empty() ||
      SealedRuntimeRegistryIdentity != Registry.identity())
    return failure(TranslationLinkGraphErrorCode::InvalidManifest,
                   "runtime registry identity is not the sealed v1 identity");
  if (SealedRuntimeSymbols.size() != Registry.entries().size())
    return failure(TranslationLinkGraphErrorCode::InvalidManifest,
                   "runtime manifest is not the complete sealed registry");

  llvm::StringSet<> BlockNames;
  llvm::StringSet<> RuntimeNames;
  llvm::StringSet<> AllObjectNames;
  const llvm::StringRef Prefix = objectPrefix(Format);
  for (const TranslationObjectSymbolV1 &Symbol : ExpectedBlockSymbols) {
    if (Symbol.IRName.empty() || Symbol.ObjectName.empty() ||
        llvm::StringRef(Symbol.IRName).contains('\0') ||
        llvm::StringRef(Symbol.ObjectName).contains('\0') ||
        Symbol.ObjectName != (Prefix + Symbol.IRName).str() ||
        !BlockNames.insert(Symbol.ObjectName).second ||
        !AllObjectNames.insert(Symbol.ObjectName).second)
      return failure(TranslationLinkGraphErrorCode::InvalidManifest,
                     "block symbol mapping is empty, noncanonical, or "
                     "duplicated");
  }

  for (size_t Index = 0; Index != SealedRuntimeSymbols.size(); ++Index) {
    const TranslationObjectSymbolV1 &Symbol = SealedRuntimeSymbols[Index];
    if (Symbol.IRName != Registry.entries()[Index].name() ||
        Symbol.ObjectName != (Prefix + Symbol.IRName).str() ||
        !RuntimeNames.insert(Symbol.ObjectName).second ||
        !AllObjectNames.insert(Symbol.ObjectName).second)
      return failure(TranslationLinkGraphErrorCode::InvalidManifest,
                     "runtime symbol mapping disagrees with the sealed v1 "
                     "registry");
  }
  return std::make_pair(std::move(BlockNames), std::move(RuntimeNames));
}

bool tripleComponentCompatible(unsigned GraphValue, unsigned ExpectedValue,
                               unsigned UnknownValue) {
  return GraphValue == UnknownValue || ExpectedValue == UnknownValue ||
         GraphValue == ExpectedValue;
}

bool operatingSystemCompatible(const llvm::Triple &GraphTriple,
                               const llvm::Triple &ExpectedTriple,
                               ObjectFormat Format) {
  if (tripleComponentCompatible(static_cast<unsigned>(GraphTriple.getOS()),
                                static_cast<unsigned>(ExpectedTriple.getOS()),
                                static_cast<unsigned>(llvm::Triple::UnknownOS)))
    return true;
  // A relocatable Mach-O header identifies the Darwin family but does not
  // distinguish the legacy "darwin" spelling from "macosx". Treat those two
  // spellings as equivalent evidence without conflating macOS with iOS or the
  // other Darwin-family platforms.
  return Format == ObjectFormat::MachO && GraphTriple.isMacOSX() &&
         ExpectedTriple.isMacOSX();
}

llvm::Error validateGraphTarget(const LinkGraph &Graph,
                                const ResolvedHostTarget &HostTarget,
                                ObjectFormat Format) {
  const llvm::Triple &GraphTriple = Graph.getTargetTriple();
  const llvm::Triple ExpectedTriple(HostTarget.triple());
  if (GraphTriple.getArch() != llvm::Triple::aarch64 ||
      Graph.getPointerSize() != 8 ||
      Graph.getEndianness() != llvm::endianness::little)
    return failure(TranslationLinkGraphErrorCode::GraphTargetMismatch,
                   "LinkGraph is not little-endian AArch64 with 64-bit "
                   "pointers");
  if ((Format == ObjectFormat::ELF && !GraphTriple.isOSBinFormatELF()) ||
      (Format == ObjectFormat::MachO && !GraphTriple.isOSBinFormatMachO()))
    return failure(TranslationLinkGraphErrorCode::GraphTargetMismatch,
                   "LinkGraph object format disagrees with the host target");

  // ELF does not encode a complete source triple, and Mach-O omits the
  // deployment version.  Compare every component that the object carried and
  // reject conflicts while treating an unknown component as absent evidence.
  if (!tripleComponentCompatible(
          static_cast<unsigned>(GraphTriple.getVendor()),
          static_cast<unsigned>(ExpectedTriple.getVendor()),
          static_cast<unsigned>(llvm::Triple::UnknownVendor)) ||
      !operatingSystemCompatible(GraphTriple, ExpectedTriple, Format) ||
      !tripleComponentCompatible(
          static_cast<unsigned>(GraphTriple.getEnvironment()),
          static_cast<unsigned>(ExpectedTriple.getEnvironment()),
          static_cast<unsigned>(llvm::Triple::UnknownEnvironment)))
    return failure(TranslationLinkGraphErrorCode::GraphTargetMismatch,
                   "LinkGraph triple conflicts with the expected target");
  return llvm::Error::success();
}

bool isTextSection(ObjectFormat Format, llvm::StringRef Name) {
  return Name == (Format == ObjectFormat::ELF ? ".text" : "__TEXT,__text");
}

struct ContentRange {
  uintptr_t Begin = 0;
  uintptr_t End = 0;
};

struct ExpectedBlockRange {
  const Block *GraphBlock = nullptr;
  uint64_t Begin = 0;
  uint64_t End = 0;
};

llvm::Error validateSection(const Section &GraphSection, ObjectFormat Format,
                            llvm::ArrayRef<uint8_t> ObjectBytes,
                            std::vector<ContentRange> &ContentRanges) {
  const llvm::StringRef Name = GraphSection.getName();
  const llvm::orc::MemProt Read = llvm::orc::MemProt::Read;
  const llvm::orc::MemProt ReadExec = static_cast<llvm::orc::MemProt>(
      static_cast<unsigned>(llvm::orc::MemProt::Read) |
      static_cast<unsigned>(llvm::orc::MemProt::Exec));
  const bool IsText = isTextSection(Format, Name);

  if (IsText) {
    if (GraphSection.getMemProt() != ReadExec ||
        GraphSection.getMemLifetime() != llvm::orc::MemLifetime::Standard)
      return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                     "text section does not have exact RX standard-lifetime "
                     "policy");
  } else if (Format == ObjectFormat::ELF &&
             (Name == ".strtab" || Name == ".shstrtab" ||
              Name == ".rela.text" || Name == ".comment" ||
              Name == ".note.GNU-stack" || Name == ".llvm_addrsig" ||
              Name == ".symtab")) {
    if (GraphSection.getMemProt() != Read ||
        GraphSection.getMemLifetime() != llvm::orc::MemLifetime::NoAlloc)
      return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                     "ELF metadata section is allocatable or writable");
  } else {
    return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                   "section is outside the v1 allowlist: " + Name);
  }

  const uintptr_t ObjectBegin = reinterpret_cast<uintptr_t>(ObjectBytes.data());
  if (ObjectBytes.size() > std::numeric_limits<uintptr_t>::max() - ObjectBegin)
    return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                   "object byte range overflows the host address space");
  const uintptr_t ObjectEnd = ObjectBegin + ObjectBytes.size();

  for (const Block *GraphBlock : GraphSection.blocks()) {
    if (GraphBlock->getAlignment() == 0 ||
        GraphBlock->getAlignmentOffset() >= GraphBlock->getAlignment())
      return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                     "block alignment is invalid");
    if (IsText && (GraphBlock->getAlignment() < alignof(uint32_t) ||
                   (GraphBlock->getAlignmentOffset() & 3U) != 0 ||
                   (GraphBlock->getAddress().getValue() & 3U) != 0 ||
                   (GraphBlock->getSize() & 3U) != 0))
      return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                     "AArch64 text block is not four-byte aligned");
    if (!IsText && !GraphBlock->edges_empty())
      return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                     "metadata block carries relocation edges");
    if (GraphBlock->getSize() == 0)
      continue;
    if (GraphBlock->isZeroFill())
      return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                     "zero-fill content is outside the v1 allowlist");
    const llvm::ArrayRef<char> Content = GraphBlock->getContent();
    const uintptr_t Begin = reinterpret_cast<uintptr_t>(Content.data());
    if (Begin < ObjectBegin || Begin > ObjectEnd ||
        Content.size() > ObjectEnd - Begin)
      return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                     "graph block content does not reference the audited "
                     "object bytes");
    ContentRanges.push_back({Begin, Begin + Content.size()});
  }
  return llvm::Error::success();
}

llvm::Error validateSections(const LinkGraph &Graph, ObjectFormat Format,
                             llvm::ArrayRef<uint8_t> ObjectBytes,
                             TranslationLinkGraphAuditV1 &Audit) {
  bool SawText = false;
  std::vector<ContentRange> ContentRanges;
  for (const Section &GraphSection : Graph.sections()) {
    ++Audit.SectionCount;
    SawText |= isTextSection(Format, GraphSection.getName());
    Audit.BlockCount += GraphSection.blocks_size();
    if (llvm::Error Error =
            validateSection(GraphSection, Format, ObjectBytes, ContentRanges))
      return Error;
  }
  if (!SawText)
    return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                   "object has no allowed text section");

  llvm::sort(ContentRanges, [](const ContentRange &Left,
                               const ContentRange &Right) {
    return std::tie(Left.Begin, Left.End) < std::tie(Right.Begin, Right.End);
  });
  for (size_t Index = 1; Index != ContentRanges.size(); ++Index)
    if (ContentRanges[Index].Begin < ContentRanges[Index - 1].End)
      return failure(TranslationLinkGraphErrorCode::SectionPolicyViolation,
                     "graph block content ranges overlap");
  return llvm::Error::success();
}

llvm::Error validateSymbols(LinkGraph &Graph, ObjectFormat Format,
                            const llvm::StringSet<> &ExpectedBlockNames,
                            const llvm::StringSet<> &AllowedRuntimeNames,
                            llvm::StringMap<uint64_t> &ExternalEdgeCounts,
                            const Symbol *&ELFNullSymbol,
                            std::vector<ExpectedBlockRange> &ExpectedRanges,
                            TranslationLinkGraphAuditV1 &Audit) {
  llvm::StringMap<uint64_t> BlockCounts;
  for (const Symbol *GraphSymbol : Graph.defined_symbols()) {
    ++Audit.DefinedSymbolCount;
    if (GraphSymbol->hasName() &&
        ExpectedBlockNames.contains(*GraphSymbol->getName())) {
      ++BlockCounts[*GraphSymbol->getName()];
      if (GraphSymbol->getLinkage() != Linkage::Strong ||
          GraphSymbol->getScope() == Scope::Local ||
          !GraphSymbol->isCallable() || GraphSymbol->getSize() == 0 ||
          !isTextSection(Format, GraphSymbol->getSection().getName()))
        return failure(
            TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch,
            "manifest block is not one strong callable text definition");
      const uint64_t Offset = GraphSymbol->getOffset();
      const uint64_t Size = GraphSymbol->getSize();
      const uint64_t BlockSize = GraphSymbol->getBlock().getSize();
      if (Offset > BlockSize || Size > BlockSize - Offset ||
          (Offset & 3U) != 0 || (Size & 3U) != 0 ||
          (GraphSymbol->getAddress().getValue() & 3U) != 0)
        return failure(
            TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch,
            "manifest block range is not one aligned in-bounds AArch64 "
            "instruction range");
      ExpectedRanges.push_back(
          {&GraphSymbol->getBlock(), Offset, Offset + Size});
      continue;
    }
  }
  for (const auto &Entry : ExpectedBlockNames)
    if (BlockCounts.lookup(Entry.getKey()) != 1)
      return failure(TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch,
                     "a required block definition is missing or duplicated: " +
                         Entry.getKey());

  for (const Symbol *GraphSymbol : Graph.defined_symbols()) {
    if (GraphSymbol->hasName() &&
        ExpectedBlockNames.contains(*GraphSymbol->getName()))
      continue;
    if (GraphSymbol->getScope() != Scope::Local || GraphSymbol->isLive())
      return failure(
          TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch,
          "object contains an additional externally visible definition");
    if (!isTextSection(Format, GraphSymbol->getSection().getName())) {
      if (!GraphSymbol->isCallable())
        continue;
      return failure(TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch,
                     "local callable symbol is outside executable text");
    }

    const uint64_t Offset = GraphSymbol->getOffset();
    const uint64_t Size = GraphSymbol->getSize();
    const uint64_t BlockSize = GraphSymbol->getBlock().getSize();
    if (Offset > BlockSize || Size > BlockSize - Offset)
      return failure(TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch,
                     "local text symbol range escapes its graph block");

    bool IsMetadataMarker = false;
    bool IsExactLocalAlias = false;
    for (const ExpectedBlockRange &Range : ExpectedRanges) {
      if (Range.GraphBlock != &GraphSymbol->getBlock())
        continue;
      IsMetadataMarker |= !GraphSymbol->isCallable() && Size == 0 &&
                          Offset >= Range.Begin && Offset < Range.End;
      IsExactLocalAlias |= GraphSymbol->hasName() && Size != 0 &&
                           Offset == Range.Begin && Offset + Size == Range.End;
    }
    if (!IsMetadataMarker && !IsExactLocalAlias)
      return failure(
          TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch,
          "object contains an additional local executable definition");
  }

  for (Symbol *GraphSymbol : Graph.external_symbols()) {
    ++Audit.ExternalSymbolCount;
    if (!GraphSymbol->hasName() ||
        !AllowedRuntimeNames.contains(*GraphSymbol->getName()) ||
        GraphSymbol->isWeaklyReferenced() || GraphSymbol->getSize() != 0)
      return failure(
          TranslationLinkGraphErrorCode::RuntimeSymbolManifestMismatch,
          "external symbol is not an exact strong runtime-manifest entry");
    ExternalEdgeCounts.try_emplace(*GraphSymbol->getName(), 0);
  }

  for (Symbol *GraphSymbol : Graph.absolute_symbols()) {
    // The ELF builder materializes the mandatory symbol-table index zero as a
    // private sentinel.  It is not an object entry point and may not be the
    // target of any edge.  No object-provided absolute is accepted.
    const bool IsELFNullSentinel =
        Format == ObjectFormat::ELF && !ELFNullSymbol &&
        GraphSymbol->hasName() &&
        *GraphSymbol->getName() == "__jitlink_ELF_SYM_UND_0" &&
        GraphSymbol->getScope() == Scope::Local &&
        GraphSymbol->getLinkage() == Linkage::Strong &&
        !GraphSymbol->isLive() && !GraphSymbol->isCallable() &&
        GraphSymbol->getSize() == 0 &&
        GraphSymbol->getAddress().getValue() == 0;
    if (!IsELFNullSentinel)
      return failure(TranslationLinkGraphErrorCode::AbsoluteSymbolRejected,
                     "object contains an absolute symbol");
    ELFNullSymbol = GraphSymbol;
  }
  return llvm::Error::success();
}

llvm::Error
validateExecutableCoverage(const LinkGraph &Graph, ObjectFormat Format,
                           std::vector<ExpectedBlockRange> &ExpectedRanges) {
  llvm::sort(ExpectedRanges, [](const ExpectedBlockRange &Left,
                                const ExpectedBlockRange &Right) {
    if (Left.GraphBlock != Right.GraphBlock)
      return std::less<const Block *>()(Left.GraphBlock, Right.GraphBlock);
    return std::tie(Left.Begin, Left.End) < std::tie(Right.Begin, Right.End);
  });

  for (size_t Index = 1; Index != ExpectedRanges.size(); ++Index)
    if (ExpectedRanges[Index].GraphBlock ==
            ExpectedRanges[Index - 1].GraphBlock &&
        ExpectedRanges[Index].Begin < ExpectedRanges[Index - 1].End)
      return failure(TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch,
                     "manifest block ranges overlap");

  for (const Block *GraphBlock : Graph.blocks()) {
    if (!isTextSection(Format, GraphBlock->getSection().getName()))
      continue;
    uint64_t CoveredEnd = 0;
    bool SawExpectedRange = false;
    for (const ExpectedBlockRange &Range : ExpectedRanges) {
      if (Range.GraphBlock != GraphBlock)
        continue;
      SawExpectedRange = true;
      if (Range.Begin != CoveredEnd)
        return failure(
            TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch,
            "AArch64 text contains an anonymous executable gap");
      CoveredEnd = Range.End;
    }
    if (!SawExpectedRange || CoveredEnd != GraphBlock->getSize())
      return failure(TranslationLinkGraphErrorCode::BlockSymbolManifestMismatch,
                     "AArch64 text is not exactly covered by manifest blocks");
  }
  return llvm::Error::success();
}

bool isAArch64BranchInstruction(uint32_t Instruction) {
  return (Instruction & 0x7c000000U) == 0x14000000U &&
         (Instruction & 0x03ffffffU) == 0;
}

llvm::Error validateEdges(const LinkGraph &Graph, ObjectFormat Format,
                          const llvm::StringSet<> &AllowedRuntimeNames,
                          llvm::StringMap<uint64_t> &ExternalEdgeCounts,
                          const Symbol *ELFNullSymbol,
                          llvm::ArrayRef<ExpectedBlockRange> ExpectedRanges,
                          TranslationLinkGraphAuditV1 &Audit) {
  for (const Block *GraphBlock : Graph.blocks()) {
    std::vector<std::pair<uint64_t, uint64_t>> FixupRanges;
    for (const Edge &GraphEdge : GraphBlock->edges()) {
      ++Audit.EdgeCount;
      if (&GraphEdge.getTarget() == ELFNullSymbol)
        return failure(TranslationLinkGraphErrorCode::EdgePolicyViolation,
                       "edge targets the ELF null-symbol sentinel");
      if (GraphEdge.getKind() != llvm::jitlink::aarch64::Branch26PCRel ||
          !isTextSection(Format, GraphBlock->getSection().getName()))
        return failure(
            TranslationLinkGraphErrorCode::EdgePolicyViolation,
            "edge kind or source section is outside the v1 "
            "allowlist: " +
                llvm::StringRef(Graph.getEdgeKindName(GraphEdge.getKind())));
      if (GraphBlock->isZeroFill() ||
          GraphEdge.getOffset() > GraphBlock->getSize() ||
          GraphBlock->getSize() - GraphEdge.getOffset() < sizeof(uint32_t) ||
          (GraphEdge.getOffset() & 3U) != 0 ||
          (GraphBlock->getFixupAddress(GraphEdge).getValue() & 3U) != 0)
        return failure(TranslationLinkGraphErrorCode::EdgePolicyViolation,
                       "Branch26 fixup is not one aligned in-bounds word");
      const uint64_t FixupBegin = GraphEdge.getOffset();
      const uint64_t FixupEnd = FixupBegin + sizeof(uint32_t);
      for (const auto &[AcceptedBegin, AcceptedEnd] : FixupRanges)
        if (FixupBegin < AcceptedEnd && AcceptedBegin < FixupEnd)
          return failure(TranslationLinkGraphErrorCode::EdgePolicyViolation,
                         "Branch26 fixup ranges overlap");
      FixupRanges.emplace_back(FixupBegin, FixupEnd);

      unsigned OwnerCount = 0;
      for (const ExpectedBlockRange &Range : ExpectedRanges)
        if (Range.GraphBlock == GraphBlock && FixupBegin >= Range.Begin &&
            FixupEnd <= Range.End)
          ++OwnerCount;
      if (OwnerCount != 1)
        return failure(
            TranslationLinkGraphErrorCode::EdgePolicyViolation,
            "Branch26 source is not owned by exactly one manifest block");
      const uint32_t Instruction = llvm::support::endian::read32le(
          GraphBlock->getContent().data() + GraphEdge.getOffset());
      if (!isAArch64BranchInstruction(Instruction))
        return failure(TranslationLinkGraphErrorCode::EdgePolicyViolation,
                       "Branch26 edge does not point at a zero-addend B/BL "
                       "instruction");

      const Symbol &Target = GraphEdge.getTarget();
      if (!Target.isExternal())
        return failure(TranslationLinkGraphErrorCode::EdgePolicyViolation,
                       "v1 Branch26 target is defined or absolute");
      if (!Target.hasName() ||
          !AllowedRuntimeNames.contains(*Target.getName()) ||
          GraphEdge.getAddend() != 0)
        return failure(TranslationLinkGraphErrorCode::EdgePolicyViolation,
                       "external Branch26 target or addend is not sealed");
      ++ExternalEdgeCounts[*Target.getName()];
    }
  }
  for (const auto &Entry : ExternalEdgeCounts)
    if (Entry.getValue() == 0)
      return failure(
          TranslationLinkGraphErrorCode::RuntimeSymbolManifestMismatch,
          "external runtime symbol is not targeted by any audited edge: " +
              Entry.getKey());
  return llvm::Error::success();
}

} // namespace

llvm::Expected<TranslationLinkGraphAuditV1>
detail::auditTranslationLinkGraphPrePruneV1(
    LinkGraph &Graph, llvm::ArrayRef<uint8_t> ObjectBytes,
    const ResolvedHostTarget &ExpectedHostTarget,
    llvm::ArrayRef<TranslationObjectSymbolV1> ExpectedBlockSymbols,
    llvm::ArrayRef<TranslationObjectSymbolV1> SealedRuntimeSymbols,
    llvm::StringRef SealedRuntimeRegistryIdentity) {
  if (ObjectBytes.empty())
    return failure(TranslationLinkGraphErrorCode::InvalidInput,
                   "object is empty");
  llvm::Expected<ObjectFormat> FormatOrErr =
      validateHostTarget(ExpectedHostTarget);
  if (!FormatOrErr)
    return FormatOrErr.takeError();
  llvm::Expected<std::pair<llvm::StringSet<>, llvm::StringSet<>>> NamesOrErr =
      validateManifest(*FormatOrErr, ExpectedBlockSymbols, SealedRuntimeSymbols,
                       SealedRuntimeRegistryIdentity);
  if (!NamesOrErr)
    return NamesOrErr.takeError();

  TranslationLinkGraphAuditV1 Audit;
  Audit.GraphTriple = Graph.getTargetTriple().str();
  if (llvm::Error Error =
          validateGraphTarget(Graph, ExpectedHostTarget, *FormatOrErr))
    return std::move(Error);
  if (llvm::Error Error =
          validateSections(Graph, *FormatOrErr, ObjectBytes, Audit))
    return std::move(Error);

  llvm::StringMap<uint64_t> ExternalEdgeCounts;
  const Symbol *ELFNullSymbol = nullptr;
  std::vector<ExpectedBlockRange> ExpectedRanges;
  if (llvm::Error Error = validateSymbols(
          Graph, *FormatOrErr, NamesOrErr->first, NamesOrErr->second,
          ExternalEdgeCounts, ELFNullSymbol, ExpectedRanges, Audit))
    return std::move(Error);
  if (llvm::Error Error =
          validateExecutableCoverage(Graph, *FormatOrErr, ExpectedRanges))
    return std::move(Error);
  if (llvm::Error Error = validateEdges(Graph, *FormatOrErr, NamesOrErr->second,
                                        ExternalEdgeCounts, ELFNullSymbol,
                                        ExpectedRanges, Audit))
    return std::move(Error);
  return Audit;
}

llvm::Expected<TranslationLinkGraphAuditV1> verifyTranslationLinkGraphV1(
    llvm::ArrayRef<uint8_t> ObjectBytes,
    const ResolvedHostTarget &ExpectedHostTarget,
    llvm::ArrayRef<TranslationObjectSymbolV1> ExpectedBlockSymbols,
    llvm::ArrayRef<TranslationObjectSymbolV1> SealedRuntimeSymbols,
    llvm::StringRef SealedRuntimeRegistryIdentity) {
  if (ObjectBytes.empty())
    return failure(TranslationLinkGraphErrorCode::InvalidInput,
                   "object is empty");
  llvm::Expected<ObjectFormat> FormatOrErr =
      validateHostTarget(ExpectedHostTarget);
  if (!FormatOrErr)
    return FormatOrErr.takeError();
  llvm::Expected<std::pair<llvm::StringSet<>, llvm::StringSet<>>> NamesOrErr =
      validateManifest(*FormatOrErr, ExpectedBlockSymbols, SealedRuntimeSymbols,
                       SealedRuntimeRegistryIdentity);
  if (!NamesOrErr)
    return NamesOrErr.takeError();

  const llvm::StringRef ObjectData(
      reinterpret_cast<const char *>(ObjectBytes.data()), ObjectBytes.size());
  auto StringPool = std::make_shared<llvm::orc::SymbolStringPool>();
  llvm::Expected<std::unique_ptr<LinkGraph>> GraphOrErr =
      llvm::jitlink::createLinkGraphFromObject(
          llvm::MemoryBufferRef(ObjectData, "translation-object-v1"),
          std::move(StringPool));
  if (!GraphOrErr)
    return failure(TranslationLinkGraphErrorCode::ObjectGraphCreationFailed,
                   llvm::toString(GraphOrErr.takeError()));
  LinkGraph &Graph = **GraphOrErr;
  return detail::auditTranslationLinkGraphPrePruneV1(
      Graph, ObjectBytes, ExpectedHostTarget, ExpectedBlockSymbols,
      SealedRuntimeSymbols, SealedRuntimeRegistryIdentity);
}

llvm::Expected<TranslationLinkGraphAuditV1>
verifyTranslationLinkGraphV1(const TranslationObjectArtifactV1 &Artifact) {
  return verifyTranslationLinkGraphV1(
      Artifact.bytes(), Artifact.hostTarget(), Artifact.blockSymbols(),
      Artifact.runtimeSymbols(), Artifact.runtimeRegistryIdentity());
}

} // namespace neverd::translate
