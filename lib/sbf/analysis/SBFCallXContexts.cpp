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
#include "neverd/sbf/runtime/SBFVMConfig.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
  case CallXContextResult::ProvenTargetSet:
    return "proven_target_set";
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
    case RegisterValue::Kind::UnsignedRange:
      return "unsigned_range";
    case RegisterValue::Kind::SymbolicAddress:
      return "captured_address";
    }
    llvm_unreachable("unknown register value kind");
  };
  llvm::json::Object Result{{"kind", Kind()}};
  if (Value.ValueKind == RegisterValue::Kind::UnsignedRange) {
    const auto Range = *unsignedValueRange(Value);
    Result["lower"] = addressText(Range.Lower);
    Result["upper"] = addressText(Range.Upper);
  } else if (Value.ValueKind == RegisterValue::Kind::SymbolicAddress) {
    Result["capture"] = static_cast<int64_t>(Value.Value);
    Result["offset"] = Value.Offset;
  } else if (Value.ValueKind != RegisterValue::Kind::Unknown) {
    Result["base"] = addressText(Value.Value);
    Result["offset"] = Value.Offset;
  }
  return Result;
}

llvm::json::Object memoryWriteJSON(const CallXMemoryWrite &Write) {
  llvm::json::Object Row{{"offset", Write.Offset},
                         {"bytes", static_cast<int64_t>(Write.Size)}};
  switch (Write.Base) {
  case CallXMemoryWriteBase::UnsignedRange:
    Row["base"] = "unsigned_range";
    Row["lower"] = addressText(Write.Address);
    Row["upper"] = addressText(static_cast<uint64_t>(Write.Offset));
    Row["wraps"] = Write.Address > static_cast<uint64_t>(Write.Offset);
    Row.erase("offset");
    break;
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
  return Row;
}

llvm::json::Object inputExpressionJSON(const CallXInputExpression &Expression) {
  switch (Expression.Kind) {
  case CallXInputExpressionKind::InputRegister:
    return llvm::json::Object{{"kind", "entry_register"},
                              {"register", Expression.Register},
                              {"offset", Expression.Offset}};
  case CallXInputExpressionKind::Constant:
    return llvm::json::Object{{"kind", "constant"},
                              {"value", valueJSON(Expression.Constant)}};
  case CallXInputExpressionKind::Instruction: {
    llvm::json::Object Row{
        {"kind", "normalized_instruction"},
        {"input", static_cast<int64_t>(*Expression.Input)},
        {"instruction_slot", static_cast<int64_t>(Expression.InstructionSlot)}};
    if (Expression.MemoryGuard)
      Row["memory_guard"] = static_cast<int64_t>(*Expression.MemoryGuard);
    return Row;
  }
  case CallXInputExpressionKind::InputAlternatives: {
    llvm::json::Array Inputs;
    for (size_t ID : Expression.Alternatives)
      Inputs.push_back(static_cast<int64_t>(ID));
    return llvm::json::Object{{"kind", "entry_input_alternatives"},
                              {"inputs", std::move(Inputs)}};
  }
  }
  llvm_unreachable("unknown input expression kind");
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
  case CallXInputGeneralization::EquivalentSpecializedInputs:
    return "reused_proven_equivalent_specialized_inputs";
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
    return "bounded_targets_include_symbolic_or_outside_raw_text_values";
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
  std::map<size_t, std::set<std::vector<va_t>>> TargetSets;
  std::map<size_t, std::set<std::pair<size_t, size_t>>> TargetInputExpressions;
  size_t ProvenContexts = 0;
  size_t ProvenTargetSetContexts = 0;
  size_t ProvenTargetSetCallerContexts = 0;
  size_t CallerContexts = 0;
  size_t ProvenCallerContexts = 0;
  std::set<size_t> BeyondEntrySites;
  std::set<size_t> TargetSetSites;
  std::set<size_t> BeyondEntryTargetSetSites;
  // Set evidence already guarantees literal in-range addresses. Serialize the
  // raw slot mapping without strengthening it to successful call execution.
  const auto TargetJSON = [&](va_t Target) {
    const size_t Slot = (Target - Program.Low.TextAddress) / kInstructionSize;
    return llvm::json::Object{{"target_address", addressText(Target)},
                              {"target_slot", static_cast<int64_t>(Slot)},
                              {"target_is_continuation",
                               Program.Low.Instructions[Slot].IsContinuation}};
  };
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
                            : !Evidence.InputExpressionValues.empty()
                                ? "instantiated_generic_summary_inputs"
                                : "whole_function_summary";
    if (Evidence.InputExpression)
      Row["target_input_expression"] =
          static_cast<int64_t>(*Evidence.InputExpression);
    if (Evidence.InputExpression && !Evidence.EntryPrefixOnly)
      TargetInputExpressions[Evidence.SummaryID].emplace(
          Evidence.CallXSlot, *Evidence.InputExpression);
    if (!Evidence.InputExpressionValues.empty()) {
      llvm::json::Array Steps;
      for (const auto &Step : Evidence.InputExpressionValues) {
        llvm::json::Array Inputs, Values;
        for (const auto &Value : Step.Inputs.values())
          Inputs.push_back(valueJSON(Value));
        for (const auto &Value : Step.Values.values())
          Values.push_back(valueJSON(Value));
        Steps.push_back(llvm::json::Object{
            {"expression", static_cast<int64_t>(Step.Expression)},
            {"inputs", std::move(Inputs)},
            {"values", std::move(Values)}});
      }
      Row["input_expression_proof"] = std::move(Steps);
    }
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
    if (Evidence.Result == CallXContextResult::ProvenTargetSet) {
      llvm::json::Array Candidates;
      std::vector<va_t> Addresses;
      for (const auto &Value : Evidence.TargetValueAlternatives.values()) {
        Candidates.push_back(TargetJSON(Value.Value));
        Addresses.push_back(Value.Value);
      }
      Row["target_set"] = std::move(Candidates);
      std::sort(Addresses.begin(), Addresses.end());
      TargetSets[Evidence.CallXSlot].insert(std::move(Addresses));
      TargetSetSites.insert(Evidence.CallXSlot);
      ++ProvenTargetSetContexts;
      if (!Evidence.IsRootContext)
        ++ProvenTargetSetCallerContexts;
      if (Evidence.BeyondEntryBlock)
        BeyondEntryTargetSetSites.insert(Evidence.CallXSlot);
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
    llvm::json::Array CandidateSets;
    for (const auto &Addresses : TargetSets[Slot]) {
      llvm::json::Array Candidates;
      for (va_t Address : Addresses)
        Candidates.push_back(TargetJSON(Address));
      CandidateSets.push_back(std::move(Candidates));
    }
    Sites.push_back(llvm::json::Object{
        {"callx_slot", static_cast<int64_t>(Slot)},
        {"callx_address",
         addressText(Program.Low.TextAddress + Slot * kInstructionSize)},
        {"analyzed_contexts", static_cast<int64_t>(ContextCounts[Slot])},
        {"coverage",
         ProvenSites.contains(Slot)      ? "conditional_targets_only"
         : TargetSetSites.contains(Slot) ? "conditional_target_sets_only"
         : ContextCounts[Slot] ? "analyzed_contexts_unproven"
                               : "not_reached_from_recognized_entries"},
        {"conditional_target_slots", std::move(CandidateSlots)},
        {"conditional_target_sets", std::move(CandidateSets)},
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
    for (const auto &Write : Summary.MemoryWrites)
      Writes.push_back(memoryWriteJSON(Write));
    llvm::json::Object Expression;
    if (Summary.ReturnInputExpression) {
      Expression["register"] = Summary.ReturnInputExpression->first;
      Expression["offset"] = Summary.ReturnInputExpression->second;
    }
    llvm::json::Array ReturnInputs;
    for (const auto &Alternative : Summary.ReturnInputAlternatives)
      ReturnInputs.push_back(inputExpressionJSON(Alternative));
    llvm::json::Array InputExpressions;
    for (const auto &[Slot, ID] : TargetInputExpressions[Summary.ID])
      InputExpressions.push_back(
          llvm::json::Object{{"callx_slot", static_cast<int64_t>(Slot)},
                             {"expression", static_cast<int64_t>(ID)}});
    llvm::json::Object Row{
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
        {"conditional_reuse_input_register_mask",
         Summary.ReuseInputRegisterMask},
        {"conditional_reuse_reads_input_memory", Summary.ReuseReadsInputMemory},
        {"target_input_expressions", std::move(InputExpressions)},
        {"may_return", Summary.MayReturn},
        {"return_value", valueJSON(Summary.ReturnValue)},
        {"return_value_alternatives", std::move(Returns)},
        {"return_input_expression", std::move(Expression)},
        {"return_input_alternatives", std::move(ReturnInputs)},
        {"input_scratch_words",
         static_cast<int64_t>(Summary.InputScratchWords)},
        {"output_scratch_words",
         static_cast<int64_t>(Summary.OutputScratchWords)},
        {"output_memory_copies",
         static_cast<int64_t>(Summary.OutputMemoryCopies)},
        {"output_parameterized_words",
         static_cast<int64_t>(Summary.OutputParameterizedWords)}};
    if (Summary.ReturnValueExpression)
      Row["return_value_expression"] =
          static_cast<int64_t>(*Summary.ReturnValueExpression);
    Summaries.push_back(std::move(Row));
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
    Row["applied_stored_values"] =
        static_cast<int64_t>(Edge.AppliedStoredValues);
    Row["generalized_unknown_inputs"] = Edge.GeneralizedInputs;
    Row["input_generalization"] = generalizationName(Edge.InputGeneralization);
    if (Edge.InstantiatedCopySlot) {
      Row["instantiated_copy_syscall_slot"] =
          static_cast<int64_t>(*Edge.InstantiatedCopySlot);
      Row["instantiated_copy_bytes"] =
          static_cast<int64_t>(Edge.InstantiatedCopyBytes);
    }
    if (Edge.CalleeSummaryID)
      Row["callee_summary_id"] = static_cast<int64_t>(*Edge.CalleeSummaryID);
    if (Edge.CalleeEntrySlot)
      Row["callee_entry_slot"] = static_cast<int64_t>(*Edge.CalleeEntrySlot);
    if (Edge.CallXTargetAddress)
      Row["callx_target_address"] = addressText(*Edge.CallXTargetAddress);
    Edges.push_back(std::move(Row));
  }
  llvm::json::Array Expressions;
  for (size_t ID = 0; ID < Report.InputExpressions.size(); ++ID) {
    auto Row = inputExpressionJSON(Report.InputExpressions[ID]);
    Row["expression"] = static_cast<int64_t>(ID);
    Expressions.push_back(std::move(Row));
  }
  llvm::json::Array Guards;
  for (size_t ID = 0; ID < Report.InputMemoryGuards.size(); ++ID) {
    const auto &Guard = Report.InputMemoryGuards[ID];
    llvm::json::Array Writes;
    for (const auto &Write : Guard.Writes)
      Writes.push_back(memoryWriteJSON(Write));
    Guards.push_back(llvm::json::Object{
        {"memory_guard", static_cast<int64_t>(ID)},
        {"may_write_callee_frames", Guard.WritesCalleeFrames},
        {"may_write_intervals", std::move(Writes)}});
  }
  llvm::json::Array Captures;
  for (size_t ID = 0; ID < Report.CapturedAddresses.size(); ++ID) {
    const auto Bounds = Report.CapturedAddresses[ID];
    Captures.push_back(
        llvm::json::Object{{"capture", static_cast<int64_t>(ID)},
                           {"lower", addressText(Bounds.Lower)},
                           {"upper", addressText(Bounds.Upper)}});
  }
  const auto Translation =
      frameTranslationLimit(Program.Low.TheVersion, Program.Config);
  return llvm::json::Object{
      {"schema", 7},
      {"captured_addresses", std::move(Captures)},
      {"distinct_captures_prove_no_alias", false},
      {"frame_coordinates",
       llvm::json::Object{
           {"stack_frame_bytes",
            static_cast<int64_t>(Program.Config.StackFrameSize)},
           {"max_call_depth",
            static_cast<int64_t>(Program.Config.MaxCallDepth)},
           {"maximum_translation_bytes",
            Translation ? llvm::json::Value(static_cast<int64_t>(*Translation))
                        : llvm::json::Value(nullptr)}}},
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
      {"proven_target_set_contexts",
       static_cast<int64_t>(ProvenTargetSetContexts)},
      {"proven_target_set_caller_contexts",
       static_cast<int64_t>(ProvenTargetSetCallerContexts)},
      {"callx_sites_with_proven_target_set_context",
       static_cast<int64_t>(TargetSetSites.size())},
      {"callx_sites_beyond_entry_with_proven_target_set_context",
       static_cast<int64_t>(BeyondEntryTargetSetSites.size())},
      {"caller_scratch_precision",
       "bounded_complete_value_sets_with_explicit_input_generalization"},
      {"specializations_per_entry",
       static_cast<int64_t>(kCallXContextSpecializationsPerEntry)},
      {"dynamic_fallback_required", true},
      {"instruction_visits", static_cast<int64_t>(Report.InstructionVisits)},
      {"summary_cache_hits", static_cast<int64_t>(Report.SummaryCacheHits)},
      {"reclaimed_unreferenced_summaries",
       static_cast<int64_t>(Report.SummaryCacheEvictions)},
      {"generalized_redundant_published_summaries",
       static_cast<int64_t>(Report.GeneralizedPublishedSummaries)},
      {"equivalent_generic_leaf_reuses",
       static_cast<int64_t>(Report.EquivalentGenericReuses)},
      {"equivalent_specialized_summary_reuses",
       static_cast<int64_t>(Report.EquivalentSpecializedReuses)},
      {"skipped_non_callx_root_specializations",
       static_cast<int64_t>(Report.SkippedNonCallXRootSpecializations)},
      {"constant_branch_successors_skipped",
       static_cast<int64_t>(Report.SkippedConstantBranchSuccessors)},
      {"instantiated_target_input_expressions",
       static_cast<int64_t>(Report.InstantiatedTargetInputExpressions)},
      {"target_input_expressions", std::move(Expressions)},
      {"input_memory_guards", std::move(Guards)},
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
