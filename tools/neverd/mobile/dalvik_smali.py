"""Independent, bounded smali reader for the shared Dalvik source model.

PCs are ordinal positions; a data payload occupies one nonexecutable position.
Payload contents are attached to the referring instruction. Debug names/lines
and the validated MemberClasses inventory have no representation in the model.
Other annotations and unsupported executable syntax are rejected explicitly.
"""
from __future__ import annotations

from dataclasses import replace
import re
import struct

from .common import MobileError
from .dalvik_model import (Budget, Class, Field, Instruction, Method, TryRegion,
                           access_flags, descriptor, field_ref, method_ref,
                           prototype, width)

_LABEL = re.compile(r":[\w$.-]+", re.UNICODE)
_REGISTER = re.compile(r"([pv])(\d+)")
_NAME = re.compile(r'[^\s:;()/\[\]"\',{}=]+')
_ACCESS = {
    "class": set("public private protected static final interface abstract synthetic annotation enum".split()),
    "field": set("public private protected static final volatile transient synthetic enum".split()),
    "method": set("public private protected static final synchronized bridge varargs native abstract strictfp synthetic constructor declared-synchronized".split()),
}
_UNARY = {"neg-int", "not-int", "neg-long", "not-long", "neg-float", "neg-double"}
_CONVERSIONS = {f"{a}-to-{b}" for a in ("int", "long", "float", "double")
                for b in ("int", "long", "float", "double") if a != b} | {"int-to-byte", "int-to-char", "int-to-short"}
_BINARY = {f"{op}-{typ}" for typ in ("int", "long", "float", "double")
           for op in ("add", "sub", "mul", "div", "rem")} | {
               f"{op}-{typ}" for typ in ("int", "long") for op in ("and", "or", "xor", "shl", "shr", "ushr")}
_SUFFIXES = ("", "-wide", "-object", "-boolean", "-byte", "-char", "-short")
_FIELDS = {base + suffix for base in ("iget", "iput", "sget", "sput") for suffix in _SUFFIXES}
_ARRAYS = {base + suffix for base in ("aget", "aput") for suffix in _SUFFIXES}
_INVOKES = {"invoke-" + kind for kind in ("virtual", "super", "direct", "static", "interface")}


def _comment(line: str) -> str:
    quote = None
    escape = False
    for i, char in enumerate(line):
        if quote:
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char == "#":
            return line[:i].strip()
    if quote:
        raise MobileError("unterminated quoted smali literal")
    return line.strip()


def _parts(text: str) -> list[str]:
    """Split commas outside quoted strings and register/annotation lists."""
    result, start, level, quote, escape = [], 0, 0, None, False
    for i, char in enumerate(text):
        if quote:
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char == "{":
            level += 1
        elif char == "}":
            level -= 1
            if level < 0:
                raise MobileError("unbalanced smali operand list")
        elif char == "," and level == 0:
            result.append(text[start:i].strip())
            start = i + 1
    if quote or level:
        raise MobileError("unbalanced smali operand list")
    result.append(text[start:].strip())
    if any(not part for part in result):
        raise MobileError("empty smali operand")
    return result


def _quoted(value: str, quote: str = '"') -> str:
    if len(value) < 2 or value[0] != quote or value[-1] != quote:
        raise MobileError("expected a quoted smali literal")
    result, i = [], 1
    escapes = {"b": "\b", "t": "\t", "n": "\n", "f": "\f", "r": "\r", "'": "'", '"': '"', "\\": "\\"}
    while i < len(value) - 1:
        char = value[i]
        i += 1
        if char == quote or ord(char) < 32:
            raise MobileError("invalid character in smali literal")
        if char == "\\":
            if i >= len(value) - 1:
                raise MobileError("unfinished smali escape")
            char = value[i]
            i += 1
            if char == "u":
                digits = value[i:i + 4]
                if len(digits) != 4 or not re.fullmatch(r"[0-9a-fA-F]{4}", digits):
                    raise MobileError("invalid smali Unicode escape")
                char = chr(int(digits, 16))
                i += 4
            elif char in escapes:
                char = escapes[char]
            else:
                raise MobileError("unsupported smali escape")
        result.append(char)
    text = "".join(result)
    if quote == "'" and (len(text) != 1 or ord(text) > 0xffff):
        raise MobileError("smali character literal is not one UTF-16 code unit")
    return text


def _integer(value: str, bits: int = 64, *, signed: bool = True) -> int:
    if value.startswith("'"):
        number = ord(_quoted(value, "'"))
    else:
        match = re.fullmatch(r"([+-]?)(0[xX][0-9a-fA-F]+|[0-9]+)([tTsSlL]?)", value)
        if not match or len(match[2]) > 32:
            raise MobileError("invalid or oversized smali integer literal")
        if len(match[2]) > 1 and match[2][0] == "0" and match[2][1] not in "xX":
            raise MobileError("ambiguous leading-zero smali integer literal")
        number = int(match[2], 16 if match[2].lower().startswith("0x") else 10)
        if match[1] == "-":
            number = -number
        suffix_bits = {"t": 8, "s": 16, "l": 64}.get(match[3].lower())
        if suffix_bits and not -(1 << (suffix_bits - 1)) <= number < 1 << suffix_bits:
            raise MobileError("smali literal exceeds its declared suffix width")
        if suffix_bits and number >= 1 << (suffix_bits - 1):
            number -= 1 << suffix_bits
    if not -(1 << (bits - 1)) <= number < 1 << bits or not signed and number < 0:
        raise MobileError("smali integer literal exceeds its operand width")
    if signed and number >= 1 << (bits - 1):
        number -= 1 << bits
    return number


def _float(value: str) -> float:
    core = value[:-1] if value[-1:] in ("f", "F", "d", "D") else value
    if not re.fullmatch(r"[+-]?(?:(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?|0[xX][0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?[pP][+-]?\d+|Infinity|NaN)", core):
        raise MobileError("invalid smali floating literal")
    try:
        return float.fromhex(core) if "p" in core.lower() else float(core)
    except (ValueError, OverflowError) as error:
        raise MobileError("invalid smali floating literal") from error


def _bits(value: str, size: int) -> int:
    if ("." in value or "p" in value.lower() or re.search(r"[eE][+-]?\d", value) or re.fullmatch(r"[+-]?\d+[fFdD]", value)
            or "NaN" in value or "Infinity" in value) and not re.fullmatch(r"[+-]?0[xX][0-9a-fA-F]+[tTsSlL]?", value):
        if size not in (4, 8):
            raise MobileError("floating smali literal has a nonfloating width")
        literal_width = 4 if value[-1:] in ("f", "F") else 8
        if literal_width != size:
            raise MobileError("smali floating literal suffix disagrees with its raw carrier width")
        try:
            packed = struct.pack("<f" if size == 4 else "<d", _float(value))
        except (OverflowError, struct.error) as error:
            raise MobileError("smali floating literal exceeds its width") from error
        return int.from_bytes(packed, "little", signed=True)
    return _integer(value, size * 8)


def _class_type(value: str) -> str:
    result = descriptor(value)
    if not result.startswith("L"):
        raise MobileError("smali declaration requires a class descriptor")
    return result


def _access(tokens: list[str], kind: str) -> frozenset[str]:
    if len(set(tokens)) != len(tokens) or set(tokens) - _ACCESS[kind]:
        raise MobileError("unknown or duplicate smali access flag")
    if len(set(tokens) & {"public", "private", "protected"}) > 1:
        raise MobileError("conflicting smali visibility flags")
    return frozenset(tokens)


class _Reader:
    def __init__(self, text: str, input_id: str, budget: Budget):
        if not isinstance(text, str) or not isinstance(input_id, str):
            raise MobileError("invalid smali text or source identity")
        budget.tick(len(text) + 1)
        if len(text) > budget.limits.max_bytes or len(text.encode("utf-8", "surrogatepass")) > budget.limits.max_bytes or "\0" in text:
            raise MobileError("smali input is invalid or exceeds its byte budget")
        self.budget, self.input_id, self.position, self.line = budget, input_id, 0, 1
        self.lines = []
        for number, line in enumerate(text.splitlines(), 1):
            self.line = number
            clean = _comment(line)
            if clean:
                self.lines.append((number, clean))
        self.line = 1

    def take(self) -> str:
        if self.position == len(self.lines):
            raise MobileError("unexpected end of smali input")
        self.line, value = self.lines[self.position]
        self.position += 1
        self.budget.tick()
        return value

    def peek(self) -> str:
        return self.lines[self.position][1] if self.position < len(self.lines) else ""

    def annotation(self, header: str, cls: Class, seen: set[str]) -> None:
        match = re.fullmatch(r"\.annotation system (L[^\s]+;)", header)
        if not match:
            raise MobileError("unsupported smali annotation visibility or declaration")
        typ = _class_type(match[1])
        if typ in seen:
            raise MobileError("duplicate smali structural annotation")
        seen.add(typ)
        allowed = {"Ldalvik/annotation/InnerClass;", "Ldalvik/annotation/EnclosingClass;", "Ldalvik/annotation/MemberClasses;"}
        if typ not in allowed:
            raise MobileError("smali annotation is not representable in the source model")
        body = []
        while self.peek() != ".end annotation":
            body.append(self.take())
        self.take()
        if typ.endswith("/InnerClass;"):
            values = {}
            for line in body:
                key, sep, value = line.partition("=")
                key, value = key.strip(), value.strip()
                if not sep or key not in ("name", "accessFlags") or key in values:
                    raise MobileError("invalid InnerClass annotation member")
                values[key] = value
            if set(values) != {"name", "accessFlags"}:
                raise MobileError("incomplete InnerClass annotation")
            cls.inner_name = None if values["name"] == "null" else _quoted(values["name"])
            cls.inner_access = access_flags(_integer(values["accessFlags"], 32, signed=False))
            if cls.inner_access - _ACCESS["class"]:
                raise MobileError("invalid InnerClass access flags")
        elif typ.endswith("/EnclosingClass;"):
            match = re.fullmatch(r"value\s*=\s*(\S+)", " ".join(body))
            if not match:
                raise MobileError("invalid EnclosingClass annotation")
            cls.enclosing = _class_type(match[1])
            if cls.enclosing == cls.name:
                raise MobileError("class cannot enclose itself")
        else:
            match = re.fullmatch(r"value\s*=\s*\{(.*)\}", " ".join(body))
            if not match:
                raise MobileError("invalid MemberClasses annotation")
            values = []
            for value in _parts(match[1]) if match[1].strip() else []:
                self.budget.tick()
                values.append(_class_type(value))
            if len(values) != len(set(values)) or cls.name in values:
                raise MobileError("invalid MemberClasses identities")

    def field(self, line: str, owner: str) -> Field:
        declaration, sep, value = line[len(".field "):].partition("=")
        words = declaration.split()
        if not words or ":" not in words[-1]:
            raise MobileError("invalid smali field declaration")
        name, typ = words[-1].split(":", 1)
        if not _NAME.fullmatch(name):
            raise MobileError("invalid smali field name")
        ref = field_ref(owner + "->" + name + ":" + typ)
        flags = _access(words[:-1], "field")
        result = None
        if sep:
            value = value.strip()
            if "static" not in flags:
                raise MobileError("instance field cannot carry a smali encoded initializer")
            if typ.startswith(("L", "[")):
                if value != "null":
                    if typ != "Ljava/lang/String;":
                        raise MobileError("unsupported reference field initializer")
                    result = _quoted(value)
            elif typ == "Z":
                if value not in ("true", "false", "0", "1", "0x0", "0x1"):
                    raise MobileError("invalid boolean field initializer")
                result = value in ("true", "1", "0x1")
            elif typ in ("F", "D"):
                try:
                    packed = struct.pack("<f" if typ == "F" else "<d", _float(value))
                except (OverflowError, struct.error) as error:
                    raise MobileError("floating field initializer exceeds its width") from error
                result = {"kind": "float-bits" if typ == "F" else "double-bits",
                          "bits": int.from_bytes(packed, "little")}
            else:
                result = _integer(value, {"B": 8, "C": 16, "S": 16, "I": 32, "J": 64}[typ], signed=typ != "C")
        if self.peek().startswith(".annotation"):
            raise MobileError("field annotations are not represented in the source model")
        if self.peek() == ".end field":
            self.take()
        return Field(ref, flags, result)

    def debug(self, line: str) -> bool:
        if line in (".prologue", ".epilogue", ".end param"):
            return True
        if line.startswith(".source "):
            _quoted(line[8:].strip())
            return True
        if re.fullmatch(r"\.line \d+", line):
            return True
        if re.fullmatch(r"\.(?:end|restart) local [pv]\d+", line):
            return True
        if line.startswith(".param "):
            parts = _parts(line[7:])
            if len(parts) not in (1, 2) or not _REGISTER.fullmatch(parts[0]):
                raise MobileError("invalid smali parameter debug directive")
            if len(parts) == 2:
                _quoted(parts[1])
            return True
        if line.startswith(".local "):
            parts = _parts(line[7:])
            if len(parts) not in (2, 3) or not _REGISTER.fullmatch(parts[0]):
                raise MobileError("invalid smali local debug directive")
            match = re.fullmatch(r'(null|"(?:\\.|[^"\\])*"):(\S+)', parts[1])
            if not match:
                raise MobileError("invalid smali local type/name")
            if match[1] != "null":
                _quoted(match[1])
            descriptor(match[2])
            if len(parts) == 3:
                _quoted(parts[2])
            return True
        return False

    def method(self, header: str, owner: str) -> Method:
        words = header[len(".method "):].split()
        if not words or "(" not in words[-1]:
            raise MobileError("invalid smali method declaration")
        ref = method_ref(owner + "->" + words[-1])
        if not _NAME.fullmatch(ref.name):
            raise MobileError("invalid smali method name")
        flags = _access(words[:-1], "method")
        if ref.name in ("<init>", "<clinit>"):
            if ref.returns != "V" or ((ref.name == "<clinit>") != ("static" in flags)) or ref.name == "<clinit>" and ref.parameters:
                raise MobileError("invalid smali constructor signature")
        elif "constructor" in flags or "<" in ref.name or ">" in ref.name:
            raise MobileError("invalid smali constructor declaration")
        if "abstract" in flags and flags & {"native", "static", "private", "final"}:
            raise MobileError("invalid abstract smali method flags")
        method = Method(ref, flags, 0)
        total = None
        raw, labels, label_positions, payloads, catches = [], {}, set(), {}, []
        pc = 0
        while True:
            line = self.take()
            if line == ".end method":
                break
            if line.startswith((".locals ", ".registers ")):
                if total is not None or raw or payloads:
                    raise MobileError("duplicate or late smali register declaration")
                count = line.split()
                if len(count) != 2:
                    raise MobileError("invalid smali register declaration")
                total = _integer(count[1], 16, signed=False)
                if count[0] == ".locals":
                    total += method.incoming_words
                if not method.incoming_words <= total <= 65535:
                    raise MobileError("invalid smali register frame")
                method.registers = total
            elif line.startswith(":"):
                if not _LABEL.fullmatch(line) or line in labels:
                    raise MobileError("invalid or duplicate smali label")
                labels[line] = pc
                label_positions.add(pc)
            elif line.startswith((".catch ", ".catchall ")):
                match = re.fullmatch(r"\.(catch|catchall)\s+(?:(\S+)\s+)?\{(:[\w$.-]+)\s*\.\.\s*(:[\w$.-]+)\}\s+(:[\w$.-]+)", line)
                if not match or (match[1] == "catch") != bool(match[2]):
                    raise MobileError("invalid smali catch directive")
                catches.append((_class_type(match[2]) if match[2] else None, match[3], match[4], match[5]))
            elif line.startswith((".packed-switch ", ".sparse-switch", ".array-data ")):
                if pc not in label_positions:
                    raise MobileError("smali payload requires a preceding label")
                if raw and raw[-1][0] == pc - 1 and not raw[-1][1].split()[0].startswith(("return", "throw", "goto")):
                    raise MobileError("normal execution would fall through into a smali payload")
                payloads[pc] = self.payload(line)
                pc += 1
            elif self.debug(line):
                pass
            elif line.startswith("."):
                raise MobileError("unsupported smali method directive")
            else:
                raw.append((pc, line, self.line))
                pc += 1
        no_code = bool(flags & {"abstract", "native"})
        if no_code:
            if total is not None or raw or labels or payloads or catches:
                raise MobileError("abstract/native smali method has a body")
            return method
        if total is None or not raw or raw[0][0] != 0:
            raise MobileError("concrete smali method has no register frame or body")
        executable = {item[0] for item in raw}
        used_payloads = set()
        for ordinal, line, number in raw:
            self.budget.tick()
            self.line = number
            instruction = self.instruction(line, ordinal, method)
            target_label = instruction.target
            if target_label is not None:
                if target_label not in labels:
                    raise MobileError("undefined smali branch or payload label")
                target = labels[target_label]
                if instruction.opcode in ("packed-switch", "sparse-switch", "fill-array-data"):
                    if target not in payloads:
                        raise MobileError("smali instruction does not target a data payload")
                    kind, keys, targets, data, element_width = payloads[target]
                    if kind != instruction.opcode:
                        raise MobileError("smali payload kind disagrees with its instruction")
                    used_payloads.add(target)
                    resolved = []
                    for label in targets:
                        self.budget.tick()
                        if label not in labels or labels[label] not in executable:
                            raise MobileError("smali switch case has no executable target")
                        resolved.append(labels[label])
                    instruction = replace(instruction, target=target, keys=keys, targets=tuple(resolved), data=data, element_width=element_width)
                else:
                    if target not in executable:
                        raise MobileError("smali branch targets a nonexecutable boundary")
                    instruction = replace(instruction, target=target)
            method.instructions.append(instruction)
        if used_payloads != set(payloads):
            raise MobileError("unreferenced smali data payload cannot be preserved")
        grouped = {}
        for typ, start, end, handler in catches:
            self.budget.tick()
            if any(label not in labels for label in (start, end, handler)):
                raise MobileError("undefined smali exception boundary")
            a, b, target = labels[start], labels[end], labels[handler]
            if a not in executable or target not in executable or not a < b <= pc:
                raise MobileError("invalid smali exception region")
            handlers = grouped.setdefault((a, b), [])
            if any(t == typ or t is None for t, _ in handlers):
                raise MobileError("duplicate or unreachable smali exception handler")
            handlers.append((typ, target))
        method.tries = [TryRegion(a, b, tuple(handlers)) for (a, b), handlers in grouped.items()]
        method.code_end = pc
        self.budget.tick()
        return method

    def payload(self, header: str) -> tuple:
        words = header.split()
        kind = words[0][1:]
        if kind == "packed-switch" and len(words) == 2:
            start = _integer(words[1], 32)
        elif kind == "sparse-switch" and len(words) == 1:
            start = 0
        elif kind == "array-data" and len(words) == 2:
            start = _integer(words[1], 8, signed=False)
            if start not in (1, 2, 4, 8):
                raise MobileError("invalid smali array-data element width")
        else:
            raise MobileError("invalid smali payload header")
        keys, targets, data = [], [], []
        while self.peek() != ".end " + kind:
            line = self.take()
            if kind == "array-data":
                values = _parts(line)
                for value in values:
                    self.budget.tick()
                    data.append(_bits(value, start))
            elif kind == "packed-switch":
                if not _LABEL.fullmatch(line) or start + len(keys) >= 1 << 31:
                    raise MobileError("invalid packed-switch entry or key overflow")
                keys.append(start + len(keys))
                targets.append(line)
            else:
                match = re.fullmatch(r"(\S+)\s*->\s*(:[\w$.-]+)", line)
                if not match:
                    raise MobileError("invalid sparse-switch entry")
                key = _integer(match[1], 32)
                if keys and key <= keys[-1]:
                    raise MobileError("sparse-switch keys are duplicate or unsorted")
                keys.append(key)
                targets.append(match[2])
            if len(keys) > 65535:
                raise MobileError("smali switch exceeds its case-count limit")
        self.take()
        return ("fill-array-data" if kind == "array-data" else kind,
                tuple(keys), tuple(targets), tuple(data), start if kind == "array-data" else 0)

    def instruction(self, text: str, pc: int, method: Method) -> Instruction:
        # Tabs are whitespace too; splitting once retains quoted operand spaces.
        pieces = text.split(None, 1)
        opcode, operands = pieces[0], pieces[1] if len(pieces) == 2 else ""
        args = _parts(operands) if operands else []
        registers = []
        literal = target = reference = None
        def reg(value: str, bits: int = 8, wide: bool = False) -> int:
            match = _REGISTER.fullmatch(value)
            if not match or len(match[2]) > 5:
                raise MobileError("invalid smali register operand")
            number = int(match[2])
            if match[1] == "p":
                if number >= method.incoming_words:
                    raise MobileError("smali parameter register is out of range")
                number += method.registers - method.incoming_words
            if number >= method.registers or number >= 1 << bits or wide and number + 1 >= method.registers:
                raise MobileError("smali register exceeds its frame or instruction format")
            return number
        def regs(count: int, bits: int = 8, wide: tuple[int, ...] = ()) -> None:
            if len(args) != count:
                raise MobileError("wrong smali instruction operand count")
            registers.extend(reg(v, bits, i in wide) for i, v in enumerate(args))
        base = opcode.removesuffix("/range")
        if opcode in ("nop", "return-void"):
            regs(0)
        elif opcode in ("move-result", "move-result-wide", "move-result-object", "move-exception", "return", "return-wide", "return-object", "monitor-enter", "monitor-exit", "throw"):
            regs(1, wide=(0,) if opcode.endswith("-wide") else ())
        elif re.fullmatch(r"move(?:-wide|-object)?(?:/from16|/16)?", opcode):
            if len(args) != 2:
                raise MobileError("wrong smali move operand count")
            bits = (16, 16) if opcode.endswith("/16") else (8, 16) if opcode.endswith("/from16") else (4, 4)
            registers = [reg(v, bits[i], "-wide" in opcode) for i, v in enumerate(args)]
        elif opcode in _UNARY | _CONVERSIONS | {"array-length"}:
            wide = tuple(i for i, typ in enumerate(opcode.split("-to-") if "-to-" in opcode else [opcode, opcode]) if typ.endswith(("long", "double")))
            if "-to-" in opcode:
                wide = tuple(1-i for i in wide)
            regs(2, 4, wide)
        elif opcode in _BINARY or opcode.removesuffix("/2addr") in _BINARY and opcode.endswith("/2addr"):
            typ = opcode.split("-")[-1].split("/")[0]
            count = 2 if opcode.endswith("/2addr") else 3
            wide = tuple(range(count)) if typ in ("long", "double") else ()
            if typ == "long" and opcode.startswith(("shl-", "shr-", "ushr-")):
                wide = tuple(range(count-1))
            regs(count, 4 if count == 2 else 8, wide)
        elif opcode in {"cmp-long", "cmpl-float", "cmpg-float", "cmpl-double", "cmpg-double"}:
            regs(3, wide=(1, 2) if opcode.endswith(("long", "double")) else ())
        elif opcode in _ARRAYS:
            regs(3, wide=(0,) if opcode.endswith("-wide") else ())
        elif opcode in _FIELDS:
            count = 2 if opcode[0] == "s" else 3
            if len(args) != count:
                raise MobileError("wrong smali field operand count")
            reference = field_ref(args[-1])
            _class_type(reference.owner)
            if not _NAME.fullmatch(reference.name):
                raise MobileError("invalid smali field reference name")
            suffix = opcode.split("-", 1)[1] if "-" in opcode else ""
            valid_type = (reference.type in ("J", "D") if suffix == "wide" else
                          reference.type.startswith(("L", "[")) if suffix == "object" else
                          reference.type == {"boolean": "Z", "byte": "B", "char": "C", "short": "S"}[suffix] if suffix else
                          reference.type in ("I", "F"))
            if not valid_type:
                raise MobileError("smali field opcode disagrees with its referenced type")
            registers = [reg(v, 8 if count == 2 else 4, i == 0 and opcode.endswith("-wide")) for i, v in enumerate(args[:-1])]
        elif opcode.startswith("const") and opcode in {"const/4", "const/16", "const", "const/high16", "const-wide/16", "const-wide/32", "const-wide", "const-wide/high16", "const-string", "const-string/jumbo", "const-class", "const-method-type"}:
            if len(args) != 2:
                raise MobileError("wrong smali constant operand count")
            wide = opcode.startswith("const-wide")
            registers = [reg(args[0], 4 if opcode == "const/4" else 8, wide)]
            if opcode.startswith("const-string"):
                literal = _quoted(args[1])
            elif opcode == "const-class":
                reference = descriptor(args[1])
            elif opcode == "const-method-type":
                prototype(args[1]); reference = args[1]
            else:
                literal = _bits(args[1], 8 if wide else 4)
                if opcode.endswith("/high16"):
                    if literal & ((1 << (48 if wide else 16)) - 1):
                        raise MobileError("smali high16 constant has nonzero low bits")
                else:
                    bits = 4 if opcode.endswith("/4") else 16 if opcode.endswith("/16") else 32 if opcode.endswith("/32") or not wide else 64
                    if not -(1 << (bits-1)) <= literal < 1 << (bits-1):
                        raise MobileError("smali constant exceeds its instruction format")
        elif opcode in ("new-instance", "check-cast", "instance-of", "new-array"):
            count = 3 if opcode in ("instance-of", "new-array") else 2
            if len(args) != count:
                raise MobileError("wrong smali type operand count")
            reference = descriptor(args[-1])
            if opcode == "new-instance":
                _class_type(reference)
            elif opcode == "new-array" and not reference.startswith("["):
                raise MobileError("new-array requires an array descriptor")
            elif opcode in ("check-cast", "instance-of") and not reference.startswith(("L", "[")):
                raise MobileError("smali reference operation has a primitive type")
            registers = [reg(v, 4 if count == 3 else 8) for v in args[:-1]]
        elif base in _INVOKES | {"filled-new-array"}:
            if len(args) != 2 or not args[0].startswith("{") or not args[0].endswith("}"):
                raise MobileError("invalid smali invoke register list")
            contents = args[0][1:-1].strip()
            if opcode.endswith("/range") and contents:
                match = re.fullmatch(r"([pv]\d+)\s*\.\.\s*([pv]\d+)", contents)
                if not match:
                    raise MobileError("invalid smali register range")
                first, last = reg(match[1], 16), reg(match[2], 16)
                if last < first or last-first >= 255:
                    raise MobileError("invalid or oversized smali register range")
                registers = list(range(first, last+1))
            else:
                registers = [reg(v, 4) for v in _parts(contents)] if contents else []
                if len(registers) > 5:
                    raise MobileError("smali invoke needs a range format for more than five words")
            if base == "filled-new-array":
                reference = descriptor(args[1])
                if not reference.startswith("[") or reference[1:] in ("J", "D"):
                    raise MobileError("invalid filled-new-array element type")
            else:
                reference = method_ref(args[1])
                if not reference.owner.startswith(("L", "[")) or not _NAME.fullmatch(reference.name):
                    raise MobileError("invalid smali method reference")
                if reference.name == "<init>":
                    if base != "invoke-direct" or reference.returns != "V" or not reference.owner.startswith("L"):
                        raise MobileError("invalid smali constructor invocation")
                elif "<" in reference.name or ">" in reference.name:
                    raise MobileError("invalid special smali invocation name")
                expected = sum(width(p) for p in reference.parameters) + (base != "invoke-static")
                if len(registers) != expected:
                    raise MobileError("smali invoke word count disagrees with its prototype")
                at = int(base != "invoke-static")
                for typ in reference.parameters:
                    if width(typ) == 2 and registers[at+1] != registers[at]+1:
                        raise MobileError("wide smali invoke argument is not an adjacent register pair")
                    at += width(typ)
        elif opcode in ("goto", "goto/16", "goto/32"):
            if len(args) != 1 or not _LABEL.fullmatch(args[0]):
                raise MobileError("invalid smali goto target")
            target = args[0]
        elif opcode in {"if-" + op + suffix for op in ("eq", "ne", "lt", "ge", "gt", "le") for suffix in ("", "z")}:
            count = 2 if opcode.endswith("z") else 3
            if len(args) != count or not _LABEL.fullmatch(args[-1]):
                raise MobileError("invalid smali conditional branch")
            registers = [reg(v, 8 if count == 2 else 4) for v in args[:-1]]
            target = args[-1]
        elif opcode in ("packed-switch", "sparse-switch", "fill-array-data"):
            if len(args) != 2 or not _LABEL.fullmatch(args[1]):
                raise MobileError("invalid smali payload reference")
            registers, target = [reg(args[0])], args[1]
        elif opcode == "rsub-int" or re.fullmatch(r"(?:add|mul|div|rem|and|or|xor)-int/lit16|(?:add|rsub|mul|div|rem|and|or|xor|shl|shr|ushr)-int/lit8", opcode):
            if len(args) != 3:
                raise MobileError("invalid smali literal arithmetic operands")
            bits = 8 if opcode.endswith("/lit8") else 16
            registers = [reg(v, 8 if bits == 8 else 4) for v in args[:2]]
            literal = _integer(args[2], bits)
        else:
            raise MobileError("unsupported smali opcode: " + opcode)
        if opcode in ("return", "return-wide", "return-object", "return-void"):
            typ = method.reference.returns
            expected = "return-void" if typ == "V" else "return-wide" if typ in ("J", "D") else "return-object" if typ.startswith(("L", "[")) else "return"
            if opcode != expected:
                raise MobileError("smali return opcode disagrees with its method prototype")
        return Instruction(pc, opcode, tuple(registers), literal, target, reference)

    def parse(self) -> Class:
        header = self.take().split()
        if len(header) < 2 or header[0] != ".class":
            raise MobileError("smali input must begin with one class declaration")
        cls = Class(_class_type(header[-1]), None, _access(header[1:-1], "class"), self.input_id)
        seen_annotations, methods, fields, interfaces = set(), set(), set(), set()
        superclass = source = False
        while self.peek():
            line = self.take()
            if line.startswith(".super "):
                if superclass:
                    raise MobileError("duplicate smali superclass")
                cls.superclass = _class_type(line[7:].strip())
                if cls.superclass == cls.name:
                    raise MobileError("class cannot extend itself")
                superclass = True
            elif line.startswith(".implements "):
                typ = _class_type(line[12:].strip())
                if typ in interfaces:
                    raise MobileError("duplicate smali interface")
                interfaces.add(typ)
                cls.interfaces.append(typ)
            elif line.startswith(".source "):
                if source:
                    raise MobileError("duplicate smali source directive")
                _quoted(line[8:].strip()); source = True
            elif line.startswith(".annotation "):
                self.annotation(line, cls, seen_annotations)
            elif line.startswith(".field "):
                item = self.field(line, cls.name)
                key = (item.reference.name, item.reference.type)
                if key in fields:
                    raise MobileError("duplicate smali field")
                fields.add(key); cls.fields.append(item)
            elif line.startswith(".method "):
                item = self.method(line, cls.name)
                if item.reference.identity in methods:
                    raise MobileError("duplicate smali method")
                methods.add(item.reference.identity); cls.methods.append(item)
            else:
                raise MobileError("unknown or misplaced smali class syntax")
        if not superclass and cls.name != "Ljava/lang/Object;":
            raise MobileError("smali class has no superclass declaration")
        self.budget.tick()
        return cls


def parse_smali(text: str, *, input_id: str, budget: Budget) -> Class:
    reader = None
    try:
        reader = _Reader(text, input_id, budget)
        return reader.parse()
    except MobileError as error:
        line = reader.line if reader else 1
        raise MobileError(f"smali {input_id}:{line}: {error}") from error
