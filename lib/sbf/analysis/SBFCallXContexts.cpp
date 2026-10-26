//===- SBFCallXContexts.cpp - Conditional indirect call evidence ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Serializes conditional whole-function summaries and their call dependencies.
/// Dynamic fallback remains mandatory for every reported site.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/analysis/SBFCallXContexts.h"

#include "neverd/sbf/analysis/SBFAnalysisLimits.h"

#include "llvm/Support/FormatVariadic.h"

#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace neverd::sbf {
namespace {

llvm::StringRef resultName(CallXContextResult Result) {
  switch (Result) {
  case CallXContextResult::ProvenTarget:
    return "proven_target";
  case CallXContextResult::UnknownTarget:
    return "unknown_target_value";
  case CallXContextResult::InvalidTarget:
    return "target_outside_raw_text_slots";
  case CallXContextResult::LegacyRuntimeMayMutateArguments:
    return "registered_legacy_runtime_may_mutate_arguments";
  }
  llvm_unreachable("unhandled CALLX context result");
}

std::string addressText(va_t Address) {
  return llvm::formatv("{0:X}", Address).str();
}

llvm::json::Object valueJSON(const RegisterValue &Value) {
  const auto Kind = [&]() -> llvm::StringRef {
    switch (Value.ValueKind) {
    case RegisterValue::Kind::Unknown:
      return "unknown";
    case RegisterValue::Kind::Constant:
      return "constant";
    case RegisterValue::Kind::StackAddress:
      return "frame_relative_address";
    case RegisterValue::Kind::RodataAddress:
      return "rodata_address";
    case RegisterValue::Kind::InstructionDataAddress:
      return "instruction_data_address";
    }
    llvm_unreachable("unknown register value kind");
  };
  llvm::json::Object Result{{"kind", Kind()}};
  if (Value.ValueKind != RegisterValue::Kind::Unknown) {
    Result["base"] = addressText(Value.Value);
    Result["offset"] = Value.Offset;
  }
  return Result;
}

llvm::StringRef barrierName(CallXSummaryBarrier Barrier) {
  switch (Barrier) {
  case CallXSummaryBarrier::None:
    return "none";
  case CallXSummaryBarrier::RecursiveCall:
    return "recursive_call_effects_unknown";
  case CallXSummaryBarrier::CallDepth:
    return "vm_call_depth_summary_boundary";
  case CallXSummaryBarrier::UnknownTarget:
    return "unknown_call_target_effects";
  case CallXSummaryBarrier::UnsupportedRawEntry:
    return "raw_target_without_decoded_block_entry";
  case CallXSummaryBarrier::SummaryBudget:
    return "summary_unavailable_after_specialization_budget";
  }
  llvm_unreachable("unknown summary barrier");
}

llvm::StringRef generalizationName(CallXInputGeneralization Kind) {
  switch (Kind) {
  case CallXInputGeneralization::None:
    return "none";
  case CallXInputGeneralization::UnknownEntry:
    return "unknown_entry_inputs";
  case CallXInputGeneralization::UnknownCaller:
    return "generic_caller_inputs";
  case CallXInputGeneralization::SpecializationLimit:
    return "per_entry_specialization_limit";
  case CallXInputGeneralization::ResourceBudget:
    return "specialization_resource_budget";
  case CallXInputGeneralization::EquivalentGenericInputs:
    return "reused_equivalent_generic_leaf_inputs";
  }
  llvm_unreachable("unknown input generalization");
}

llvm::StringRef missingFactName(CallXMissingFactKind Kind) {
  switch (Kind) {
  case CallXMissingFactKind::None:
    return "none";
  case CallXMissingFactKind::IncomingRegister:
    return "unknown_incoming_register";
  case CallXMissingFactKind::ScratchWord:
    return "unproven_full_width_scratch_value";
  case CallXMissingFactKind::ScratchLoadWidth:
    return "partial_scratch_load_not_modeled";
  case CallXMissingFactKind::RuntimeMemory:
    return "runtime_memory_contents_unknown";
  case CallXMissingFactKind::UnmappedLoad:
    return "unmapped_or_non_data_visible_load";
  case CallXMissingFactKind::RegisterWrite:
    return "unproven_register_write";
  case CallXMissingFactKind::BlockEntry:
    return "unknown_value_at_block_entry";
  case CallXMissingFactKind::FunctionReturn:
    return "unproven_function_return_or_clobbered_argument";
  case CallXMissingFactKind::SymbolicTarget:
    return "symbolic_target_without_literal_code_address";
  case CallXMissingFactKind::MultipleTargetValues:
    return "bounded_target_values_require_multi_target_summary";
  }
  llvm_unreachable("unhandled CALLX missing fact");
}

} // namespace

llvm::json::Object callXContextReportJSON(const SBFProgram &Program,
                                          const CallXContextReport &Report) {
  llvm::json::Array Contexts;
  std::set<size_t> ProvenSites;
  std::map<size_t, size_t> ContextCounts;
  std::map<size_t, std::set<size_t>> Targets;
  size_t ProvenContexts = 0;
  size_t CallerContexts = 0;
  size_t ProvenCallerContexts = 0;
  std::set<size_t> BeyondEntrySites;
  for (const CallXContextEvidence &Evidence : Report.Contexts) {
    ++ContextCounts[Evidence.CallXSlot];
    const auto Address = [&](size_t Slot) {
      return addressText(Program.Low.TextAddress + Slot * kInstructionSize);
    };
    llvm::json::Object Row{
        {"context_kind",
         Evidence.IsRootContext ? "recognized_entry_root" : "caller_summary"},
        {"callee_entry_slot", static_cast<int64_t>(Evidence.CalleeEntrySlot)},
        {"callee_entry_address", Address(Evidence.CalleeEntrySlot)},
        {"callx_slot", static_cast<int64_t>(Evidence.CallXSlot)},
        {"callx_address", Address(Evidence.CallXSlot)},
        {"beyond_entry_basic_block", Evidence.BeyondEntryBlock},
        {"generalized_unknown_inputs", Evidence.GeneralizedInputs},
        {"input_generalization",
         generalizationName(Evidence.InputGeneralization)},
        {"result", resultName(Evidence.Result)},
        {"dynamic_fallback_required", true}};
    Row["evidence_scope"] = Evidence.EntryPrefixOnly
                                ? "direct_caller_entry_prefix"
                                : "whole_function_summary";
    if (!Evidence.EntryPrefixOnly)
      Row["summary_id"] = static_cast<int64_t>(Evidence.SummaryID);
    if (!Evidence.IsRootContext) {
      ++CallerContexts;
      Row["caller_slot"] = static_cast<int64_t>(Evidence.CallerSlot);
      Row["caller_address"] = Address(Evidence.CallerSlot);
      if (Evidence.CallerSummaryID)
        Row["caller_summary_id"] =
            static_cast<int64_t>(*Evidence.CallerSummaryID);
    }
    if (Evidence.TargetAddress)
      Row["target_address"] = addressText(*Evidence.TargetAddress);
    if (!Evidence.TargetValueAlternatives.unknown()) {
      llvm::json::Array Values;
      for (const auto &Value : Evidence.TargetValueAlternatives.values())
        Values.push_back(valueJSON(Value));
      Row["target_value_alternatives"] = std::move(Values);
    }
    if (Evidence.TargetSlot) {
      Row["target_slot"] = static_cast<int64_t>(*Evidence.TargetSlot);
      Row["target_is_continuation"] = Evidence.TargetIsContinuation;
      ProvenSites.insert(Evidence.CallXSlot);
      Targets[Evidence.CallXSlot].insert(*Evidence.TargetSlot);
      ++ProvenContexts;
      if (!Evidence.IsRootContext)
        ++ProvenCallerContexts;
      if (Evidence.BeyondEntryBlock)
        BeyondEntrySites.insert(Evidence.CallXSlot);
    }
    if (Evidence.Missing.Kind != CallXMissingFactKind::None) {
      llvm::json::Object Missing{
          {"reason", missingFactName(Evidence.Missing.Kind)}};
      if (Evidence.Missing.Slot) {
        Missing["slot"] = static_cast<int64_t>(*Evidence.Missing.Slot);
        Missing["instruction_address"] = Address(*Evidence.Missing.Slot);
      }
      if (Evidence.Missing.Address) {
        Missing["memory_address"] = addressText(*Evidence.Missing.Address);
        Missing["memory_coordinate"] =
            Evidence.Missing.Coordinate == MemoryCoordinate::FrameRelative
                ? "frame_relative_analysis_coordinate"
                : "absolute_vm_address";
      }
      if (Evidence.Missing.InputRegister)
        Missing["input_register"] =
            static_cast<int64_t>(*Evidence.Missing.InputRegister);
      Row["missing_fact"] = std::move(Missing);
    }
    Contexts.push_back(std::move(Row));
  }
  llvm::json::Array Sites;
  for (size_t Slot : Report.UnresolvedCallXSlots) {
    llvm::json::Array CandidateSlots;
    for (size_t Target : Targets[Slot])
      CandidateSlots.push_back(static_cast<int64_t>(Target));
    Sites.push_back(llvm::json::Object{
        {"callx_slot", static_cast<int64_t>(Slot)},
        {"callx_address",
         addressText(Program.Low.TextAddress + Slot * kInstructionSize)},
        {"analyzed_contexts", static_cast<int64_t>(ContextCounts[Slot])},
        {"coverage", ProvenSites.contains(Slot) ? "conditional_targets_only"
                     : ContextCounts[Slot]
                         ? "analyzed_contexts_unproven"
                         : "not_reached_from_recognized_entries"},
        {"conditional_target_slots", std::move(CandidateSlots)},
        {"unknown_other_contexts", true},
        {"dynamic_fallback_required", true}});
  }
  llvm::json::Array Summaries;
  for (const auto &Summary : Report.Summaries) {
    llvm::json::Array Inputs;
    for (unsigned R = 0; R < kRegisterCount; ++R)
      if (Summary.Inputs[R].ValueKind != RegisterValue::Kind::Unknown) {
        auto Value = valueJSON(Summary.Inputs[R]);
        Value["register"] = static_cast<int64_t>(R);
        Inputs.push_back(std::move(Value));
      }
    llvm::json::Array Alternatives;
    for (const auto &[Register, Values] : Summary.InputAlternatives) {
      llvm::json::Array Candidates;
      for (const auto &Value : Values.values())
        Candidates.push_back(valueJSON(Value));
      Alternatives.push_back(llvm::json::Object{
          {"register", Register}, {"values", std::move(Candidates)}});
    }
    llvm::json::Array Returns;
    if (Summary.ReturnAlternatives.values().size() > 1)
      for (const auto &Value : Summary.ReturnAlternatives.values())
        Returns.push_back(valueJSON(Value));
    llvm::json::Array Writes;
    for (const auto &Write : Summary.MemoryWrites) {
      llvm::json::Object Row{{"offset", Write.Offset},
                             {"bytes", static_cast<int64_t>(Write.Size)}};
      switch (Write.Base) {
      case CallXMemoryWriteBase::Absolute:
        Row["base"] = "absolute";
        Row["address"] = addressText(Write.Address);
        break;
      case CallXMemoryWriteBase::FramePointer:
        Row["base"] = "entry_frame_pointer";
        break;
      case CallXMemoryWriteBase::InputRegister:
        Row["base"] = "input_register";
        Row["register"] = Write.Register;
        break;
      }
      Writes.push_back(std::move(Row));
    }
    llvm::json::Object Expression;
    if (Summary.ReturnInputExpression) {
      Expression["register"] = Summary.ReturnInputExpression->first;
      Expression["offset"] = Summary.ReturnInputExpression->second;
    }
    Summaries.push_back(llvm::json::Object{
        {"summary_id", static_cast<int64_t>(Summary.ID)},
        {"entry_slot", static_cast<int64_t>(Summary.EntrySlot)},
        {"recognized_entry_root", Summary.IsRoot},
        {"generalized_unknown_inputs", Summary.GeneralizedInputs},
        {"preserves_all_caller_memory", Summary.PreservesCallerMemory},
        {"writes_only_own_and_child_frames",
         Summary.WritesOnlyOwnAndChildFrames},
        {"may_write_unknown_memory", Summary.MayWriteUnknownMemory},
        {"may_write_callee_frames", Summary.MayWriteCalleeFrames},
        {"may_write_intervals", std::move(Writes)},
        {"minimum_call_depth", static_cast<int64_t>(Summary.MinimumDepth)},
        {"known_inputs", std::move(Inputs)},
        {"input_value_alternatives", std::move(Alternatives)},
        {"input_register_dependency_mask", Summary.InputRegisterMask},
        {"may_read_input_memory", Summary.ReadsInputMemory},
        {"may_return", Summary.MayReturn},
        {"return_value", valueJSON(Summary.ReturnValue)},
        {"return_value_alternatives", std::move(Returns)},
        {"return_input_expression", std::move(Expression)},
        {"input_scratch_words",
         static_cast<int64_t>(Summary.InputScratchWords)},
        {"output_scratch_words",
         static_cast<int64_t>(Summary.OutputScratchWords)}});
  }
  llvm::json::Array Edges;
  for (const auto &Edge : Report.SummaryEdges) {
    llvm::json::Object Row{
        {"caller_summary_id", static_cast<int64_t>(Edge.CallerSummaryID)},
        {"caller_slot", static_cast<int64_t>(Edge.CallerSlot)},
        {"barrier", barrierName(Edge.Barrier)},
        {"legacy_runtime_first", Edge.LegacyRuntime}};
    Row["preserved_older_frame_words"] = Edge.PreservedOlderFrames;
    Row["applied_parameterized_memory_effects"] = Edge.AppliedMemoryEffects;
    Row["generalized_unknown_inputs"] = Edge.GeneralizedInputs;
    Row["input_generalization"] = generalizationName(Edge.InputGeneralization);
    if (Edge.CalleeSummaryID)
      Row["callee_summary_id"] = static_cast<int64_t>(*Edge.CalleeSummaryID);
    if (Edge.CalleeEntrySlot)
      Row["callee_entry_slot"] = static_cast<int64_t>(*Edge.CalleeEntrySlot);
    Edges.push_back(std::move(Row));
  }
  return llvm::json::Object{
      {"schema", 3},
      {"scope", "context_keyed_whole_function_cfg_summaries"},
      {"caller_entry_assumption", "recognized_cfg_and_function_roots"},
      {"unknown_indirect_entries_are_not_closed", true},
      {"unresolved_callx_count",
       static_cast<int64_t>(Report.UnresolvedCallXSlots.size())},
      {"entry_prefix_scratch_precision",
       Report.PrefixScratchPrecision == ScratchFlowPrecision::Exact
           ? "exact_must_facts"
           : "widened_to_block_local_facts"},
      {"entry_prefix_callx_count",
       static_cast<int64_t>(Report.EntryPrefixCallXCount)},
      {"analyzed_caller_contexts", static_cast<int64_t>(CallerContexts)},
      {"analyzed_contexts", static_cast<int64_t>(Report.Contexts.size())},
      {"proven_caller_contexts", static_cast<int64_t>(ProvenCallerContexts)},
      {"proven_contexts", static_cast<int64_t>(ProvenContexts)},
      {"callx_sites_beyond_entry_with_proven_context",
       static_cast<int64_t>(BeyondEntrySites.size())},
      {"callx_sites_with_proven_context",
       static_cast<int64_t>(ProvenSites.size())},
      {"caller_scratch_precision",
       "bounded_complete_value_sets_with_explicit_input_generalization"},
      {"specializations_per_entry",
       static_cast<int64_t>(kCallXContextSpecializationsPerEntry)},
      {"dynamic_fallback_required", true},
      {"instruction_visits", static_cast<int64_t>(Report.InstructionVisits)},
      {"summary_cache_hits", static_cast<int64_t>(Report.SummaryCacheHits)},
      {"reclaimed_unreferenced_summaries",
       static_cast<int64_t>(Report.SummaryCacheEvictions)},
      {"equivalent_generic_leaf_reuses",
       static_cast<int64_t>(Report.EquivalentGenericReuses)},
      {"skipped_non_callx_root_specializations",
       static_cast<int64_t>(Report.SkippedNonCallXRootSpecializations)},
      {"projected_unused_register_facts",
       static_cast<int64_t>(Report.ProjectedRegisters)},
      {"projected_unread_scratch_snapshots",
       static_cast<int64_t>(Report.ProjectedScratchSnapshots)},
      {"input_dependency_instruction_visits",
       static_cast<int64_t>(Report.InputDependencyVisits)},
      {"value_alternatives_limit",
       static_cast<int64_t>(kCallXContextValueAlternatives)},
      {"specialization_root_order", "ascending_entry_slot"},
      {"specialization_budget_exhausted", Report.SpecializationBudgetExhausted},
      {"peak_retained_state_bytes",
       static_cast<int64_t>(Report.PeakRetainedBytes)},
      {"function_summaries", std::move(Summaries)},
      {"summary_edges", std::move(Edges)},
      {"callx_sites", std::move(Sites)},
      {"contexts", std::move(Contexts)}};
}

} // namespace neverd::sbf
