#!/usr/bin/env python3
"""Fail when the NeverD C ABI, Python SDK, docs, or delivery policy drifts."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
C_API_HEADER = ROOT / "include" / "neverd" / "sdk" / "NeverDCAPI.h"
SANITIZER_HEADER = ROOT / "include" / "neverd" / "sdk" / "NeverDCAPIPatch.h"
SYMBOLIC_HEADER = ROOT / "include" / "neverd" / "sdk" / "NeverDCAPISymbolic.h"
TRANSLATE_HEADER = ROOT / "include" / "neverd" / "sdk" / "NeverDCAPITranslate.h"
PLUGIN_HEADER = ROOT / "include" / "neverd" / "sdk" / "NeverDPlugin.h"
OUTPUT_LANGUAGES = ROOT / "include" / "neverd" / "OutputLanguages.def"
PYTHON_PACKAGE = ROOT / "pluginsdk" / "python"
SDK_WORKFLOW = ROOT / ".github" / "workflows" / "python-plugin-sdk.yml"
MAIN_CI_WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"
DOCUMENT_LOCALES = (
    "",
    ".zh-CN",
    ".zh-TW",
    ".ja",
    ".ko",
    ".fr",
    ".de",
    ".es",
    ".it",
    ".ru",
    ".ar",
)
CONCOLIC_V1_REGISTER_SEED_CEILING = 4096

# Keep the audit directly runnable from any working directory.  CI sets
# PYTHONPATH for the SDK test suite, but contributors should not need to know
# that implementation detail just to check repository drift.
sys.path.insert(0, str(PYTHON_PACKAGE))


def _without_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", source)


def normalize_c_type(value: str) -> str:
    value = re.sub(r"\s+", " ", value.strip())
    value = re.sub(r"\s*\*\s*", " *", value)
    return value.strip()


def parse_c_api(source: str) -> dict[str, tuple[str, tuple[str, ...]]]:
    declarations: dict[str, tuple[str, tuple[str, ...]]] = {}
    pattern = re.compile(
        r"^[ \t]*NEVERD_API[ \t]+(.+?)\b(neverd_[A-Za-z0-9_]+)\s*" r"\((.*?)\)\s*;",
        flags=re.MULTILINE | re.DOTALL,
    )
    for match in pattern.finditer(_without_comments(source)):
        result = normalize_c_type(match.group(1))
        name = match.group(2)
        raw_arguments = match.group(3).strip()
        arguments: list[str] = []
        if raw_arguments and raw_arguments != "void":
            for declaration in raw_arguments.split(","):
                declaration = declaration.strip()
                without_name = re.sub(r"\b[A-Za-z_][A-Za-z0-9_]*\s*$", "", declaration)
                arguments.append(normalize_c_type(without_name))
        if name in declarations:
            raise ValueError(f"duplicate C API declaration: {name}")
        declarations[name] = (result, tuple(arguments))
    return declarations


def parse_c_api_header(path: Path) -> dict[str, tuple[str, tuple[str, ...]]]:
    """Parse the C API exposed by a public header and its local includes."""

    sources: list[str] = []
    pending = [path]
    visited: set[Path] = set()
    while pending:
        current = pending.pop()
        resolved = current.resolve()
        if resolved in visited:
            continue
        visited.add(resolved)

        source = current.read_text(encoding="utf-8")
        sources.append(source)
        for include in re.findall(
            r'^\s*#\s*include\s+"([^"]+)"',
            _without_comments(source),
            flags=re.MULTILINE,
        ):
            included = ROOT / "include" / include
            if included.is_file():
                pending.append(included)

    return parse_c_api("\n".join(sources))


def _parse_c_integer_literal(value: str) -> int:
    match = re.fullmatch(r"(-?)(0[xX][0-9A-Fa-f]+|[0-9]+)[uUlL]*", value.strip())
    if match is None:
        raise ValueError(f"unsupported explicit C integer value {value!r}")
    magnitude = int(match.group(2), 0)
    return -magnitude if match.group(1) else magnitude


def _parse_c_enum_entries(body: str, label: str) -> dict[str, int]:
    entries: dict[str, int] = {}
    for name, expression in re.findall(
        r"\b([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^,}]+)", body
    ):
        operands = expression.strip().split("<<")
        if len(operands) == 1:
            value = _parse_c_integer_literal(operands[0])
        elif len(operands) == 2:
            value = _parse_c_integer_literal(operands[0]) << _parse_c_integer_literal(
                operands[1]
            )
        else:
            raise ValueError(
                f"unsupported explicit C integer expression {expression!r} in {label}"
            )
        entries[name] = value
    if not entries:
        raise ValueError(f"C enum {label} has no explicit values")
    return entries


def parse_c_enum_tag(source: str, tag_name: str) -> dict[str, int]:
    match = re.search(
        r"\benum\s+" + re.escape(tag_name) + r"\s*\{([^}]*)\}\s*;",
        _without_comments(source),
        flags=re.DOTALL,
    )
    if match is None:
        raise ValueError(f"cannot find C enum tag {tag_name}")
    return _parse_c_enum_entries(match.group(1), tag_name)


def parse_c_enum(source: str, typedef_name: str) -> dict[str, int]:
    pattern = re.compile(
        r"\btypedef\s+enum(?:\s+[A-Za-z_][A-Za-z0-9_]*)?\s*"
        r"\{([^}]*)\}\s*" + re.escape(typedef_name) + r"\s*;",
        flags=re.DOTALL,
    )
    match = pattern.search(_without_comments(source))
    if match is None:
        tag_name = typedef_name.removesuffix("_t")
        fixed_width = re.search(
            r"\btypedef\s+uint32_t\s+" + re.escape(typedef_name) + r"\s*;",
            _without_comments(source),
        )
        tag = re.search(
            r"\benum\s+" + re.escape(tag_name) + r"\s*\{([^}]*)\}\s*;",
            _without_comments(source),
            flags=re.DOTALL,
        )
        if fixed_width is None or tag is None:
            raise ValueError(f"cannot find C enum contract {typedef_name}")
        match = tag
    return _parse_c_enum_entries(match.group(1), typedef_name)


def parse_c_struct_layout(
    source: str, typedef_name: str
) -> tuple[tuple[str, str], ...]:
    pattern = re.compile(
        r"\btypedef\s+struct(?:\s+[A-Za-z_][A-Za-z0-9_]*)?\s*"
        r"\{([^}]*)\}\s*" + re.escape(typedef_name) + r"\s*;",
        flags=re.DOTALL,
    )
    match = pattern.search(_without_comments(source))
    if match is None:
        raise ValueError(f"cannot find C struct typedef {typedef_name}")
    fields: list[tuple[str, str]] = []
    for declaration in match.group(1).split(";"):
        declaration = declaration.strip()
        if not declaration:
            continue
        field = re.search(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*$", declaration)
        if field is None:
            raise ValueError(
                f"cannot parse a field in C struct typedef {typedef_name}: "
                f"{declaration!r}"
            )
        fields.append((field.group(1), normalize_c_type(declaration[: field.start()])))
    if not fields:
        raise ValueError(f"C struct typedef {typedef_name} has no fields")
    return tuple(fields)


def parse_c_struct_fields(source: str, typedef_name: str) -> tuple[str, ...]:
    return tuple(name for name, _c_type in parse_c_struct_layout(source, typedef_name))


BORROWED_STRING_FUNCTIONS = frozenset(
    {
        "neverd_optimization_stop_name",
        "neverd_proof_status_name",
        "neverd_sanitize_status_name",
        "neverd_synthesis_outcome_name",
        "neverd_translate_error_code_name",
    }
)


def expected_ownership(name: str, result: str):
    from neverd_plugin.abi import Ownership

    if name in BORROWED_STRING_FUNCTIONS:
        return Ownership.BORROWED_STRING
    if result == "const char *":
        return Ownership.OWNED_STRING
    if result == "const unsigned char *":
        return Ownership.BORROWED_BUFFER
    return Ownership.VALUE


def check_abi(errors: list[str]) -> None:
    from neverd_plugin import abi

    declarations = parse_c_api_header(C_API_HEADER)
    native_names = set(declarations)
    python_names = set(abi.FUNCTION_SPECS)
    for name in sorted(native_names - python_names):
        errors.append(f"Python ABI is missing exported function {name}")
    for name in sorted(python_names - native_names):
        errors.append(f"Python ABI declares non-exported function {name}")
    for name in sorted(native_names & python_names):
        result, arguments = declarations[name]
        spec = abi.FUNCTION_SPECS[name]
        if spec.c_result != result:
            errors.append(
                f"{name} result mismatch: header={result!r}, Python={spec.c_result!r}"
            )
        if spec.c_arguments != arguments:
            errors.append(
                f"{name} argument mismatch: header={arguments!r}, "
                f"Python={spec.c_arguments!r}"
            )
        ownership = expected_ownership(name, result)
        if spec.ownership is not ownership:
            errors.append(
                f"{name} ownership mismatch: expected={ownership.value}, "
                f"Python={spec.ownership.value}"
            )


def check_plugin_enums(errors: list[str]) -> None:
    from neverd_plugin.abi import EventType, PluginType

    source = PLUGIN_HEADER.read_text(encoding="utf-8")
    native_plugin_types = {
        name.removeprefix("NEVERD_PLUGIN_"): value
        for name, value in parse_c_enum(source, "neverd_plugin_type_t").items()
    }
    python_plugin_types = {member.name: member.value for member in PluginType}
    if native_plugin_types != python_plugin_types:
        errors.append(
            "plugin type mismatch: "
            f"native={native_plugin_types!r}, Python={python_plugin_types!r}"
        )

    native_event_types = {}
    for name, value in parse_c_enum(source, "neverd_event_type_t").items():
        normalized = name.removeprefix("NEVERD_EVT_")
        normalized = normalized.replace("FUNC_", "FUNCTION_", 1)
        normalized = normalized.replace("ADDR_", "ADDRESS_", 1)
        native_event_types[normalized] = value
    python_event_types = {member.name: member.value for member in EventType}
    if native_event_types != python_event_types:
        errors.append(
            "event type mismatch: "
            f"native={native_event_types!r}, Python={python_event_types!r}"
        )


def check_output_languages(errors: list[str]) -> None:
    from neverd_plugin.abi import OutputLanguage

    entries = {
        name: int(value)
        for name, value in re.findall(
            r"^NEVERD_OUTPUT_LANGUAGE\(\s*([A-Z0-9_]+)\s*,\s*([0-9]+)\s*,",
            OUTPUT_LANGUAGES.read_text(encoding="utf-8"),
            flags=re.MULTILINE,
        )
    }
    python_entries = {member.name: member.value for member in OutputLanguage}
    if entries != python_entries:
        errors.append(
            f"output language mismatch: native={entries!r}, Python={python_entries!r}"
        )


def check_translation_abi(errors: list[str]) -> None:
    import ctypes

    from neverd_plugin import abi

    source = TRANSLATE_HEADER.read_text(encoding="utf-8")
    enum_contracts = (
        (
            "neverd_translate_object_format_t",
            "NEVERD_TRANSLATE_OBJECT_FORMAT_",
            abi.TranslationObjectFormat,
        ),
        (
            "neverd_translate_error_code_t",
            "NEVERD_TRANSLATE_ERROR_",
            abi.TranslationErrorCode,
        ),
        (
            "neverd_translate_semantic_stop_t",
            "NEVERD_TRANSLATE_SEMANTIC_",
            abi.TranslationSemanticStop,
        ),
        (
            "neverd_translate_proof_status_t",
            "NEVERD_TRANSLATE_PROOF_",
            abi.TranslationProofStatus,
        ),
    )
    for typedef_name, prefix, python_enum in enum_contracts:
        if not re.search(
            r"\btypedef\s+uint32_t\s+" + re.escape(typedef_name) + r"\s*;",
            _without_comments(source),
        ):
            errors.append(f"{typedef_name} must use fixed uint32_t storage")
        native = {
            name.removeprefix(prefix): value
            for name, value in parse_c_enum(source, typedef_name).items()
        }
        python = {member.name: member.value for member in python_enum}
        if native != python:
            errors.append(
                f"{typedef_name} mismatch: native={native!r}, Python={python!r}"
            )

    struct_contracts = (
        (
            "neverd_translate_object_request_v1",
            abi.NeverDTranslateObjectRequestV1,
        ),
        (
            "neverd_translate_object_result_v1",
            abi.NeverDTranslateObjectResultV1,
        ),
    )
    native_ctypes = {
        "size_t": ctypes.c_size_t,
        "int": ctypes.c_int,
        "unsigned": ctypes.c_uint,
        "uint32_t": ctypes.c_uint32,
        "uint64_t": ctypes.c_uint64,
        "const char *": ctypes.c_char_p,
        "const unsigned char *": ctypes.POINTER(ctypes.c_ubyte),
        "neverd_translate_object_format_t": ctypes.c_uint32,
        "neverd_translate_error_code_t": ctypes.c_uint32,
        "neverd_translate_semantic_stop_t": ctypes.c_uint32,
        "neverd_translate_proof_status_t": ctypes.c_uint32,
    }
    for typedef_name, python_struct in struct_contracts:
        native_layout = parse_c_struct_layout(source, typedef_name)
        python_layout = tuple(python_struct._fields_)
        native_fields = tuple(name for name, _c_type in native_layout)
        python_fields = tuple(name for name, _ctype in python_layout)
        if native_fields != python_fields:
            errors.append(
                f"{typedef_name} field mismatch: native={native_fields!r}, "
                f"Python={python_fields!r}"
            )
            continue
        for (field_name, c_type), (_python_name, python_ctype) in zip(
            native_layout, python_layout, strict=True
        ):
            expected_ctype = native_ctypes.get(c_type)
            if expected_ctype is None:
                errors.append(
                    f"{typedef_name}.{field_name} has an unaudited C type {c_type!r}"
                )


def check_sanitizer_abi(errors: list[str]) -> None:
    import ctypes

    from neverd_plugin import abi

    source = SANITIZER_HEADER.read_text(encoding="utf-8")
    enum_contracts = (
        (
            "neverd_sanitize_strategy_t",
            "neverd_sanitize_strategy",
            "NEVERD_SANITIZE_STRATEGY_",
            abi.SanitizeStrategy,
        ),
        (
            "neverd_sanitize_status_t",
            "neverd_sanitize_status",
            "NEVERD_SANITIZE_STATUS_",
            abi.SanitizeStatus,
        ),
        (
            "neverd_sanitize_publication_outcome_t",
            "neverd_sanitize_publication_outcome",
            "NEVERD_SANITIZE_PUBLICATION_OUTCOME_",
            abi.SanitizePublicationOutcome,
        ),
        (
            "neverd_sanitize_publication_namespace_t",
            "neverd_sanitize_publication_namespace",
            "NEVERD_SANITIZE_PUBLICATION_NAMESPACE_",
            abi.SanitizePublicationNamespace,
        ),
        (
            "neverd_sanitize_publication_guarantees_t",
            "neverd_sanitize_publication_guarantee",
            "NEVERD_SANITIZE_PUBLICATION_GUARANTEE_",
            abi.SanitizePublicationGuarantee,
        ),
        (
            "neverd_sanitize_publication_operand_binding_t",
            "neverd_sanitize_publication_operand_binding",
            "NEVERD_SANITIZE_PUBLICATION_OPERAND_BINDING_",
            abi.SanitizePublicationOperandBinding,
        ),
    )
    for typedef_name, tag_name, prefix, python_enum in enum_contracts:
        if not re.search(
            r"\btypedef\s+uint32_t\s+" + re.escape(typedef_name) + r"\s*;",
            _without_comments(source),
        ):
            errors.append(f"{typedef_name} must use fixed uint32_t storage")
        native = {
            name.removeprefix(prefix): value
            for name, value in parse_c_enum_tag(source, tag_name).items()
        }
        python = {member.name: member.value for member in python_enum}
        if native != python:
            errors.append(
                f"{typedef_name} mismatch: native={native!r}, Python={python!r}"
            )

    version = re.search(
        r"\bNEVERD_SANITIZE_PUBLICATION_ABI_VERSION\s*=\s*([0-9]+)",
        _without_comments(source),
    )
    if version is None or int(version.group(1)) != 1:
        errors.append("sanitizer publication ABI version must be 1")

    native_ctypes = {
        "size_t": ctypes.c_size_t,
        "int": ctypes.c_int,
        "uint32_t": ctypes.c_uint32,
        "uint64_t": ctypes.c_uint64,
        "neverd_sanitize_status_t": ctypes.c_uint32,
        "neverd_sanitize_publication_outcome_t": ctypes.c_uint32,
        "neverd_sanitize_publication_namespace_t": ctypes.c_uint32,
        "neverd_sanitize_publication_guarantees_t": ctypes.c_uint32,
        "neverd_sanitize_publication_operand_binding_t": ctypes.c_uint32,
    }
    struct_contracts = (
        ("neverd_sanitize_options_v1", abi.NeverDSanitizeOptionsV1),
        ("neverd_sanitize_result_v1", abi.NeverDSanitizeResultV1),
    )
    for typedef_name, python_struct in struct_contracts:
        native_layout = parse_c_struct_layout(source, typedef_name)
        python_layout = tuple(python_struct._fields_)
        native_fields = tuple(name for name, _c_type in native_layout)
        python_fields = tuple(name for name, _ctype in python_layout)
        if native_fields != python_fields:
            errors.append(
                f"{typedef_name} field mismatch: native={native_fields!r}, "
                f"Python={python_fields!r}"
            )
            continue
        for (field_name, c_type), (_python_name, python_ctype) in zip(
            native_layout, python_layout, strict=True
        ):
            expected_ctype = native_ctypes.get(c_type)
            if expected_ctype is None:
                errors.append(
                    f"{typedef_name}.{field_name} has an unaudited C type {c_type!r}"
                )
            elif python_ctype is not expected_ctype:
                errors.append(
                    f"{typedef_name}.{field_name} type mismatch: "
                    f"header={c_type!r}, Python={python_ctype!r}, "
                    f"expected={expected_ctype!r}"
                )

def check_concolic_abi(errors: list[str]) -> None:
    """Audit the frozen concolic-v1 ctypes layout and owned result contract."""

    import ctypes

    from neverd_plugin import abi, api

    source = SYMBOLIC_HEADER.read_text(encoding="utf-8")
    seed_ceiling = re.search(
        r"^\s*#\s*define\s+NEVERD_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1"
        r"\s+([0-9]+)[uUlL]*\s*$",
        _without_comments(source),
        flags=re.MULTILINE,
    )
    if seed_ceiling is None:
        errors.append("cannot read native concolic register-seed ceiling")
    else:
        native_seed_ceiling = int(seed_ceiling.group(1))
        python_seed_ceiling = api._LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1
        if native_seed_ceiling != python_seed_ceiling:
            errors.append(
                "concolic register-seed ceiling mismatch: "
                f"header={native_seed_ceiling}, Python={python_seed_ceiling}"
            )
        elif native_seed_ceiling != CONCOLIC_V1_REGISTER_SEED_CEILING:
            errors.append(
                "concolic v1 register-seed ceiling changed: "
                f"expected={CONCOLIC_V1_REGISTER_SEED_CEILING}, "
                f"header={native_seed_ceiling}, Python={python_seed_ceiling}"
            )
    native_ctypes = {
        "size_t": ctypes.c_size_t,
        "unsigned": ctypes.c_uint,
        "uint32_t": ctypes.c_uint32,
        "uint64_t": ctypes.c_uint64,
        "const neverd_lowir_concolic_register_seed_v1 *": ctypes.POINTER(
            abi.NeverDLowIRConcolicRegisterSeedV1
        ),
    }
    struct_contracts = (
        (
            "neverd_lowir_concolic_register_seed_v1",
            abi.NeverDLowIRConcolicRegisterSeedV1,
        ),
        (
            "neverd_lowir_concolic_options_v1",
            abi.NeverDLowIRConcolicOptionsV1,
        ),
    )
    for typedef_name, python_struct in struct_contracts:
        native_layout = parse_c_struct_layout(source, typedef_name)
        python_layout = tuple(python_struct._fields_)
        native_fields = tuple(name for name, _c_type in native_layout)
        python_fields = tuple(name for name, _ctype in python_layout)
        if native_fields != python_fields:
            errors.append(
                f"{typedef_name} field mismatch: native={native_fields!r}, "
                f"Python={python_fields!r}"
            )
            continue
        for (field_name, c_type), (_python_name, python_ctype) in zip(
            native_layout, python_layout, strict=True
        ):
            expected_ctype = native_ctypes.get(c_type)
            if expected_ctype is None:
                errors.append(
                    f"{typedef_name}.{field_name} has an unaudited C type {c_type!r}"
                )
            elif python_ctype is not expected_ctype:
                errors.append(
                    f"{typedef_name}.{field_name} type mismatch: "
                    f"header={c_type!r}, Python={python_ctype!r}, "
                    f"expected={expected_ctype!r}"
                )

    function_name = "neverd_lowir_concolic_json_v1"
    declaration = parse_c_api(source).get(function_name)
    expected_declaration = (
        "const char *",
        (
            "neverd_session_t",
            "neverd_va_t",
            "const neverd_lowir_concolic_options_v1 *",
        ),
    )
    if declaration != expected_declaration:
        errors.append(
            f"{function_name} declaration mismatch: "
            f"header={declaration!r}, expected={expected_declaration!r}"
        )
    spec = abi.FUNCTION_SPECS.get(function_name)
    if spec is None:
        errors.append(f"Python ABI is missing exported function {function_name}")
    elif spec.ownership is not abi.Ownership.OWNED_STRING:
        errors.append(
            f"{function_name} ownership mismatch: "
            f"expected={abi.Ownership.OWNED_STRING.value}, "
            f"Python={spec.ownership.value}"
        )


def _required_match(path: Path, pattern: str, label: str, errors: list[str]) -> None:
    source = path.read_text(encoding="utf-8")
    if not re.search(pattern, source, flags=re.MULTILINE | re.DOTALL):
        errors.append(f"{path.relative_to(ROOT)} is missing {label}")


def check_versions(errors: list[str]) -> None:
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    version_module = (PYTHON_PACKAGE / "neverd_plugin" / "_version.py").read_text(
        encoding="utf-8"
    )
    pyproject = (PYTHON_PACKAGE / "pyproject.toml").read_text(encoding="utf-8")
    workflow = (
        SDK_WORKFLOW.read_text(encoding="utf-8") if SDK_WORKFLOW.is_file() else ""
    )
    patterns = {
        "CMake": (cmake, r"project\(NeverD\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)"),
        "Python module": (
            version_module,
            r"__version__\s*=\s*[\"']([0-9]+\.[0-9]+\.[0-9]+)[\"']",
        ),
        "pyproject": (
            pyproject,
            r"(?m)^version\s*=\s*[\"']([0-9]+\.[0-9]+\.[0-9]+)[\"']",
        ),
        "release workflow": (
            workflow,
            r"(?m)^\s*SDK_VERSION:\s*[\"']([0-9]+\.[0-9]+\.[0-9]+)[\"']",
        ),
    }
    versions: dict[str, str] = {}
    for label, (source, pattern) in patterns.items():
        match = re.search(pattern, source)
        if match is None:
            errors.append(f"cannot read {label} version")
        else:
            versions[label] = match.group(1)
    if len(set(versions.values())) > 1:
        errors.append(f"SDK version mismatch: {versions!r}")

    expected_project_urls = {
        "Homepage": "https://github.com/NeverSight/NeverD",
        "Documentation": (
            "https://github.com/NeverSight/NeverD/blob/dev/docs/python-plugins.md"
        ),
        "Issues": "https://github.com/NeverSight/NeverD/issues",
    }
    for label, url in expected_project_urls.items():
        if f'{label} = "{url}"' not in pyproject:
            errors.append(f"pyproject has incorrect {label} URL")


def _fenced_code_blocks(source: str) -> tuple[tuple[str, str], ...]:
    return tuple(
        (language.strip(), body)
        for language, body in re.findall(
            r"^```([^\n]*)\n(.*?)^```\s*$",
            source,
            flags=re.MULTILINE | re.DOTALL,
        )
    )


def check_documentation(errors: list[str]) -> None:
    docs = ROOT / "docs"
    document_names = tuple(f"python-plugins{locale}.md" for locale in DOCUMENT_LOCALES)
    english_path = docs / document_names[0]
    if not english_path.is_file():
        errors.append("missing docs/python-plugins.md")
        return

    english_blocks = _fenced_code_blocks(english_path.read_text(encoding="utf-8"))
    if not english_blocks:
        errors.append("docs/python-plugins.md has no fenced examples")

    required_tokens = (
        "NEVERD_ENABLE_PYTHON_PLUGINS",
        "neverd-plugin",
        "neverd_plugins_load_file",
        "session.raw.session_borrowed_bytes",
        "neverd_last_error",
        "scripts/check_python_plugin_sdk.py",
        "Python Plugin SDK",
        "Trusted Publishing",
    )
    for locale, document_name in zip(DOCUMENT_LOCALES, document_names):
        document = docs / document_name
        index = docs / f"README{locale}.md"
        if not document.is_file():
            errors.append(f"missing docs/{document_name}")
            continue
        if not index.is_file():
            errors.append(f"missing docs/{index.name}")
            continue

        source = document.read_text(encoding="utf-8")
        index_source = index.read_text(encoding="utf-8")
        if f"]({document_name})" not in index_source:
            errors.append(f"docs/{index.name} does not link to {document_name}")
        for linked_name in document_names:
            if f"]({linked_name})" not in source:
                errors.append(f"docs/{document_name} does not link to {linked_name}")
        for token in required_tokens:
            if token not in source:
                errors.append(f"docs/{document_name} is missing {token}")
        if _fenced_code_blocks(source) != english_blocks:
            errors.append(
                f"docs/{document_name} code examples differ from python-plugins.md"
            )


ACTION_PINS = {
    "actions/checkout": "3d3c42e5aac5ba805825da76410c181273ba90b1",
    "actions/setup-python": "5fda3b95a4ea91299a34e894583c3862153e4b97",
    "actions/upload-artifact": "043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
    "actions/download-artifact": "3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c",
    "pypa/gh-action-pypi-publish": "6733eb7d741f0b11ec6a39b58540dab7590f9b7d",
}


def check_workflows(errors: list[str]) -> None:
    if not SDK_WORKFLOW.is_file():
        errors.append("missing .github/workflows/python-plugin-sdk.yml")
        return
    workflow = SDK_WORKFLOW.read_text(encoding="utf-8")
    main_ci = MAIN_CI_WORKFLOW.read_text(encoding="utf-8")

    required_sdk_patterns = {
        "pull-request trigger": r"(?m)^\s{2}pull_request:",
        "manual trigger": r"(?m)^\s{2}workflow_dispatch:",
        "published-release trigger": r"release:\s*\n\s+types:\s*\[published\]",
        "Python 3.10 lane": r"[\"']3\.10[\"']",
        "Python 3.14 lane": r"[\"']3\.14[\"']",
        "distribution artifact": r"name:\s*neverd-python-plugin-dist",
        "same-run package dependency": r"(?m)^\s+needs:\s*package\s*$",
        "PyPI environment": r"(?m)^\s+environment:\s*pypi\s*$",
        "OIDC permission": r"(?m)^\s+id-token:\s*write\s*$",
        "release-only publish guard": r"github\.event_name\s*==\s*'release'",
        "release tag/version guard": r"GITHUB_REF_NAME.*v\$SDK_VERSION",
        "wheel build": r"python\s+-m\s+build\s+pluginsdk/python",
        "wheel verification": r"twine\s+check\s+dist/\*",
        "strict package type check": r"python\s+-m\s+mypy.*pluginsdk/python/neverd_plugin",
        "pinned build frontend": r"build==1\.5\.0",
        "pinned type checker": r"mypy==2\.3\.0",
        "pinned metadata checker": r"twine==7\.0\.0",
    }
    for label, pattern in required_sdk_patterns.items():
        if not re.search(pattern, workflow, flags=re.MULTILINE | re.DOTALL):
            errors.append(f"Python SDK workflow is missing {label}")

    for action, pin in ACTION_PINS.items():
        expected = f"uses: {action}@{pin}"
        if expected not in workflow:
            errors.append(f"Python SDK workflow must pin {action} to {pin}")

    main_patterns = {
        "explicit Python plugin feature": r"-DNEVERD_ENABLE_PYTHON_PLUGINS=ON",
        "SDK unit tests": r"pluginsdk/python/tests",
        "SDK drift audit": r"scripts/check_python_plugin_sdk\.py",
        "pinned setup-python": (
            r"uses:\s*actions/setup-python@"
            + re.escape(ACTION_PINS["actions/setup-python"])
        ),
    }
    for label, pattern in main_patterns.items():
        if not re.search(pattern, main_ci):
            errors.append(f"main CI workflow is missing {label}")


def audit_repository(*, include_workflows: bool = True) -> list[str]:
    errors: list[str] = []
    check_abi(errors)
    check_plugin_enums(errors)
    check_output_languages(errors)
    check_translation_abi(errors)
    check_sanitizer_abi(errors)
    check_concolic_abi(errors)
    check_versions(errors)
    check_documentation(errors)
    if include_workflows:
        check_workflows(errors)
    return errors


def main(arguments: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--without-workflows",
        action="store_true",
        help="skip delivery-policy checks while bootstrapping the workflow",
    )
    options = parser.parse_args(arguments)
    errors = audit_repository(include_workflows=not options.without_workflows)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print("NeverD Python plugin SDK audit passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
