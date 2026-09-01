"""Public authoring API for NeverD Python plugins."""

from __future__ import annotations

import builtins
import ctypes
from dataclasses import dataclass
from enum import Enum
from itertools import islice
import json
import os
import re
import sys
from types import ModuleType
from types import MappingProxyType
from typing import Any, Callable, Iterable, Iterator, Mapping, Protocol, TypeVar, cast

from .abi import (
    EventType,
    LLVMOptimizationLevel,
    NeverDOptimizeLLVMOptions,
    NeverDOptimizeLLVMResult,
    NeverDLowIRConcolicOptionsV1,
    NeverDLowIRConcolicRegisterSeedV1,
    NeverDSimplifyOptions,
    NeverDSimplifyResult,
    NeverDSynthesizeOptions,
    NeverDSynthesizeResult,
    NeverDSafetyOptions,
    NeverDSymbolicExploreOptions,
    NeverDTranslateObjectRequestV1,
    NeverDTranslateObjectResultV1,
    OptimizationMode,
    OptimizationStop,
    OutputLanguage,
    PluginType,
    ProofStatus,
    SimplifyEvidence,
    SimplifyOutcome,
    SynthesisOutcome,
    TranslationErrorCode,
    TranslationObjectFormat,
    TranslationProofStatus,
    TranslationSemanticStop,
)
from .ffi import HostAPI


_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1 = 4096


_SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)


_TRANSFORM_COUNT_FUNCTIONS = {
    "instruction_substitution": "neverd_patch_substitution_count",
    "constant_encryption": "neverd_patch_constant_encryption_count",
    "opaque_predicate": "neverd_patch_opaque_predicate_count",
    "control_flow_flattening": "neverd_patch_control_flow_flattening_count",
    "bogus_control_flow": "neverd_patch_bogus_control_flow_count",
    "indirect_branch": "neverd_patch_indirect_branch_count",
    "indirect_call": "neverd_patch_indirect_call_count",
    "mba": "neverd_patch_mba_count",
    "indirect_global": "neverd_patch_indirect_global_count",
    "value_laundering": "neverd_patch_value_launder_count",
    "constant_pooling": "neverd_patch_constant_pooling_count",
    "bit_masking": "neverd_patch_bit_masking_count",
}


@dataclass(frozen=True, slots=True)
class PluginSpec:
    name: str
    version: str
    version_info: tuple[int, int, int]
    prerelease: str
    build_metadata: str
    author: str
    description: str
    type: PluginType
    plugin_class: builtins.type[Any]


_PluginClass = TypeVar("_PluginClass", bound=builtins.type[Any])


def _metadata_string(name: str, value: Any, *, allow_empty: bool) -> str:
    if not isinstance(value, str):
        raise TypeError(f"plugin {name} must be a string")
    if (not allow_empty and not value) or "\0" in value:
        qualifier = "non-empty and " if not allow_empty else ""
        raise ValueError(f"plugin {name} must be {qualifier}free of NUL characters")
    try:
        value.encode("utf-8", errors="strict")
    except UnicodeEncodeError as error:
        raise ValueError(f"plugin {name} must be valid UTF-8") from error
    return value


def Plugin(
    *,
    name: str,
    version: str,
    author: str = "",
    description: str = "",
    type: PluginType | int | str = PluginType.GENERIC,
) -> Callable[[_PluginClass], _PluginClass]:
    """Declare the single NeverD plugin class exported by a script."""

    name = _metadata_string("name", name, allow_empty=False)
    if not isinstance(version, str):
        raise TypeError("plugin version must be a string")
    match = _SEMVER.fullmatch(version)
    if match is None:
        raise ValueError(f"plugin version is not strict SemVer: {version!r}")
    prerelease = match.group(4) or ""
    if any(
        identifier.isdigit() and len(identifier) > 1 and identifier.startswith("0")
        for identifier in prerelease.split(".")
    ):
        raise ValueError(f"plugin version is not strict SemVer: {version!r}")
    version_info = (
        int(match.group(1)),
        int(match.group(2)),
        int(match.group(3)),
    )
    if any(component > 0xFFFFFFFF for component in version_info):
        raise ValueError("plugin version component exceeds uint32")
    author = _metadata_string("author", author, allow_empty=True)
    description = _metadata_string("description", description, allow_empty=True)
    if isinstance(type, str):
        try:
            plugin_type = PluginType[type.upper()]
        except KeyError as error:
            raise ValueError(f"unknown plugin type: {type!r}") from error
    else:
        if isinstance(type, bool):
            raise TypeError("plugin type must not be a boolean")
        try:
            plugin_type = PluginType(type)
        except (TypeError, ValueError) as error:
            raise ValueError(f"unknown plugin type: {type!r}") from error

    def decorate(plugin_class: _PluginClass) -> _PluginClass:
        if not isinstance(plugin_class, builtins.type):
            raise TypeError("Plugin can decorate only a class")
        module = sys.modules.get(plugin_class.__module__)
        if not isinstance(module, ModuleType):
            raise RuntimeError("plugin class is not attached to an imported module")
        if hasattr(module, "__neverd_plugin__"):
            raise RuntimeError("a module may declare only one NeverD plugin")
        spec = PluginSpec(
            name=name,
            version=version,
            version_info=version_info,
            prerelease=prerelease,
            build_metadata=match.group(5) or "",
            author=author,
            description=description,
            type=plugin_type,
            plugin_class=plugin_class,
        )
        setattr(module, "__neverd_plugin__", spec)
        setattr(plugin_class, "__neverd_plugin__", spec)
        return plugin_class

    return decorate


class _NativeBridge(Protocol):
    def session_address(self, native_handle: object) -> object: ...


class _Host(Protocol):
    def function(self, name: str) -> Any: ...

    def call(self, name: str, *arguments: object) -> Any: ...

    def owned_string(self, name: str, *arguments: object) -> str | None: ...

    def borrowed_bytes(self, name: str, *arguments: object) -> bytes: ...


def _native_bridge() -> _NativeBridge:
    try:
        import _neverd_plugin as native_module  # type: ignore[import-not-found]
    except ModuleNotFoundError as error:
        raise RuntimeError(
            "NeverD's native Python plugin bridge is unavailable; "
            "Session objects are supplied only to Python-enabled NeverD plugins"
        ) from error
    return cast(_NativeBridge, native_module)


@dataclass(frozen=True, slots=True)
class SymbolicPath:
    outcome: str
    block: int
    blocks: tuple[int, ...]
    constraints: int
    unmodelled_ops: int
    predicate: str | None = None
    target: str | None = None


@dataclass(frozen=True, slots=True)
class SymbolicExploration:
    function: str
    entry: int
    lift_complete: bool
    complete: bool
    exact: bool
    reachable_paths: int
    executed_steps: int
    unmodelled_ops: int
    paths: tuple[SymbolicPath, ...]


class ConcolicImageIdentityStatus(str, Enum):
    EXACT_LOADED_SNAPSHOT = "exact_loaded_snapshot"
    UNAVAILABLE = "unavailable"


class ConcolicEndianness(str, Enum):
    LITTLE = "little"
    BIG = "big"


class ConcolicTraceOutcome(str, Enum):
    RETURNED = "returned"
    LEFT_FUNCTION = "left_function"
    UNRESOLVED_BRANCH = "unresolved_branch"
    INVALID_CONTROL_TARGET = "invalid_control_target"
    LOOP_BUDGET = "loop_budget"
    STEP_BUDGET = "step_budget"
    INFEASIBLE = "infeasible"


class ConcolicTraceReason(str, Enum):
    NONE = "none"
    INVALID_INITIAL_SEED = "invalid_initial_seed"
    NO_TRACE = "no_trace"
    AMBIGUOUS_TRACE = "ambiguous_trace"
    INCOMPLETE_CONCRETE_TRACE = "incomplete_concrete_trace"
    INCOMPLETE_LIFT = "incomplete_lift"
    UNSUPPORTED_EFFECTS = "unsupported_effects"
    INCOMPLETE_OUTCOME = "incomplete_outcome"
    INVALID_DECISION_HISTORY = "invalid_decision_history"


class ConcolicDecisionKind(str, Enum):
    CONDITIONAL_BRANCH = "conditional_branch"
    INDIRECT_BRANCH_TARGET = "indirect_branch_target"


class ConcolicFlipStatus(str, Enum):
    VERIFIED = "verified"
    UNSAT = "unsat"
    SOLVER_UNKNOWN = "solver_unknown"
    INVALID_QUERY = "invalid_query"
    PROJECTION_REJECTED = "projection_rejected"
    REPLAY_REJECTED = "replay_rejected"
    VERIFIED_DUPLICATE = "verified_duplicate"
    ATTEMPT_BUDGET_EXCEEDED = "attempt_budget_exceeded"
    CANDIDATE_BUDGET_EXCEEDED = "candidate_budget_exceeded"


class ConcolicSolverStatus(str, Enum):
    NOT_RUN = "not_run"
    UNSAT = "unsat"
    SAT = "sat"
    UNKNOWN = "unknown"
    INVALID = "invalid"


class ConcolicEncodingError(str, Enum):
    NONE = "none"
    WIDTH_TOO_LARGE = "width too large"
    TOO_MANY_GATES = "too many gates"
    MALFORMED_EXPRESSION = "malformed expression"


class ConcolicProjectionStatus(str, Enum):
    NOT_RUN = "not_run"
    ACCEPTED = "accepted"
    REJECTED = "rejected"


class ConcolicProjectionReason(str, Enum):
    NONE = "none"
    INVALID_QUERY = "invalid_query"
    FRESH_VARIABLE = "fresh_variable"
    MISSING_INPUT_ORIGIN = "missing_input_origin"
    UNSUPPORTED_INPUT_KIND = "unsupported_input_kind"
    NONZERO_INPUT_EPOCH = "nonzero_input_epoch"
    INVALID_INPUT_WIDTH = "invalid_input_width"
    MISSING_MODEL_VALUE = "missing_model_value"
    OVERLAPPING_INPUT_ORIGINS = "overlapping_input_origins"
    MISSING_BASELINE_BYTE = "missing_baseline_byte"
    CANDIDATE_DOES_NOT_SATISFY_QUERY = "candidate_does_not_satisfy_query"


class ConcolicReplayStatus(str, Enum):
    NOT_RUN = "not_run"
    VERIFIED = "verified"
    REJECTED = "rejected"


class ConcolicReplayReason(str, Enum):
    NONE = "none"
    NO_REPLAY_TRACE = "no_replay_trace"
    EARLIER_DECISION_MISSING = "earlier_decision_missing"
    EARLIER_OCCURRENCE_MISMATCH = "earlier_occurrence_mismatch"
    EARLIER_POLARITY_MISMATCH = "earlier_polarity_mismatch"
    EARLIER_CONSTRAINT_PREFIX_MISMATCH = "earlier_constraint_prefix_mismatch"
    EARLIER_DECISION_NOT_CONCRETE = "earlier_decision_not_concrete"
    TARGET_DECISION_MISSING = "target_decision_missing"
    TARGET_OCCURRENCE_MISMATCH = "target_occurrence_mismatch"
    TARGET_DECISION_NOT_CONCRETE = "target_decision_not_concrete"
    TARGET_CONSTRAINT_PREFIX_MISMATCH = "target_constraint_prefix_mismatch"
    TARGET_POLARITY_NOT_FLIPPED = "target_polarity_not_flipped"


@dataclass(frozen=True, slots=True)
class ConcolicRegisterSeed:
    offset: int
    value: int
    bytes: int

    def __post_init__(self) -> None:
        offset = _unsigned("register seed offset", self.offset, 64)
        value = _unsigned("register seed value", self.value, 64)
        width = _unsigned("register seed bytes", self.bytes, 32)
        if width == 0 or width > 8:
            raise ValueError("register seed bytes must be between 1 and 8")
        if offset > ((1 << 64) - 1) - width:
            raise ValueError("register seed range overflows uint64")
        if width < 8 and value >= 1 << (width * 8):
            raise ValueError("register seed value has high bits outside its width")


@dataclass(frozen=True, slots=True)
class ConcolicImage:
    format: str
    arch: str
    bits: int
    endianness: ConcolicEndianness
    base: int
    entry: int
    identity_status: ConcolicImageIdentityStatus
    identity_reason: str | None
    sha256: str | None


@dataclass(frozen=True, slots=True)
class ConcolicFunction:
    entry: int
    name: str
    lift_complete: bool


@dataclass(frozen=True, slots=True)
class ConcolicLimits:
    max_steps: int
    max_block_visits: int
    max_loop_iterations: int
    max_flip_attempts: int
    max_candidates: int
    solver_max_conflicts: int
    solver_max_propagations: int
    solver_max_watch_visits: int
    solver_max_width: int
    solver_max_gates: int


@dataclass(frozen=True, slots=True)
class ConcolicDecisionOccurrence:
    va: int
    seq: int
    block_id: int
    op_index: int
    invocation: int
    kind: ConcolicDecisionKind


@dataclass(frozen=True, slots=True)
class ConcolicDecision:
    decision_id: int
    occurrence: ConcolicDecisionOccurrence
    taken: bool
    constraint_prefix: int
    concrete: bool

    @property
    def id(self) -> int:
        return self.decision_id


@dataclass(frozen=True, slots=True)
class ConcolicCandidate:
    candidate_id: int
    seed: tuple[ConcolicRegisterSeed, ...]

    @property
    def id(self) -> int:
        return self.candidate_id


@dataclass(frozen=True, slots=True)
class ConcolicFlip:
    decision_id: int
    occurrence: ConcolicDecisionOccurrence
    original_taken: bool
    constraint_prefix: int
    status: ConcolicFlipStatus
    solver_status: ConcolicSolverStatus
    encoding_error: ConcolicEncodingError
    projection_status: ConcolicProjectionStatus
    projection_reason: ConcolicProjectionReason
    replay_status: ConcolicReplayStatus
    replay_reason: ConcolicReplayReason
    candidate_id: int | None


@dataclass(frozen=True, slots=True)
class ConcolicReport:
    schema_version: int
    adapter: str
    mode: str
    ok: bool
    exhaustive: bool
    image: ConcolicImage
    function: ConcolicFunction
    limits: ConcolicLimits
    initial_seed: tuple[ConcolicRegisterSeed, ...]
    trace_outcome: ConcolicTraceOutcome | None
    trace_complete: bool
    trace_exact: bool
    trace_reason: ConcolicTraceReason
    executed_steps: int
    unmodelled_ops: int
    opaque_ops: int
    call_havocs: int
    memory_havocs: int
    flip_attempts: int
    flip_budget_hit: bool
    candidate_budget_hit: bool
    blocks: tuple[int, ...]
    decisions: tuple[ConcolicDecision, ...]
    flips: tuple[ConcolicFlip, ...]
    candidates: tuple[ConcolicCandidate, ...]


class Session:
    """A lifetime-checked view of a host-owned NeverD session."""

    __slots__ = ("_native_handle", "_native", "_host")

    def __init__(
        self,
        native_handle: object,
        *,
        _native: _NativeBridge | None = None,
        _host: _Host | None = None,
    ) -> None:
        self._native_handle = native_handle
        self._native = _native if _native is not None else _native_bridge()
        self._host = _host

    def _host_api(self) -> _Host:
        if self._host is None:
            self._host = HostAPI()
        return self._host

    def _pointer(self) -> ctypes.c_void_p:
        address = self._native.session_address(self._native_handle)
        if not isinstance(address, int) or isinstance(address, bool) or address <= 0:
            raise RuntimeError("NeverD returned an invalid session address")
        return ctypes.c_void_p(address)

    @property
    def file_path(self) -> str:
        value = self._owned_string("neverd_session_file_path")
        if value is None:
            raise RuntimeError("NeverD returned a null session file path")
        return value

    def _owned_string(self, name: str, *arguments: object) -> str | None:
        pointer = self._pointer()
        return self._host_api().owned_string(name, pointer, *arguments)

    def _call(self, name: str, *arguments: object) -> Any:
        pointer = self._pointer()
        return self._host_api().call(name, pointer, *arguments)

    def _require_success(self, result: Any, operation: str) -> None:
        if int(result) != 0:
            return
        message = self._owned_string("neverd_last_error")
        raise NeverDError(message or f"NeverD failed to {operation}")

    def _require_zero(self, result: Any, operation: str) -> None:
        if int(result) == 0:
            return
        message = self._owned_string("neverd_last_error")
        raise NeverDError(message or f"NeverD failed to {operation}")

    @property
    def raw(self) -> "RawSessionAPI":
        """Expose the complete declarative ABI for advanced plugin code."""

        return RawSessionAPI(self)

    @property
    def loaded(self) -> bool:
        return bool(self._call("neverd_session_is_loaded"))

    @property
    def architecture(self) -> str:
        return self._owned_string("neverd_session_arch_name") or ""

    @property
    def format(self) -> str:
        return self._owned_string("neverd_session_format_name") or ""

    @property
    def is_64bit(self) -> bool:
        return bool(self._call("neverd_session_is_64bit"))

    @property
    def bitness(self) -> int:
        return int(self._call("neverd_session_bitness"))

    @property
    def debug_info_kind(self) -> str:
        """Which loader named the functions: "dwarf", "pdb", "map", or "none"."""

        return self._owned_string("neverd_session_debug_info_kind") or "none"

    @property
    def debug_info_path(self) -> str:
        """File the debug symbols came from, empty when there are none."""

        return self._owned_string("neverd_session_debug_info_path") or ""

    @property
    def file_size(self) -> int:
        return int(self._call("neverd_session_file_size"))

    @property
    def base_address(self) -> int:
        return int(self._call("neverd_session_base_addr"))

    @property
    def entry_address(self) -> int:
        return int(self._call("neverd_session_entry_addr"))

    @property
    def function_count(self) -> int:
        return int(self._call("neverd_func_count"))

    @property
    def segment_count(self) -> int:
        return int(self._call("neverd_session_segment_count"))

    @property
    def section_count(self) -> int:
        return int(self._call("neverd_session_section_count"))

    @property
    def import_count(self) -> int:
        return int(self._call("neverd_session_import_count"))

    @property
    def export_count(self) -> int:
        return int(self._call("neverd_session_export_count"))

    @property
    def symbol_count(self) -> int:
        return int(self._call("neverd_session_symbol_count"))

    @property
    def project_name(self) -> str:
        return self._host_api().owned_string("neverd_project_name") or ""

    @property
    def version(self) -> str:
        return self._host_api().owned_string("neverd_version_number") or ""

    def set_debug_info(
        self,
        *,
        enabled: bool = True,
        pdb_path: os.PathLike[str] | str | None = None,
        map_path: os.PathLike[str] | str | None = None,
    ) -> None:
        """Choose the debug symbols the next load() reads.

        The search happens during load(), so this only takes effect on a
        subsequent one.  Naming a file makes it authoritative: load() then
        fails rather than falling back to a companion file that happens to sit
        beside the binary.  Pass an empty path to resume searching, or
        enabled=False to read the image alone.
        """

        self._call(
            "neverd_session_set_debug_info_enabled",
            int(_boolean("enabled", enabled)),
        )
        if pdb_path is not None:
            self._call(
                "neverd_session_set_pdb_path",
                _utf8_argument("PDB path", os.fspath(pdb_path), allow_empty=True),
            )
        if map_path is not None:
            self._call(
                "neverd_session_set_map_path",
                _utf8_argument("MAP path", os.fspath(map_path), allow_empty=True),
            )

    def load(self, path: os.PathLike[str] | str) -> None:
        encoded = _utf8_argument("path", os.fspath(path), allow_empty=False)
        self._require_success(
            self._call("neverd_session_load", encoded), f"load {os.fspath(path)!r}"
        )

    def analyze(self) -> None:
        self._require_success(self._call("neverd_session_analyze"), "analyze")

    def functions(self) -> Iterator["Function"]:
        for index in range(self.function_count):
            yield Function(self, index)

    def read(self, address: int, size: int) -> bytes:
        address = _unsigned("address", address, 64)
        size = _c_int("size", size, minimum=0)
        if size == 0:
            return b""
        buffer = (ctypes.c_ubyte * size)()
        count = int(self._call("neverd_read_bytes", address, buffer, size))
        if count < 0 or count > size:
            raise NeverDError(f"NeverD returned invalid byte count {count}")
        return bytes(buffer[:count])

    def disassemble(self, address: int, count: int = 0) -> object:
        value = self._owned_string(
            "neverd_disasm_json",
            _unsigned("address", address, 64),
            _c_int("count", count, minimum=0),
        )
        return _decode_json("disassembly", value)

    def decompile(self, address: int) -> str:
        value = self._owned_string(
            "neverd_decompile", _unsigned("address", address, 64)
        )
        if value is None:
            raise NeverDError(
                self._owned_string("neverd_last_error") or "decompile failed"
            )
        return value

    def ir(self, address: int, level: str = "high") -> str:
        functions = {
            "low": "neverd_ir_low",
            "med": "neverd_ir_med",
            "high": "neverd_ir_high",
            "llvm": "neverd_ir_llvm",
        }
        try:
            function = functions[level.casefold()]
        except (AttributeError, KeyError) as error:
            raise ValueError("IR level must be low, med, high, or llvm") from error
        value = self._owned_string(function, _unsigned("address", address, 64))
        if value is None:
            raise NeverDError(self.last_error or f"{level} IR is unavailable")
        return value

    @property
    def last_error(self) -> str:
        return self._owned_string("neverd_last_error") or ""

    def _json(self, name: str, operation: str, *arguments: object) -> object:
        return _decode_json(operation, self._owned_string(name, *arguments))

    @property
    def imports(self) -> object:
        return self._json("neverd_imports_json", "imports")

    @property
    def exports(self) -> object:
        return self._json("neverd_exports_json", "exports")

    @property
    def segments(self) -> object:
        return self._json("neverd_segments_json", "segments")

    @property
    def sections(self) -> object:
        return self._json("neverd_sections_json", "sections")

    @property
    def symbols(self) -> object:
        return self._json("neverd_symbols_json", "symbols")

    @property
    def relocations(self) -> object:
        return self._json("neverd_relocs_json", "relocations")

    @property
    def headers(self) -> object:
        return self._json("neverd_headers_json", "headers")

    @property
    def entrypoints(self) -> object:
        return self._json("neverd_entrypoints_json", "entrypoints")

    @property
    def dashboard(self) -> object:
        return self._json("neverd_dashboard_json", "dashboard")

    @property
    def callgraph(self) -> object:
        return self._json("neverd_callgraph_json", "call graph")

    def strings(self, minimum_length: int = 4) -> object:
        return self._json(
            "neverd_strings_json",
            "strings",
            _c_int("minimum string length", minimum_length, minimum=1),
        )

    def xrefs_to(self, address: int) -> object:
        return self._json(
            "neverd_xrefs_to_json",
            "incoming cross-references",
            _unsigned("address", address, 64),
        )

    def xrefs_from(self, address: int) -> object:
        return self._json(
            "neverd_xrefs_from_json",
            "outgoing cross-references",
            _unsigned("address", address, 64),
        )

    def cfg(self, address: int) -> object:
        return self._json(
            "neverd_cfg_json",
            "control-flow graph",
            _unsigned("address", address, 64),
        )

    def symbolic_explore(
        self,
        address: int,
        *,
        max_paths: int = 64,
        max_steps: int = 1 << 16,
        max_block_visits: int = 3,
        include_expressions: bool = False,
    ) -> SymbolicExploration:
        """Explore every bounded LowIR path through a native function.

        ``complete`` is false when a resource bound or unresolved indirect
        branch stopped the walk.  ``exact`` additionally requires that no
        operation had to be replaced by an unconstrained value.
        """

        options = NeverDSymbolicExploreOptions()
        options.struct_size = ctypes.sizeof(NeverDSymbolicExploreOptions)
        options.max_paths = _unsigned("max_paths", max_paths, 32)
        options.max_steps = _unsigned("max_steps", max_steps, 32)
        options.max_block_visits = _unsigned("max_block_visits", max_block_visits, 32)
        options.include_expressions = (
            1 if _boolean("include_expressions", include_expressions) else 0
        )
        report = self._json(
            "neverd_symbolic_explore_json",
            "symbolic exploration",
            _unsigned("address", address, 64),
            ctypes.byref(options),
        )
        if not isinstance(report, Mapping) or report.get("ok") is not True:
            message = (
                report.get("error")
                if isinstance(report, Mapping)
                else "invalid symbolic exploration report"
            )
            raise NeverDError(str(message or self.last_error))

        paths: list[SymbolicPath] = []
        raw_paths = report.get("paths", ())
        if not isinstance(raw_paths, list):
            raise NeverDError("NeverD returned invalid symbolic paths")
        for raw_path in raw_paths:
            if not isinstance(raw_path, Mapping):
                raise NeverDError("NeverD returned an invalid symbolic path")
            raw_blocks = raw_path.get("blocks", ())
            if not isinstance(raw_blocks, list):
                raise NeverDError("NeverD returned invalid symbolic path blocks")
            paths.append(
                SymbolicPath(
                    outcome=str(raw_path.get("outcome", "")),
                    block=int(raw_path.get("block", -1)),
                    blocks=tuple(int(block) for block in raw_blocks),
                    constraints=int(raw_path.get("constraints", 0)),
                    unmodelled_ops=int(raw_path.get("unmodelledOps", 0)),
                    predicate=(
                        str(raw_path["predicate"]) if "predicate" in raw_path else None
                    ),
                    target=(str(raw_path["target"]) if "target" in raw_path else None),
                )
            )

        return SymbolicExploration(
            function=str(report.get("function", "")),
            entry=int(str(report.get("entry", "0")), 0),
            lift_complete=bool(report.get("liftComplete", False)),
            complete=bool(report.get("complete", False)),
            exact=bool(report.get("exact", False)),
            reachable_paths=int(report.get("reachablePaths", 0)),
            executed_steps=int(report.get("executedSteps", 0)),
            unmodelled_ops=int(report.get("unmodelledOps", 0)),
            paths=tuple(paths),
        )

    def lowir_concolic(
        self,
        address: int,
        seeds: Iterable[ConcolicRegisterSeed] = (),
        *,
        max_steps: int = 0,
        max_block_visits: int = 0,
        max_loop_iterations: int = 0,
        max_flip_attempts: int = 0,
        max_candidates: int = 0,
        solver_max_conflicts: int = 0,
        solver_max_propagations: int = 0,
        solver_max_watch_visits: int = 0,
        solver_max_gates: int = 0,
    ) -> ConcolicReport:
        """Run one bounded LowIR trace and return replay-verified branch flips."""

        entry = _unsigned("address", address, 64)
        try:
            seed_values: tuple[ConcolicRegisterSeed, ...] = tuple(
                islice(seeds, _LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1 + 1)
            )
        except TypeError as error:
            raise TypeError(
                "seeds must be an iterable of ConcolicRegisterSeed"
            ) from error
        if len(seed_values) > _LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1:
            raise ValueError(
                "register seed count must not exceed "
                f"{_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1}"
            )
        for seed in seed_values:
            if not isinstance(seed, ConcolicRegisterSeed):
                raise TypeError("seeds must contain only ConcolicRegisterSeed values")
        _validate_concolic_seed_ranges(seed_values, "register seeds")

        seed_array: object | None = None
        options = NeverDLowIRConcolicOptionsV1()
        options.struct_size = ctypes.sizeof(NeverDLowIRConcolicOptionsV1)
        if seed_values:
            seed_array_type = NeverDLowIRConcolicRegisterSeedV1 * len(seed_values)
            seed_array = seed_array_type(
                *(
                    NeverDLowIRConcolicRegisterSeedV1(
                        offset=seed.offset,
                        value=seed.value,
                        bytes=seed.bytes,
                        reserved=0,
                    )
                    for seed in seed_values
                )
            )
            options.register_seeds = ctypes.cast(
                seed_array,
                ctypes.POINTER(NeverDLowIRConcolicRegisterSeedV1),
            )
        else:
            options.register_seeds = None
        options.register_seed_count = len(seed_values)
        options.max_steps = _unsigned("max_steps", max_steps, 32)
        options.max_block_visits = _unsigned("max_block_visits", max_block_visits, 32)
        options.max_loop_iterations = _unsigned(
            "max_loop_iterations", max_loop_iterations, 32
        )
        options.max_flip_attempts = _unsigned(
            "max_flip_attempts", max_flip_attempts, 32
        )
        options.max_candidates = _unsigned("max_candidates", max_candidates, 32)
        options.reserved = 0
        options.solver_max_conflicts = _unsigned(
            "solver_max_conflicts", solver_max_conflicts, 64
        )
        options.solver_max_propagations = _unsigned(
            "solver_max_propagations", solver_max_propagations, 64
        )
        options.solver_max_watch_visits = _unsigned(
            "solver_max_watch_visits", solver_max_watch_visits, 64
        )
        options.solver_max_gates = _unsigned("solver_max_gates", solver_max_gates, 64)

        expected_limits = ConcolicLimits(
            max_steps=max_steps or 1 << 16,
            max_block_visits=max_block_visits or 3,
            max_loop_iterations=max_loop_iterations or 3,
            max_flip_attempts=max_flip_attempts or 64,
            max_candidates=max_candidates or 64,
            solver_max_conflicts=solver_max_conflicts or 1 << 18,
            solver_max_propagations=solver_max_propagations or 1 << 24,
            solver_max_watch_visits=solver_max_watch_visits or 1 << 26,
            solver_max_width=256,
            solver_max_gates=solver_max_gates or 1 << 22,
        )
        expected_seed = tuple(
            sorted(
                seed_values,
                key=lambda seed: (seed.offset, seed.bytes, seed.value),
            )
        )

        raw_report = self._json(
            "neverd_lowir_concolic_json_v1",
            "LowIR concolic",
            entry,
            ctypes.byref(options),
        )
        # ``seed_array`` intentionally remains live through the owned-string call.
        return _parse_concolic_report(
            raw_report,
            expected_entry=entry,
            expected_seed=expected_seed,
            expected_limits=expected_limits,
        )

    def _safety_options(
        self,
        *,
        max_paths: int,
        max_steps: int,
        max_loop: int,
        solver_conflicts: int,
        sinks: str | None,
        sources: str | None,
    ) -> "NeverDSafetyOptions":
        options = NeverDSafetyOptions()
        options.struct_size = ctypes.sizeof(NeverDSafetyOptions)
        options.max_paths = _unsigned("max_paths", max_paths, 32)
        options.max_steps = _unsigned("max_steps", max_steps, 32)
        options.max_loop = _unsigned("max_loop", max_loop, 32)
        options.solver_conflicts = _unsigned(
            "solver_conflicts", solver_conflicts, 64
        )
        options.sinks_path = sinks.encode("utf-8") if sinks else None
        options.sources_path = sources.encode("utf-8") if sources else None
        return options

    def audit(
        self,
        *,
        max_paths: int = 0,
        max_steps: int = 0,
        max_loop: int = 0,
        solver_conflicts: int = 0,
        sinks: str | None = None,
        sources: str | None = None,
    ) -> Mapping[str, object]:
        """Audit heap-object lifetimes and uninitialized local stack reads,
        and return the parsed JSON report."""

        options = self._safety_options(
            max_paths=max_paths,
            max_steps=max_steps,
            max_loop=max_loop,
            solver_conflicts=solver_conflicts,
            sinks=sinks,
            sources=sources,
        )
        report = self._json(
            "neverd_session_audit_json", "safety audit", ctypes.byref(options)
        )
        if not isinstance(report, Mapping) or report.get("ok") is not True:
            message = (
                report.get("error")
                if isinstance(report, Mapping)
                else "invalid safety audit report"
            )
            raise NeverDError(str(message or self.last_error))
        return report

    def hunt(
        self,
        *,
        max_paths: int = 0,
        max_steps: int = 0,
        max_loop: int = 0,
        solver_conflicts: int = 0,
        sinks: str | None = None,
        sources: str | None = None,
    ) -> Mapping[str, object]:
        """Hunt dangerous-copy overflows and return the parsed JSON report.

        Each finding carries a verdict (SAFE / UNSAFE / UNKNOWN), a confidence,
        and — for a proven overflow — a solver model plus candidate witness
        values. ``replayable`` is derived from complete ``inputs`` in the
        ``replay`` object for the ``process-input-v1`` adapter. Initially those
        plans cover exact literal environment values and the first supported
        ``read(0)``-family standard-input consumption; argv, file, network,
        custom, and ambiguous inputs remain non-replayable with
        ``replay.reason``. These evidence
        fields are additive to report schema version 1. Unknown or partially
        applicable call effects remain UNKNOWN."""

        options = self._safety_options(
            max_paths=max_paths,
            max_steps=max_steps,
            max_loop=max_loop,
            solver_conflicts=solver_conflicts,
            sinks=sinks,
            sources=sources,
        )
        report = self._json(
            "neverd_session_hunt_json", "overflow hunt", ctypes.byref(options)
        )
        if not isinstance(report, Mapping) or report.get("ok") is not True:
            message = (
                report.get("error")
                if isinstance(report, Mapping)
                else "invalid overflow hunt report"
            )
            raise NeverDError(str(message or self.last_error))
        return report

    def hex_dump(self, address: int, size: int) -> str:
        value = self._owned_string(
            "neverd_hex_dump",
            _unsigned("address", address, 64),
            _c_int("size", size, minimum=0),
        )
        if value is None:
            raise NeverDError(self.last_error or "hex dump failed")
        return value

    def function_by_name(self, name: str) -> "Function | None":
        encoded = _utf8_argument("function name", name, allow_empty=False)
        index = int(self._call("neverd_func_find_by_name", encoded))
        return None if index < 0 else Function(self, index)

    def function_at(self, address: int) -> "Function | None":
        index = int(
            self._call("neverd_func_find_by_addr", _unsigned("address", address, 64))
        )
        return None if index < 0 else Function(self, index)

    def set_annotation(self, address: int, text: str) -> None:
        self._call(
            "neverd_annotation_set",
            _unsigned("address", address, 64),
            _utf8_argument("annotation", text, allow_empty=True),
        )

    def remove_annotation(self, address: int) -> None:
        self._call("neverd_annotation_remove", _unsigned("address", address, 64))

    def annotation(self, address: int) -> str | None:
        return self._owned_string(
            "neverd_annotation_get", _unsigned("address", address, 64)
        )

    @property
    def annotations(self) -> object:
        return self._json("neverd_annotations_json", "annotations")

    def save_annotations(self) -> None:
        self._require_zero(self._call("neverd_annotations_save"), "save annotations")

    def load_annotations(self) -> None:
        self._require_zero(self._call("neverd_annotations_load"), "load annotations")

    def rename_function(self, old_name: str, new_name: str) -> None:
        self._require_zero(
            self._call(
                "neverd_rename_func",
                _utf8_argument("old function name", old_name, allow_empty=False),
                _utf8_argument("new function name", new_name, allow_empty=False),
            ),
            "rename function",
        )

    @property
    def renames(self) -> object:
        return self._json("neverd_renames_json", "renames")

    def resolve_address(self, address: int) -> str | None:
        return self._owned_string(
            "neverd_resolve_addr", _unsigned("address", address, 64)
        )

    def search_bytes(self, pattern: bytes, max_results: int = 0) -> object:
        if not isinstance(pattern, bytes):
            raise TypeError("byte pattern must be bytes")
        if not pattern:
            raise ValueError("byte pattern must not be empty")
        if len(pattern) > (1 << 31) - 1:
            raise ValueError("byte pattern is too large")
        buffer = (ctypes.c_ubyte * len(pattern)).from_buffer_copy(pattern)
        return self._json(
            "neverd_search_bytes",
            "byte search",
            buffer,
            len(pattern),
            _c_int("maximum results", max_results, minimum=0),
        )

    def search_string(
        self, pattern: str, *, case_sensitive: bool = True, max_results: int = 0
    ) -> object:
        return self._json(
            "neverd_search_string",
            "string search",
            _utf8_argument("search pattern", pattern, allow_empty=False),
            int(_boolean("case_sensitive", case_sensitive)),
            _c_int("maximum results", max_results, minimum=0),
        )

    def diff_functions(self, other: "Session") -> object:
        if not isinstance(other, Session):
            raise TypeError("other must be a Session")
        left = self._pointer()
        right = other._pointer()
        return _decode_json(
            "function diff",
            self._host_api().owned_string("neverd_diff_functions", left, right),
        )

    def diff_decompile(self, address: int, other: "Session", other_address: int) -> str:
        if not isinstance(other, Session):
            raise TypeError("other must be a Session")
        left = self._pointer()
        right = other._pointer()
        value = self._host_api().owned_string(
            "neverd_diff_decompile",
            left,
            _unsigned("address", address, 64),
            right,
            _unsigned("other address", other_address, 64),
        )
        if value is None:
            raise NeverDError(self.last_error or "decompile diff failed")
        return value

    def configure_transforms(
        self,
        *,
        instruction_substitution: bool = False,
        substitution_rounds: int = 1,
        constant_encryption: bool = False,
        opaque_predicate: bool = False,
        control_flow_flattening: bool = False,
        bogus_control_flow: bool = False,
        indirect_branch: bool = False,
        indirect_call: bool = False,
        mba: bool = False,
        indirect_global: bool = False,
        value_laundering: bool = False,
        constant_pooling: bool = False,
        bit_masking: bool = False,
    ) -> None:
        self._call(
            "neverd_set_inst_substitution",
            int(_boolean("instruction_substitution", instruction_substitution)),
            _c_int("substitution rounds", substitution_rounds, minimum=1),
        )
        options = {
            "neverd_set_constant_encryption": constant_encryption,
            "neverd_set_opaque_predicate": opaque_predicate,
            "neverd_set_control_flow_flattening": control_flow_flattening,
            "neverd_set_bogus_control_flow": bogus_control_flow,
            "neverd_set_indirect_branch": indirect_branch,
            "neverd_set_indirect_call": indirect_call,
            "neverd_set_mba": mba,
            "neverd_set_indirect_global": indirect_global,
            "neverd_set_value_launder": value_laundering,
            "neverd_set_constant_pooling": constant_pooling,
            "neverd_set_bit_masking": bit_masking,
        }
        for function, enabled in options.items():
            self._call(function, int(_boolean(function, enabled)))

    def set_text_section(self, name: str | None) -> None:
        value = (
            b""
            if name is None
            else _utf8_argument("text section", name, allow_empty=True)
        )
        self._call("neverd_set_text_section", value)

    @property
    def patch_result(self) -> "PatchResult":
        return PatchResult(
            output_path=self._owned_string("neverd_patch_output_path") or "",
            code_size=int(self._call("neverd_patch_code_size")),
            trampoline_count=int(self._call("neverd_patch_trampoline_count")),
            transform_counts=MappingProxyType(
                {
                    name: int(self._call(function))
                    for name, function in _TRANSFORM_COUNT_FUNCTIONS.items()
                }
            ),
        )

    def patch_from_ir(
        self, ir: str, output_path: os.PathLike[str] | str, *, strategy: int = 0
    ) -> "PatchResult":
        self._require_success(
            self._call(
                "neverd_patch_from_ir",
                _utf8_argument("IR", ir, allow_empty=False),
                _c_int("patch strategy", strategy),
                _utf8_argument(
                    "output path", os.fspath(output_path), allow_empty=False
                ),
            ),
            "patch from IR",
        )
        return self.patch_result

    def patch_from_c(
        self,
        source: str,
        address: int,
        output_path: os.PathLike[str] | str,
    ) -> "PatchResult":
        self._require_success(
            self._call(
                "neverd_patch_from_c",
                _utf8_argument("C source", source, allow_empty=False),
                _unsigned("address", address, 64),
                _utf8_argument(
                    "output path", os.fspath(output_path), allow_empty=False
                ),
            ),
            "patch from C",
        )
        return self.patch_result

    def decompile_all(
        self,
        path: os.PathLike[str] | str,
        *,
        language: OutputLanguage | int = OutputLanguage.C,
        optimize: bool = True,
        max_functions: int = 0,
    ) -> str:
        try:
            output_language = OutputLanguage(language)
        except (TypeError, ValueError) as error:
            raise ValueError(f"unknown output language: {language!r}") from error
        value = self._owned_string(
            "neverd_decompile_all_ex",
            _utf8_argument("input path", os.fspath(path), allow_empty=False),
            int(output_language),
            int(not _boolean("optimize", optimize)),
            _c_int("maximum functions", max_functions, minimum=0),
        )
        if value is None:
            raise NeverDError(self.last_error or "decompile-all failed")
        return value

    def set_evm(self, *, strict: bool = True, hardfork: str | None = None) -> None:
        self._call("neverd_evm_set_strict", int(_boolean("strict", strict)))
        if hardfork is not None:
            self._require_success(
                self._call(
                    "neverd_evm_set_hardfork",
                    _utf8_argument("EVM hardfork", hardfork, allow_empty=False),
                ),
                "select EVM hardfork",
            )

    def set_sbf(
        self,
        *,
        strict: bool = True,
        version: str = "auto",
        idl: str | None = None,
    ) -> None:
        self._call("neverd_sbf_set_strict", int(_boolean("strict", strict)))
        self._require_success(
            self._call(
                "neverd_sbf_set_version",
                _utf8_argument("SBF version", version, allow_empty=False),
            ),
            "select SBF version",
        )
        if idl is not None:
            self._require_success(
                self._call(
                    "neverd_sbf_set_idl",
                    _utf8_argument("Anchor IDL", idl, allow_empty=True),
                ),
                "load Anchor IDL",
            )


class NeverDError(RuntimeError):
    """A failure reported by NeverD's public C API."""


@dataclass(frozen=True, slots=True)
class PatchResult:
    output_path: str
    code_size: int
    trampoline_count: int
    transform_counts: Mapping[str, int]


class RawSessionAPI:
    """Low-level escape hatch retaining the Session lifetime check."""

    __slots__ = ("_session",)

    def __init__(self, session: Session) -> None:
        self._session = session

    def function(self, name: str) -> Any:
        return self._session._host_api().function(name)

    def call(self, name: str, *arguments: object) -> Any:
        return self._session._host_api().call(name, *arguments)

    def session_call(self, name: str, *arguments: object) -> object:
        return self._session._call(name, *arguments)

    def owned_string(self, name: str, *arguments: object) -> str | None:
        return self._session._host_api().owned_string(name, *arguments)

    def session_owned_string(self, name: str, *arguments: object) -> str | None:
        return self._session._owned_string(name, *arguments)

    def borrowed_bytes(self, name: str, *arguments: object) -> bytes:
        return self._session._host_api().borrowed_bytes(name, *arguments)

    def session_borrowed_bytes(self, name: str, *arguments: object) -> bytes:
        pointer = self._session._pointer()
        return self._session._host_api().borrowed_bytes(name, pointer, *arguments)


@dataclass(frozen=True, slots=True)
class Function:
    session: Session
    index: int

    def __post_init__(self) -> None:
        _c_int("function index", self.index, minimum=0)

    @property
    def address(self) -> int:
        return int(self.session._call("neverd_func_entry", self.index))

    @property
    def size(self) -> int:
        return int(self.session._call("neverd_func_size", self.index))

    @property
    def name(self) -> str:
        value = self.session._owned_string("neverd_func_name", self.index)
        if value is None:
            raise IndexError(f"NeverD has no function at index {self.index}")
        return value


@dataclass(frozen=True, slots=True)
class Event:
    type: EventType
    session: Session
    path: str | None = None
    address: int | None = None
    name: str | None = None
    output_path: str | None = None
    code_size: int | None = None

    @classmethod
    def from_host(
        cls,
        session: Session,
        type: EventType | int,
        *,
        path: str | None = None,
        address: int | None = None,
        name: str | None = None,
        output_path: str | None = None,
        code_size: int | None = None,
    ) -> "Event":
        """Build an immutable event after the native callback copied its payload."""

        try:
            event_type = EventType(type)
        except (TypeError, ValueError) as error:
            raise ValueError(f"unknown NeverD event type: {type!r}") from error

        event_path: str | None = None
        event_address: int | None = None
        event_name: str | None = None
        event_output_path: str | None = None
        event_code_size: int | None = None
        if event_type is EventType.BINARY_LOADED:
            if path is None:
                raise ValueError("BINARY_LOADED event requires path")
            _utf8_argument("event path", path, allow_empty=True)
            event_path = path
        elif event_type is EventType.FUNCTION_SELECTED:
            if address is None or name is None:
                raise ValueError("FUNCTION_SELECTED event requires address and name")
            event_address = _unsigned("event address", address, 64)
            _utf8_argument("event function name", name, allow_empty=True)
            event_name = name
        elif event_type is EventType.ADDRESS_CHANGED:
            if address is None:
                raise ValueError("ADDRESS_CHANGED event requires address")
            event_address = _unsigned("event address", address, 64)
        elif event_type is EventType.PATCH_APPLIED:
            if output_path is None or code_size is None:
                raise ValueError(
                    "PATCH_APPLIED event requires output_path and code_size"
                )
            _utf8_argument("event output path", output_path, allow_empty=True)
            event_output_path = output_path
            event_code_size = _c_int("event code size", code_size, minimum=0)
        return cls(
            type=event_type,
            session=session,
            path=event_path,
            address=event_address,
            name=event_name,
            output_path=event_output_path,
            code_size=event_code_size,
        )


def _utf8_argument(name: str, value: object, *, allow_empty: bool) -> bytes:
    text = _metadata_string(name, value, allow_empty=allow_empty)
    return text.encode("utf-8", errors="strict")


def _unsigned(name: str, value: object, bits: int) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise TypeError(f"{name} must be an integer")
    if value < 0 or value >= 1 << bits:
        raise ValueError(f"{name} must fit uint{bits}")
    return value


def _c_int(name: str, value: object, *, minimum: int = -(1 << 31)) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise TypeError(f"{name} must be an integer")
    if value < minimum or value > (1 << 31) - 1:
        raise ValueError(f"{name} must fit a C int")
    return value


def _boolean(name: str, value: object) -> bool:
    if not isinstance(value, bool):
        raise TypeError(f"{name} must be a boolean")
    return value


def _enum_value(name: str, value: object, enum_type: type[Any]) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise TypeError(f"{name} must be an integer enum value")
    try:
        return int(enum_type(value))
    except (TypeError, ValueError) as error:
        raise ValueError(f"unknown {name}: {value!r}") from error


def _size_t(name: str, value: object) -> int:
    return _unsigned(name, value, ctypes.sizeof(ctypes.c_size_t) * 8)


def _decode_json(operation: str, value: str | None) -> object:
    if value is None:
        raise NeverDError(f"NeverD returned no {operation} result")
    try:
        return json.loads(value)
    except json.JSONDecodeError as error:
        raise NeverDError(f"NeverD returned invalid {operation} JSON") from error


_ConcolicEnum = TypeVar("_ConcolicEnum", bound=Enum)


def _concolic_error(path: str, message: str) -> NeverDError:
    return NeverDError(f"NeverD returned invalid LowIR concolic {path}: {message}")


def _concolic_object(value: object, path: str) -> Mapping[str, object]:
    if not isinstance(value, Mapping):
        raise _concolic_error(path, "expected an object")
    return cast(Mapping[str, object], value)


def _concolic_field(value: Mapping[str, object], name: str, path: str) -> object:
    if name not in value:
        raise _concolic_error(path, f"missing field {name!r}")
    return value[name]


def _concolic_string(value: object, path: str) -> str:
    if not isinstance(value, str):
        raise _concolic_error(path, "expected a string")
    return value


def _concolic_bool(value: object, path: str) -> bool:
    if not isinstance(value, bool):
        raise _concolic_error(path, "expected a boolean")
    return value


def _concolic_int(
    value: object,
    path: str,
    *,
    minimum: int = 0,
    maximum: int = (1 << 64) - 1,
) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise _concolic_error(path, "expected an integer")
    if value < minimum or value > maximum:
        raise _concolic_error(path, "integer is out of range")
    return value


def _concolic_array(value: object, path: str) -> list[object]:
    if not isinstance(value, list):
        raise _concolic_error(path, "expected an array")
    return value


def _concolic_enum(
    value: object,
    path: str,
    enum_type: type[_ConcolicEnum],
) -> _ConcolicEnum:
    text = _concolic_string(value, path)
    try:
        return enum_type(text)
    except ValueError as error:
        raise _concolic_error(path, f"unknown enum value {text!r}") from error


def _concolic_hex(value: object, path: str, digits: int) -> int:
    text = _concolic_string(value, path)
    if re.fullmatch(rf"0x[0-9a-f]{{{digits}}}", text) is None:
        raise _concolic_error(path, f"expected a fixed-width {digits}-digit hex value")
    return int(text[2:], 16)


def _validate_concolic_seed_ranges(
    seeds: tuple[ConcolicRegisterSeed, ...],
    path: str,
) -> None:
    ordered = sorted(seeds, key=lambda seed: (seed.offset, seed.bytes, seed.value))
    for previous, current in zip(ordered, ordered[1:]):
        if current.offset < previous.offset + previous.bytes:
            raise ValueError(f"{path} overlap")


def _parse_concolic_seed(value: object, path: str) -> ConcolicRegisterSeed:
    item = _concolic_object(value, path)
    width = _concolic_int(
        _concolic_field(item, "bytes", path),
        f"{path}.bytes",
        minimum=1,
        maximum=8,
    )
    offset = _concolic_hex(
        _concolic_field(item, "offset", path),
        f"{path}.offset",
        16,
    )
    raw_value = _concolic_hex(
        _concolic_field(item, "value", path),
        f"{path}.value",
        width * 2,
    )
    try:
        return ConcolicRegisterSeed(offset=offset, value=raw_value, bytes=width)
    except (TypeError, ValueError) as error:
        raise _concolic_error(path, str(error)) from error


def _parse_concolic_seeds(value: object, path: str) -> tuple[ConcolicRegisterSeed, ...]:
    items = _concolic_array(value, path)
    if len(items) > _LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1:
        raise _concolic_error(
            path,
            f"register seed count exceeds {_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1}",
        )
    seeds = tuple(
        _parse_concolic_seed(item, f"{path}[{index}]")
        for index, item in enumerate(items)
    )
    try:
        _validate_concolic_seed_ranges(seeds, path)
    except ValueError as error:
        raise _concolic_error(path, str(error)) from error
    return seeds


def _parse_concolic_occurrence(
    value: object,
    path: str,
) -> ConcolicDecisionOccurrence:
    item = _concolic_object(value, path)
    return ConcolicDecisionOccurrence(
        va=_concolic_hex(_concolic_field(item, "va", path), f"{path}.va", 16),
        seq=_concolic_int(
            _concolic_field(item, "seq", path),
            f"{path}.seq",
            minimum=-(1 << 31),
            maximum=(1 << 31) - 1,
        ),
        block_id=_concolic_int(
            _concolic_field(item, "block_id", path),
            f"{path}.block_id",
            minimum=-(1 << 31),
            maximum=(1 << 31) - 1,
        ),
        op_index=_concolic_int(
            _concolic_field(item, "op_index", path), f"{path}.op_index"
        ),
        invocation=_concolic_int(
            _concolic_field(item, "invocation", path),
            f"{path}.invocation",
        ),
        kind=_concolic_enum(
            _concolic_field(item, "kind", path),
            f"{path}.kind",
            ConcolicDecisionKind,
        ),
    )


def _validate_concolic_flip_evidence(flip: ConcolicFlip, path: str) -> None:
    no_projection = (
        flip.projection_status is ConcolicProjectionStatus.NOT_RUN
        and flip.projection_reason is ConcolicProjectionReason.NONE
    )
    no_replay = (
        flip.replay_status is ConcolicReplayStatus.NOT_RUN
        and flip.replay_reason is ConcolicReplayReason.NONE
    )
    encoding_none = flip.encoding_error is ConcolicEncodingError.NONE
    no_candidate = flip.candidate_id is None

    if flip.status in {
        ConcolicFlipStatus.VERIFIED,
        ConcolicFlipStatus.VERIFIED_DUPLICATE,
    }:
        valid = (
            flip.solver_status is ConcolicSolverStatus.SAT
            and encoding_none
            and flip.projection_status is ConcolicProjectionStatus.ACCEPTED
            and flip.projection_reason is ConcolicProjectionReason.NONE
            and flip.replay_status is ConcolicReplayStatus.VERIFIED
            and flip.replay_reason is ConcolicReplayReason.NONE
            and flip.candidate_id is not None
        )
    elif flip.status is ConcolicFlipStatus.CANDIDATE_BUDGET_EXCEEDED:
        valid = (
            flip.solver_status is ConcolicSolverStatus.SAT
            and encoding_none
            and flip.projection_status is ConcolicProjectionStatus.ACCEPTED
            and flip.projection_reason is ConcolicProjectionReason.NONE
            and flip.replay_status is ConcolicReplayStatus.VERIFIED
            and flip.replay_reason is ConcolicReplayReason.NONE
            and no_candidate
        )
    elif flip.status is ConcolicFlipStatus.UNSAT:
        valid = (
            flip.solver_status is ConcolicSolverStatus.UNSAT
            and encoding_none
            and no_projection
            and no_replay
            and no_candidate
        )
    elif flip.status is ConcolicFlipStatus.SOLVER_UNKNOWN:
        valid = (
            flip.solver_status
            in {ConcolicSolverStatus.UNKNOWN, ConcolicSolverStatus.NOT_RUN}
            and flip.encoding_error
            in {
                ConcolicEncodingError.NONE,
                ConcolicEncodingError.WIDTH_TOO_LARGE,
                ConcolicEncodingError.TOO_MANY_GATES,
            }
            and not (
                flip.solver_status is ConcolicSolverStatus.NOT_RUN and encoding_none
            )
            and no_projection
            and no_replay
            and no_candidate
        )
    elif flip.status is ConcolicFlipStatus.INVALID_QUERY:
        valid = (
            flip.solver_status
            in {ConcolicSolverStatus.INVALID, ConcolicSolverStatus.NOT_RUN}
            and flip.encoding_error is ConcolicEncodingError.MALFORMED_EXPRESSION
            and no_projection
            and no_replay
            and no_candidate
        )
    elif flip.status is ConcolicFlipStatus.PROJECTION_REJECTED:
        valid = (
            flip.solver_status is ConcolicSolverStatus.SAT
            and encoding_none
            and flip.projection_status is ConcolicProjectionStatus.REJECTED
            and flip.projection_reason is not ConcolicProjectionReason.NONE
            and no_replay
            and no_candidate
        )
    elif flip.status is ConcolicFlipStatus.REPLAY_REJECTED:
        valid = (
            flip.solver_status is ConcolicSolverStatus.SAT
            and encoding_none
            and flip.projection_status is ConcolicProjectionStatus.ACCEPTED
            and flip.projection_reason is ConcolicProjectionReason.NONE
            and flip.replay_status is ConcolicReplayStatus.REJECTED
            and flip.replay_reason is not ConcolicReplayReason.NONE
            and no_candidate
        )
    else:
        valid = (
            flip.status is ConcolicFlipStatus.ATTEMPT_BUDGET_EXCEEDED
            and flip.solver_status is ConcolicSolverStatus.NOT_RUN
            and encoding_none
            and no_projection
            and no_replay
            and no_candidate
        )
    if not valid:
        raise _concolic_error(path, "status and evidence fields are inconsistent")


def _validate_concolic_report_consistency(report: ConcolicReport) -> None:
    if report.executed_steps > report.limits.max_steps:
        raise _concolic_error("report.executed_steps", "exceeds max_steps")
    if len(report.decisions) > report.executed_steps:
        raise _concolic_error("report.decisions", "count exceeds executed_steps")
    if report.flip_attempts > report.limits.max_flip_attempts:
        raise _concolic_error("report.flip_attempts", "exceeds max_flip_attempts")
    if len(report.candidates) > report.limits.max_candidates:
        raise _concolic_error("report.candidates", "count exceeds max_candidates")

    attempted_flips = sum(
        flip.solver_status is not ConcolicSolverStatus.NOT_RUN for flip in report.flips
    )
    if report.flip_attempts != attempted_flips:
        raise _concolic_error("report.flip_attempts", "does not match attempted flips")
    flip_budget_hit = any(
        flip.status is ConcolicFlipStatus.ATTEMPT_BUDGET_EXCEEDED
        for flip in report.flips
    )
    if report.flip_budget_hit is not flip_budget_hit:
        raise _concolic_error("report.flip_budget_hit", "does not match flip statuses")
    if report.flip_budget_hit and (
        report.flip_attempts != report.limits.max_flip_attempts
    ):
        raise _concolic_error(
            "report.flip_budget_hit", "requires an exhausted flip budget"
        )
    candidate_budget_hit = any(
        flip.status is ConcolicFlipStatus.CANDIDATE_BUDGET_EXCEEDED
        for flip in report.flips
    )
    if report.candidate_budget_hit is not candidate_budget_hit:
        raise _concolic_error(
            "report.candidate_budget_hit", "does not match flip statuses"
        )
    if report.candidate_budget_hit and (
        len(report.candidates) != report.limits.max_candidates
    ):
        raise _concolic_error(
            "report.candidate_budget_hit", "requires an exhausted candidate budget"
        )

    published_candidates = 0
    for index, flip in enumerate(report.flips):
        if flip.status is ConcolicFlipStatus.VERIFIED:
            if flip.candidate_id != published_candidates:
                raise _concolic_error(
                    f"report.flips[{index}].candidate_id",
                    "does not publish the next candidate",
                )
            published_candidates += 1
        elif (
            flip.status is ConcolicFlipStatus.VERIFIED_DUPLICATE
            and flip.candidate_id is not None
            and flip.candidate_id >= published_candidates
        ):
            raise _concolic_error(
                f"report.flips[{index}].candidate_id",
                "does not reference an earlier candidate",
            )
    if published_candidates != len(report.candidates):
        raise _concolic_error("report.candidates", "contains an unpublished candidate")
    candidate_seeds = tuple(candidate.seed for candidate in report.candidates)
    if len(set(candidate_seeds)) != len(candidate_seeds):
        raise _concolic_error("report.candidates", "contains duplicate seeds")

    occurrence_keys = tuple(
        (
            decision.occurrence.va,
            decision.occurrence.seq,
            decision.occurrence.block_id,
            decision.occurrence.op_index,
            decision.occurrence.invocation,
            decision.occurrence.kind,
        )
        for decision in report.decisions
    )
    if len(set(occurrence_keys)) != len(occurrence_keys):
        raise _concolic_error(
            "report.decisions", "contains duplicate physical occurrences"
        )

    effects = (
        report.unmodelled_ops,
        report.opaque_ops,
        report.call_havocs,
        report.memory_havocs,
    )
    terminal_outcome = report.trace_outcome in {
        ConcolicTraceOutcome.RETURNED,
        ConcolicTraceOutcome.LEFT_FUNCTION,
    }
    if report.trace_complete and not terminal_outcome:
        raise _concolic_error(
            "report.trace_complete", "requires a terminal trace outcome"
        )
    if report.trace_exact:
        if (
            not report.trace_complete
            or not report.function.lift_complete
            or report.trace_reason is not ConcolicTraceReason.NONE
            or any(effects)
            or any(not decision.concrete for decision in report.decisions)
        ):
            raise _concolic_error(
                "report.trace_exact", "exact-trace evidence is inconsistent"
            )
        prefixes = tuple(decision.constraint_prefix for decision in report.decisions)
        if any(left >= right for left, right in zip(prefixes, prefixes[1:])):
            raise _concolic_error(
                "report.decisions", "constraint prefixes are not increasing"
            )
        expected_flip_ids = tuple(
            decision.decision_id
            for decision in report.decisions
            if decision.occurrence.kind is ConcolicDecisionKind.CONDITIONAL_BRANCH
        )
        if tuple(flip.decision_id for flip in report.flips) != expected_flip_ids:
            raise _concolic_error(
                "report.flips", "does not cover each conditional decision"
            )
    else:
        if (
            report.trace_reason is ConcolicTraceReason.NONE
            or report.flips
            or report.candidates
            or report.flip_attempts != 0
            or report.flip_budget_hit
            or report.candidate_budget_hit
        ):
            raise _concolic_error(
                "report.trace_exact", "inexact traces cannot publish flip evidence"
            )

        if report.trace_reason in {
            ConcolicTraceReason.INVALID_INITIAL_SEED,
            ConcolicTraceReason.NO_TRACE,
            ConcolicTraceReason.AMBIGUOUS_TRACE,
        }:
            if (
                report.trace_outcome is not None
                or report.trace_complete
                or report.executed_steps != 0
                or report.blocks
                or report.decisions
                or any(effects)
            ):
                raise _concolic_error(
                    "report.trace_reason",
                    "pre-trace reason carries execution evidence",
                )
        elif report.trace_reason is ConcolicTraceReason.UNSUPPORTED_EFFECTS:
            if not any(effects):
                raise _concolic_error(
                    "report.trace_reason",
                    "unsupported effects require a nonzero effect count",
                )
        elif report.trace_reason is ConcolicTraceReason.INCOMPLETE_CONCRETE_TRACE:
            if report.trace_complete or any(effects):
                raise _concolic_error(
                    "report.trace_reason",
                    "incomplete concrete trace evidence is inconsistent",
                )
        elif report.trace_reason is ConcolicTraceReason.INCOMPLETE_OUTCOME:
            if (
                report.trace_complete
                or report.trace_outcome is None
                or terminal_outcome
                or any(effects)
            ):
                raise _concolic_error(
                    "report.trace_reason",
                    "incomplete outcome evidence is inconsistent",
                )
        elif report.trace_reason is ConcolicTraceReason.INCOMPLETE_LIFT:
            if (
                not report.trace_complete
                or report.function.lift_complete
                or any(effects)
            ):
                raise _concolic_error(
                    "report.trace_reason",
                    "incomplete lift evidence is inconsistent",
                )
        elif report.trace_reason is ConcolicTraceReason.INVALID_DECISION_HISTORY:
            if (
                not report.trace_complete
                or not report.function.lift_complete
                or any(effects)
            ):
                raise _concolic_error(
                    "report.trace_reason",
                    "invalid decision history evidence is inconsistent",
                )


def _parse_concolic_report(
    value: object,
    *,
    expected_entry: int,
    expected_seed: tuple[ConcolicRegisterSeed, ...],
    expected_limits: ConcolicLimits,
) -> ConcolicReport:
    root = _concolic_object(value, "report")
    schema_version = _concolic_int(
        _concolic_field(root, "schema_version", "report"),
        "report.schema_version",
        maximum=(1 << 32) - 1,
    )
    if schema_version != 1:
        raise _concolic_error("report.schema_version", "expected 1")
    adapter = _concolic_string(
        _concolic_field(root, "adapter", "report"), "report.adapter"
    )
    if adapter != "lowir-concolic-v1":
        raise _concolic_error("report.adapter", "unexpected adapter")
    mode = _concolic_string(_concolic_field(root, "mode", "report"), "report.mode")
    if mode != "concolic":
        raise _concolic_error("report.mode", "unexpected mode")
    exhaustive = _concolic_bool(
        _concolic_field(root, "exhaustive", "report"), "report.exhaustive"
    )
    if exhaustive is not False:
        raise _concolic_error("report.exhaustive", "v1 must be false")
    ok = _concolic_bool(_concolic_field(root, "ok", "report"), "report.ok")
    if ok is False:
        error_code = _concolic_string(
            _concolic_field(root, "error_code", "report"),
            "report.error_code",
        )
        message = _concolic_string(
            _concolic_field(root, "error", "report"), "report.error"
        )
        raise NeverDError(f"LowIR concolic failed ({error_code}): {message}")

    image_value = _concolic_object(
        _concolic_field(root, "image", "report"), "report.image"
    )
    identity_status = _concolic_enum(
        _concolic_field(image_value, "identity_status", "report.image"),
        "report.image.identity_status",
        ConcolicImageIdentityStatus,
    )
    raw_identity_reason = _concolic_field(
        image_value, "identity_reason", "report.image"
    )
    identity_reason = (
        None
        if raw_identity_reason is None
        else _concolic_string(raw_identity_reason, "report.image.identity_reason")
    )
    raw_sha256 = _concolic_field(image_value, "sha256", "report.image")
    sha256_value = (
        None
        if raw_sha256 is None
        else _concolic_string(raw_sha256, "report.image.sha256")
    )
    if sha256_value is not None and re.fullmatch(r"[0-9a-f]{64}", sha256_value) is None:
        raise _concolic_error("report.image.sha256", "expected lowercase SHA-256")
    if identity_status is ConcolicImageIdentityStatus.EXACT_LOADED_SNAPSHOT:
        if identity_reason is not None or sha256_value is None:
            raise _concolic_error(
                "report.image", "exact identity requires sha256 and no reason"
            )
    elif identity_reason is None or sha256_value is not None:
        raise _concolic_error(
            "report.image", "unavailable identity requires a reason and null sha256"
        )
    image = ConcolicImage(
        format=_concolic_string(
            _concolic_field(image_value, "format", "report.image"),
            "report.image.format",
        ),
        arch=_concolic_string(
            _concolic_field(image_value, "arch", "report.image"),
            "report.image.arch",
        ),
        bits=_concolic_int(
            _concolic_field(image_value, "bits", "report.image"),
            "report.image.bits",
            maximum=(1 << 32) - 1,
        ),
        endianness=_concolic_enum(
            _concolic_field(image_value, "endianness", "report.image"),
            "report.image.endianness",
            ConcolicEndianness,
        ),
        base=_concolic_hex(
            _concolic_field(image_value, "base", "report.image"),
            "report.image.base",
            16,
        ),
        entry=_concolic_hex(
            _concolic_field(image_value, "entry", "report.image"),
            "report.image.entry",
            16,
        ),
        identity_status=identity_status,
        identity_reason=identity_reason,
        sha256=sha256_value,
    )
    if image.format not in {"PE", "ELF", "Mach-O"}:
        raise _concolic_error("report.image.format", "unsupported v1 format")
    if image.arch not in {"x86_64", "aarch64"}:
        raise _concolic_error("report.image.arch", "unsupported v1 architecture")
    if image.bits != 64:
        raise _concolic_error("report.image.bits", "v1 requires 64-bit images")
    if image.endianness is not ConcolicEndianness.LITTLE:
        raise _concolic_error("report.image.endianness", "v1 requires little endian")

    function_value = _concolic_object(
        _concolic_field(root, "function", "report"), "report.function"
    )
    function = ConcolicFunction(
        entry=_concolic_hex(
            _concolic_field(function_value, "entry", "report.function"),
            "report.function.entry",
            16,
        ),
        name=_concolic_string(
            _concolic_field(function_value, "name", "report.function"),
            "report.function.name",
        ),
        lift_complete=_concolic_bool(
            _concolic_field(function_value, "lift_complete", "report.function"),
            "report.function.lift_complete",
        ),
    )
    if function.entry != expected_entry:
        raise _concolic_error("report.function.entry", "does not match the request")

    limits_value = _concolic_object(
        _concolic_field(root, "limits", "report"), "report.limits"
    )
    limit_names_32 = (
        "max_steps",
        "max_block_visits",
        "max_loop_iterations",
        "max_flip_attempts",
        "max_candidates",
    )
    parsed_limits = {
        name: _concolic_int(
            _concolic_field(limits_value, name, "report.limits"),
            f"report.limits.{name}",
            maximum=(1 << 32) - 1,
        )
        for name in limit_names_32
    }
    for name in (
        "solver_max_conflicts",
        "solver_max_propagations",
        "solver_max_watch_visits",
        "solver_max_width",
        "solver_max_gates",
    ):
        parsed_limits[name] = _concolic_int(
            _concolic_field(limits_value, name, "report.limits"),
            f"report.limits.{name}",
        )
    limits = ConcolicLimits(
        max_steps=parsed_limits["max_steps"],
        max_block_visits=parsed_limits["max_block_visits"],
        max_loop_iterations=parsed_limits["max_loop_iterations"],
        max_flip_attempts=parsed_limits["max_flip_attempts"],
        max_candidates=parsed_limits["max_candidates"],
        solver_max_conflicts=parsed_limits["solver_max_conflicts"],
        solver_max_propagations=parsed_limits["solver_max_propagations"],
        solver_max_watch_visits=parsed_limits["solver_max_watch_visits"],
        solver_max_width=parsed_limits["solver_max_width"],
        solver_max_gates=parsed_limits["solver_max_gates"],
    )
    if limits != expected_limits:
        raise _concolic_error(
            "report.limits", "does not match effective request limits"
        )

    initial_seed = _parse_concolic_seeds(
        _concolic_field(root, "initial_seed", "report"), "report.initial_seed"
    )
    if initial_seed != expected_seed:
        raise _concolic_error("report.initial_seed", "does not match the request")
    trace_outcome_value = _concolic_field(root, "trace_outcome", "report")
    trace_outcome = (
        None
        if trace_outcome_value is None
        else _concolic_enum(
            trace_outcome_value, "report.trace_outcome", ConcolicTraceOutcome
        )
    )

    decisions_value = _concolic_array(
        _concolic_field(root, "decisions", "report"), "report.decisions"
    )
    decisions: list[ConcolicDecision] = []
    for index, raw_decision in enumerate(decisions_value):
        path = f"report.decisions[{index}]"
        item = _concolic_object(raw_decision, path)
        decision_id = _concolic_int(
            _concolic_field(item, "decision_id", path), f"{path}.decision_id"
        )
        if decision_id != index:
            raise _concolic_error(f"{path}.decision_id", "must equal its array index")
        decisions.append(
            ConcolicDecision(
                decision_id=decision_id,
                occurrence=_parse_concolic_occurrence(
                    _concolic_field(item, "occurrence", path),
                    f"{path}.occurrence",
                ),
                taken=_concolic_bool(
                    _concolic_field(item, "taken", path), f"{path}.taken"
                ),
                constraint_prefix=_concolic_int(
                    _concolic_field(item, "constraint_prefix", path),
                    f"{path}.constraint_prefix",
                ),
                concrete=_concolic_bool(
                    _concolic_field(item, "concrete", path), f"{path}.concrete"
                ),
            )
        )

    candidates_value = _concolic_array(
        _concolic_field(root, "candidates", "report"), "report.candidates"
    )
    candidates: list[ConcolicCandidate] = []
    expected_shape = tuple((seed.offset, seed.bytes) for seed in initial_seed)
    for index, raw_candidate in enumerate(candidates_value):
        path = f"report.candidates[{index}]"
        item = _concolic_object(raw_candidate, path)
        candidate_record_id = _concolic_int(
            _concolic_field(item, "candidate_id", path), f"{path}.candidate_id"
        )
        if candidate_record_id != index:
            raise _concolic_error(f"{path}.candidate_id", "must equal its array index")
        seed = _parse_concolic_seeds(
            _concolic_field(item, "seed", path), f"{path}.seed"
        )
        if tuple((part.offset, part.bytes) for part in seed) != expected_shape:
            raise _concolic_error(f"{path}.seed", "shape differs from initial_seed")
        candidates.append(
            ConcolicCandidate(candidate_id=candidate_record_id, seed=seed)
        )

    flips_value = _concolic_array(
        _concolic_field(root, "flips", "report"), "report.flips"
    )
    flips: list[ConcolicFlip] = []
    for index, raw_flip in enumerate(flips_value):
        path = f"report.flips[{index}]"
        item = _concolic_object(raw_flip, path)
        decision_id = _concolic_int(
            _concolic_field(item, "decision_id", path), f"{path}.decision_id"
        )
        if decision_id >= len(decisions):
            raise _concolic_error(f"{path}.decision_id", "dangling decision ID")
        raw_candidate_id = _concolic_field(item, "candidate_id", path)
        flip_candidate_id = (
            None
            if raw_candidate_id is None
            else _concolic_int(raw_candidate_id, f"{path}.candidate_id")
        )
        if flip_candidate_id is not None and flip_candidate_id >= len(candidates):
            raise _concolic_error(f"{path}.candidate_id", "dangling candidate ID")
        flip = ConcolicFlip(
            decision_id=decision_id,
            occurrence=_parse_concolic_occurrence(
                _concolic_field(item, "occurrence", path), f"{path}.occurrence"
            ),
            original_taken=_concolic_bool(
                _concolic_field(item, "original_taken", path),
                f"{path}.original_taken",
            ),
            constraint_prefix=_concolic_int(
                _concolic_field(item, "constraint_prefix", path),
                f"{path}.constraint_prefix",
            ),
            status=_concolic_enum(
                _concolic_field(item, "status", path),
                f"{path}.status",
                ConcolicFlipStatus,
            ),
            solver_status=_concolic_enum(
                _concolic_field(item, "solver_status", path),
                f"{path}.solver_status",
                ConcolicSolverStatus,
            ),
            encoding_error=_concolic_enum(
                _concolic_field(item, "encoding_error", path),
                f"{path}.encoding_error",
                ConcolicEncodingError,
            ),
            projection_status=_concolic_enum(
                _concolic_field(item, "projection_status", path),
                f"{path}.projection_status",
                ConcolicProjectionStatus,
            ),
            projection_reason=_concolic_enum(
                _concolic_field(item, "projection_reason", path),
                f"{path}.projection_reason",
                ConcolicProjectionReason,
            ),
            replay_status=_concolic_enum(
                _concolic_field(item, "replay_status", path),
                f"{path}.replay_status",
                ConcolicReplayStatus,
            ),
            replay_reason=_concolic_enum(
                _concolic_field(item, "replay_reason", path),
                f"{path}.replay_reason",
                ConcolicReplayReason,
            ),
            candidate_id=flip_candidate_id,
        )
        decision = decisions[decision_id]
        if (
            flip.occurrence != decision.occurrence
            or flip.original_taken is not decision.taken
            or flip.constraint_prefix != decision.constraint_prefix
        ):
            raise _concolic_error(path, "does not match its referenced decision")
        _validate_concolic_flip_evidence(flip, path)
        flips.append(flip)

    blocks = tuple(
        _concolic_int(
            raw_block,
            f"report.blocks[{index}]",
            maximum=(1 << 31) - 1,
        )
        for index, raw_block in enumerate(
            _concolic_array(_concolic_field(root, "blocks", "report"), "report.blocks")
        )
    )
    report = ConcolicReport(
        schema_version=schema_version,
        adapter=adapter,
        mode=mode,
        ok=ok,
        exhaustive=exhaustive,
        image=image,
        function=function,
        limits=limits,
        initial_seed=initial_seed,
        trace_outcome=trace_outcome,
        trace_complete=_concolic_bool(
            _concolic_field(root, "trace_complete", "report"),
            "report.trace_complete",
        ),
        trace_exact=_concolic_bool(
            _concolic_field(root, "trace_exact", "report"), "report.trace_exact"
        ),
        trace_reason=_concolic_enum(
            _concolic_field(root, "trace_reason", "report"),
            "report.trace_reason",
            ConcolicTraceReason,
        ),
        executed_steps=_concolic_int(
            _concolic_field(root, "executed_steps", "report"),
            "report.executed_steps",
        ),
        unmodelled_ops=_concolic_int(
            _concolic_field(root, "unmodelled_ops", "report"),
            "report.unmodelled_ops",
            maximum=(1 << 32) - 1,
        ),
        opaque_ops=_concolic_int(
            _concolic_field(root, "opaque_ops", "report"),
            "report.opaque_ops",
            maximum=(1 << 32) - 1,
        ),
        call_havocs=_concolic_int(
            _concolic_field(root, "call_havocs", "report"),
            "report.call_havocs",
            maximum=(1 << 32) - 1,
        ),
        memory_havocs=_concolic_int(
            _concolic_field(root, "memory_havocs", "report"),
            "report.memory_havocs",
            maximum=(1 << 32) - 1,
        ),
        flip_attempts=_concolic_int(
            _concolic_field(root, "flip_attempts", "report"),
            "report.flip_attempts",
            maximum=(1 << 32) - 1,
        ),
        flip_budget_hit=_concolic_bool(
            _concolic_field(root, "flip_budget_hit", "report"),
            "report.flip_budget_hit",
        ),
        candidate_budget_hit=_concolic_bool(
            _concolic_field(root, "candidate_budget_hit", "report"),
            "report.candidate_budget_hit",
        ),
        blocks=blocks,
        decisions=tuple(decisions),
        flips=tuple(flips),
        candidates=tuple(candidates),
    )
    _validate_concolic_report_consistency(report)
    return report


class ExpressionSyntaxError(NeverDError):
    """An expression could not be read.

    ``offset`` is the character reading stopped at, which is what lets a caller
    point at the mistake rather than repeat the whole line back.
    """

    def __init__(self, message: str, offset: int) -> None:
        super().__init__(message)
        self.offset = offset


class LLVMIRSyntaxError(NeverDError):
    """Textual LLVM IR could not be parsed at ``line`` and ``column``."""

    def __init__(self, message: str, line: int, column: int) -> None:
        super().__init__(message)
        self.line = line
        self.column = column


@dataclass(frozen=True, slots=True)
class SimplifyResult:
    """What the engine made of one expression.

    ``outcome`` is the part worth reading when ``changed`` is false: leaving an
    expression alone because nothing shorter exists and leaving it alone because
    the budget ran out are different answers, and only the second is worth
    retrying.
    """

    input: str
    output: str
    changed: bool
    cost_before: int
    cost_after: int
    inputs: int
    work: int
    outcome: SimplifyOutcome
    evidence: SimplifyEvidence

    @property
    def saved(self) -> int:
        """Reading cost the rewrite removed.  Zero when nothing was rewritten."""

        return self.cost_before - self.cost_after


@dataclass(frozen=True, slots=True)
class SynthesisResult:
    """A candidate, the proof decision behind it, and both work ledgers."""

    input: str
    output: str
    changed: bool
    cost_before: int
    cost_after: int
    inputs: int
    candidate_cost: int
    outcome: SynthesisOutcome
    proof_status: ProofStatus
    search_work: int
    proof_queries: int
    proof_conflicts: int
    proof_propagations: int
    proof_watch_visits: int
    counterexample: object | None

    @property
    def saved(self) -> int:
        return self.cost_before - self.cost_after


@dataclass(frozen=True, slots=True)
class LLVMOptimizationResult:
    """Committed LLVM module and convergence/proof telemetry."""

    output_ir: str
    changed: bool
    stop: OptimizationStop
    functions_visited: int
    rounds: int
    semantic_rewrites: int
    search_work: int
    proof_queries: int
    proof_conflicts: int
    proof_propagations: int
    proof_watch_visits: int


class TranslationError(NeverDError):
    """A typed x86-64 to AArch64 object translation failure."""

    def __init__(self, code: TranslationErrorCode, detail: str) -> None:
        self.code = code
        self.detail = detail
        category = code.name.lower().replace("_", "-")
        super().__init__(f"{category}: {detail}" if detail else category)


@dataclass(frozen=True, slots=True)
class X86_64ToAArch64ObjectResult:
    """One audited relocatable object and its translation telemetry.

    The API does not link, load, publish, dispatch, execute, or debug these
    bytes.  Every byte and string is Python-owned before this value is returned.
    """

    object_bytes: bytes
    object_format: TranslationObjectFormat
    guest_entry_pc: int
    guest_instruction_count: int
    guest_byte_count: int
    executable_generation: int
    block_ir_symbol: str
    block_object_symbol: str
    host_triple: str
    host_cpu: str
    host_target_identity: str
    runtime_registry_identity: str
    request_cache_key: str
    artifact_cache_key: str
    translation_cache_identity: str
    semantic_changed: bool
    semantic_rewrites: int
    semantic_search_work: int
    semantic_proof_queries: int
    semantic_proof_conflicts: int
    semantic_proof_propagations: int
    semantic_proof_watch_visits: int
    semantic_function_pass_invocations: int
    semantic_max_rounds: int
    semantic_stop: TranslationSemanticStop
    semantic_proof: TranslationProofStatus
    llvm_optimization_pipeline_ran: bool
    object_cache_identity_version: int
    object_pipeline_schema_version: int


_HOST: HostAPI | None = None


def _simplify_host() -> HostAPI:
    """The loaded library, opened once.

    Simplification takes no session -- it works on a string -- so it cannot
    borrow the one a plugin was handed, and opening the library again for every
    expression would pay the loader's cost once per line of a corpus.
    """

    global _HOST
    if _HOST is None:
        _HOST = HostAPI()
    return _HOST


def simplify_expression(
    expression: str,
    *,
    width: int = 32,
    deep: bool = True,
    max_atoms: int = 0,
    max_work: int = 0,
    exhaustive: bool = False,
    verify_samples: int = 0,
    allow_growth: bool = False,
    host: HostAPI | None = None,
) -> SimplifyResult:
    """Simplify one bitvector expression written in the engine's infix syntax.

    ``width`` is what every leaf without a ``#bits`` suffix is created at.
    ``deep`` walks into the subterms a single measurement has to treat as
    opaque, which is what layered obfuscation needs.  Every other argument left
    at its default keeps the engine's own: ``max_atoms`` bounds how many inputs
    one measurement spans (the cost is 2^n), ``max_work`` bounds the layered
    walk and combinatorial polynomial search.  ``exhaustive`` selects the
    native unlimited MBA policy and removes the native parser's nesting and
    width policy ceilings; physical memory and IR representation limits still
    apply.

    ``host`` names the library to work through.  It defaults to the NeverD this
    is running inside, but simplification needs no session and no loaded binary,
    so passing a library opened by hand is enough to use this from an ordinary
    script.

    Raises ``ExpressionSyntaxError`` when the expression cannot be read.
    """

    text = _utf8_argument("expression", expression, allow_empty=False)
    use_exhaustive = _boolean("exhaustive", exhaustive)

    options = NeverDSimplifyOptions()
    options.struct_size = ctypes.sizeof(NeverDSimplifyOptions)
    options.width = _unsigned("width", width, 32)
    options.shallow = 0 if _boolean("deep", deep) else 1
    options.max_atoms = _unsigned("max_atoms", max_atoms, 32)
    options.max_work = (
        ctypes.c_size_t(-1).value
        if use_exhaustive
        else _unsigned("max_work", max_work, 64)
    )
    options.verify_samples = _unsigned("verify_samples", verify_samples, 32)
    options.allow_growth = 1 if _boolean("allow_growth", allow_growth) else 0
    options.exhaustive = 1 if use_exhaustive else 0

    result = NeverDSimplifyResult()
    result.struct_size = ctypes.sizeof(NeverDSimplifyResult)

    library = host if host is not None else _simplify_host()
    status = library.call(
        "neverd_simplify_expr",
        text,
        ctypes.byref(options),
        ctypes.byref(result),
    )
    try:
        if status != 0:
            raise NeverDError("NeverD refused the simplification request")
        if not result.ok:
            raise ExpressionSyntaxError(
                (result.error or b"").decode("utf-8", errors="replace"),
                int(result.error_offset),
            )
        return SimplifyResult(
            input=(result.input or b"").decode("utf-8", errors="strict"),
            output=(result.output or b"").decode("utf-8", errors="strict"),
            changed=bool(result.changed),
            cost_before=int(result.cost_before),
            cost_after=int(result.cost_after),
            inputs=int(result.inputs),
            work=int(result.work),
            outcome=SimplifyOutcome(int(result.outcome)),
            evidence=SimplifyEvidence(int(result.evidence)),
        )
    finally:
        # The library owns the strings whatever happened, including the error
        # path, so this cannot be conditional on having read them.
        library.call("neverd_simplify_result_dispose", ctypes.byref(result))


def synthesize_expression(
    expression: str,
    *,
    width: int = 32,
    max_cost: int = 0,
    max_samples: int = 0,
    verify_samples: int = 0,
    max_work: int = 0,
    max_leaves: int = 0,
    max_constants: int = 0,
    stochastic_slots: int = 0,
    stochastic_restarts: int = 0,
    stochastic_iterations: int = 0,
    solver_max_conflicts: int = 0,
    solver_max_propagations: int = 0,
    solver_max_watch_visits: int = 0,
    exhaustive: bool = False,
    host: HostAPI | None = None,
) -> SynthesisResult:
    """Search for a shorter expression and commit only a proven equivalent.

    Search and proof budgets remain separate.  ``exhaustive=True`` is the
    explicit opt-in that removes their engine-side ceilings together with the
    native parser's nesting and width policy ceilings.  No additional Python
    limit is added; physical memory and IR representation limits still apply.
    """

    text = _utf8_argument("expression", expression, allow_empty=False)
    options = NeverDSynthesizeOptions()
    options.struct_size = ctypes.sizeof(NeverDSynthesizeOptions)
    options.width = _unsigned("width", width, 32)
    options.max_cost = _size_t("max_cost", max_cost)
    options.max_samples = _size_t("max_samples", max_samples)
    options.verify_samples = _unsigned("verify_samples", verify_samples, 32)
    options.max_work = _size_t("max_work", max_work)
    options.max_leaves = _unsigned("max_leaves", max_leaves, 32)
    options.max_constants = _unsigned("max_constants", max_constants, 32)
    options.stochastic_slots = _unsigned("stochastic_slots", stochastic_slots, 32)
    options.stochastic_restarts = _unsigned(
        "stochastic_restarts", stochastic_restarts, 32
    )
    options.stochastic_iterations = _size_t(
        "stochastic_iterations", stochastic_iterations
    )
    options.solver_max_conflicts = _unsigned(
        "solver_max_conflicts", solver_max_conflicts, 64
    )
    options.solver_max_propagations = _unsigned(
        "solver_max_propagations", solver_max_propagations, 64
    )
    options.solver_max_watch_visits = _unsigned(
        "solver_max_watch_visits", solver_max_watch_visits, 64
    )
    options.exhaustive = 1 if _boolean("exhaustive", exhaustive) else 0

    result = NeverDSynthesizeResult()
    result.struct_size = ctypes.sizeof(NeverDSynthesizeResult)
    library = host if host is not None else _simplify_host()
    status = library.call(
        "neverd_synthesize_expr", text, ctypes.byref(options), ctypes.byref(result)
    )
    try:
        if status != 0:
            raise NeverDError("NeverD refused the synthesis request")
        if not result.ok:
            raise ExpressionSyntaxError(
                (result.error or b"").decode("utf-8", errors="replace"),
                int(result.error_offset),
            )
        counterexample = None
        if result.counterexample_json:
            try:
                counterexample = json.loads(
                    result.counterexample_json.decode("utf-8", errors="strict")
                )
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                raise NeverDError(
                    "NeverD returned an invalid synthesis counterexample"
                ) from error
        return SynthesisResult(
            input=(result.input or b"").decode("utf-8", errors="strict"),
            output=(result.output or b"").decode("utf-8", errors="strict"),
            changed=bool(result.changed),
            cost_before=int(result.cost_before),
            cost_after=int(result.cost_after),
            inputs=int(result.inputs),
            candidate_cost=int(result.candidate_cost),
            outcome=SynthesisOutcome(int(result.outcome)),
            proof_status=ProofStatus(int(result.proof_status)),
            search_work=int(result.search_work),
            proof_queries=int(result.proof_queries),
            proof_conflicts=int(result.proof_conflicts),
            proof_propagations=int(result.proof_propagations),
            proof_watch_visits=int(result.proof_watch_visits),
            counterexample=counterexample,
        )
    finally:
        library.call("neverd_synthesize_result_dispose", ctypes.byref(result))


def optimize_llvm_ir(
    ir: str,
    *,
    mode: OptimizationMode | int = OptimizationMode.DEFAULT,
    llvm_level: LLVMOptimizationLevel | int = LLVMOptimizationLevel.DEFAULT,
    max_rounds: int = 0,
    enable_synthesis: bool = False,
    synthesis_max_cost: int = 0,
    synthesis_max_samples: int = 0,
    synthesis_verify_samples: int = 0,
    synthesis_max_work: int = 0,
    synthesis_max_leaves: int = 0,
    synthesis_max_constants: int = 0,
    synthesis_stochastic_slots: int = 0,
    synthesis_stochastic_restarts: int = 0,
    synthesis_stochastic_iterations: int = 0,
    solver_max_conflicts: int = 0,
    solver_max_propagations: int = 0,
    solver_max_watch_visits: int = 0,
    exhaustive: bool = False,
    host: HostAPI | None = None,
) -> LLVMOptimizationResult:
    """Run NeverD's transactional semantic and standard LLVM pipeline.

    Synthesis policy requires ``enable_synthesis=True`` and is unavailable in
    conservative mode, whose contract excludes semantic value rewriting.
    """

    text = _utf8_argument("LLVM IR", ir, allow_empty=False)
    options = NeverDOptimizeLLVMOptions()
    options.struct_size = ctypes.sizeof(NeverDOptimizeLLVMOptions)
    mode_value = _enum_value("optimization mode", mode, OptimizationMode)
    options.mode = mode_value
    options.llvm_level = _enum_value(
        "LLVM optimization level", llvm_level, LLVMOptimizationLevel
    )
    options.max_rounds = _unsigned("max_rounds", max_rounds, 32)
    synthesis_enabled = _boolean("enable_synthesis", enable_synthesis)
    options.enable_synthesis = 1 if synthesis_enabled else 0
    options.synthesis_max_cost = _size_t("synthesis_max_cost", synthesis_max_cost)
    options.synthesis_max_samples = _size_t(
        "synthesis_max_samples", synthesis_max_samples
    )
    options.synthesis_verify_samples = _unsigned(
        "synthesis_verify_samples", synthesis_verify_samples, 32
    )
    options.synthesis_max_work = _size_t("synthesis_max_work", synthesis_max_work)
    options.synthesis_max_leaves = _unsigned(
        "synthesis_max_leaves", synthesis_max_leaves, 32
    )
    options.synthesis_max_constants = _unsigned(
        "synthesis_max_constants", synthesis_max_constants, 32
    )
    options.synthesis_stochastic_slots = _unsigned(
        "synthesis_stochastic_slots", synthesis_stochastic_slots, 32
    )
    options.synthesis_stochastic_restarts = _unsigned(
        "synthesis_stochastic_restarts", synthesis_stochastic_restarts, 32
    )
    options.synthesis_stochastic_iterations = _size_t(
        "synthesis_stochastic_iterations", synthesis_stochastic_iterations
    )
    options.solver_max_conflicts = _unsigned(
        "solver_max_conflicts", solver_max_conflicts, 64
    )
    options.solver_max_propagations = _unsigned(
        "solver_max_propagations", solver_max_propagations, 64
    )
    options.solver_max_watch_visits = _unsigned(
        "solver_max_watch_visits", solver_max_watch_visits, 64
    )
    options.exhaustive = 1 if _boolean("exhaustive", exhaustive) else 0
    if synthesis_enabled and mode_value == int(OptimizationMode.CONSERVATIVE):
        raise ValueError("enable_synthesis is incompatible with conservative mode")
    if not synthesis_enabled and any(
        (
            options.synthesis_max_cost,
            options.synthesis_max_samples,
            options.synthesis_verify_samples,
            options.synthesis_max_work,
            options.synthesis_max_leaves,
            options.synthesis_max_constants,
            options.synthesis_stochastic_slots,
            options.synthesis_stochastic_restarts,
            options.synthesis_stochastic_iterations,
            options.solver_max_conflicts,
            options.solver_max_propagations,
            options.solver_max_watch_visits,
        )
    ):
        raise ValueError("synthesis and solver options require enable_synthesis")

    result = NeverDOptimizeLLVMResult()
    result.struct_size = ctypes.sizeof(NeverDOptimizeLLVMResult)
    library = host if host is not None else _simplify_host()
    status = library.call(
        "neverd_optimize_llvm_ir", text, ctypes.byref(options), ctypes.byref(result)
    )
    try:
        if status != 0:
            raise NeverDError("NeverD refused the LLVM optimization request")
        if not result.ok:
            message = (result.error or b"").decode("utf-8", errors="replace")
            stop = OptimizationStop(int(result.stop))
            if stop is OptimizationStop.INPUT_INVALID and result.error_line:
                raise LLVMIRSyntaxError(
                    message or "invalid LLVM IR",
                    int(result.error_line),
                    int(result.error_column),
                )
            raise NeverDError(message or f"LLVM optimization stopped: {stop.name}")
        if not result.output_ir:
            raise NeverDError("NeverD returned no committed LLVM IR")
        return LLVMOptimizationResult(
            output_ir=result.output_ir.decode("utf-8", errors="strict"),
            changed=bool(result.changed),
            stop=OptimizationStop(int(result.stop)),
            functions_visited=int(result.functions_visited),
            rounds=int(result.rounds),
            semantic_rewrites=int(result.semantic_rewrites),
            search_work=int(result.search_work),
            proof_queries=int(result.proof_queries),
            proof_conflicts=int(result.proof_conflicts),
            proof_propagations=int(result.proof_propagations),
            proof_watch_visits=int(result.proof_watch_visits),
        )
    finally:
        library.call("neverd_optimize_llvm_ir_result_dispose", ctypes.byref(result))


def _translation_failure(code_value: int, message: bytes | None) -> TranslationError:
    try:
        code = TranslationErrorCode(code_value)
    except ValueError:
        return TranslationError(
            TranslationErrorCode.INTERNAL_FAILURE,
            f"NeverD returned unknown translation error code {code_value}",
        )
    if code is TranslationErrorCode.NONE:
        return TranslationError(
            TranslationErrorCode.INTERNAL_FAILURE,
            "NeverD reported translation failure without an error category",
        )
    detail = (message or b"").decode("utf-8", errors="replace")
    return TranslationError(code, detail or "translation failed")


def _translation_owned_utf8(value: bytes | None, field: str) -> str:
    if value is None:
        raise TranslationError(
            TranslationErrorCode.INTERNAL_FAILURE,
            f"NeverD returned no {field}",
        )
    try:
        return bytes(value).decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise TranslationError(
            TranslationErrorCode.INTERNAL_FAILURE,
            f"NeverD returned invalid UTF-8 in {field}",
        ) from error


def translate_x86_64_block_to_aarch64_object(
    guest_bytes: bytes | bytearray | memoryview,
    *,
    entry_pc: int,
    object_format: TranslationObjectFormat | int,
    executable_generation: int = 0,
    host: HostAPI | None = None,
) -> X86_64ToAArch64ObjectResult:
    """Compile one x86-64 v1 scalar-register block into an AArch64 object.

    The published fail-closed subset accepts only canonical encodings without
    legacy prefixes: REX.W full-width GPR ``MOV``, ``ADD``/``SUB``, and
    ``AND``/``OR``/``XOR`` forms over supported register/immediate LowIR shapes;
    full-width register-only ``CMP`` ``39/3B`` and register/immediate ``CMP``
    ``81/7``, ``83/7``, and ``3D``; and full-width register-only ``TEST`` ``85``
    plus register/immediate ``TEST`` ``F7/0`` and ``A9``. Arithmetic forms retain
    their scalar flag computations; logical and ``TEST`` forms compute their
    architecturally defined flags while preserving ``AF`` in the NeverD state
    model.
    Canonical ``C3`` ``RET`` or ``C2 iw`` ``RET imm16`` terminates a return
    block, and direct-relative ``EB cb`` or ``E9 cd`` ``JMP`` terminates a
    direct-branch block.  The published lowering schema is 9.  Canonical,
    legacy-prefix-free traditional Jcc comprises ``JO``/``JNO`` short
    ``70/71 cb`` or near ``0F 80/81 cd``, ``JB``/``JAE`` ``72/73 cb`` or
    ``0F 82/83 cd``, ``JE``/``JNE`` ``74/75 cb`` or ``0F 84/85 cd``,
    ``JBE``/``JA`` ``76/77 cb`` or ``0F 86/87 cd``, ``JS``/``JNS``
    ``78/79 cb`` or ``0F 88/89 cd``, ``JP``/``JNP`` ``7A/7B cb`` or
    ``0F 8A/8B cd``, ``JL``/``JGE`` ``7C/7D cb`` or ``0F 8C/8D cd``, and
    ``JLE``/``JG`` ``7E/7F cb`` or ``0F 8E/8F cd``.
    ``JRCXZ``/``JECXZ``/``JCXZ`` and ``LOOP``/``LOOPE``/``LOOPNE`` remain
    unpublished and fail closed. Reserved ``F7 /1``, ordinary guest-memory
    operations, partial-register forms, legacy prefixes, semantically redundant
    REX extension bits, any instruction or encoding outside that exact subset,
    all other control flow, and unimplemented LowIR are rejected.
    ``guest_bytes`` must contain exactly one block, including its terminating
    return, direct jump, or published Jcc branch; trailing
    bytes are rejected.
    ``object_format`` must be ``ELF`` or ``MACHO``.  Local argument validation
    raises ``TypeError`` or ``ValueError``; native translation failures raise
    ``TranslationError`` carrying a ``TranslationErrorCode``.  The current v1
    contract produces only audited relocatable objects: this function does not
    link, load, publish, dispatch, execute, or debug the result.
    """

    if not isinstance(guest_bytes, (bytes, bytearray, memoryview)):
        raise TypeError("guest_bytes must be a bytes-like object")
    code = bytes(guest_bytes)
    if not code:
        raise ValueError("guest_bytes must not be empty")
    if len(code) > ctypes.c_size_t(-1).value:
        raise ValueError("guest_bytes does not fit size_t")

    guest_entry = _unsigned("entry_pc", entry_pc, 64)
    generation = _unsigned("executable_generation", executable_generation, 64)
    format_value = _enum_value(
        "AArch64 object format", object_format, TranslationObjectFormat
    )
    if format_value not in (
        int(TranslationObjectFormat.ELF),
        int(TranslationObjectFormat.MACHO),
    ):
        raise ValueError("AArch64 object format must be ELF or MACHO")
    if guest_entry > (1 << 64) - 1 - len(code):
        raise ValueError("guest byte address range must not wrap uint64")

    native_code = (ctypes.c_ubyte * len(code)).from_buffer_copy(code)
    request = NeverDTranslateObjectRequestV1()
    request.struct_size = ctypes.sizeof(NeverDTranslateObjectRequestV1)
    request.guest_bytes = ctypes.cast(native_code, ctypes.POINTER(ctypes.c_ubyte))
    request.guest_bytes_size = len(code)
    request.entry_pc = guest_entry
    request.executable_generation = generation
    request.object_format = format_value
    request.reserved = 0

    result = NeverDTranslateObjectResultV1()
    result.struct_size = ctypes.sizeof(NeverDTranslateObjectResultV1)
    library = host if host is not None else _simplify_host()
    translate = library.function(
        "neverd_translate_x86_64_block_to_aarch64_object_v1"
    )
    dispose = library.function("neverd_translate_object_result_dispose")
    try:
        status = translate(
            ctypes.byref(request),
            ctypes.byref(result),
        )
        if status != 0:
            raise TranslationError(
                TranslationErrorCode.INTERNAL_FAILURE,
                "NeverD refused the translation ABI request",
            )
        if not result.ok:
            raise _translation_failure(int(result.error_code), result.error_message)
        if int(result.error_code) != int(TranslationErrorCode.NONE):
            raise TranslationError(
                TranslationErrorCode.INTERNAL_FAILURE,
                "NeverD returned a successful object with a failure category",
            )

        try:
            returned_format = TranslationObjectFormat(int(result.object_format))
            semantic_stop = TranslationSemanticStop(int(result.semantic_stop))
            semantic_proof = TranslationProofStatus(int(result.semantic_proof))
        except ValueError as error:
            raise TranslationError(
                TranslationErrorCode.INTERNAL_FAILURE,
                "NeverD returned an unknown translation result enum value",
            ) from error
        if returned_format is not TranslationObjectFormat(format_value):
            raise TranslationError(
                TranslationErrorCode.INTERNAL_FAILURE,
                "NeverD returned an object in a format that was not requested",
            )
        if int(result.guest_entry_pc) != guest_entry:
            raise TranslationError(
                TranslationErrorCode.INTERNAL_FAILURE,
                "NeverD returned an object for a guest entry that was not requested",
            )
        if int(result.executable_generation) != generation:
            raise TranslationError(
                TranslationErrorCode.INTERNAL_FAILURE,
                "NeverD returned an object for an executable generation that was "
                "not requested",
            )
        if int(result.guest_byte_count) != len(code):
            raise TranslationError(
                TranslationErrorCode.INTERNAL_FAILURE,
                "NeverD returned a translation that did not consume the complete "
                "guest block",
            )

        object_size = int(result.object_size)
        if object_size == 0 or not result.object_bytes:
            raise TranslationError(
                TranslationErrorCode.INTERNAL_FAILURE,
                "NeverD returned no relocatable object bytes",
            )
        if object_size > sys.maxsize:
            raise TranslationError(
                TranslationErrorCode.INTERNAL_FAILURE,
                "NeverD returned an impossibly large relocatable object",
            )

        # Copy every owned native allocation before the result disposer runs.
        object_bytes = bytes(ctypes.string_at(result.object_bytes, object_size))
        block_ir_symbol = _translation_owned_utf8(
            result.block_ir_symbol, "block IR symbol"
        )
        block_object_symbol = _translation_owned_utf8(
            result.block_object_symbol, "block object symbol"
        )
        host_triple = _translation_owned_utf8(result.host_triple, "host triple")
        host_cpu = _translation_owned_utf8(result.host_cpu, "host CPU")
        host_target_identity = _translation_owned_utf8(
            result.host_target_identity, "host target identity"
        )
        runtime_registry_identity = _translation_owned_utf8(
            result.runtime_registry_identity, "runtime registry identity"
        )
        request_cache_key = _translation_owned_utf8(
            result.request_cache_key, "request cache key"
        )
        artifact_cache_key = _translation_owned_utf8(
            result.artifact_cache_key, "artifact cache key"
        )
        translation_cache_identity = _translation_owned_utf8(
            result.translation_cache_identity, "translation cache identity"
        )

        return X86_64ToAArch64ObjectResult(
            object_bytes=object_bytes,
            object_format=returned_format,
            guest_entry_pc=int(result.guest_entry_pc),
            guest_instruction_count=int(result.guest_instruction_count),
            guest_byte_count=int(result.guest_byte_count),
            executable_generation=int(result.executable_generation),
            block_ir_symbol=block_ir_symbol,
            block_object_symbol=block_object_symbol,
            host_triple=host_triple,
            host_cpu=host_cpu,
            host_target_identity=host_target_identity,
            runtime_registry_identity=runtime_registry_identity,
            request_cache_key=request_cache_key,
            artifact_cache_key=artifact_cache_key,
            translation_cache_identity=translation_cache_identity,
            semantic_changed=bool(result.semantic_changed),
            semantic_rewrites=int(result.semantic_rewrites),
            semantic_search_work=int(result.semantic_search_work),
            semantic_proof_queries=int(result.semantic_proof_queries),
            semantic_proof_conflicts=int(result.semantic_proof_conflicts),
            semantic_proof_propagations=int(result.semantic_proof_propagations),
            semantic_proof_watch_visits=int(result.semantic_proof_watch_visits),
            semantic_function_pass_invocations=int(
                result.semantic_function_pass_invocations
            ),
            semantic_max_rounds=int(result.semantic_max_rounds),
            semantic_stop=semantic_stop,
            semantic_proof=semantic_proof,
            llvm_optimization_pipeline_ran=bool(result.llvm_optimization_pipeline_ran),
            object_cache_identity_version=int(result.object_cache_identity_version),
            object_pipeline_schema_version=int(result.object_pipeline_schema_version),
        )
    finally:
        dispose(ctypes.byref(result))


__all__ = [
    "ConcolicCandidate",
    "ConcolicDecision",
    "ConcolicDecisionKind",
    "ConcolicDecisionOccurrence",
    "ConcolicEncodingError",
    "ConcolicEndianness",
    "ConcolicFlip",
    "ConcolicFlipStatus",
    "ConcolicFunction",
    "ConcolicImage",
    "ConcolicImageIdentityStatus",
    "ConcolicLimits",
    "ConcolicProjectionReason",
    "ConcolicProjectionStatus",
    "ConcolicRegisterSeed",
    "ConcolicReplayReason",
    "ConcolicReplayStatus",
    "ConcolicReport",
    "ConcolicSolverStatus",
    "ConcolicTraceOutcome",
    "ConcolicTraceReason",
    "Event",
    "EventType",
    "ExpressionSyntaxError",
    "Function",
    "NeverDError",
    "LLVMIRSyntaxError",
    "LLVMOptimizationLevel",
    "LLVMOptimizationResult",
    "OptimizationMode",
    "OptimizationStop",
    "OutputLanguage",
    "PatchResult",
    "Plugin",
    "PluginSpec",
    "PluginType",
    "ProofStatus",
    "RawSessionAPI",
    "Session",
    "SimplifyEvidence",
    "SimplifyOutcome",
    "SimplifyResult",
    "SynthesisOutcome",
    "SynthesisResult",
    "SymbolicExploration",
    "SymbolicPath",
    "TranslationError",
    "TranslationErrorCode",
    "TranslationObjectFormat",
    "TranslationProofStatus",
    "TranslationSemanticStop",
    "X86_64ToAArch64ObjectResult",
    "simplify_expression",
    "synthesize_expression",
    "optimize_llvm_ir",
    "translate_x86_64_block_to_aarch64_object",
]
