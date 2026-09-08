"""Project verified native method signatures and bodies into Objective-C.

The native batch is authoritative for the C signature. Runtime metadata is
authoritative for method identity and the Objective-C declaration. This module
does not infer either one from symbol spelling or an implementation address.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import re

from .common import MobileError
from .macho import (_FOUNDATION_CATEGORY_CLASSES, _IDENTIFIER, _types,
                    objc_category_inventory, objc_header, objc_ivar_layout)


_KEYWORDS = frozenset("auto break case char const continue default do double else enum extern float for goto if inline int long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while _Alignas _Alignof _Atomic _Bool _Complex _Generic _Imaginary _Noreturn _Static_assert _Thread_local self _cmd super id Class SEL BOOL nil YES NO".split())
_QUALIFIERS = frozenset({"const", "volatile", "restrict", "__restrict", "__restrict__"})
_STORAGE = frozenset({"static", "inline", "extern", "_Noreturn"})
_INTEGER_WIDTHS = {
    "char": 1, "signed char": 1, "unsigned char": 1, "int8_t": 1, "uint8_t": 1,
    "short": 2, "short int": 2, "signed short": 2, "signed short int": 2,
    "unsigned short": 2, "unsigned short int": 2, "int16_t": 2, "uint16_t": 2,
    "int": 4, "signed": 4, "signed int": 4, "unsigned": 4, "unsigned int": 4,
    "int32_t": 4, "uint32_t": 4,
    "long long": 8, "long long int": 8, "signed long long": 8,
    "signed long long int": 8, "unsigned long long": 8,
    "unsigned long long int": 8, "int64_t": 8, "uint64_t": 8,
    "BOOL": 1, "bool": 1, "_Bool": 1,
}
_WORD_INTEGERS = frozenset({"long", "long int", "signed long", "signed long int", "unsigned long", "unsigned long int", "intptr_t", "uintptr_t", "size_t", "ptrdiff_t"})


class _Unrecovered(ValueError):
    pass


@dataclass(frozen=True)
class _Token:
    value: str
    start: int
    end: int
    kind: str = "code"


def _tokens(source: str) -> list[_Token]:
    """Keep offsets while excluding comments from C delimiter matching."""
    result = []
    cursor = 0
    while cursor < len(source):
        start = cursor
        char = source[cursor]
        if char.isspace():
            cursor += 1
            continue
        if source.startswith("//", cursor):
            while True:
                end = source.find("\n", cursor)
                if end < 0:
                    cursor = len(source)
                    break
                continued = source[cursor:end].rstrip("\r").endswith("\\")
                cursor = end + 1
                if not continued:
                    break
            continue
        if source.startswith("/*", cursor):
            end = source.find("*/", cursor + 2)
            if end < 0:
                raise _Unrecovered("unterminated C comment")
            cursor = end + 2
            continue
        literal_prefix = next((prefix for prefix in ("u8", "u", "U", "L")
                               if source.startswith(prefix, cursor) and cursor + len(prefix) < len(source)
                               and source[cursor + len(prefix)] in "\"'"), "")
        if char in "\"'" or literal_prefix:
            cursor += len(literal_prefix)
            quote = source[cursor]
            cursor += 1
            while cursor < len(source):
                if source[cursor] == "\\":
                    cursor += 2
                elif source[cursor] == quote:
                    cursor += 1
                    break
                elif source[cursor] in "\r\n":
                    raise _Unrecovered("unescaped newline in C literal")
                else:
                    cursor += 1
            else:
                raise _Unrecovered("unterminated C literal")
            if cursor > len(source):
                raise _Unrecovered("unterminated C literal")
            result.append(_Token(source[start:cursor], start, cursor, "literal"))
            continue
        if char == "#":
            if source[source.rfind("\n", 0, cursor) + 1:cursor].strip():
                raise _Unrecovered("preprocessor marker outside a directive")
            while True:
                end = source.find("\n", cursor)
                if end < 0:
                    cursor = len(source)
                    break
                cursor = end + 1
                if not source[start:end].rstrip("\r").endswith("\\"):
                    break
            result.append(_Token(source[start:cursor], start, cursor, "directive"))
            continue
        if char.isascii() and (char.isalpha() or char == "_"):
            cursor += 1
            while cursor < len(source) and source[cursor].isascii() and (source[cursor].isalnum() or source[cursor] == "_"):
                cursor += 1
        elif source.startswith("->", cursor):
            cursor += 2
        else:
            cursor += 1
        result.append(_Token(source[start:cursor], start, cursor))
    return result


def _matches(tokens: list[_Token]) -> dict[int, int]:
    stack: list[int] = []
    pairs: dict[int, int] = {}
    closing = {")": "(", "]": "[", "}": "{"}
    for index, token in enumerate(tokens):
        if token.kind != "code":
            continue
        if token.value in ("(", "[", "{"):
            stack.append(index)
        elif token.value in closing:
            if not stack or tokens[stack[-1]].value != closing[token.value]:
                raise _Unrecovered("unbalanced C delimiters")
            opening = stack.pop()
            pairs[opening] = index
            pairs[index] = opening
    if stack:
        raise _Unrecovered("unbalanced C delimiters")
    return pairs


@dataclass(frozen=True)
class _Definition:
    name: str
    start: int
    name_index: int
    params_open: int
    params_close: int
    body_open: int
    body_close: int


def _definitions(tokens: list[_Token], pairs: dict[int, int]) -> list[_Definition]:
    definitions = []
    start = cursor = 0
    while cursor < len(tokens):
        token = tokens[cursor]
        if token.kind == "directive":
            # Conditional directives could make an apparent definition dead
            # code; do not pretend to evaluate a preprocessor here.
            directive = token.value[1:].lstrip().split(None, 1)[0] if token.value[1:].strip() else ""
            if directive not in {"include", "import", "pragma"}:
                raise _Unrecovered("conditional or mutating preprocessor directives are unsupported")
            start = cursor + 1
        elif token.value == ";":
            start = cursor + 1
        elif token.kind == "code" and token.value == "{":
            end = pairs[cursor]
            if cursor and tokens[cursor - 1].value == ")":
                params_open = pairs[cursor - 1]
                name_index = params_open - 1
                if name_index < start or not _safe_identifier(tokens[name_index].value):
                    raise _Unrecovered("unsupported C function declarator")
                definitions.append(_Definition(tokens[name_index].value, start, name_index,
                                               params_open, cursor - 1, cursor, end))
                start = end + 1
            cursor = end
        elif token.kind == "code" and token.value in ("(", "["):
            cursor = pairs[cursor]
        cursor += 1
    return definitions


def _safe_identifier(value: object) -> bool:
    return isinstance(value, str) and bool(_IDENTIFIER.fullmatch(value)) and value not in _KEYWORDS


def _type_words(value: str) -> list[str]:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z_0-9 \t*]+", value):
        raise _Unrecovered("native type must be text")
    tokens = _tokens(value)
    if not tokens or any(token.kind != "code" or not (_IDENTIFIER.fullmatch(token.value) or token.value == "*") for token in tokens):
        raise _Unrecovered("unsupported native scalar/pointer type")
    words = [token.value for token in tokens if token.value not in _QUALIFIERS]
    if "*" in words:
        first = words.index("*")
        if not first or any(word != "*" for word in words[first:]):
            raise _Unrecovered("unsupported native pointer declarator")
    return words


def _abi_type(value: str, pointer_size: int | None) -> tuple[str, int | None]:
    words = _type_words(value)
    stars = words.count("*")
    base = " ".join(word for word in words if word != "*")
    if base not in {*_INTEGER_WIDTHS, *_WORD_INTEGERS, "void", "float", "double", "id", "Class", "SEL"}:
        raise _Unrecovered("unsupported native scalar/pointer type")
    if stars or base in {"id", "Class", "SEL"}:
        return "pointer", pointer_size
    if base in _INTEGER_WIDTHS:
        return "integer", _INTEGER_WIDTHS[base]
    if base in _WORD_INTEGERS:
        if pointer_size not in {4, 8}:
            raise _Unrecovered("native word-sized type requires pointer_size 4 or 8")
        return "integer", pointer_size
    return base, {"void": 0, "float": 4, "double": 8}[base]


def _declaration_words(tokens: list[_Token]) -> list[str]:
    words = []
    index = 0
    pairs = _matches(tokens)
    while index < len(tokens):
        token = tokens[index]
        if token.value == "__attribute__" and index + 1 < len(tokens) and tokens[index + 1].value == "(":
            index = pairs[index + 1] + 1
            continue
        if token.value not in _STORAGE:
            words.append(token.value)
        index += 1
    return words


def _identity(method: dict, class_name: object | None = None) -> tuple:
    return (method.get("class_name", class_name), method.get("selector"), method.get("class_method"),
            method.get("category_name") or "", method.get("category_address") or "0x0")


def _fingerprint(value: object) -> str:
    return json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":"))


def _diagnostics(native: dict) -> list[str]:
    diagnostics = native.get("diagnostics", [])
    if not isinstance(diagnostics, list) or any(not isinstance(item, str) for item in diagnostics):
        raise _Unrecovered("native method diagnostics must be a list of strings")
    return diagnostics.copy()


def _rewrite(source: str, tokens: list[_Token], names: dict[str, str]) -> str:
    chunks = []
    cursor = 0
    for token in tokens:
        if token.kind == "code" and token.value in names:
            chunks.extend((source[cursor:token.start], names[token.value]))
            cursor = token.end
    chunks.append(source[cursor:])
    return "".join(chunks)


def _render_method(native: dict, runtime: dict, class_name: str, pointer_size: int | None) -> tuple[str, str, dict[str, tuple[str, ...]], dict[str, tuple[str, str]]]:
    selector = runtime.get("selector")
    if not _safe_identifier(class_name) or not isinstance(selector, str):
        raise _Unrecovered("unsafe Objective-C class or selector identifier")
    category = runtime.get("category_name") or ""
    if category and not _safe_identifier(category):
        raise _Unrecovered("unsafe Objective-C category identifier")
    parts = selector.split(":")
    arguments = selector.count(":")
    if (arguments and parts[-1]) or not all(_safe_identifier(part) for part in (parts[:-1] if arguments else parts)):
        raise _Unrecovered("unsafe Objective-C selector")
    if not isinstance(runtime.get("class_method"), bool) or not isinstance(native.get("class_method"), bool):
        raise _Unrecovered("class_method must be a boolean")
    encoding = runtime.get("type_encoding")
    types = _types(encoding) if isinstance(encoding, str) else None
    if not types or len(types) != arguments + 3 or types[1] not in {"id", "Class"} or types[2] != "SEL" or any(value == "void" for value in types[3:]):
        raise _Unrecovered("unsupported Objective-C ABI or type encoding")
    if (any(native.get(key) != runtime.get(key) for key in ("selector", "class_method", "implementation", "type_encoding")) or
            _identity(native) != _identity(runtime, class_name)):
        raise _Unrecovered("native method identity disagrees with runtime metadata")
    if native.get("status") != "recovered":
        raise _Unrecovered(str(native.get("reason") or "native backend did not recover this method"))
    name = native.get("function_name")
    if not _safe_identifier(name):
        raise _Unrecovered("unsafe native function identifier")
    parameters = native.get("parameters")
    expected_names = ["objc_self", "objc_cmd", *[f"arg{index}" for index in range(arguments)]]
    if not isinstance(parameters, list) or len(parameters) != len(expected_names):
        raise _Unrecovered("native parameter count disagrees with the runtime signature")
    native_types = [native.get("return_type")]
    for parameter, expected in zip(parameters, expected_names):
        if not isinstance(parameter, dict) or parameter.get("name") != expected:
            raise _Unrecovered("native parameter names must be objc_self, objc_cmd, arg0, ... in order")
        native_types.append(parameter.get("type"))
    for native_type, runtime_type in zip(native_types, types):
        if _abi_type(native_type, pointer_size) != _abi_type(runtime_type, pointer_size):
            raise _Unrecovered("native scalar/pointer ABI disagrees with the runtime encoding")
    source = native.get("source")
    if not isinstance(source, str) or not source.strip():
        raise _Unrecovered("native source is missing or empty")
    tokens = _tokens(source)
    pairs = _matches(tokens)
    definitions = _definitions(tokens, pairs)
    targets = [definition for definition in definitions if definition.name == name]
    if len(targets) != 1:
        raise _Unrecovered("expected exactly one definition for function_name")
    target = targets[0]
    if any(token.value == "__attribute__" for token in tokens[target.start:target.name_index]):
        raise _Unrecovered("native function attributes cannot be transferred to an Objective-C method")
    declared_return = _declaration_words(tokens[target.start:target.name_index])
    if declared_return != [token.value for token in _tokens(native_types[0])]:
        raise _Unrecovered("emitted return type disagrees with the native report")
    param_tokens = tokens[target.params_open + 1:target.params_close]
    groups: list[list[_Token]] = [[]]
    for token in param_tokens:
        if token.value == ",":
            groups.append([])
        else:
            groups[-1].append(token)
    if len(groups) != len(parameters):
        raise _Unrecovered("emitted parameter count disagrees with the native report")
    for group, parameter in zip(groups, parameters):
        expected = [token.value for token in _tokens(parameter["type"])] + [parameter["name"]]
        if [token.value for token in group] != expected:
            raise _Unrecovered("emitted parameter declaration disagrees with the native report")
    body_tokens = tokens[target.body_open + 1:target.body_close]
    if not body_tokens or all(token.value == ";" for token in body_tokens):
        raise _Unrecovered("native method body is empty")
    if any(token.kind == "directive" or token.value in {"self", "_cmd", "super"} for token in body_tokens):
        raise _Unrecovered("native method body conflicts with Objective-C implicit bindings")
    # Rebinding a native parameter in the same block would change its storage
    # or collide with the aliases we add. Nested scopes may shadow normally.
    depth = 0
    statement_start = True
    declaring = False
    for token in body_tokens:
        if token.value == "{":
            depth += 1
        elif token.value == "}":
            depth -= 1
        elif depth == 0:
            if statement_start:
                declaring = token.value in {*_INTEGER_WIDTHS, *_WORD_INTEGERS, *_QUALIFIERS, "void", "float", "double", "id", "Class", "SEL"}
                statement_start = False
            elif declaring and token.value in expected_names:
                raise _Unrecovered("native body redeclares a bound parameter")
            if token.value == "=":
                declaring = False
            elif token.value == ";":
                statement_start = True
                declaring = False
    if len({definition.name for definition in definitions}) != len(definitions):
        raise _Unrecovered("duplicate inconsistent C function definitions")
    tag = hashlib.sha256(_fingerprint([*_identity(runtime, class_name), runtime.get("implementation")]).encode()).hexdigest()[:16]
    shared_names = native.get("shared_block_functions", [])
    if (not isinstance(shared_names, list) or any(not isinstance(name, str) for name in shared_names)
            or len(shared_names) != len(set(shared_names))):
        raise _Unrecovered("invalid shared Block function inventory")
    defined_names = {definition.name for definition in definitions}
    for name in shared_names:
        if (name not in defined_names or name == target.name
                or not re.fullmatch(r"neverd_block_(?:invoke_[0-9a-f]+|(?:descriptor|literal)_[0-9a-f]+_address)", name)):
            raise _Unrecovered("shared Block function has no exact generated definition")
    names = {definition.name: f"neverd_objc_{tag}_{definition.name}" for definition in definitions
             if definition.name not in shared_names}
    if any(token.value in names.values() for token in tokens):
        raise _Unrecovered("native identifiers collide with generated support names")
    # Keep the original C definitions as well as their extracted method body.
    # This preserves helpers and exact direct-recursion/function-pointer targets.
    shared: dict[str, tuple[str, str]] = {}
    chunks = []
    cursor = 0
    for definition in definitions:
        if definition.name not in shared_names:
            continue
        start, end = tokens[definition.start].start, tokens[definition.body_close].end
        prototype = source[start:tokens[definition.body_open].start].rstrip() + ";"
        complete = source[start:end]
        shared[definition.name] = (_rewrite(prototype, _tokens(prototype), names),
                                   _rewrite(complete, _tokens(complete), names))
        chunks.extend((source[cursor:start], prototype))
        cursor = end
    chunks.append(source[cursor:])
    support_text = "".join(chunks)
    support = _rewrite(support_text, _tokens(support_text), names)
    body_start, body_end = tokens[target.body_open].end, tokens[target.body_close].start
    body = source[body_start:body_end]
    body = _rewrite(body, _tokens(body), names)
    used = {token.value for token in tokens}
    argument_names = []
    for index in range(arguments):
        candidate = f"neverd_objc_argument_{tag}_{index}"
        if candidate in used:
            raise _Unrecovered("native identifiers collide with generated argument names")
        argument_names.append(candidate)
    prefix = "+" if runtime["class_method"] else "-"
    signature = selector if not arguments else " ".join(f"{part}:({types[index + 3]}){argument_names[index]}" for index, part in enumerate(parts[:-1]))
    aliases = [f"    {parameters[0]['type']} objc_self = ({parameters[0]['type']})self;",
               f"    {parameters[1]['type']} objc_cmd = ({parameters[1]['type']})_cmd;"]
    aliases.extend(f"    {parameter['type']} arg{index} = ({parameter['type']}){argument_names[index]};" for index, parameter in enumerate(parameters[2:]))
    method_source = f"{prefix} ({types[0]}){signature}\n{{\n" + "\n".join(aliases) + "\n" + body + "\n}\n"
    # External prototypes may repeat identically but must not silently conflict
    # when separate native projections share one Objective-C translation unit.
    declarations: dict[str, tuple[str, ...]] = {}
    cursor = 0
    while cursor < len(tokens):
        if tokens[cursor].kind == "directive":
            cursor += 1
            continue
        definition = next((item for item in definitions if item.start == cursor), None)
        if definition:
            cursor = definition.body_close + 1
            continue
        end = cursor
        while end < len(tokens) and tokens[end].value != ";":
            if tokens[end].value in {"(", "[", "{"}:
                end = pairs[end]
            end += 1
        if end == len(tokens):
            raise _Unrecovered("unrecognized trailing C source")
        unit = tokens[cursor:end]
        if unit:
            opening = next((index for index, token in enumerate(unit) if token.value == "("), None)
            if opening is not None and opening > 0 and unit[-1].value == ")":
                symbol = unit[opening - 1].value
                if not _safe_identifier(symbol):
                    raise _Unrecovered("unsupported external declaration")
                if symbol not in names:
                    spelling = tuple(token.value for token in unit if token.value != "extern")
                    if symbol in declarations and declarations[symbol] != spelling:
                        raise _Unrecovered("conflicting external declarations")
                    declarations[symbol] = spelling
            elif (len(unit) == 6 and [token.value for token in unit[:3]] == ["extern", "void", "*"]
                  and unit[3].value in {"_NSConcreteStackBlock", "_NSConcreteGlobalBlock"}
                  and [token.value for token in unit[4:]] == ["[", "]"]):
                symbol = unit[3].value
                spelling = tuple(token.value for token in unit)
                if symbol in declarations and declarations[symbol] != spelling:
                    raise _Unrecovered("conflicting external declarations")
                declarations[symbol] = spelling
            elif unit[0].value == "extern" and _safe_identifier(unit[-1].value):
                symbol = unit[-1].value
                spelling = tuple(token.value for token in unit)
                if symbol in declarations and declarations[symbol] != spelling:
                    raise _Unrecovered("conflicting external declarations")
                declarations[symbol] = spelling
            else:
                # Type/global definitions cannot be made private just by
                # renaming function tokens; leave them explicitly unsupported.
                raise _Unrecovered("unsupported non-function C support declaration")
        cursor = end + 1
    return method_source, support, declarations, shared


def render_objc_sources(report: dict, objc_metadata: dict) -> tuple[str, dict]:
    """Return a genuine-body .m projection and explicit per-method coverage."""
    if not isinstance(report, dict) or report.get("schema_version") != 1 or not isinstance(report.get("methods"), list):
        raise MobileError("unsupported Objective-C native method report schema")
    if not isinstance(objc_metadata, dict) or not isinstance(objc_metadata.get("classes"), list):
        raise MobileError("invalid Objective-C runtime metadata")
    local_classes, external_categories = objc_category_inventory(objc_metadata)
    runtime_entries = list(local_classes)
    external_owners: dict[str, dict] = {}
    for category in external_categories:
        owner = external_owners.setdefault(category["class_name"],
            {"name": category["class_name"], "methods": [], "_external_category_owner": True})
        owner["methods"].extend(category["methods"])
    runtime_entries.extend(external_owners.values())
    pointer_size = report.get("pointer_size")
    if pointer_size is not None and (type(pointer_size) is not int or pointer_size not in {4, 8}):
        raise MobileError("invalid Objective-C native pointer_size")
    batches: dict[tuple, list[dict]] = {}
    for native in report["methods"]:
        if not isinstance(native, dict):
            raise MobileError("native Objective-C method entry must be an object")
        key = _identity(native)
        if any(not isinstance(part, (str, bool, type(None))) for part in key):
            raise MobileError("invalid native Objective-C method identity")
        batches.setdefault(key, []).append(native)
    coverage = []
    candidates = []
    safe_classes = []
    encountered = set()
    class_definitions: dict[str, str] = {}
    classes: dict[str, dict] = {}
    conflicting_classes = set()
    for entry in local_classes:
        if not isinstance(entry, dict) or not isinstance(entry.get("methods"), list) or not isinstance(entry.get("name"), str):
            raise MobileError("invalid Objective-C class metadata")
        name = entry["name"]
        fingerprint = _fingerprint(entry)
        if name in class_definitions and class_definitions[name] != fingerprint:
            conflicting_classes.add(name)
        class_definitions[name] = fingerprint
        classes[name] = entry
    layouts = {name: objc_ivar_layout(entry, classes, pointer_size or 8)[1]
               for name, entry in classes.items()}
    category_addresses: dict[tuple[str, str], set[str]] = {}
    for entry in runtime_entries:
        name = entry["name"]
        for method in entry["methods"]:
            if isinstance(method, dict) and isinstance(method.get("category_name"), str) and method["category_name"]:
                category_addresses.setdefault((name, method["category_name"]), set()).add(str(method.get("category_address") or "0x0"))
    for entry in runtime_entries:
        name = entry["name"]
        if _safe_identifier(name) and name not in conflicting_classes and not entry.get("_external_category_owner"):
            safe_classes.append({**entry, "methods": [method for method in entry["methods"]
                if isinstance(method, dict) and isinstance(method.get("selector"), str)
                and isinstance(method.get("type_encoding"), str) and isinstance(method.get("class_method"), bool)]})
        for runtime in entry["methods"]:
            if not isinstance(runtime, dict):
                raise MobileError("invalid Objective-C runtime method entry")
            key = _identity(runtime, name)
            if any(not isinstance(part, (str, bool, type(None))) for part in key):
                raise MobileError("invalid Objective-C runtime method identity")
            if key in encountered:
                # Every duplicate runtime declaration must agree exactly.
                for item in coverage:
                    if _identity(item) == key:
                        item.update(status="unrecovered", reason="duplicate runtime method definitions")
                continue
            encountered.add(key)
            item = {"class_name": name, "selector": runtime.get("selector"), "class_method": runtime.get("class_method"),
                    "category_name": runtime.get("category_name") or "", "category_address": runtime.get("category_address") or "0x0",
                    "implementation": runtime.get("implementation"), "status": "unrecovered", "diagnostics": []}
            coverage.append(item)
            try:
                if entry.get("_external_category_owner") and name not in _FOUNDATION_CATEGORY_CLASSES:
                    raise _Unrecovered("external category requires an unavailable class declaration: " + name)
                if name in conflicting_classes:
                    raise _Unrecovered("duplicate inconsistent Objective-C class definitions")
                if len(category_addresses.get((name, runtime.get("category_name") or ""), set())) > 1:
                    raise _Unrecovered("duplicate Objective-C category names have distinct runtime definitions")
                native_entries = batches.get(key, [])
                if not native_entries:
                    raise _Unrecovered("native method report is missing")
                if len({_fingerprint(value) for value in native_entries}) != 1:
                    raise _Unrecovered("duplicate inconsistent native method records")
                item["diagnostics"] = _diagnostics(native_entries[0])
                dependencies = native_entries[0].get("instance_layout_classes", [])
                if not isinstance(dependencies, list) or any(not isinstance(value, str) for value in dependencies):
                    raise _Unrecovered("invalid instance-layout dependency inventory")
                # Old batches do not identify individual ivar uses. An explicit
                # but invalid class layout cannot back a reconstructed body.
                if "instance_layout_classes" not in native_entries[0] and "ivar_status" in entry and layouts[name]:
                    dependencies = [name]
                checked = set()
                while dependencies:
                    dependency = dependencies[0]
                    dependencies = dependencies[1:]
                    if dependency in checked:
                        continue
                    checked.add(dependency)
                    if dependency not in classes or dependency in conflicting_classes or layouts[dependency]:
                        raise _Unrecovered("required instance-variable layout is unavailable: " + dependency)
                    parent = classes[dependency].get("superclass")
                    if parent in classes:
                        dependencies.append(parent)
                method, support, declarations, shared = _render_method(native_entries[0], runtime, name, pointer_size)
                item["status"] = "recovered"
                candidates.append((item, method, support, declarations, shared))
            except _Unrecovered as error:
                item["reason"] = str(error)
    for key, entries in batches.items():
        if key not in encountered:
            native = entries[0]
            item = {"class_name": native.get("class_name"), "selector": native.get("selector"), "class_method": native.get("class_method"),
                    "category_name": native.get("category_name") or "", "category_address": native.get("category_address") or "0x0",
                    "implementation": native.get("implementation"), "status": "unrecovered", "diagnostics": [],
                    "reason": "native method has no matching runtime metadata"}
            try:
                item["diagnostics"] = _diagnostics(native)
            except _Unrecovered as error:
                item["reason"] += f"; {error}"
            coverage.append(item)
    externals: dict[str, list[tuple[dict, tuple[str, ...]]]] = {}
    for item, _, _, declarations, _ in candidates:
        for symbol, spelling in declarations.items():
            externals.setdefault(symbol, []).append((item, spelling))
    for occurrences in externals.values():
        if len({spelling for _, spelling in occurrences}) > 1:
            for item, _ in occurrences:
                item.update(status="unrecovered", reason="conflicting external declarations across methods")
    shared_occurrences: dict[str, list[tuple[dict, tuple[str, str]]]] = {}
    for item, _, _, _, shared in candidates:
        for symbol, definition in shared.items():
            shared_occurrences.setdefault(symbol, []).append((item, definition))
    for occurrences in shared_occurrences.values():
        if len({definition for _, definition in occurrences}) != 1:
            for item, _ in occurrences:
                item.update(status="unrecovered", reason="conflicting shared Block function definitions")
    lines = ["// Objective-C bodies reconstructed from native code; see objc-methods.json for coverage.",
             "#include <stdint.h>", "#include <stdbool.h>", objc_header({"classes": safe_classes, "categories": external_categories, "pointer_size": pointer_size or 8})]
    methods_by_class: dict[tuple[str, str], list[str]] = {}
    shared_output: dict[str, tuple[str, str]] = {}
    for item, method, support, _, shared in candidates:
        if item["status"] == "recovered":
            lines.extend([support, ""])
            shared_output.update(shared)
            methods_by_class.setdefault((item["class_name"], item["category_name"]), []).append(method)
    # Publish prototypes first so the shared literal/descriptor/invoke graph
    # retains one storage identity even when several methods reference it.
    lines.extend(prototype for prototype, _ in shared_output.values())
    lines.extend(definition for _, definition in shared_output.values())
    for (name, category), methods in methods_by_class.items():
        suffix = f" ({category})" if category else ""
        lines.extend([f"@implementation {name}{suffix}", *methods, "@end", ""])
    recovered = sum(item["status"] == "recovered" for item in coverage)
    metadata_incomplete = objc_metadata.get("status") != "recovered"
    status = "no-methods" if not coverage else "unrecovered" if not recovered else "partial" if recovered != len(coverage) or metadata_incomplete else "recovered"
    result = {"schema_version": 1, "status": status, "method_count": len(coverage),
              "recovered_method_count": recovered, "unrecovered_method_count": len(coverage) - recovered,
              "methods": coverage, "limitations": [
                  "Objective-C method bodies are projections of native C; original source and method-level semantic equivalence are not guaranteed.",
                  "Only supported scalar and pointer runtime signatures with a verified native definition are emitted.",
                  "Runtime @? Block parameters are declared as id; their original invoke prototype is not encoded there. Block calls require a separately verified native signature.",
                  "Coverage applies only to the discovered runtime method inventory; an empty inventory does not prove that no methods exist.",
                  "Recovered C helper definitions and direct-call targets are retained; verified identical Block storage helpers share their object identity across methods. External dependencies may require manual linking.",
              ]}
    return "\n".join(lines).rstrip() + "\n", result
