//===- SBFCallXContexts.h - Conditional indirect call evidence --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recover CALLX targets under proven direct-caller argument and scratch facts.
/// Evidence is conditional on the recorded caller, never a closed global target
/// set or permission to remove dynamic dispatch.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYSIS_SBFCALLXCONTEXTS_H
#define NEVERD_SBF_ANALYSIS_SBFCALLXCONTEXTS_H

#include "neverd/sbf/SBFIR.h"
#include "neverd/sbf/analysis/SBFDataflow.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::sbf {

enum class CallXContextResult : uint8_t {
  ProvenTarget,
  UnknownTarget,
  InvalidTarget,
  LegacyRuntimeMayMutateArguments,
};

enum class CallXMissingFactKind : uint8_t {
  None,
  IncomingRegister,
  ScratchWord,
  ScratchLoadWidth,
  RuntimeMemory,
  UnmappedLoad,
  RegisterWrite,
  SymbolicTarget,
  BlockEntry,
  FunctionReturn,
  MultipleTargetValues,
};

struct CallXMissingFact {
  CallXMissingFactKind Kind = CallXMissingFactKind::None;
  std::optional<size_t> Slot;
  std::optional<va_t> Address;
  MemoryCoordinate Coordinate = MemoryCoordinate::Absolute;
  std::optional<unsigned> InputRegister;
};

enum class CallXInputGeneralization : uint8_t {
  None,
  UnknownEntry,
  UnknownCaller,
  SpecializationLimit,
  ResourceBudget,
  EquivalentGenericInputs,
};

struct CallXContextEvidence {
  size_t CallerSlot = 0;
  size_t CalleeEntrySlot = 0;
  size_t CallXSlot = 0;
  CallXContextResult Result = CallXContextResult::UnknownTarget;
  std::optional<va_t> TargetAddress;
  std::optional<size_t> TargetSlot;
  bool TargetIsContinuation = false;
  CallXMissingFact Missing;
  ProvenValueSet TargetValueAlternatives;
  size_t SummaryID = 0;
  std::optional<size_t> CallerSummaryID;
  bool IsRootContext = false;
  bool BeyondEntryBlock = false;
  bool EntryPrefixOnly = false;
  bool GeneralizedInputs = false;
  CallXInputGeneralization InputGeneralization = CallXInputGeneralization::None;
};

enum class CallXSummaryBarrier : uint8_t {
  None,
  RecursiveCall,
  CallDepth,
  UnknownTarget,
  UnsupportedRawEntry,
  SummaryBudget,
};

struct CallXSummaryEdge {
  size_t CallerSummaryID = 0;
  size_t CallerSlot = 0;
  std::optional<size_t> CalleeSummaryID;
  std::optional<size_t> CalleeEntrySlot;
  CallXSummaryBarrier Barrier = CallXSummaryBarrier::None;
  bool LegacyRuntime = false;
  bool PreservedOlderFrames = false;
  bool AppliedMemoryEffects = false;
  bool GeneralizedInputs = false;
  CallXInputGeneralization InputGeneralization = CallXInputGeneralization::None;
};

enum class CallXMemoryWriteBase : uint8_t {
  InputRegister,
  FramePointer,
  Absolute
};

struct CallXMemoryWrite {
  CallXMemoryWriteBase Base = CallXMemoryWriteBase::InputRegister;
  unsigned Register = 0;
  va_t Address = 0;
  int64_t Offset = 0;
  uint64_t Size = 0;
};

struct CallXFunctionSummary {
  size_t ID = 0;
  size_t EntrySlot = 0;
  uint16_t InputRegisterMask = 0;
  bool ReadsInputMemory = true;
  RegisterState Inputs{};
  std::vector<std::pair<unsigned, ProvenValueSet>> InputAlternatives;
  RegisterValue ReturnValue;
  ProvenValueSet ReturnAlternatives;
  std::optional<std::pair<unsigned, int64_t>> ReturnInputExpression;
  bool MayReturn = false;
  bool IsRoot = false;
  bool GeneralizedInputs = false;
  bool PreservesCallerMemory = false;
  bool WritesOnlyOwnAndChildFrames = false;
  bool MayWriteUnknownMemory = true;
  bool MayWriteCalleeFrames = false;
  std::vector<CallXMemoryWrite> MemoryWrites;
  size_t MinimumDepth = 0;
  size_t InputScratchWords = 0;
  size_t OutputScratchWords = 0;
};

struct CallXContextReport {
  std::vector<size_t> UnresolvedCallXSlots;
  size_t EntryPrefixCallXCount = 0;
  ScratchFlowPrecision PrefixScratchPrecision = ScratchFlowPrecision::Exact;
  std::vector<CallXContextEvidence> Contexts;
  std::vector<CallXFunctionSummary> Summaries;
  std::vector<CallXSummaryEdge> SummaryEdges;
  size_t InstructionVisits = 0;
  size_t SummaryCacheHits = 0;
  uint64_t PeakRetainedBytes = 0;
  bool SpecializationBudgetExhausted = false;
  size_t ProjectedRegisters = 0;
  size_t ProjectedScratchSnapshots = 0;
  size_t InputDependencyVisits = 0;
  size_t SummaryCacheEvictions = 0;
  size_t EquivalentGenericReuses = 0;
  size_t SkippedNonCallXRootSpecializations = 0;
};

/// Analyze complete recognized function CFGs with context-keyed summaries.
/// Pre-call parameters and typed memory enter the callee; converged returns
/// provide r0 and memory, while the canonical ABI clobbers r1-r5 and restores
/// r6-r10. Loops meet all paths. Recursive/unknown calls havoc their effects;
/// unknown indirect entries and other callers remain open.
/// Specialization exhaustion reuses completed generic summaries and records
/// precision loss. Generic-analysis/output exhaustion fails the whole query;
/// no provisional target or unfinished summary is published.
llvm::Expected<CallXContextReport>
recoverCallXContexts(const SBFProgram &Program);

llvm::json::Object callXContextReportJSON(const SBFProgram &Program,
                                          const CallXContextReport &Report);

} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYSIS_SBFCALLXCONTEXTS_H
