//===- TranslationJITLinker.cpp - Sealed in-process linking --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/translate/TranslationJITLinker.h"

#include "TranslationJITLinkerInternal.h"
#include "TranslationLinkGraphVerifierInternal.h"

#include "neverd/translate/TranslationArtifactVerifier.h"
#include "neverd/translate/TranslationLinkGraphVerifier.h"
#include "neverd/translate/TranslationObjectRequest.h"
#include "neverd/translate/TranslationTargetMachine.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/JITLinkMemoryManager.h"
#include "llvm/ExecutionEngine/JITLink/aarch64.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MSVCErrorWorkarounds.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::translate {

char TranslationJITLinkerError::ID;

TranslationJITLinkerError::TranslationJITLinkerError(
    TranslationJITLinkerErrorCode Code, std::string Detail)
    : Code(Code), Detail(std::move(Detail)) {}

void TranslationJITLinkerError::log(llvm::raw_ostream &OS) const {
  OS << "sealed translation linker: ";
  switch (Code) {
  case TranslationJITLinkerErrorCode::InvalidArtifact:
    OS << "invalid compiler artifact";
    break;
  case TranslationJITLinkerErrorCode::ArtifactTargetNotNative:
    OS << "artifact target is not native";
    break;
  case TranslationJITLinkerErrorCode::UnsupportedProcessTarget:
    OS << "process target is unsupported";
    break;
  case TranslationJITLinkerErrorCode::ProcessTargetMismatch:
    OS << "artifact target does not match the process";
    break;
  case TranslationJITLinkerErrorCode::RuntimeRegistryMismatch:
    OS << "runtime registry identity mismatch";
    break;
  case TranslationJITLinkerErrorCode::ArtifactAuditFailed:
    OS << "raw artifact audit failed";
    break;
  case TranslationJITLinkerErrorCode::LinkGraphAuditFailed:
    OS << "preallocation LinkGraph audit failed";
    break;
  case TranslationJITLinkerErrorCode::LinkGraphCreationFailed:
    OS << "LinkGraph creation failed";
    break;
  case TranslationJITLinkerErrorCode::PrePruneAuditFailed:
    OS << "pre-prune audit failed";
    break;
  case TranslationJITLinkerErrorCode::PostPruneAuditFailed:
    OS << "post-prune audit failed";
    break;
  case TranslationJITLinkerErrorCode::PostAllocationAuditFailed:
    OS << "post-allocation audit failed";
    break;
  case TranslationJITLinkerErrorCode::RuntimeSymbolLookupFailed:
    OS << "runtime symbol lookup failed";
    break;
  case TranslationJITLinkerErrorCode::ResolutionAuditFailed:
    OS << "resolution audit failed";
    break;
  case TranslationJITLinkerErrorCode::PreFixupAuditFailed:
    OS << "pre-fixup audit failed";
    break;
  case TranslationJITLinkerErrorCode::PostFixupAuditFailed:
    OS << "post-fixup audit failed";
    break;
  case TranslationJITLinkerErrorCode::FinalizationFailed:
    OS << "finalization failed";
    break;
  case TranslationJITLinkerErrorCode::EntryPointUnavailable:
    OS << "entry point is unavailable";
    break;
  case TranslationJITLinkerErrorCode::InvocationRejected:
    OS << "invocation rejected";
    break;
  case TranslationJITLinkerErrorCode::UnloadFailed:
    OS << "unload failed";
    break;
  }
  if (!Detail.empty())
    OS << ": " << Detail;
}

std::error_code TranslationJITLinkerError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

bool detail::isSealedAArch64Branch26FixupV1(uint32_t OriginalInstruction,
                                            uint32_t FixedInstruction,
                                            uint64_t FixupAddress,
                                            uint64_t ExpectedTargetAddress) {
  constexpr uint32_t ImmediateMask = 0x03ffffffU;
  constexpr uint32_t OpcodeMask = ~ImmediateMask;
  if ((OriginalInstruction & 0x7c000000U) != 0x14000000U ||
      (OriginalInstruction & ImmediateMask) != 0 ||
      (FixedInstruction & OpcodeMask) != (OriginalInstruction & OpcodeMask))
    return false;
  const int64_t Delta = llvm::SignExtend64<28>(
      static_cast<uint64_t>(FixedInstruction & ImmediateMask) << 2);
  return llvm::orc::ExecutorAddr(FixupAddress) + Delta ==
         llvm::orc::ExecutorAddr(ExpectedTargetAddress);
}

namespace {

using llvm::jitlink::Block;
using llvm::jitlink::Edge;
using llvm::jitlink::JITLinkContext;
using llvm::jitlink::JITLinkMemoryManager;
using llvm::jitlink::Linkage;
using llvm::jitlink::LinkGraph;
using llvm::jitlink::PassConfiguration;
using llvm::jitlink::Scope;
using llvm::jitlink::Section;
using llvm::jitlink::Symbol;

constexpr llvm::StringLiteral GOTSectionName("$__GOT");
constexpr llvm::StringLiteral StubSectionName("$__STUBS");
constexpr llvm::StringLiteral
    ArtifactIdentityPrefix("neverd.translation-object-artifact.v1.sha256:");
constexpr llvm::StringLiteral
    RequestIdentityPrefix("neverd.translation-object-request.v1.sha256:");

llvm::Error failure(TranslationJITLinkerErrorCode Code,
                    const llvm::Twine &Detail = {}) {
  return llvm::make_error<TranslationJITLinkerError>(Code, Detail.str());
}

void completeStage(TranslationJITLinkAuditReceiptV1 &Receipt,
                   TranslationJITLinkAuditStageV1 Stage) {
  Receipt.CompletedStages |= static_cast<uint32_t>(Stage);
}

bool hasStage(const TranslationJITLinkAuditReceiptV1 &Receipt,
              TranslationJITLinkAuditStageV1 Stage) {
  return Receipt.completed(Stage);
}

llvm::orc::MemProt readExecProtection() {
  return static_cast<llvm::orc::MemProt>(
      static_cast<unsigned>(llvm::orc::MemProt::Read) |
      static_cast<unsigned>(llvm::orc::MemProt::Exec));
}

struct GraphCounts {
  uint64_t Sections = 0;
  uint64_t Blocks = 0;
  uint64_t DefinedSymbols = 0;
  uint64_t ExternalSymbols = 0;
  uint64_t AbsoluteSymbols = 0;
  uint64_t Edges = 0;
};

GraphCounts countGraph(LinkGraph &Graph) {
  GraphCounts Counts;
  for (Section &GraphSection : Graph.sections()) {
    ++Counts.Sections;
    Counts.Blocks += GraphSection.blocks_size();
    Counts.DefinedSymbols += GraphSection.symbols_size();
    for (Block *GraphBlock : GraphSection.blocks())
      Counts.Edges += GraphBlock->edges_size();
  }
  Counts.ExternalSymbols = static_cast<uint64_t>(std::distance(
      Graph.external_symbols().begin(), Graph.external_symbols().end()));
  Counts.AbsoluteSymbols = static_cast<uint64_t>(std::distance(
      Graph.absolute_symbols().begin(), Graph.absolute_symbols().end()));
  return Counts;
}

struct RuntimeBinding {
  std::string IRName;
  llvm::orc::ExecutorAddr Address;
};

struct OriginalBlockRecord {
  Block *GraphBlock = nullptr;
  std::vector<char> Content;
};

struct RuntimeTargetRecord {
  Symbol *External = nullptr;
  std::string ObjectName;
  std::string IRName;
  llvm::orc::ExecutorAddr RegistryAddress;
  Block *StubBlock = nullptr;
  Symbol *StubSymbol = nullptr;
  Block *GOTBlock = nullptr;
  Symbol *GOTSymbol = nullptr;
};

struct RuntimeCallRecord {
  Block *SourceBlock = nullptr;
  uint64_t Offset = 0;
  size_t TargetIndex = 0;
  uint32_t OriginalInstruction = 0;
};

struct LinkCompletion {
  JITLinkMemoryManager::FinalizedAlloc Allocation;
  llvm::orc::ExecutorAddr EntryAddress;
  TranslationJITLinkAuditReceiptV1 Receipt;
};

struct SealedLinkState {
  explicit SealedLinkState(const TranslationObjectArtifactV1 &Artifact,
                           const RuntimeSymbolRegistryV1 &Registry)
      : Artifact(&Artifact), Registry(&Registry),
        ObjectBytes(Artifact.bytes().begin(), Artifact.bytes().end()),
        SymbolPool(std::make_shared<llvm::orc::SymbolStringPool>()) {
    Receipt.ArtifactIdentity = Artifact.artifactCacheKey().str();
    Receipt.RuntimeRegistryIdentity = Registry.identity().str();
    Receipt.HostTriple = Artifact.hostTarget().triple().str();
    Receipt.HostCPU = Artifact.hostTarget().cpu().str();
    Receipt.ManifestBlockCount = Artifact.blockSymbols().size();
    for (const TranslationObjectSymbolV1 &Runtime : Artifact.runtimeSymbols()) {
      llvm::Expected<llvm::orc::ExecutorAddr> Address =
          Registry.lookup(Runtime.IRName);
      if (!Address) {
        BindingError = llvm::toString(Address.takeError());
        continue;
      }
      Bindings.try_emplace(Runtime.ObjectName,
                           RuntimeBinding{Runtime.IRName, *Address});
    }
  }

  const TranslationObjectArtifactV1 *Artifact = nullptr;
  const RuntimeSymbolRegistryV1 *Registry = nullptr;
  std::vector<uint8_t> ObjectBytes;
  std::shared_ptr<llvm::orc::SymbolStringPool> SymbolPool;
  TranslationJITLinkAuditReceiptV1 Receipt;
  llvm::StringMap<RuntimeBinding> Bindings;
  std::string BindingError;
  GraphCounts OriginalCounts;
  std::set<Section *> OriginalSections;
  std::set<Block *> OriginalBlocks;
  std::set<Symbol *> OriginalDefinedSymbols;
  std::set<Symbol *> OriginalExternalSymbols;
  std::set<Symbol *> OriginalAbsoluteSymbols;
  std::vector<OriginalBlockRecord> OriginalBlockContents;
  std::vector<RuntimeTargetRecord> RuntimeTargets;
  std::vector<RuntimeCallRecord> RuntimeCalls;
  Symbol *ManifestSymbol = nullptr;
  llvm::orc::ExecutorAddr ResolvedEntryAddress;
  bool TargetNodesCaptured = false;
  std::promise<llvm::MSVCPExpected<LinkCompletion>> CompletionPromise;
};

bool containsSection(LinkGraph &Graph, const Section *Expected) {
  return llvm::any_of(Graph.sections(),
                      [&](const Section &Value) { return &Value == Expected; });
}

bool containsBlock(LinkGraph &Graph, const Block *Expected) {
  return llvm::is_contained(Graph.blocks(), Expected);
}

bool containsDefinedSymbol(LinkGraph &Graph, const Symbol *Expected) {
  return llvm::is_contained(Graph.defined_symbols(), Expected);
}

bool containsExternalSymbol(LinkGraph &Graph, const Symbol *Expected) {
  return llvm::is_contained(Graph.external_symbols(), Expected);
}

bool containsAbsoluteSymbol(LinkGraph &Graph, const Symbol *Expected) {
  return llvm::is_contained(Graph.absolute_symbols(), Expected);
}

bool sameBytes(llvm::ArrayRef<char> Left, llvm::ArrayRef<char> Right) {
  return Left.size() == Right.size() &&
         std::equal(Left.begin(), Left.end(), Right.begin());
}

Symbol *onlySymbolForBlock(Section &GraphSection, Block &GraphBlock) {
  Symbol *Result = nullptr;
  for (Symbol *GraphSymbol : GraphSection.symbols()) {
    if (&GraphSymbol->getBlock() != &GraphBlock)
      continue;
    if (Result)
      return nullptr;
    Result = GraphSymbol;
  }
  return Result;
}

Edge *edgeAt(Block &GraphBlock, uint64_t Offset) {
  Edge *Result = nullptr;
  for (Edge &GraphEdge : GraphBlock.edges()) {
    if (GraphEdge.getOffset() != Offset)
      continue;
    if (Result)
      return nullptr;
    Result = &GraphEdge;
  }
  return Result;
}

llvm::Error capturePrePruneGraph(LinkGraph &Graph, SealedLinkState &State) {
  State.OriginalCounts = countGraph(Graph);
  for (Section &GraphSection : Graph.sections()) {
    State.OriginalSections.insert(&GraphSection);
    for (Block *GraphBlock : GraphSection.blocks()) {
      State.OriginalBlocks.insert(GraphBlock);
      State.OriginalBlockContents.push_back(
          {GraphBlock, std::vector<char>(GraphBlock->getContent().begin(),
                                         GraphBlock->getContent().end())});
    }
    for (Symbol *GraphSymbol : GraphSection.symbols())
      State.OriginalDefinedSymbols.insert(GraphSymbol);
  }
  for (Symbol *GraphSymbol : Graph.external_symbols()) {
    State.OriginalExternalSymbols.insert(GraphSymbol);
    if (!GraphSymbol->hasName())
      return failure(TranslationJITLinkerErrorCode::PrePruneAuditFailed,
                     "an external graph symbol has no name");
    const auto Binding = State.Bindings.find(*GraphSymbol->getName());
    if (Binding == State.Bindings.end())
      return failure(TranslationJITLinkerErrorCode::PrePruneAuditFailed,
                     "external graph symbol has no sealed binding: " +
                         *GraphSymbol->getName());
    State.RuntimeTargets.push_back(
        {GraphSymbol, (*GraphSymbol->getName()).str(), Binding->second.IRName,
         Binding->second.Address});
  }
  for (Symbol *GraphSymbol : Graph.absolute_symbols())
    State.OriginalAbsoluteSymbols.insert(GraphSymbol);

  std::map<Symbol *, size_t> TargetIndices;
  for (size_t Index = 0; Index != State.RuntimeTargets.size(); ++Index)
    TargetIndices.emplace(State.RuntimeTargets[Index].External, Index);
  for (Block *GraphBlock : Graph.blocks()) {
    for (Edge &GraphEdge : GraphBlock->edges()) {
      auto Target = TargetIndices.find(&GraphEdge.getTarget());
      if (Target == TargetIndices.end())
        return failure(TranslationJITLinkerErrorCode::PrePruneAuditFailed,
                       "pre-prune edge does not target a sealed runtime");
      const llvm::ArrayRef<char> Content = GraphBlock->getContent();
      if (GraphEdge.getOffset() > Content.size() ||
          Content.size() - GraphEdge.getOffset() < sizeof(uint32_t))
        return failure(TranslationJITLinkerErrorCode::PrePruneAuditFailed,
                       "pre-prune runtime-call fixup is out of bounds");
      State.RuntimeCalls.push_back(
          {GraphBlock, GraphEdge.getOffset(), Target->second,
           llvm::support::endian::read32le(Content.data() +
                                           GraphEdge.getOffset())});
    }
  }
  const bool HasRuntimeReferences =
      !State.RuntimeCalls.empty() || !State.RuntimeTargets.empty();
  if ((HasRuntimeReferences && State.RuntimeCalls.empty()) ||
      State.RuntimeCalls.size() != State.OriginalCounts.Edges)
    return failure(TranslationJITLinkerErrorCode::PrePruneAuditFailed,
                   "v1 graph has no complete runtime-call edge set");
  if ((HasRuntimeReferences && State.RuntimeTargets.empty()) ||
      State.RuntimeTargets.size() != State.OriginalCounts.ExternalSymbols)
    return failure(TranslationJITLinkerErrorCode::PrePruneAuditFailed,
                   "v1 graph has no complete external runtime set");

  const llvm::StringRef ManifestName =
      State.Artifact->blockSymbols().front().ObjectName;
  for (Symbol *GraphSymbol : Graph.defined_symbols()) {
    if (!GraphSymbol->hasName() || *GraphSymbol->getName() != ManifestName)
      continue;
    if (State.ManifestSymbol)
      return failure(TranslationJITLinkerErrorCode::PrePruneAuditFailed,
                     "manifest entry is duplicated");
    State.ManifestSymbol = GraphSymbol;
  }
  if (!State.ManifestSymbol)
    return failure(TranslationJITLinkerErrorCode::PrePruneAuditFailed,
                   "manifest entry is missing");
  State.Receipt.RuntimeReferenceCount = State.RuntimeCalls.size();
  return llvm::Error::success();
}

llvm::Error verifyOriginalNodes(LinkGraph &Graph, const SealedLinkState &State,
                                TranslationJITLinkerErrorCode Code) {
  for (Section *GraphSection : State.OriginalSections)
    if (!containsSection(Graph, GraphSection))
      return failure(Code, "an original section was removed or replaced");
  for (Block *GraphBlock : State.OriginalBlocks)
    if (!containsBlock(Graph, GraphBlock))
      return failure(Code, "an original block was removed or replaced");
  for (Symbol *GraphSymbol : State.OriginalDefinedSymbols)
    if (!containsDefinedSymbol(Graph, GraphSymbol))
      return failure(Code,
                     "an original defined symbol was removed or replaced");
  for (Symbol *GraphSymbol : State.OriginalExternalSymbols)
    if (!containsExternalSymbol(Graph, GraphSymbol))
      return failure(Code,
                     "an original external symbol was removed or replaced");
  for (Symbol *GraphSymbol : State.OriginalAbsoluteSymbols)
    if (!containsAbsoluteSymbol(Graph, GraphSymbol))
      return failure(Code,
                     "an original absolute symbol was removed or replaced");
  return llvm::Error::success();
}

llvm::Error verifyOriginalContent(const SealedLinkState &State,
                                  TranslationJITLinkerErrorCode Code) {
  for (const OriginalBlockRecord &Record : State.OriginalBlockContents)
    if (!sameBytes(Record.GraphBlock->getContent(), Record.Content))
      return failure(Code, "original block content changed before fixups");
  return llvm::Error::success();
}

llvm::Error verifyTargetSection(const Section &GraphSection,
                                uint64_t ExpectedBlocks,
                                TranslationJITLinkerErrorCode Code) {
  if (GraphSection.getMemProt() != readExecProtection() ||
      GraphSection.getMemLifetime() != llvm::orc::MemLifetime::Standard ||
      GraphSection.blocks_size() != ExpectedBlocks ||
      GraphSection.symbols_size() != ExpectedBlocks)
    return failure(Code,
                   "target-created section protection or cardinality differs "
                   "from the sealed policy");
  return llvm::Error::success();
}

llvm::Error captureAndVerifyTargetNodes(LinkGraph &Graph,
                                        SealedLinkState &State,
                                        TranslationJITLinkerErrorCode Code) {
  Section *GOTSection = Graph.findSectionByName(GOTSectionName);
  Section *StubSection = Graph.findSectionByName(StubSectionName);
  if (State.RuntimeTargets.empty()) {
    if (GOTSection || StubSection)
      return failure(
          Code,
          "target-created sections exist without referenced runtime targets");
    State.TargetNodesCaptured = true;
    return llvm::Error::success();
  }
  if (!GOTSection || !StubSection)
    return failure(Code, "the exact AArch64 GOT/stub sections are missing");
  if (llvm::Error Error =
          verifyTargetSection(*GOTSection, State.RuntimeTargets.size(), Code))
    return Error;
  if (llvm::Error Error =
          verifyTargetSection(*StubSection, State.RuntimeTargets.size(), Code))
    return Error;

  std::map<Symbol *, size_t> ExternalIndices;
  for (size_t Index = 0; Index != State.RuntimeTargets.size(); ++Index)
    ExternalIndices.emplace(State.RuntimeTargets[Index].External, Index);

  const llvm::ArrayRef<char> NullPointer(
      llvm::jitlink::aarch64::NullPointerContent,
      sizeof(llvm::jitlink::aarch64::NullPointerContent));
  for (Block *GraphBlock : GOTSection->blocks()) {
    if (GraphBlock->getSize() != sizeof(uint64_t) ||
        GraphBlock->getAlignment() != alignof(uint64_t) ||
        GraphBlock->getAlignmentOffset() != 0 ||
        !sameBytes(GraphBlock->getContent(), NullPointer) ||
        GraphBlock->edges_size() != 1)
      return failure(Code, "a GOT block is not one canonical pointer entry");
    Edge &Pointer = *GraphBlock->edges().begin();
    const auto Target = ExternalIndices.find(&Pointer.getTarget());
    if (Pointer.getKind() != llvm::jitlink::aarch64::Pointer64 ||
        Pointer.getOffset() != 0 || Pointer.getAddend() != 0 ||
        Target == ExternalIndices.end())
      return failure(Code, "a GOT entry does not bind one sealed external");
    RuntimeTargetRecord &Record = State.RuntimeTargets[Target->second];
    if (Record.GOTBlock)
      return failure(Code, "a runtime target has duplicate GOT entries");
    Symbol *GOTSymbol = onlySymbolForBlock(*GOTSection, *GraphBlock);
    if (!GOTSymbol || GOTSymbol->hasName() ||
        GOTSymbol->getLinkage() != Linkage::Strong ||
        GOTSymbol->getScope() != Scope::Local || GOTSymbol->isCallable() ||
        GOTSymbol->isLive() || GOTSymbol->getOffset() != 0 ||
        GOTSymbol->getSize() != sizeof(uint64_t))
      return failure(Code, "a GOT symbol is not the canonical anonymous node");
    Record.GOTBlock = GraphBlock;
    Record.GOTSymbol = GOTSymbol;
  }

  const llvm::ArrayRef<char> StubTemplate(
      llvm::jitlink::aarch64::PointerJumpStubContent,
      sizeof(llvm::jitlink::aarch64::PointerJumpStubContent));
  for (Block *GraphBlock : StubSection->blocks()) {
    if (GraphBlock->getSize() != StubTemplate.size() ||
        GraphBlock->getAlignment() != alignof(uint32_t) ||
        GraphBlock->getAlignmentOffset() != 0 ||
        !sameBytes(GraphBlock->getContent(), StubTemplate) ||
        GraphBlock->edges_size() != 2)
      return failure(Code, "a stub block is not one canonical 12-byte entry");
    Edge *Page = edgeAt(*GraphBlock, 0);
    Edge *Offset = edgeAt(*GraphBlock, sizeof(uint32_t));
    if (!Page || !Offset || Page->getKind() != llvm::jitlink::aarch64::Page21 ||
        Offset->getKind() != llvm::jitlink::aarch64::PageOffset12 ||
        Page->getAddend() != 0 || Offset->getAddend() != 0 ||
        &Page->getTarget() != &Offset->getTarget())
      return failure(Code, "stub edges do not form canonical ADRP/LDR fixups");
    size_t TargetIndex = State.RuntimeTargets.size();
    for (size_t Index = 0; Index != State.RuntimeTargets.size(); ++Index)
      if (State.RuntimeTargets[Index].GOTSymbol == &Page->getTarget()) {
        TargetIndex = Index;
        break;
      }
    if (TargetIndex == State.RuntimeTargets.size())
      return failure(Code, "stub does not target a sealed GOT entry");
    RuntimeTargetRecord &Record = State.RuntimeTargets[TargetIndex];
    if (Record.StubBlock)
      return failure(Code, "a runtime target has duplicate stubs");
    Symbol *StubSymbol = onlySymbolForBlock(*StubSection, *GraphBlock);
    if (!StubSymbol || StubSymbol->hasName() ||
        StubSymbol->getLinkage() != Linkage::Strong ||
        StubSymbol->getScope() != Scope::Local || !StubSymbol->isCallable() ||
        StubSymbol->isLive() || StubSymbol->getOffset() != 0 ||
        StubSymbol->getSize() != StubTemplate.size())
      return failure(Code,
                     "a stub symbol is not the canonical anonymous callable");
    Record.StubBlock = GraphBlock;
    Record.StubSymbol = StubSymbol;
  }

  for (const RuntimeTargetRecord &Record : State.RuntimeTargets)
    if (!Record.GOTBlock || !Record.GOTSymbol || !Record.StubBlock ||
        !Record.StubSymbol)
      return failure(Code, "a runtime target lacks its unique GOT/stub chain");
  State.TargetNodesCaptured = true;
  return llvm::Error::success();
}

llvm::Error verifyCapturedTargetNodes(LinkGraph &Graph,
                                      const SealedLinkState &State,
                                      TranslationJITLinkerErrorCode Code,
                                      bool VerifyUnfixedContent) {
  Section *GOTSection = Graph.findSectionByName(GOTSectionName);
  Section *StubSection = Graph.findSectionByName(StubSectionName);
  if (State.RuntimeTargets.empty()) {
    if (!State.TargetNodesCaptured || GOTSection || StubSection)
      return failure(Code, "the sealed zero-reference target state changed");
    return llvm::Error::success();
  }
  if (!State.TargetNodesCaptured || !GOTSection || !StubSection)
    return failure(Code, "target-created nodes were not sealed post-prune");
  if (llvm::Error Error =
          verifyTargetSection(*GOTSection, State.RuntimeTargets.size(), Code))
    return Error;
  if (llvm::Error Error =
          verifyTargetSection(*StubSection, State.RuntimeTargets.size(), Code))
    return Error;

  const llvm::ArrayRef<char> NullPointer(
      llvm::jitlink::aarch64::NullPointerContent,
      sizeof(llvm::jitlink::aarch64::NullPointerContent));
  const llvm::ArrayRef<char> StubTemplate(
      llvm::jitlink::aarch64::PointerJumpStubContent,
      sizeof(llvm::jitlink::aarch64::PointerJumpStubContent));
  for (const RuntimeTargetRecord &Record : State.RuntimeTargets) {
    if (!containsBlock(Graph, Record.GOTBlock) ||
        !containsDefinedSymbol(Graph, Record.GOTSymbol) ||
        !containsBlock(Graph, Record.StubBlock) ||
        !containsDefinedSymbol(Graph, Record.StubSymbol) ||
        &Record.GOTBlock->getSection() != GOTSection ||
        &Record.StubBlock->getSection() != StubSection)
      return failure(Code, "a sealed target-created node was replaced");
    Edge *Pointer = edgeAt(*Record.GOTBlock, 0);
    Edge *Page = edgeAt(*Record.StubBlock, 0);
    Edge *Offset = edgeAt(*Record.StubBlock, sizeof(uint32_t));
    if (Record.GOTBlock->edges_size() != 1 ||
        Record.StubBlock->edges_size() != 2 || !Pointer || !Page || !Offset ||
        Pointer->getKind() != llvm::jitlink::aarch64::Pointer64 ||
        Page->getKind() != llvm::jitlink::aarch64::Page21 ||
        Offset->getKind() != llvm::jitlink::aarch64::PageOffset12 ||
        Pointer->getAddend() != 0 || Page->getAddend() != 0 ||
        Offset->getAddend() != 0 || &Pointer->getTarget() != Record.External ||
        &Page->getTarget() != Record.GOTSymbol ||
        &Offset->getTarget() != Record.GOTSymbol)
      return failure(Code, "a sealed target-created edge chain changed");
    if (VerifyUnfixedContent &&
        (!sameBytes(Record.GOTBlock->getContent(), NullPointer) ||
         !sameBytes(Record.StubBlock->getContent(), StubTemplate)))
      return failure(Code, "target-created content changed before fixups");
  }
  return llvm::Error::success();
}

llvm::Error verifyRuntimeCallRoutes(LinkGraph &Graph,
                                    const SealedLinkState &State,
                                    TranslationJITLinkerErrorCode Code) {
  for (const RuntimeCallRecord &Call : State.RuntimeCalls) {
    if (!containsBlock(Graph, Call.SourceBlock) ||
        Call.TargetIndex >= State.RuntimeTargets.size())
      return failure(Code, "a sealed runtime call source was replaced");
    Edge *Branch = edgeAt(*Call.SourceBlock, Call.Offset);
    if (!Branch || Branch->getKind() != llvm::jitlink::aarch64::Branch26PCRel ||
        Branch->getAddend() != 0 ||
        &Branch->getTarget() !=
            State.RuntimeTargets[Call.TargetIndex].StubSymbol)
      return failure(Code, "a runtime call no longer targets its sealed stub");
  }
  return llvm::Error::success();
}

llvm::Error verifyGraphCardinality(LinkGraph &Graph,
                                   const SealedLinkState &State,
                                   TranslationJITLinkerErrorCode Code) {
  const GraphCounts Counts = countGraph(Graph);
  const uint64_t RuntimeTargetCount = State.RuntimeTargets.size();
  const uint64_t GeneratedSectionCount = RuntimeTargetCount == 0 ? 0 : 2;
  if (Counts.Sections !=
          State.OriginalCounts.Sections + GeneratedSectionCount ||
      Counts.Blocks != State.OriginalCounts.Blocks + 2 * RuntimeTargetCount ||
      Counts.DefinedSymbols !=
          State.OriginalCounts.DefinedSymbols + 2 * RuntimeTargetCount ||
      Counts.ExternalSymbols != State.OriginalCounts.ExternalSymbols ||
      Counts.AbsoluteSymbols != State.OriginalCounts.AbsoluteSymbols ||
      Counts.Edges != State.OriginalCounts.Edges + 3 * RuntimeTargetCount)
    return failure(Code, "LinkGraph node or edge cardinality expanded");
  if (!Graph.allocActions().empty())
    return failure(Code, "LinkGraph contains allocation actions");
  for (Section &GraphSection : Graph.sections())
    if (!State.OriginalSections.contains(&GraphSection) &&
        GraphSection.getName() != GOTSectionName &&
        GraphSection.getName() != StubSectionName)
      return failure(Code, "LinkGraph contains an unsealed generated section");
  return llvm::Error::success();
}

llvm::Error auditPostPrune(LinkGraph &Graph, SealedLinkState &State) {
  constexpr auto Code = TranslationJITLinkerErrorCode::PostPruneAuditFailed;
  if (llvm::Error Error = verifyOriginalNodes(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyOriginalContent(State, Code))
    return Error;
  if (llvm::Error Error = captureAndVerifyTargetNodes(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyRuntimeCallRoutes(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyGraphCardinality(Graph, State, Code))
    return Error;
  State.Receipt.StubCount = State.RuntimeTargets.size();
  State.Receipt.GOTEntryCount = State.RuntimeTargets.size();
  completeStage(State.Receipt, TranslationJITLinkAuditStageV1::PostPrune);
  return llvm::Error::success();
}

llvm::Error verifyAllocatedAddresses(LinkGraph &Graph,
                                     const SealedLinkState &State,
                                     TranslationJITLinkerErrorCode Code) {
  for (Block *GraphBlock : Graph.blocks()) {
    if (GraphBlock->getSection().getMemLifetime() ==
        llvm::orc::MemLifetime::NoAlloc)
      continue;
    const uint64_t Address = GraphBlock->getAddress().getValue();
    if (Address == 0 || GraphBlock->getAlignment() == 0 ||
        Address % GraphBlock->getAlignment() !=
            GraphBlock->getAlignmentOffset())
      return failure(Code, "an allocated block address violates alignment");
  }
  for (const RuntimeTargetRecord &Record : State.RuntimeTargets) {
    if ((Record.StubBlock->getAddress().getValue() & 3U) != 0 ||
        (Record.StubSymbol->getAddress().getValue() & 3U) != 0 ||
        (Record.GOTBlock->getAddress().getValue() & 7U) != 0 ||
        (Record.GOTSymbol->getAddress().getValue() & 7U) != 0)
      return failure(Code, "a generated GOT/stub address is misaligned");
  }
  return llvm::Error::success();
}

llvm::Error auditPostAllocation(LinkGraph &Graph, SealedLinkState &State) {
  constexpr auto Code =
      TranslationJITLinkerErrorCode::PostAllocationAuditFailed;
  if (!hasStage(State.Receipt, TranslationJITLinkAuditStageV1::PostPrune))
    return failure(Code, "post-prune audit did not complete");
  if (llvm::Error Error = verifyOriginalNodes(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyOriginalContent(State, Code))
    return Error;
  if (llvm::Error Error = verifyCapturedTargetNodes(Graph, State, Code, true))
    return Error;
  if (llvm::Error Error = verifyRuntimeCallRoutes(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyGraphCardinality(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyAllocatedAddresses(Graph, State, Code))
    return Error;
  completeStage(State.Receipt, TranslationJITLinkAuditStageV1::PostAllocation);
  return llvm::Error::success();
}

llvm::Error auditResolved(LinkGraph &Graph, SealedLinkState &State) {
  constexpr auto Code = TranslationJITLinkerErrorCode::ResolutionAuditFailed;
  if (!hasStage(State.Receipt, TranslationJITLinkAuditStageV1::PostAllocation))
    return failure(Code, "post-allocation audit did not complete");
  if (!State.ManifestSymbol ||
      !containsDefinedSymbol(Graph, State.ManifestSymbol) ||
      State.ManifestSymbol->getAddress().isNull() ||
      (State.ManifestSymbol->getAddress().getValue() & 3U) != 0)
    return failure(Code, "manifest entry has no aligned resolved address");
  for (Symbol *External : Graph.external_symbols())
    if (!External->getAddress().isNull())
      return failure(Code, "an external was resolved before sealed lookup");
  State.ResolvedEntryAddress = State.ManifestSymbol->getAddress();
  completeStage(State.Receipt, TranslationJITLinkAuditStageV1::Resolved);
  return llvm::Error::success();
}

bool branch26InRange(llvm::orc::ExecutorAddr Fixup,
                     llvm::orc::ExecutorAddr Target) {
  const int64_t Delta = Target - Fixup;
  return (Delta & 3) == 0 && Delta >= -(int64_t{1} << 27) &&
         Delta <= (int64_t{1} << 27) - 1;
}

llvm::Error auditPreFixup(LinkGraph &Graph, SealedLinkState &State) {
  constexpr auto Code = TranslationJITLinkerErrorCode::PreFixupAuditFailed;
  if (!hasStage(State.Receipt, TranslationJITLinkAuditStageV1::Resolved))
    return failure(Code, "resolution audit did not complete");
  if (llvm::Error Error = verifyOriginalNodes(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyOriginalContent(State, Code))
    return Error;
  if (llvm::Error Error = verifyCapturedTargetNodes(Graph, State, Code, true))
    return Error;
  if (llvm::Error Error = verifyRuntimeCallRoutes(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyGraphCardinality(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyAllocatedAddresses(Graph, State, Code))
    return Error;

  for (const RuntimeTargetRecord &Record : State.RuntimeTargets) {
    if (Record.External->getAddress() != Record.RegistryAddress)
      return failure(Code,
                     "external address differs from the production registry");
    const int64_t PageDelta =
        static_cast<int64_t>(Record.GOTSymbol->getAddress().getValue() &
                             ~uint64_t{0xfff}) -
        static_cast<int64_t>(Record.StubSymbol->getAddress().getValue() &
                             ~uint64_t{0xfff});
    if ((PageDelta & 0xfff) != 0 || PageDelta < -(int64_t{1} << 32) ||
        PageDelta > (int64_t{1} << 32) - 0x1000)
      return failure(Code, "stub cannot address its GOT page with ADRP");
  }
  for (const RuntimeCallRecord &Call : State.RuntimeCalls) {
    const Edge *Branch = edgeAt(*Call.SourceBlock, Call.Offset);
    if (!Branch ||
        !branch26InRange(
            Call.SourceBlock->getFixupAddress(*Branch),
            State.RuntimeTargets[Call.TargetIndex].StubSymbol->getAddress()))
      return failure(Code, "runtime call cannot reach its sealed stub");
  }
  completeStage(State.Receipt, TranslationJITLinkAuditStageV1::PreFixup);
  return llvm::Error::success();
}

llvm::orc::ExecutorAddr decodeADRPPage(llvm::orc::ExecutorAddr Fixup,
                                       uint32_t Instruction) {
  const uint64_t Immediate =
      ((static_cast<uint64_t>(Instruction) >> 29) & 0x3U) |
      (((static_cast<uint64_t>(Instruction) >> 5) & 0x7ffffU) << 2);
  const int64_t Delta = llvm::SignExtend64<33>(Immediate << 12);
  return llvm::orc::ExecutorAddr(Fixup.getValue() & ~uint64_t{0xfff}) + Delta;
}

llvm::Error verifyFixedOriginalContent(const SealedLinkState &State,
                                       TranslationJITLinkerErrorCode Code) {
  std::map<Block *, std::set<uint64_t>> Fixups;
  for (const RuntimeCallRecord &Call : State.RuntimeCalls)
    Fixups[Call.SourceBlock].insert(Call.Offset);
  for (const OriginalBlockRecord &Record : State.OriginalBlockContents) {
    const llvm::ArrayRef<char> Current = Record.GraphBlock->getContent();
    if (Current.size() != Record.Content.size())
      return failure(Code, "an original block changed size after fixups");
    for (uint64_t Offset = 0; Offset != Current.size(); ++Offset) {
      bool IsBranchByte = false;
      const auto BlockFixups = Fixups.find(Record.GraphBlock);
      if (BlockFixups != Fixups.end())
        for (uint64_t Fixup : BlockFixups->second)
          IsBranchByte |= Offset >= Fixup && Offset < Fixup + sizeof(uint32_t);
      if (!IsBranchByte && Current[Offset] != Record.Content[Offset])
        return failure(Code,
                       "non-fixup bytes changed in an original text block");
    }
  }
  return llvm::Error::success();
}

llvm::Error auditPostFixup(LinkGraph &Graph, SealedLinkState &State) {
  constexpr auto Code = TranslationJITLinkerErrorCode::PostFixupAuditFailed;
  if (!hasStage(State.Receipt, TranslationJITLinkAuditStageV1::PreFixup))
    return failure(Code, "pre-fixup audit did not complete");
  if (llvm::Error Error = verifyOriginalNodes(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyCapturedTargetNodes(Graph, State, Code, false))
    return Error;
  if (llvm::Error Error = verifyRuntimeCallRoutes(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyGraphCardinality(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyAllocatedAddresses(Graph, State, Code))
    return Error;
  if (llvm::Error Error = verifyFixedOriginalContent(State, Code))
    return Error;

  for (const RuntimeCallRecord &Call : State.RuntimeCalls) {
    const Edge *Branch = edgeAt(*Call.SourceBlock, Call.Offset);
    const uint32_t Instruction = llvm::support::endian::read32le(
        Call.SourceBlock->getContent().data() + Call.Offset);
    if (!Branch || !detail::isSealedAArch64Branch26FixupV1(
                       Call.OriginalInstruction, Instruction,
                       Call.SourceBlock->getFixupAddress(*Branch).getValue(),
                       State.RuntimeTargets[Call.TargetIndex]
                           .StubSymbol->getAddress()
                           .getValue()))
      return failure(
          Code,
          "final Branch26 changed opcode or missed its sealed stub target");
  }

  for (const RuntimeTargetRecord &Record : State.RuntimeTargets) {
    const llvm::ArrayRef<char> Stub = Record.StubBlock->getContent();
    const uint32_t ADRP = llvm::support::endian::read32le(Stub.data());
    const uint32_t LDR =
        llvm::support::endian::read32le(Stub.data() + sizeof(uint32_t));
    const uint32_t BR =
        llvm::support::endian::read32le(Stub.data() + 2 * sizeof(uint32_t));
    constexpr uint32_t ADRPImmediateMask = 0x60ffffe0U;
    constexpr uint32_t LDRImmediateMask = 0x003ffc00U;
    if ((ADRP & ~ADRPImmediateMask) != 0x90000010U ||
        (LDR & ~LDRImmediateMask) != 0xf9400210U || BR != 0xd61f0200U)
      return failure(Code, "final stub is not canonical ADRP/LDR/BR x16");
    const llvm::orc::ExecutorAddr GOTPage =
        decodeADRPPage(Record.StubBlock->getAddress(), ADRP);
    const uint64_t GOTOffset = ((LDR >> 10) & 0xfffU) << 3;
    if (GOTPage + GOTOffset != Record.GOTSymbol->getAddress())
      return failure(Code, "final ADRP/LDR does not resolve to the sealed GOT");
    const uint64_t Pointer =
        llvm::support::endian::read64le(Record.GOTBlock->getContent().data());
    if (Pointer != Record.RegistryAddress.getValue())
      return failure(Code,
                     "final GOT pointer differs from the production registry");
  }

  const GraphCounts Counts = countGraph(Graph);
  State.Receipt.FinalSectionCount = Counts.Sections;
  State.Receipt.FinalBlockCount = Counts.Blocks;
  State.Receipt.FinalEdgeCount = Counts.Edges;
  completeStage(State.Receipt, TranslationJITLinkAuditStageV1::PostFixup);
  return llvm::Error::success();
}

llvm::Error buildAArch64Tables(LinkGraph &Graph, const SealedLinkState &State) {
  if (State.RuntimeTargets.empty())
    return llvm::Error::success();
  llvm::jitlink::aarch64::GOTTableManager GOT(Graph);
  llvm::jitlink::aarch64::PLTTableManager PLT(Graph, GOT);
  llvm::jitlink::visitExistingEdges(Graph, GOT, PLT);
  return llvm::Error::success();
}

llvm::Error normalizeLinkError(llvm::Error Error,
                               TranslationJITLinkerErrorCode Fallback) {
  std::optional<TranslationJITLinkerErrorCode> TypedCode;
  std::string Detail;
  llvm::raw_string_ostream Stream(Detail);
  llvm::handleAllErrors(
      std::move(Error),
      [&](const TranslationJITLinkerError &LinkerError) {
        if (!TypedCode)
          TypedCode = LinkerError.code();
        if (!LinkerError.detail().empty()) {
          if (!Detail.empty())
            Stream << "; ";
          Stream << LinkerError.detail();
        }
      },
      [&](const llvm::ErrorInfoBase &Other) {
        if (!Detail.empty())
          Stream << "; ";
        Other.log(Stream);
      });
  return failure(TypedCode.value_or(Fallback), Stream.str());
}

class SealedJITLinkContext final : public JITLinkContext {
public:
  SealedJITLinkContext(llvm::jitlink::JITLinkMemoryManager &MemoryManager,
                       std::shared_ptr<SealedLinkState> State)
      : JITLinkContext(nullptr), MemoryManager(MemoryManager),
        State(std::move(State)) {}

  llvm::jitlink::JITLinkMemoryManager &getMemoryManager() override {
    return MemoryManager;
  }

  void notifyFailed(llvm::Error Error) override {
    State->CompletionPromise.set_value(normalizeLinkError(
        std::move(Error), TranslationJITLinkerErrorCode::FinalizationFailed));
  }

  void lookup(const LookupMap &Symbols,
              std::unique_ptr<llvm::jitlink::JITLinkAsyncLookupContinuation>
                  Continuation) override {
    // A synchronous continuation may finish the link and destroy its graph
    // before run() returns.  Its AsyncLookupResult still owns SymbolStringPtr
    // keys at that point, so keep the pool alive through their destruction.
    const std::shared_ptr<llvm::orc::SymbolStringPool> SymbolPoolKeepAlive =
        State->SymbolPool;
    if (Symbols.size() != State->RuntimeTargets.size()) {
      Continuation->run(failure(
          TranslationJITLinkerErrorCode::RuntimeSymbolLookupFailed,
          "lookup set does not exactly equal referenced runtime targets"));
      return;
    }
    llvm::jitlink::AsyncLookupResult Result;
    for (const auto &[Name, LookupFlags] : Symbols) {
      if (LookupFlags != llvm::jitlink::SymbolLookupFlags::RequiredSymbol) {
        Continuation->run(
            failure(TranslationJITLinkerErrorCode::RuntimeSymbolLookupFailed,
                    "weak runtime lookup is forbidden"));
        return;
      }
      const auto Binding = State->Bindings.find(*Name);
      if (Binding == State->Bindings.end()) {
        Continuation->run(failure(
            TranslationJITLinkerErrorCode::RuntimeSymbolLookupFailed,
            "runtime lookup name is not in the exact object manifest: " +
                *Name));
        return;
      }
      llvm::Expected<llvm::orc::ExecutorAddr> Address =
          State->Registry->lookup(Binding->second.IRName);
      if (!Address || *Address != Binding->second.Address) {
        std::string LookupDetail = Address
                                       ? "runtime lookup address changed"
                                       : llvm::toString(Address.takeError());
        Continuation->run(
            failure(TranslationJITLinkerErrorCode::RuntimeSymbolLookupFailed,
                    LookupDetail));
        return;
      }
      Result[Name] = {*Address, llvm::JITSymbolFlags::Callable};
    }
    Continuation->run(std::move(Result));
  }

  llvm::Error notifyResolved(LinkGraph &Graph) override {
    return auditResolved(Graph, *State);
  }

  void
  notifyFinalized(JITLinkMemoryManager::FinalizedAlloc Allocation) override {
    if (!hasStage(State->Receipt, TranslationJITLinkAuditStageV1::PostFixup) ||
        State->ResolvedEntryAddress.isNull()) {
      llvm::Error DeallocateError =
          MemoryManager.deallocate(std::move(Allocation));
      std::string Detail = "finalized before all sealed audits completed";
      if (DeallocateError)
        Detail += "; deallocation failed: " +
                  llvm::toString(std::move(DeallocateError));
      State->CompletionPromise.set_value(
          failure(TranslationJITLinkerErrorCode::FinalizationFailed, Detail));
      return;
    }
    completeStage(State->Receipt, TranslationJITLinkAuditStageV1::Finalized);
    State->CompletionPromise.set_value(LinkCompletion{
        std::move(Allocation), State->ResolvedEntryAddress, State->Receipt});
  }

  bool shouldAddDefaultTargetPasses(const llvm::Triple &) const override {
    return false;
  }

  llvm::Error modifyPassConfig(LinkGraph &,
                               PassConfiguration &Config) override {
    Config.PrePrunePasses.push_back([State = State](
                                        LinkGraph &Graph) -> llvm::Error {
      llvm::Expected<TranslationLinkGraphAuditV1> Audit =
          detail::auditTranslationLinkGraphPrePruneV1(
              Graph, State->ObjectBytes, State->Artifact->hostTarget(),
              State->Artifact->blockSymbols(),
              State->Artifact->runtimeSymbols(),
              State->Artifact->runtimeRegistryIdentity());
      if (!Audit)
        return failure(TranslationJITLinkerErrorCode::PrePruneAuditFailed,
                       llvm::toString(Audit.takeError()));
      if (llvm::Error Error = capturePrePruneGraph(Graph, *State))
        return Error;
      completeStage(State->Receipt, TranslationJITLinkAuditStageV1::PrePrune);
      return llvm::Error::success();
    });
    Config.PrePrunePasses.push_back(llvm::jitlink::markAllSymbolsLive);
    Config.PostPrunePasses.push_back([State = State](LinkGraph &Graph) {
      return buildAArch64Tables(Graph, *State);
    });
    Config.PostPrunePasses.push_back([State = State](LinkGraph &Graph) {
      return auditPostPrune(Graph, *State);
    });
    Config.PostAllocationPasses.push_back([State = State](LinkGraph &Graph) {
      return auditPostAllocation(Graph, *State);
    });
    Config.PreFixupPasses.push_back([State = State](LinkGraph &Graph) {
      return auditPreFixup(Graph, *State);
    });
    Config.PostFixupPasses.push_back([State = State](LinkGraph &Graph) {
      return auditPostFixup(Graph, *State);
    });
    return llvm::Error::success();
  }

private:
  llvm::jitlink::JITLinkMemoryManager &MemoryManager;
  std::shared_ptr<SealedLinkState> State;
};

llvm::Error
verifyProductionRegistry(const TranslationObjectArtifactV1 &Artifact,
                         const RuntimeSymbolRegistryV1 &Registry) {
  llvm::Expected<RuntimeSymbolRegistryV1> ProductionOrErr =
      RuntimeSymbolRegistryV1::create();
  if (!ProductionOrErr)
    return failure(TranslationJITLinkerErrorCode::RuntimeRegistryMismatch,
                   llvm::toString(ProductionOrErr.takeError()));
  const RuntimeSymbolRegistryV1 &Production = *ProductionOrErr;
  if (Artifact.runtimeRegistryIdentity().empty() ||
      Artifact.runtimeRegistryIdentity() != Registry.identity() ||
      Registry.identity() != Production.identity() ||
      Registry.entries().size() != Production.entries().size())
    return failure(TranslationJITLinkerErrorCode::RuntimeRegistryMismatch,
                   "registry identity or cardinality differs from production");
  for (size_t Index = 0; Index != Registry.entries().size(); ++Index) {
    const RuntimeSymbolEntryV1 &Actual = Registry.entries()[Index];
    const RuntimeSymbolEntryV1 &Expected = Production.entries()[Index];
    if (Actual.name() != Expected.name() ||
        Actual.helperClass() != Expected.helperClass() ||
        Actual.address() != Expected.address())
      return failure(
          TranslationJITLinkerErrorCode::RuntimeRegistryMismatch,
          "registry name, class, or address differs from production: " +
              Actual.name());
  }
  return llvm::Error::success();
}

llvm::Expected<ResolvedHostTarget> currentAArch64Target() {
  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;
  Options.Mode = TranslationMode::JIT;
  Options.Target.Kind = HostTargetKind::Native;
  llvm::Expected<TranslationTargetMachineV1> Machine =
      createTranslationTargetMachineV1(Options);
  if (!Machine)
    return failure(TranslationJITLinkerErrorCode::UnsupportedProcessTarget,
                   llvm::toString(Machine.takeError()));
  ResolvedHostTarget Target = Machine->hostTarget();
  const llvm::Triple Triple(Target.triple());
  if (Target.architecture() != GuestArchitecture::AArch64 ||
      Triple.getArch() != llvm::Triple::aarch64 || Triple.isArm64e() ||
      !Triple.isLittleEndian() || Triple.getArchPointerBitWidth() != 64 ||
      (!Triple.isOSBinFormatELF() && !Triple.isOSBinFormatMachO()))
    return failure(TranslationJITLinkerErrorCode::UnsupportedProcessTarget,
                   "v1 requires a native little-endian AArch64 ELF or Mach-O "
                   "process");
  return Target;
}

bool equalTarget(const ResolvedHostTarget &Left,
                 const ResolvedHostTarget &Right) {
  return Left.requestedTarget().Kind == Right.requestedTarget().Kind &&
         Left.architecture() == Right.architecture() &&
         Left.triple() == Right.triple() && Left.cpu() == Right.cpu() &&
         Left.features() == Right.features() && Left.hasCanonicalDataLayout() &&
         Right.hasCanonicalDataLayout() &&
         Left.canonicalDataLayout() == Right.canonicalDataLayout() &&
         Left.cacheKey() == Right.cacheKey();
}

llvm::Error
verifyCompilerArtifactShape(const TranslationObjectArtifactV1 &Artifact) {
  if (Artifact.bytes().empty() || Artifact.blockSymbols().size() != 1 ||
      Artifact.requestCacheKey().empty() ||
      !Artifact.requestCacheKey().starts_with(RequestIdentityPrefix) ||
      Artifact.artifactCacheKey().empty() ||
      !Artifact.artifactCacheKey().starts_with(ArtifactIdentityPrefix) ||
      !Artifact.hostTarget().hasCanonicalDataLayout())
    return failure(TranslationJITLinkerErrorCode::InvalidArtifact,
                   "artifact lacks one complete compiler-owned v1 identity");
  return llvm::Error::success();
}

llvm::Error verifyRawArtifact(const TranslationObjectArtifactV1 &Artifact) {
  llvm::SmallVector<llvm::StringRef, 4> Blocks;
  llvm::SmallVector<llvm::StringRef, 16> Runtime;
  for (const TranslationObjectSymbolV1 &Symbol : Artifact.blockSymbols())
    Blocks.push_back(Symbol.ObjectName);
  for (const TranslationObjectSymbolV1 &Symbol : Artifact.runtimeSymbols())
    Runtime.push_back(Symbol.ObjectName);
  const TranslationArtifactPolicyV1 Policy(Blocks, Runtime);
  if (llvm::Error Error = verifyTranslationArtifact(
          Artifact.bytes(), llvm::Triple(Artifact.hostTarget().triple()),
          Policy))
    return failure(TranslationJITLinkerErrorCode::ArtifactAuditFailed,
                   llvm::toString(std::move(Error)));
  return llvm::Error::success();
}

} // namespace

struct LinkedTranslationBlockV1::Impl {
  Impl(std::unique_ptr<llvm::jitlink::InProcessMemoryManager> MemoryManager,
       JITLinkMemoryManager::FinalizedAlloc Allocation,
       llvm::orc::ExecutorAddr EntryAddress)
      : MemoryManager(std::move(MemoryManager)),
        Allocation(std::move(Allocation)), EntryAddress(EntryAddress) {}

  ~Impl() { llvm::consumeError(unload()); }

  bool isLoaded() const noexcept {
    std::lock_guard<std::mutex> Lock(Mutex);
    return static_cast<bool>(Allocation) && !EntryAddress.isNull() &&
           !Unloading;
  }

  std::optional<llvm::orc::ExecutorAddr> beginInvocation() {
    std::lock_guard<std::mutex> Lock(Mutex);
    if (!Allocation || EntryAddress.isNull() || Unloading)
      return std::nullopt;
    ++ActiveInvocations;
    return EntryAddress;
  }

  void endInvocation() noexcept {
    std::lock_guard<std::mutex> Lock(Mutex);
    assert(ActiveInvocations != 0 && "unbalanced translation invocation");
    --ActiveInvocations;
    if (ActiveInvocations == 0)
      InvocationEnded.notify_all();
  }

  llvm::Error unload() {
    std::unique_lock<std::mutex> Lock(Mutex);
    InvocationEnded.wait(Lock, [&] { return !Unloading; });
    if (!Allocation) {
      EntryAddress = llvm::orc::ExecutorAddr();
      return llvm::Error::success();
    }
    Unloading = true;
    EntryAddress = llvm::orc::ExecutorAddr();
    InvocationEnded.wait(Lock, [&] { return ActiveInvocations == 0; });
    JITLinkMemoryManager::FinalizedAlloc AllocationToRelease =
        std::move(Allocation);
    Lock.unlock();
    llvm::Error DeallocateError =
        MemoryManager->deallocate(std::move(AllocationToRelease));
    Lock.lock();
    Unloading = false;
    InvocationEnded.notify_all();
    if (DeallocateError)
      return failure(TranslationJITLinkerErrorCode::UnloadFailed,
                     llvm::toString(std::move(DeallocateError)));
    return llvm::Error::success();
  }

  std::unique_ptr<llvm::jitlink::InProcessMemoryManager> MemoryManager;
  mutable std::mutex Mutex;
  std::condition_variable InvocationEnded;
  JITLinkMemoryManager::FinalizedAlloc Allocation;
  llvm::orc::ExecutorAddr EntryAddress;
  uint64_t ActiveInvocations = 0;
  bool Unloading = false;
  std::optional<RuntimeCodeCredentialV1> InvocationCredential;
};

LinkedTranslationBlockV1::LinkedTranslationBlockV1(
    std::unique_ptr<Impl> State, TranslationJITLinkAuditReceiptV1 Receipt)
    : State(std::move(State)), Receipt(std::move(Receipt)) {}

LinkedTranslationBlockV1::LinkedTranslationBlockV1(
    LinkedTranslationBlockV1 &&) noexcept = default;

LinkedTranslationBlockV1 &LinkedTranslationBlockV1::operator=(
    LinkedTranslationBlockV1 &&) noexcept = default;

LinkedTranslationBlockV1::~LinkedTranslationBlockV1() = default;

bool LinkedTranslationBlockV1::isLoaded() const noexcept {
  return State && State->isLoaded();
}

llvm::Expected<uint32_t>
LinkedTranslationBlockV1::invoke(RuntimeGuestStateX86_64V1 &GuestState,
                                 RuntimeCallFrameV1 &Runtime) const {
  if (!State)
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   "linked block is unloaded");
  const std::optional<llvm::orc::ExecutorAddr> EntryAddress =
      State->beginInvocation();
  if (!EntryAddress)
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   "linked block is unloaded");
  llvm::scope_exit InvocationLease(
      [State = State.get()] { State->endInvocation(); });
  if (llvm::Error Error = validateRuntimeGuestStateX86_64V1(GuestState))
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   llvm::toString(std::move(Error)));
  if (!Runtime.Memory)
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   "runtime frame has no guest-memory owner");
  if (!State->InvocationCredential ||
      Runtime.Published != *State->InvocationCredential ||
      Runtime.Validated != *State->InvocationCredential)
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   "runtime credential is not bound to this linked block");
  if (GuestState.RIP != State->InvocationCredential->EntryPC)
    return failure(
        TranslationJITLinkerErrorCode::InvocationRejected,
        "runtime guest RIP does not match the bound credential entry PC");
  if (llvm::Error Error = validateRuntimeControlBlockV1(Runtime.Control))
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   llvm::toString(std::move(Error)));
  if (llvm::Error Error =
          validateRuntimeCodeCredentialV1(Runtime.Published, Runtime.Validated))
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   llvm::toString(std::move(Error)));

  using EntryFunction =
      uint32_t(RuntimeGuestStateX86_64V1 *, RuntimeCallFrameV1 *);
  EntryFunction *Entry = EntryAddress->toPtr<EntryFunction>();
  const uint32_t Result = Entry(&GuestState, &Runtime);
  if (llvm::Error Error = validateRuntimeGuestStateX86_64V1(GuestState))
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   "generated code produced invalid guest state: " +
                       llvm::toString(std::move(Error)));
  if (llvm::Error Error =
          validateRuntimeControlBlockAfterInvocationV1(Runtime.Control, Result))
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   "runtime helper produced invalid control state: " +
                       llvm::toString(std::move(Error)));
  return Result;
}

llvm::Error LinkedTranslationBlockV1::unload() {
  if (!State)
    return llvm::Error::success();
  return State->unload();
}

llvm::Expected<LinkedTranslationBlockV1>
linkTranslationObjectV1(const TranslationObjectArtifactV1 &Artifact,
                        const RuntimeSymbolRegistryV1 &Registry) {
  if (llvm::Error Error = verifyCompilerArtifactShape(Artifact))
    return std::move(Error);
  if (Artifact.hostTarget().requestedTarget().Kind != HostTargetKind::Native)
    return failure(TranslationJITLinkerErrorCode::ArtifactTargetNotNative);
  if (llvm::Error Error = verifyProductionRegistry(Artifact, Registry))
    return std::move(Error);

  llvm::Expected<ResolvedHostTarget> CurrentTarget = currentAArch64Target();
  if (!CurrentTarget)
    return CurrentTarget.takeError();
  if (!equalTarget(Artifact.hostTarget(), *CurrentTarget))
    return failure(TranslationJITLinkerErrorCode::ProcessTargetMismatch,
                   "triple, CPU, features, data layout, or cache identity "
                   "differs from the current process");
  if (llvm::Error Error = verifyRawArtifact(Artifact))
    return std::move(Error);

  llvm::Expected<TranslationLinkGraphAuditV1> GraphAudit =
      verifyTranslationLinkGraphV1(Artifact);
  if (!GraphAudit)
    return failure(TranslationJITLinkerErrorCode::LinkGraphAuditFailed,
                   llvm::toString(GraphAudit.takeError()));

  std::shared_ptr<SealedLinkState> LinkState =
      std::make_shared<SealedLinkState>(Artifact, Registry);
  if (!LinkState->BindingError.empty() ||
      LinkState->Bindings.size() != Artifact.runtimeSymbols().size())
    return failure(TranslationJITLinkerErrorCode::RuntimeRegistryMismatch,
                   LinkState->BindingError.empty()
                       ? "runtime object-name mapping is duplicated"
                       : LinkState->BindingError);
  completeStage(LinkState->Receipt,
                TranslationJITLinkAuditStageV1::RawArtifact);
  completeStage(LinkState->Receipt,
                TranslationJITLinkAuditStageV1::PreallocationGraph);

  const llvm::StringRef ObjectData(
      reinterpret_cast<const char *>(LinkState->ObjectBytes.data()),
      LinkState->ObjectBytes.size());
  llvm::Expected<std::unique_ptr<LinkGraph>> Graph =
      llvm::jitlink::createLinkGraphFromObject(
          llvm::MemoryBufferRef(ObjectData, "sealed-translation-object-v1"),
          LinkState->SymbolPool);
  if (!Graph)
    return failure(TranslationJITLinkerErrorCode::LinkGraphCreationFailed,
                   llvm::toString(Graph.takeError()));

  llvm::Expected<std::unique_ptr<llvm::jitlink::InProcessMemoryManager>>
      MemoryManager = llvm::jitlink::InProcessMemoryManager::Create();
  if (!MemoryManager)
    return failure(TranslationJITLinkerErrorCode::FinalizationFailed,
                   llvm::toString(MemoryManager.takeError()));

  std::future<llvm::MSVCPExpected<LinkCompletion>> CompletionFuture =
      LinkState->CompletionPromise.get_future();
  llvm::jitlink::link(std::move(*Graph), std::make_unique<SealedJITLinkContext>(
                                             **MemoryManager, LinkState));
  llvm::MSVCPExpected<LinkCompletion> Completion = CompletionFuture.get();
  if (!Completion)
    return Completion.takeError();

  auto Implementation = std::make_unique<LinkedTranslationBlockV1::Impl>(
      std::move(*MemoryManager), std::move(Completion->Allocation),
      Completion->EntryAddress);
  return LinkedTranslationBlockV1(std::move(Implementation),
                                  std::move(Completion->Receipt));
}

llvm::Expected<LinkedTranslationBlockV1>
linkTranslationObjectV1(const TranslationObjectResultV1 &Object,
                        const RuntimeSymbolRegistryV1 &Registry,
                        const RuntimeCodeCredentialV1 &Credential) {
  if (llvm::Error Error =
          validateTranslationBlockDescriptorV1(Object.descriptor()))
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   "translation descriptor is invalid: " +
                       llvm::toString(std::move(Error)));
  if (llvm::Error Error =
          validateRuntimeCodeCredentialV1(Credential, Credential))
    return failure(TranslationJITLinkerErrorCode::InvocationRejected,
                   "invocation credential is invalid: " +
                       llvm::toString(std::move(Error)));
  if (Credential.EntryPC != Object.descriptor().Header.EntryPC)
    return failure(
        TranslationJITLinkerErrorCode::InvocationRejected,
        "credential entry PC does not match the translation descriptor");
  llvm::Expected<LinkedTranslationBlockV1> Linked =
      linkTranslationObjectV1(Object.artifact(), Registry);
  if (!Linked)
    return Linked.takeError();
  Linked->State->InvocationCredential = Credential;
  Linked->Receipt.InvocationCredentialBound = true;
  return std::move(*Linked);
}

} // namespace neverd::translate
