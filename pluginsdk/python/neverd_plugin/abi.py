"""Exact ``ctypes`` declarations for NeverD's public C/plugin ABI.

This module intentionally contains no host policy.  It is an auditable table of
native layouts, function signatures, and ownership classifications consumed by
``neverd_plugin.ffi``.
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
from enum import Enum, IntEnum


SessionHandle = ctypes.c_void_p
VirtualAddress = ctypes.c_ulonglong


class OutputLanguage(IntEnum):
    C = 0
    SOLIDITY = 1
    RUST = 2


class PluginType(IntEnum):
    GENERIC = 0
    LOADER = 1
    PROCESSOR = 2
    UI = 3


class EventType(IntEnum):
    BINARY_LOADED = 1
    BINARY_CLOSING = 2
    FUNCTION_SELECTED = 3
    ADDRESS_CHANGED = 4
    ANALYSIS_DONE = 5
    PATCH_APPLIED = 6


class BinaryLoadedData(ctypes.Structure):
    _fields_ = [("Path", ctypes.c_char_p)]


class FunctionSelectedData(ctypes.Structure):
    _fields_ = [("Addr", VirtualAddress), ("Name", ctypes.c_char_p)]


class AddressChangedData(ctypes.Structure):
    _fields_ = [("Addr", VirtualAddress)]


class PatchAppliedData(ctypes.Structure):
    _fields_ = [("OutputPath", ctypes.c_char_p), ("CodeSize", ctypes.c_int)]


class EventData(ctypes.Union):
    _fields_ = [
        ("BinaryLoaded", BinaryLoadedData),
        ("FuncSelected", FunctionSelectedData),
        ("AddrChanged", AddressChangedData),
        ("PatchApplied", PatchAppliedData),
    ]


class NeverDEvent(ctypes.Structure):
    _fields_ = [
        ("Type", ctypes.c_int),
        ("Session", SessionHandle),
        ("Data", EventData),
    ]


PluginInitCallback = ctypes.CFUNCTYPE(ctypes.c_int, SessionHandle)
PluginTermCallback = ctypes.CFUNCTYPE(None)
PluginRunCallback = ctypes.CFUNCTYPE(ctypes.c_int, SessionHandle, ctypes.c_int)
PluginEventCallback = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(NeverDEvent))


class NeverDPlugin(ctypes.Structure):
    _fields_ = [
        ("Name", ctypes.c_char_p),
        ("Version", ctypes.c_char_p),
        ("Author", ctypes.c_char_p),
        ("Description", ctypes.c_char_p),
        ("Type", ctypes.c_int),
        ("Init", PluginInitCallback),
        ("Term", PluginTermCallback),
        ("Run", PluginRunCallback),
        ("Event", PluginEventCallback),
    ]


class SimplifyOutcome(IntEnum):
    """Why the simplifier returned the expression it was given."""

    NOT_APPLICABLE = 0
    ALREADY_SHORTEST = 1
    TOO_MANY_INPUTS = 2
    BUDGET_EXHAUSTED = 3
    REWRITTEN = 4


class SimplifyEvidence(IntEnum):
    """What stands behind a rewrite that was made."""

    NONE = 0
    DERIVATION = 1
    SAMPLES = 2


class ProofStatus(IntEnum):
    """Disposition of the last equivalence query.

    ``UNKNOWN`` is budget-driven; ``INVALID`` means the proof question itself
    was malformed.  Neither permits a rewrite.
    """

    NOT_RUN = 0
    EQUIVALENT = 1
    DIFFERENT = 2
    UNKNOWN = 3
    INVALID = 4


class SynthesisOutcome(IntEnum):
    """Why proof-gated synthesis did or did not commit a rewrite."""

    NOT_APPLICABLE = 0
    ALREADY_SHORTEST = 1
    TOO_MANY_INPUTS = 2
    SEARCH_BUDGET_EXHAUSTED = 3
    COUNTEREXAMPLE = 4
    PROOF_INCOMPLETE = 5
    REWRITTEN = 6


class OptimizationStop(IntEnum):
    """Observable reason a transactional module optimization stopped."""

    STABLE = 0
    CYCLE_DETECTED = 1
    BUDGET_EXHAUSTED = 2
    VERIFICATION_FAILED = 3
    INPUT_INVALID = 4


class OptimizationMode(IntEnum):
    DEFAULT = 0
    CONSERVATIVE = 1
    THIN = 2
    DEEP = 3


class LLVMOptimizationLevel(IntEnum):
    DEFAULT = 0
    O0 = 1
    O1 = 2
    O2 = 3
    O3 = 4


class NeverDSimplifyOptions(ctypes.Structure):
    """Layout of ``neverd_simplify_options``.

    ``struct_size`` leads the struct so the library can tell how much of it the
    caller actually has.  Fields are only ever appended, and a field left zero
    means "keep the engine's default", which is why the layout can grow without
    this declaration having to move in step with it.
    """

    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("width", ctypes.c_uint),
        ("shallow", ctypes.c_int),
        ("max_atoms", ctypes.c_uint),
        ("max_work", ctypes.c_size_t),
        ("verify_samples", ctypes.c_uint),
        ("allow_growth", ctypes.c_int),
        ("exhaustive", ctypes.c_int),
    ]


class NeverDSimplifyResult(ctypes.Structure):
    """Layout of ``neverd_simplify_result``.

    The string members are owned by the library; releasing them is what
    ``neverd_simplify_result_dispose`` is for, and it must be called whatever
    the outcome.
    """

    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("ok", ctypes.c_int),
        ("error", ctypes.c_char_p),
        ("error_offset", ctypes.c_size_t),
        ("input", ctypes.c_char_p),
        ("output", ctypes.c_char_p),
        ("changed", ctypes.c_int),
        ("cost_before", ctypes.c_size_t),
        ("cost_after", ctypes.c_size_t),
        ("inputs", ctypes.c_uint),
        ("work", ctypes.c_size_t),
        ("outcome", ctypes.c_int),
        ("evidence", ctypes.c_int),
        ("outcome_name", ctypes.c_char_p),
        ("evidence_name", ctypes.c_char_p),
    ]


class NeverDSynthesizeOptions(ctypes.Structure):
    """Append-only layout of ``neverd_synthesize_options``."""

    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("width", ctypes.c_uint),
        ("max_cost", ctypes.c_size_t),
        ("max_samples", ctypes.c_size_t),
        ("verify_samples", ctypes.c_uint),
        ("max_work", ctypes.c_size_t),
        ("max_leaves", ctypes.c_uint),
        ("max_constants", ctypes.c_uint),
        ("stochastic_slots", ctypes.c_uint),
        ("stochastic_restarts", ctypes.c_uint),
        ("stochastic_iterations", ctypes.c_size_t),
        ("solver_max_conflicts", ctypes.c_uint64),
        ("solver_max_propagations", ctypes.c_uint64),
        ("solver_max_watch_visits", ctypes.c_uint64),
        ("exhaustive", ctypes.c_int),
    ]


class NeverDSynthesizeResult(ctypes.Structure):
    """Owned typed result from proof-gated synthesis."""

    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("ok", ctypes.c_int),
        ("error", ctypes.c_char_p),
        ("error_offset", ctypes.c_size_t),
        ("input", ctypes.c_char_p),
        ("output", ctypes.c_char_p),
        ("changed", ctypes.c_int),
        ("cost_before", ctypes.c_size_t),
        ("cost_after", ctypes.c_size_t),
        ("inputs", ctypes.c_uint),
        ("candidate_cost", ctypes.c_size_t),
        ("outcome", ctypes.c_int),
        ("proof_status", ctypes.c_int),
        ("search_work", ctypes.c_uint64),
        ("proof_queries", ctypes.c_uint64),
        ("proof_conflicts", ctypes.c_uint64),
        ("proof_propagations", ctypes.c_uint64),
        ("proof_watch_visits", ctypes.c_uint64),
        ("counterexample_json", ctypes.c_char_p),
    ]


class NeverDOptimizeLLVMOptions(ctypes.Structure):
    """Append-only layout of ``neverd_optimize_llvm_ir_options``."""

    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("mode", ctypes.c_int),
        ("llvm_level", ctypes.c_int),
        ("max_rounds", ctypes.c_uint),
        ("enable_synthesis", ctypes.c_int),
        ("synthesis_max_cost", ctypes.c_size_t),
        ("synthesis_max_samples", ctypes.c_size_t),
        ("synthesis_verify_samples", ctypes.c_uint),
        ("synthesis_max_work", ctypes.c_size_t),
        ("synthesis_max_leaves", ctypes.c_uint),
        ("synthesis_max_constants", ctypes.c_uint),
        ("synthesis_stochastic_slots", ctypes.c_uint),
        ("synthesis_stochastic_restarts", ctypes.c_uint),
        ("synthesis_stochastic_iterations", ctypes.c_size_t),
        ("solver_max_conflicts", ctypes.c_uint64),
        ("solver_max_propagations", ctypes.c_uint64),
        ("solver_max_watch_visits", ctypes.c_uint64),
        ("exhaustive", ctypes.c_int),
    ]


class NeverDOptimizeLLVMResult(ctypes.Structure):
    """Owned result and telemetry from a transactional LLVM pipeline."""

    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("ok", ctypes.c_int),
        ("error", ctypes.c_char_p),
        ("error_line", ctypes.c_size_t),
        ("error_column", ctypes.c_size_t),
        ("output_ir", ctypes.c_char_p),
        ("changed", ctypes.c_int),
        ("stop", ctypes.c_int),
        ("functions_visited", ctypes.c_uint64),
        ("rounds", ctypes.c_uint),
        ("semantic_rewrites", ctypes.c_uint64),
        ("search_work", ctypes.c_uint64),
        ("proof_queries", ctypes.c_uint64),
        ("proof_conflicts", ctypes.c_uint64),
        ("proof_propagations", ctypes.c_uint64),
        ("proof_watch_visits", ctypes.c_uint64),
    ]


class NeverDSymbolicExploreOptions(ctypes.Structure):
    """Layout of ``neverd_symbolic_explore_options``."""

    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("max_paths", ctypes.c_uint),
        ("max_steps", ctypes.c_uint),
        ("max_block_visits", ctypes.c_uint),
        ("include_expressions", ctypes.c_int),
    ]


class Ownership(Enum):
    """Memory/lifetime contract for a native result."""

    VALUE = "value"
    BORROWED_STRING = "borrowed_string"
    OWNED_STRING = "owned_string"
    BORROWED_BUFFER = "borrowed_buffer"


_C_TYPES: dict[str, object] = {
    "void": None,
    "int": ctypes.c_int,
    "unsigned": ctypes.c_uint,
    "unsigned short": ctypes.c_ushort,
    "unsigned long long": ctypes.c_ulonglong,
    "neverd_session_t": SessionHandle,
    "neverd_va_t": VirtualAddress,
    "neverd_slot_t": ctypes.c_ulonglong,
    "neverd_output_language_t": ctypes.c_int,
    "neverd_proof_status_t": ctypes.c_int,
    "neverd_synthesis_outcome_t": ctypes.c_int,
    "neverd_optimization_stop_t": ctypes.c_int,
    "const char *": ctypes.c_char_p,
    "unsigned char *": ctypes.POINTER(ctypes.c_ubyte),
    "const unsigned char *": ctypes.POINTER(ctypes.c_ubyte),
    "unsigned long long *": ctypes.POINTER(ctypes.c_ulonglong),
    "const void *": ctypes.c_void_p,
    "const neverd_simplify_options *": ctypes.POINTER(NeverDSimplifyOptions),
    "neverd_simplify_result *": ctypes.POINTER(NeverDSimplifyResult),
    "const neverd_synthesize_options *": ctypes.POINTER(
        NeverDSynthesizeOptions
    ),
    "neverd_synthesize_result *": ctypes.POINTER(NeverDSynthesizeResult),
    "const neverd_optimize_llvm_ir_options *": ctypes.POINTER(
        NeverDOptimizeLLVMOptions
    ),
    "neverd_optimize_llvm_ir_result *": ctypes.POINTER(
        NeverDOptimizeLLVMResult
    ),
    "const neverd_symbolic_explore_options *": ctypes.POINTER(
        NeverDSymbolicExploreOptions
    ),
}


@dataclass(frozen=True, slots=True)
class FunctionSpec:
    name: str
    c_result: str
    c_arguments: tuple[str, ...]
    ownership: Ownership = Ownership.VALUE

    @property
    def restype(self) -> object:
        if self.ownership is Ownership.OWNED_STRING:
            # Preserve the original pointer so neverd_free_string can own it.
            return ctypes.c_void_p
        return _C_TYPES[self.c_result]

    @property
    def argtypes(self) -> tuple[object, ...]:
        return tuple(_C_TYPES[argument] for argument in self.c_arguments)


FUNCTION_SPECS: dict[str, FunctionSpec] = {}


def _declare(
    name: str,
    result: str,
    arguments: list[str],
    *,
    ownership: Ownership = Ownership.VALUE,
) -> None:
    if name in FUNCTION_SPECS:
        raise RuntimeError(f"duplicate NeverD ABI declaration: {name}")
    if result not in _C_TYPES:
        raise RuntimeError(f"unknown NeverD ABI result type: {result}")
    unknown = [argument for argument in arguments if argument not in _C_TYPES]
    if unknown:
        raise RuntimeError(f"unknown NeverD ABI argument types: {unknown}")
    if ownership is Ownership.OWNED_STRING and result != "const char *":
        raise RuntimeError(f"owned string declaration {name} has result {result}")
    if ownership is Ownership.BORROWED_BUFFER and result != "const unsigned char *":
        raise RuntimeError(f"borrowed buffer declaration {name} has result {result}")
    FUNCTION_SPECS[name] = FunctionSpec(
        name=name,
        c_result=result,
        c_arguments=tuple(arguments),
        ownership=ownership,
    )


_declare("neverd_session_create", "neverd_session_t", [])
_declare("neverd_session_destroy", "void", ["neverd_session_t"])
_declare("neverd_session_load", "int", ["neverd_session_t", "const char *"])
_declare("neverd_session_is_loaded", "int", ["neverd_session_t"])
_declare("neverd_session_analyze", "int", ["neverd_session_t"])
_declare(
    "neverd_session_file_path",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_session_arch_name",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_session_format_name",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_session_is_64bit", "int", ["neverd_session_t"])
_declare("neverd_session_bitness", "int", ["neverd_session_t"])
_declare("neverd_func_count", "int", ["neverd_session_t"])
_declare("neverd_func_entry", "neverd_va_t", ["neverd_session_t", "int"])
_declare("neverd_func_size", "int", ["neverd_session_t", "int"])
_declare(
    "neverd_func_name",
    "const char *",
    ["neverd_session_t", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_disasm_json",
    "const char *",
    ["neverd_session_t", "neverd_va_t", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_decompile",
    "const char *",
    ["neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_ir_low",
    "const char *",
    ["neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_ir_med",
    "const char *",
    ["neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_ir_high",
    "const char *",
    ["neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_ir_llvm",
    "const char *",
    ["neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_func_find_by_name", "int", ["neverd_session_t", "const char *"])
_declare("neverd_func_find_by_addr", "int", ["neverd_session_t", "neverd_va_t"])
_declare(
    "neverd_read_bytes",
    "int",
    ["neverd_session_t", "neverd_va_t", "unsigned char *", "int"],
)
_declare(
    "neverd_imports_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_exports_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_segments_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_strings_json",
    "const char *",
    ["neverd_session_t", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_xrefs_to_json",
    "const char *",
    ["neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_xrefs_from_json",
    "const char *",
    ["neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_cfg_json",
    "const char *",
    ["neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_symbolic_explore_json",
    "const char *",
    [
        "neverd_session_t",
        "neverd_va_t",
        "const neverd_symbolic_explore_options *",
    ],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_patch_from_ir",
    "int",
    ["neverd_session_t", "const char *", "int", "const char *"],
)
_declare(
    "neverd_patch_from_c",
    "int",
    ["neverd_session_t", "const char *", "neverd_va_t", "const char *"],
)
_declare("neverd_set_inst_substitution", "void", ["neverd_session_t", "int", "int"])
_declare("neverd_patch_substitution_count", "int", ["neverd_session_t"])
_declare("neverd_set_constant_encryption", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_constant_encryption_count", "int", ["neverd_session_t"])
_declare("neverd_set_opaque_predicate", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_opaque_predicate_count", "int", ["neverd_session_t"])
_declare("neverd_set_control_flow_flattening", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_control_flow_flattening_count", "int", ["neverd_session_t"])
_declare("neverd_set_bogus_control_flow", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_bogus_control_flow_count", "int", ["neverd_session_t"])
_declare("neverd_set_indirect_branch", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_indirect_branch_count", "int", ["neverd_session_t"])
_declare("neverd_set_indirect_call", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_indirect_call_count", "int", ["neverd_session_t"])
_declare("neverd_set_mba", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_mba_count", "int", ["neverd_session_t"])
_declare("neverd_set_indirect_global", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_indirect_global_count", "int", ["neverd_session_t"])
_declare("neverd_set_value_launder", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_value_launder_count", "int", ["neverd_session_t"])
_declare("neverd_set_constant_pooling", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_constant_pooling_count", "int", ["neverd_session_t"])
_declare("neverd_set_bit_masking", "void", ["neverd_session_t", "int"])
_declare("neverd_patch_bit_masking_count", "int", ["neverd_session_t"])
_declare("neverd_set_text_section", "void", ["neverd_session_t", "const char *"])
_declare(
    "neverd_patch_output_path",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_patch_code_size", "unsigned long long", ["neverd_session_t"])
_declare("neverd_patch_trampoline_count", "int", ["neverd_session_t"])
_declare(
    "neverd_last_error",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_free_string", "void", ["const char *"])
_declare("neverd_session_file_size", "unsigned long long", ["neverd_session_t"])
_declare("neverd_session_base_addr", "neverd_va_t", ["neverd_session_t"])
_declare("neverd_session_entry_addr", "neverd_va_t", ["neverd_session_t"])
_declare("neverd_session_segment_count", "int", ["neverd_session_t"])
_declare("neverd_session_section_count", "int", ["neverd_session_t"])
_declare("neverd_session_import_count", "int", ["neverd_session_t"])
_declare("neverd_session_export_count", "int", ["neverd_session_t"])
_declare("neverd_session_symbol_count", "int", ["neverd_session_t"])
_declare(
    "neverd_hex_dump",
    "const char *",
    ["neverd_session_t", "neverd_va_t", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_annotation_set", "void", ["neverd_session_t", "neverd_va_t", "const char *"]
)
_declare("neverd_annotation_remove", "void", ["neverd_session_t", "neverd_va_t"])
_declare(
    "neverd_annotation_get",
    "const char *",
    ["neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_annotations_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_annotations_save", "int", ["neverd_session_t"])
_declare("neverd_annotations_load", "int", ["neverd_session_t"])
_declare(
    "neverd_diff_functions",
    "const char *",
    ["neverd_session_t", "neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_diff_decompile",
    "const char *",
    ["neverd_session_t", "neverd_va_t", "neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_rename_func", "int", ["neverd_session_t", "const char *", "const char *"]
)
_declare(
    "neverd_renames_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_renames_save", "int", ["neverd_session_t"])
_declare("neverd_renames_load", "int", ["neverd_session_t"])
_declare(
    "neverd_callgraph_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_resolve_addr",
    "const char *",
    ["neverd_session_t", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_search_bytes",
    "const char *",
    ["neverd_session_t", "const unsigned char *", "int", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_search_string",
    "const char *",
    ["neverd_session_t", "const char *", "int", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_sections_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_symbols_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_relocs_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_headers_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_entrypoints_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_dashboard_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_apply_signatures", "int", ["neverd_session_t", "const char *"])
_declare("neverd_auto_apply_signatures", "int", ["neverd_session_t", "const char *"])
_declare("neverd_apply_signature_file", "int", ["neverd_session_t", "const char *"])
_declare("neverd_sig_match_count", "int", ["neverd_session_t"])
_declare(
    "neverd_sig_matches_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_lift_module",
    "const char *",
    ["neverd_session_t", "const char *", "int", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_lift_to_obj", "int", ["neverd_session_t", "const char *", "int", "int"]
)
_declare(
    "neverd_roundtrip_ir",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_roundtrip_obj",
    "const unsigned char *",
    ["neverd_session_t", "unsigned long long *"],
    ownership=Ownership.BORROWED_BUFFER,
)
_declare("neverd_roundtrip_func_count", "int", ["neverd_session_t"])
_declare(
    "neverd_roundtrip_func_name",
    "const char *",
    ["neverd_session_t", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_roundtrip_func_offset", "unsigned long long", ["neverd_session_t", "int"]
)
_declare(
    "neverd_roundtrip_func_size", "unsigned long long", ["neverd_session_t", "int"]
)
_declare("neverd_roundtrip_func_param_count", "int", ["neverd_session_t", "int"])
_declare(
    "neverd_lift_dump",
    "const char *",
    ["neverd_session_t", "const char *", "int", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_patch_full",
    "int",
    [
        "neverd_session_t",
        "const char *",
        "const char *",
        "int",
        "int",
        "int",
        "int",
        "int",
    ],
)
_declare(
    "neverd_decompile_all",
    "const char *",
    ["neverd_session_t", "const char *", "int", "int", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_decompile_all_ex",
    "const char *",
    ["neverd_session_t", "const char *", "neverd_output_language_t", "int", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_evm_set_strict", "void", ["neverd_session_t", "int"])
_declare("neverd_evm_set_hardfork", "int", ["neverd_session_t", "const char *"])
_declare("neverd_sbf_set_strict", "void", ["neverd_session_t", "int"])
_declare("neverd_sbf_set_version", "int", ["neverd_session_t", "const char *"])
_declare("neverd_sbf_set_idl", "int", ["neverd_session_t", "const char *"])
_declare("neverd_sbf_set_cluster", "int", ["neverd_session_t", "const char *"])
_declare("neverd_sbf_set_slot", "void", ["neverd_session_t", "neverd_slot_t"])
_declare("neverd_sbf_set_loader", "int", ["neverd_session_t", "const char *"])
_declare("neverd_sbf_set_purpose", "int", ["neverd_session_t", "const char *"])
_declare("neverd_inject_hello", "int", ["neverd_session_t"])
_declare(
    "neverd_disasm_text",
    "const char *",
    ["neverd_session_t", "const char *", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_xrefs_scan",
    "const char *",
    ["neverd_session_t", "const char *", "neverd_va_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_cfg_dot",
    "const char *",
    ["neverd_session_t", "const char *", "const char *", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_bench_run",
    "const char *",
    ["neverd_session_t", "const char *", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_bench_decode",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_sig_compute_crc16", "unsigned short", ["const unsigned char *", "int"])
_declare("neverd_plugins_load_dir", "int", ["neverd_session_t", "const char *"])
_declare("neverd_plugins_load_file", "int", ["neverd_session_t", "const char *"])
_declare(
    "neverd_plugins_list_json",
    "const char *",
    ["neverd_session_t"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_plugins_init", "void", ["neverd_session_t"])
_declare("neverd_plugins_term", "void", ["neverd_session_t"])
_declare("neverd_plugins_run", "int", ["neverd_session_t", "const char *", "int"])
_declare("neverd_plugins_count", "int", ["neverd_session_t"])
_declare("neverd_plugins_dispatch_event", "void", ["neverd_session_t", "const void *"])
_declare(
    "neverd_simplify_expr",
    "int",
    [
        "const char *",
        "const neverd_simplify_options *",
        "neverd_simplify_result *",
    ],
)
_declare(
    "neverd_simplify_result_dispose", "void", ["neverd_simplify_result *"]
)
_declare(
    "neverd_simplify_expr_json",
    "const char *",
    ["const char *", "unsigned", "int"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_proof_status_name",
    "const char *",
    ["neverd_proof_status_t"],
    ownership=Ownership.BORROWED_STRING,
)
_declare(
    "neverd_synthesis_outcome_name",
    "const char *",
    ["neverd_synthesis_outcome_t"],
    ownership=Ownership.BORROWED_STRING,
)
_declare(
    "neverd_synthesize_expr",
    "int",
    [
        "const char *",
        "const neverd_synthesize_options *",
        "neverd_synthesize_result *",
    ],
)
_declare(
    "neverd_synthesize_result_dispose", "void", ["neverd_synthesize_result *"]
)
_declare(
    "neverd_synthesize_expr_json_v1",
    "const char *",
    ["const char *", "const neverd_synthesize_options *"],
    ownership=Ownership.OWNED_STRING,
)
_declare(
    "neverd_optimization_stop_name",
    "const char *",
    ["neverd_optimization_stop_t"],
    ownership=Ownership.BORROWED_STRING,
)
_declare(
    "neverd_optimize_llvm_ir",
    "int",
    [
        "const char *",
        "const neverd_optimize_llvm_ir_options *",
        "neverd_optimize_llvm_ir_result *",
    ],
)
_declare(
    "neverd_optimize_llvm_ir_result_dispose",
    "void",
    ["neverd_optimize_llvm_ir_result *"],
)
_declare(
    "neverd_optimize_llvm_ir_json_v1",
    "const char *",
    ["const char *", "const neverd_optimize_llvm_ir_options *"],
    ownership=Ownership.OWNED_STRING,
)
_declare("neverd_version", "const char *", [], ownership=Ownership.OWNED_STRING)
_declare("neverd_project_name", "const char *", [], ownership=Ownership.OWNED_STRING)
_declare("neverd_version_number", "const char *", [], ownership=Ownership.OWNED_STRING)


__all__ = [
    "AddressChangedData",
    "BinaryLoadedData",
    "EventData",
    "EventType",
    "FUNCTION_SPECS",
    "FunctionSelectedData",
    "FunctionSpec",
    "NeverDEvent",
    "NeverDPlugin",
    "NeverDOptimizeLLVMOptions",
    "NeverDOptimizeLLVMResult",
    "NeverDSimplifyOptions",
    "NeverDSimplifyResult",
    "NeverDSynthesizeOptions",
    "NeverDSynthesizeResult",
    "NeverDSymbolicExploreOptions",
    "OutputLanguage",
    "LLVMOptimizationLevel",
    "OptimizationMode",
    "OptimizationStop",
    "Ownership",
    "PatchAppliedData",
    "PluginEventCallback",
    "PluginInitCallback",
    "PluginRunCallback",
    "PluginTermCallback",
    "PluginType",
    "ProofStatus",
    "SessionHandle",
    "SimplifyEvidence",
    "SimplifyOutcome",
    "SynthesisOutcome",
    "VirtualAddress",
]
