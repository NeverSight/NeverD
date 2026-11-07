//===- InterpreterRecoveryView.h - Recovery request adapter ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Parse source-view requests into the versioned C SDK. The SDK owns recovery,
/// resource defaults, source layouts and validation of semantic premises.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_WORKER_INTERPRETERRECOVERYVIEW_H
#define NEVERD_TOOLS_NEVERD_WORKER_INTERPRETERRECOVERYVIEW_H

#include "EngineSymbols.h"
#include "Protocol.h"

#include "neverd/sdk/NeverDCAPIDevirtualize.h"

#include <cstdint>
#include <limits>
#include <string_view>

namespace neverd::worker::interpreter_recovery {

inline constexpr const char *kCapability = "interpreter_recovery";
using RecoverFunction = const char *(*)(neverd_session_t, neverd_va_t,
                                        const neverd_devirtualize_options_v10 *,
                                        const char **);

inline RecoverFunction function() {
  static const auto Function =
      engineSymbol<RecoverFunction>("neverd_devirtualize_machine_source_v10");
  return Function;
}

inline neverd_devirtualize_options_v10
options(const Json &Request, std::string_view Representation) {
  if (!Request.is_object() || Request.size() != 1 ||
      !Request.contains(kCapability) || !Request.at(kCapability).is_object())
    throw Error(
        "invalid_request",
        "interpreter_recovery must be the only source option and an object");
  if (Representation != "c" && Representation != "source" &&
      Representation != "llvmc")
    throw Error("invalid_request",
                "Interpreter recovery requires a C source view");
  const auto &Fields = Request.at(kCapability);
  neverd_devirtualize_options_v10 O{};
  auto &Proof = O.base;
  auto &Visits = Proof.base;
  auto &Work = Visits.base;
  auto &Separation = Work.base;
  auto &Alignment = Separation.base;
  auto &Frame = Alignment.base;
  auto &Discovery = Frame.base;
  auto &Refinement = Discovery.base;
  auto &Base = Refinement.base;
  Base.struct_size = sizeof(O);
  Base.use_llvm = Representation == "llvmc";
  const auto Unsigned = [](const Json &Value, uint64_t Max) {
    if ((!Value.is_number_integer() && !Value.is_number_unsigned()) ||
        (Value.is_number_integer() && !Value.is_number_unsigned() &&
         Value.get<int64_t>() < 0) ||
        Value.get<uint64_t>() > Max)
      throw Error("invalid_request", "Recovery limits must be nonnegative "
                                     "integers within their SDK field width");
    return Value.get<uint64_t>();
  };
  const auto Signed = [](const Json &Value) {
    if (!Value.is_number_integer() ||
        (Value.is_number_unsigned() &&
         Value.get<uint64_t>() > uint64_t(std::numeric_limits<int64_t>::max())))
      throw Error("invalid_request",
                  "Entry-frame bounds must be signed 64-bit integers");
    return Value.get<int64_t>();
  };
  const auto Boolean = [](const Json &Value) {
    if (!Value.is_boolean())
      throw Error("invalid_request", "Recovery switches must be booleans");
    return Value.get<bool>();
  };
  for (const auto &[Name, Value] : Fields.items()) {
    uint32_t *Word = nullptr;
    if (Name == "max_nodes")
      Word = &Base.max_nodes;
    else if (Name == "max_contexts_per_address")
      Word = &Base.max_contexts_per_address;
    else if (Name == "max_control_refinements")
      Word = &Refinement.max_control_refinements;
    else if (Name == "max_control_fields")
      Word = &Discovery.max_control_fields;
    else if (Name == "max_solver_queries")
      Word = &Discovery.max_solver_queries;
    else if (Name == "max_chained_transfers")
      Word = &Frame.max_chained_transfers;
    else if (Name == "entry_frame_alignment")
      Word = &Alignment.entry_frame_alignment;
    else if (Name == "entry_frame_residue")
      Word = &Alignment.entry_frame_residue;
    else if (Name == "max_symbolic_nodes")
      Word = &Alignment.max_symbolic_nodes;
    else if (Name == "max_node_evaluations")
      Word = &Work.max_node_evaluations;
    else if (Name == "max_discovery_visits")
      Word = &Work.max_discovery_visits;
    else if (Name == "max_chained_visits_per_destination")
      Word = &Visits.max_chained_visits_per_destination;
    if (Word) {
      *Word = static_cast<uint32_t>(
          Unsigned(Value, std::numeric_limits<uint32_t>::max()));
    } else if (Name == "max_operations") {
      Base.max_operations =
          Unsigned(Value, std::numeric_limits<uint64_t>::max());
    } else if (Name == "source_abi") {
      if (Value == "scalar")
        O.machine_source_abi = NEVERD_DEVIRTUALIZE_MACHINE_SCALAR_V1;
      else if (Value == "queries")
        O.machine_source_abi = NEVERD_DEVIRTUALIZE_MACHINE_QUERIES_V1;
      else if (Value == "xmm")
        O.machine_source_abi = NEVERD_DEVIRTUALIZE_MACHINE_QUERIES_XMM_V2;
      else
        throw Error("invalid_request",
                    "source_abi expects scalar, queries or xmm");
    } else if (Name == "entry_frame_begin") {
      Frame.entry_frame_begin = Signed(Value);
    } else if (Name == "entry_frame_end") {
      Frame.entry_frame_end = Signed(Value);
    } else if (Name == "external_stores_disjoint_frame") {
      if (Boolean(Value))
        Separation.flags |=
            NEVERD_DEVIRTUALIZE_V6_EXTERNAL_STORES_DISJOINT_ENTRY_FRAME;
    } else if (Name == "stop_chain_at_repeat") {
      if (Boolean(Value))
        Work.flags |= NEVERD_DEVIRTUALIZE_V7_STOP_CHAIN_AT_REPEAT;
    } else if (Name == "prove_store_return_separation") {
      if (Boolean(Value))
        Proof.flags |=
            NEVERD_DEVIRTUALIZE_V9_PROVE_STORE_RETURN_SLOT_SEPARATION;
    } else if (Name == "prove_store_frame_separation") {
      if (Boolean(Value))
        Proof.flags |= NEVERD_DEVIRTUALIZE_V9_PROVE_STORE_FRAME_SEPARATION;
    } else if (Name == "discover_control_state") {
      if (!Boolean(Value))
        Frame.flags |= NEVERD_DEVIRTUALIZE_V4_DISABLE_CONTROL_DISCOVERY;
    } else if (Name == "no_opt") {
      Base.no_opt = Boolean(Value);
    } else {
      throw Error("invalid_request",
                  "Unknown interpreter recovery option: " + Name);
    }
  }
  if (Fields.contains("entry_frame_begin") !=
      Fields.contains("entry_frame_end"))
    throw Error("invalid_request", "Both entry-frame endpoints are required");
  if (Fields.contains("entry_frame_begin"))
    Frame.flags |= NEVERD_DEVIRTUALIZE_V4_HAS_ENTRY_FRAME_BOUNDS;
  return O;
}

} // namespace neverd::worker::interpreter_recovery

#endif // NEVERD_TOOLS_NEVERD_WORKER_INTERPRETERRECOVERYVIEW_H
