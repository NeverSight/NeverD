"""Public authoring API for NeverD Python plugins."""

from __future__ import annotations

import builtins
import ctypes
from dataclasses import dataclass
import json
import os
import re
import sys
from types import ModuleType
from types import MappingProxyType
from typing import Any, Callable, Iterator, Mapping, Protocol, TypeVar, cast

from .abi import (
    EventType,
    NeverDSimplifyOptions,
    NeverDSimplifyResult,
    OutputLanguage,
    PluginType,
    SimplifyEvidence,
    SimplifyOutcome,
)
from .ffi import HostAPI


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


def _decode_json(operation: str, value: str | None) -> object:
    if value is None:
        raise NeverDError(f"NeverD returned no {operation} result")
    try:
        return json.loads(value)
    except json.JSONDecodeError as error:
        raise NeverDError(f"NeverD returned invalid {operation} JSON") from error


class ExpressionSyntaxError(NeverDError):
    """An expression could not be read.

    ``offset`` is the character reading stopped at, which is what lets a caller
    point at the mistake rather than repeat the whole line back.
    """

    def __init__(self, message: str, offset: int) -> None:
        super().__init__(message)
        self.offset = offset


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
    walk and combinatorial polynomial search, and ``exhaustive`` removes that
    resource budget.

    ``host`` names the library to work through.  It defaults to the NeverD this
    is running inside, but simplification needs no session and no loaded binary,
    so passing a library opened by hand is enough to use this from an ordinary
    script.

    Raises ``ExpressionSyntaxError`` when the expression cannot be read.
    """

    text = _utf8_argument("expression", expression, allow_empty=False)

    options = NeverDSimplifyOptions()
    options.struct_size = ctypes.sizeof(NeverDSimplifyOptions)
    options.width = _unsigned("width", width, 32)
    options.shallow = 0 if _boolean("deep", deep) else 1
    options.max_atoms = _unsigned("max_atoms", max_atoms, 32)
    options.max_work = (
        ctypes.c_size_t(-1).value
        if _boolean("exhaustive", exhaustive)
        else _unsigned("max_work", max_work, 64)
    )
    options.verify_samples = _unsigned("verify_samples", verify_samples, 32)
    options.allow_growth = 1 if _boolean("allow_growth", allow_growth) else 0

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


__all__ = [
    "Event",
    "EventType",
    "ExpressionSyntaxError",
    "Function",
    "NeverDError",
    "OutputLanguage",
    "PatchResult",
    "Plugin",
    "PluginSpec",
    "PluginType",
    "RawSessionAPI",
    "Session",
    "SimplifyEvidence",
    "SimplifyOutcome",
    "SimplifyResult",
    "simplify_expression",
]
