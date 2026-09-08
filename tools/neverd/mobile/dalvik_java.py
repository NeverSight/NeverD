"""Verify Dalvik register flow and emit independently executable Java bodies."""
from __future__ import annotations

from collections import deque
import json
import re

from .common import MobileError
from .dalvik_model import Budget, Class, FieldRef, Instruction, Method, MethodRef, width


_KEYWORDS = set(("abstract assert boolean break byte case catch char class const continue default "
                 "do double else enum extends final finally float for goto if implements import "
                 "instanceof int interface long native new package private protected public return "
                 "short static strictfp super switch synchronized this throw throws transient try "
                 "void volatile while true false null _ record sealed permits yield var").split())
_PRIMITIVES = dict(zip("VZBCSIJFD", ("void", "boolean", "byte", "char", "short", "int", "long", "float", "double")))
_UNDEF = frozenset({"?"})
_THROWING = ("invoke-", "new-", "filled-", "aget", "aput", "iget", "iput", "sget", "sput",
             "array-length", "fill-array-data", "check-cast", "throw", "div-int", "rem-int",
             "div-long", "rem-long", "monitor-", "const-string", "const-class")


class _Lines(list):
    """Bound generated fragments before retaining or joining them."""
    def __init__(self, budget, initial=(), *, cumulative=False):
        super().__init__()
        self.budget, self.cumulative, self.bytes = budget, cumulative, 0
        self.extend(initial)

    def append(self, value):
        size = len(value.encode("utf-8")) + 1
        self.bytes += size
        if self.bytes > self.budget.limits.max_bytes:
            raise MobileError("Android generated source exceeded its byte budget")
        if self.cumulative:
            self.budget.output(size)
        super().append(value)

    def extend(self, values):
        for value in values:
            self.append(value)

    def __iadd__(self, values):
        self.extend(values)
        return self


def identifier(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z_$][A-Za-z0-9_$]*", value) or value in _KEYWORDS:
        raise MobileError(f"Android identifier cannot be represented in Java: {value!r}")
    return value


def string(value: str) -> str:
    # Java processes Unicode escapes before tokenization. Ordinary JSON escapes
    # protect backslash/u text, while non-ASCII UTF-16 code units stay explicit.
    return json.dumps(value, ensure_ascii=True)


def type_name(typ: str, classes: dict[str, Class]) -> str:
    if typ.startswith("["):
        return type_name(typ[1:], classes) + "[]"
    if typ in _PRIMITIVES:
        return _PRIMITIVES[typ]
    if not typ.startswith("L") or not typ.endswith(";"):
        raise MobileError("invalid Java projection type")
    cls = classes.get(typ)
    if cls and cls.enclosing:
        if not cls.inner_name or cls.enclosing not in classes:
            raise MobileError("nested Android type has no available enclosing declaration")
        return type_name(cls.enclosing, classes) + "." + identifier(cls.inner_name)
    return ".".join(identifier(part) for part in typ[1:-1].split("/"))


def _ref(typ: str) -> bool:
    return typ.startswith(("L", "[", "self:"))


def _wide(typ: str) -> bool:
    return typ in {"J", "D", "bits64"}


def _literal(value: int, bits: int) -> str:
    value &= (1 << bits) - 1
    return f"0x{value:0{bits // 4}x}" + ("L" if bits == 64 else "")


def _member(ref, classes, budget):
    """Resolve known declarations without allowing Java to silently rebind them."""
    if ref.owner not in classes:
        return None
    pending, seen = [ref.owner], set()
    while pending:
        budget.tick()
        owner = pending.pop(0)
        if owner in seen or owner not in classes:
            continue
        seen.add(owner)
        cls = classes[owner]
        candidates = cls.methods if isinstance(ref, MethodRef) else cls.fields
        for member in candidates:
            actual = member.reference
            if actual.name != ref.name:
                continue
            if isinstance(ref, MethodRef):
                if actual.parameters != ref.parameters:
                    continue
                matches = actual.returns == ref.returns
            else:
                matches = actual.type == ref.type
            if not matches:
                raise MobileError(f"{ref.owner}: member type differs from the exact referenced declaration")
            return member
        if isinstance(ref, MethodRef) and ref.name == "<init>":
            break  # Constructors are never inherited.
        pending.extend([cls.superclass, *cls.interfaces])
    raise MobileError(f"{ref.owner}: referenced member has no proven local declaration")


def _helper_name(cls, base):
    names = {m.reference.name for m in cls.methods}
    while base in names:
        base += "_"
    return base


def _constructor_throws(method, classes, budget, seen=None):
    """Carry an external super constructor's unknown checked exceptions outward."""
    seen = set() if seen is None else seen
    while True:
        budget.tick()
        if method.reference in seen:
            raise MobileError("recursive constructor delegation")
        seen.add(method.reference)
        first = method.instructions[0] if method.instructions else None
        if not first or not isinstance(first.reference, MethodRef) or first.reference.name != "<init>":
            return False  # Body validation diagnoses a missing leading initializer.
        ref = first.reference
        target = _member(ref, classes, budget)
        if target is None:
            return ref != MethodRef("Ljava/lang/Object;", "<init>", (), "V")
        method = target


class Body:
    def __init__(self, method: Method, classes: dict[str, Class], budget: Budget):
        self.method, self.classes, self.budget = method, classes, budget
        self.code = method.instructions
        self.by_pc = {op.pc: i for i, op in enumerate(self.code)}
        self.states: dict[int, tuple[frozenset[str], ...]] = {}
        self.prefix = ""
        self.first = 0
        self.receivers = {}
        self.pending_results = {}
        self.constructors = {}
        self._validate_shape()

    def fail(self, message: str):
        raise MobileError(f"{self.method.reference.identity}: {message}")

    def _validate_shape(self):
        m = self.method
        if not self.code or len(self.by_pc) != len(self.code):
            self.fail("missing or duplicate instruction positions")
        if any(a.pc >= b.pc for a, b in zip(self.code, self.code[1:])) or m.code_end <= self.code[-1].pc:
            self.fail("invalid method code boundaries")
        if m.registers > 1024:
            self.fail("register frame exceeds the bounded Java projection limit")
        for op in self.code:
            self.budget.tick()
            if any(type(r) is not int or not 0 <= r < m.registers for r in op.registers):
                self.fail("instruction register is outside its frame")
            targets = op.targets if "switch" in op.opcode else ((op.target,) if op.opcode.startswith(("goto", "if-")) else ())
            if any(target not in self.by_pc for target in targets):
                self.fail("branch does not target an executable instruction")
            if "switch" in op.opcode and (len(op.keys) != len(op.targets) or len(set(op.keys)) != len(op.keys)):
                self.fail("switch keys and destinations disagree")
        previous_end = -1
        for region in m.tries:
            if (region.start not in self.by_pc or region.end not in {*self.by_pc, m.code_end}
                    or region.start >= region.end or region.start < previous_end or not region.handlers):
                self.fail("invalid or overlapping exception regions")
            previous_end = region.end
            for index, (typ, target) in enumerate(region.handlers):
                if target not in self.by_pc or self.code[self.by_pc[target]].opcode != "move-exception":
                    self.fail("exception handler must start with move-exception")
                if typ is None and index != len(region.handlers) - 1:
                    self.fail("catch-all must be the final exception handler")
        for i, op in enumerate(self.code):
            if op.opcode.startswith("move-result"):
                if i == 0 or not self.code[i - 1].opcode.startswith(("invoke-", "filled-new-array")):
                    self.fail("move-result does not immediately follow its producer")
                previous = self.code[i - 1]
                typ = previous.reference.returns if isinstance(previous.reference, MethodRef) else previous.reference
                if not isinstance(typ, str) or typ == "V":
                    self.fail("move-result has no value-producing signature")
                wanted = "move-result-object" if _ref(typ) else "move-result-wide" if width(typ) == 2 else "move-result"
                if op.opcode != wanted:
                    self.fail("move-result carrier disagrees with the native prototype")
                self.pending_results[op.pc] = typ
        if m.reference.name == "<init>":
            first = self.code[0]
            self_reg = m.registers - m.incoming_words
            if (first.opcode not in {"invoke-direct", "invoke-direct/range"}
                    or not isinstance(first.reference, MethodRef) or first.reference.name != "<init>"
                    or not first.registers or first.registers[0] != self_reg):
                self.fail("constructor requires a representable leading super/this call")
            cls = self.classes[m.reference.owner]
            if first.reference.owner not in {cls.name, cls.superclass}:
                self.fail("constructor receiver does not match its owner or superclass")
            self.validate_invocation(first)
            original = {}
            reg = self_reg + 1
            for index, typ in enumerate(m.reference.parameters):
                original[reg] = (typ, f"arg{index}")
                reg += width(typ)
            args, cursor = [], 1
            for typ in first.reference.parameters:
                if cursor >= len(first.registers) or first.registers[cursor] not in original:
                    self.fail("constructor prefix requires unavailable argument evaluation")
                actual, name = original[first.registers[cursor]]
                if actual != typ:
                    self.fail("constructor prefix argument types disagree")
                args.append(name)
                if width(typ) == 2 and (cursor + 1 >= len(first.registers) or first.registers[cursor + 1] != first.registers[cursor] + 1):
                    self.fail("constructor prefix has a broken wide register pair")
                cursor += width(typ)
            if cursor != len(first.registers):
                self.fail("constructor prefix argument count disagrees")
            self.prefix = ("this" if first.reference.owner == cls.name else "super") + "(" + ", ".join(args) + ");"
            self.first = 1
            if self.first == len(self.code):
                self.fail("constructor has no return boundary")
            if m.tries and m.tries[0].start == first.pc:
                self.fail("constructor prefix has unrepresentable exception handling")

    def successors(self, index: int) -> list[int]:
        op = self.code[index]
        if op.opcode.startswith(("return", "throw")):
            return []
        if op.opcode.startswith("goto"):
            return [op.target]
        result = [self.code[index + 1].pc] if index + 1 < len(self.code) else []
        if not result:
            self.fail("method can fall through its code boundary")
        if op.opcode.startswith("if-"):
            result.append(op.target)
        elif "switch" in op.opcode:
            result.extend(op.targets)
        return list(dict.fromkeys(result))

    def handlers(self, pc: int):
        for region in self.method.tries:
            if region.start <= pc < region.end:
                return region.handlers
        return ()

    def compatible(self, kind: str, wanted: str) -> bool:
        if kind.startswith("self:"):
            kind = kind[5:]
        if kind == "?":
            return False
        if wanted == "I":
            return kind in {"I", "Z", "B", "S", "C", "bits32", "zero"}
        if wanted in {"Z", "B", "S", "C"}:
            return self.compatible(kind, "I")
        if wanted == "F":
            return kind in {"F", "bits32", "zero"}
        if wanted in {"J", "D"}:
            return kind in {wanted, "bits64"}
        if _ref(wanted):
            if kind in {"zero", "null"}:
                return True
            if not _ref(kind):
                return False
            if wanted == "Ljava/lang/Object;" or wanted == kind:
                return True
            seen = set()
            while kind in self.classes and kind not in seen:
                seen.add(kind)
                cls = self.classes[kind]
                if wanted in cls.interfaces:
                    return True
                kind = cls.superclass
                if kind == wanted:
                    return True
            return False
        return kind == wanted

    def read(self, state, reg: int, typ: str, *, strict: bool):
        kinds = state[reg]
        if strict and (not all(self.compatible(kind, typ) for kind in kinds)
                       or width(typ) == 2 and (reg + 1 >= len(state) or state[reg + 1] != frozenset({"wide-high"}))):
            self.fail(f"undefined or incompatible register v{reg} for {typ}")
        if _ref(typ):
            return f"(({type_name(typ, self.classes)}) o{reg})"
        if typ == "F":
            return f"((java.lang.Float) null).intBitsToFloat(v{reg})"
        if typ == "D":
            return f"((java.lang.Double) null).longBitsToDouble(w{reg})"
        if typ == "J":
            return f"w{reg}"
        if typ == "Z":
            return f"(v{reg} != 0)"
        if typ in {"B", "S", "C"}:
            return f"(({_PRIMITIVES[typ]}) v{reg})"
        return f"v{reg}"

    def write(self, state: list, reg: int, typ: str, expression: str) -> str:
        size = 2 if width(typ) == 2 or typ == "bits64" else 1
        if reg + size > len(state):
            self.fail("wide result exceeds its register frame")
        # Retire both words of every old pair intersecting the write. Clearing
        # only the overwritten word leaves an orphan high marker that can later
        # invalidate an unrelated, newly formed pair.
        pairs = [low for low in range(max(0, reg - 1), min(len(state) - 1, reg + size))
                 if any(_wide(kind) for kind in state[low])]
        for low in pairs:
            state[low] = state[low + 1] = _UNDEF
        if size == 2:
            state[reg + 1] = frozenset({"wide-high"})
        state[reg] = frozenset({typ})
        if _ref(typ) or typ.startswith("new:") or typ == "null":
            return f"o{reg} = {expression};"
        if typ == "F":
            return f"v{reg} = ((java.lang.Float) null).floatToRawIntBits({expression});"
        if typ == "D":
            return f"w{reg} = ((java.lang.Double) null).doubleToRawLongBits({expression});"
        if typ in {"J", "bits64"}:
            return f"w{reg} = {expression};"
        if typ == "Z":
            return f"v{reg} = ({expression}) ? 1 : 0;"
        if typ == "zero":
            return f"v{reg} = 0; o{reg} = null;"
        return f"v{reg} = {expression};"

    def validate_invocation(self, op):
        ref = op.reference
        if not isinstance(ref, MethodRef):
            self.fail("invocation has no method identity")
        style = op.opcode.split("/")[0]
        if style not in {"invoke-static", "invoke-virtual", "invoke-interface", "invoke-direct", "invoke-super"}:
            self.fail("unsupported invocation dispatch")
        if ref.name == "<init>" and (style != "invoke-direct" or ref.returns != "V"):
            self.fail("constructor has invalid dispatch or return type")
        declaration = _member(ref, self.classes, self.budget)
        if declaration:
            static = "static" in declaration.access
            if static != (style == "invoke-static"):
                self.fail("invocation dispatch disagrees with the declared static/instance kind")
            private = "private" in declaration.access
            if style == "invoke-direct" and ref.name != "<init>" and not private:
                self.fail("direct invocation requires a private method or constructor")
            if private and style not in {"invoke-direct", "invoke-static"}:
                self.fail("virtual invocation cannot dispatch to a private method")
            interface = "interface" in self.classes[ref.owner].access
            if style in {"invoke-interface", "invoke-virtual"} and interface != (style == "invoke-interface"):
                self.fail("invocation dispatch disagrees with the declared class/interface kind")
        if style == "invoke-super" and ref.owner != self.classes[self.method.reference.owner].superclass:
            self.fail("super invocation cannot be proven to bind the immediate superclass")
        return declaration

    def static_owner(self, owner, *, field=False):
        qualified = type_name(owner, self.classes)
        cls = self.classes.get(owner)
        if field or cls and "interface" not in cls.access:
            # A type-context cast cannot bind to a same-named field or local.
            # The pure null qualifier of a static member is never dereferenced.
            return f"(({qualified}) null)"
        # Java requires interface static methods to use a type qualifier. For
        # unavailable declarations we cannot assume the owner is a class.
        root = qualified.split(".")[0]
        names = {"pc", "caught", "failure", "result32", "result64", "resultObject"}
        names.update(f"arg{i}" for i in range(len(self.method.reference.parameters)))
        names.update(f"{prefix}{i}" for prefix in ("v", "w", "o") for i in range(self.method.registers))
        current, seen = self.method.reference.owner, set()
        while current in self.classes and current not in seen:
            self.budget.tick()
            seen.add(current)
            names.update(f.reference.name for f in self.classes[current].fields)
            current = self.classes[current].superclass
        if root in names:
            self.fail("static method type qualifier is shadowed without a proven class binding")
        return qualified

    def _array_type(self, state, reg, *, strict):
        kinds = state[reg] - {"zero", "null", "?"}
        if len(kinds) == 1 and next(iter(kinds)).startswith("["):
            return next(iter(kinds))
        if strict:
            self.fail("array operation lacks one proven element type")
        return "[I"

    def operation(self, op: Instruction, incoming, *, strict: bool):
        self.budget.tick()
        state, lines = list(incoming), _Lines(self.budget)
        name, regs = op.opcode, op.registers
        base = name.split("/")[0]
        def arity(n):
            if len(regs) != n:
                self.fail(f"wrong register count for {name}")
        def read(reg, typ):
            return self.read(incoming, reg, typ, strict=strict)
        def put(reg, typ, value):
            lines.append(self.write(state, reg, typ, value))
        if name == "nop":
            arity(0)
        elif base in {"move", "move-object", "move-wide"}:
            arity(2)
            kinds = incoming[regs[1]]
            if base == "move-object":
                if strict and not all(_ref(k) or k in {"zero", "null"} or k.startswith("new:") for k in kinds):
                    self.fail("move-object reads an undefined or scalar carrier")
                put(regs[0], "Ljava/lang/Object;", f"o{regs[1]}")
            elif base == "move-wide":
                if strict and (not all(_wide(k) for k in kinds) or regs[1] + 1 >= len(incoming) or incoming[regs[1] + 1] != frozenset({"wide-high"})):
                    self.fail("move-wide reads an undefined or broken wide carrier")
                put(regs[0], "bits64", f"w{regs[1]}")
            else:
                if strict and not all(k in {"I", "B", "C", "S", "Z", "F", "bits32", "zero"} for k in kinds):
                    self.fail("move reads an undefined or incompatible carrier")
                put(regs[0], "bits32", f"v{regs[1]}")
                if "zero" in kinds:
                    lines.append(f"o{regs[0]} = o{regs[1]};")
            state[regs[0]] = kinds
        elif name.startswith("move-result"):
            arity(1)
            typ = self.pending_results[op.pc]
            put(regs[0], typ, self._result_read(typ))
        elif name == "move-exception":
            arity(1)
            catches = [typ or "Ljava/lang/Throwable;" for region in self.method.tries for typ, pc in region.handlers if pc == op.pc]
            if not catches:
                self.fail("move-exception is outside a handler entry")
            put(regs[0], "Ljava/lang/Throwable;", "caught")
            state[regs[0]] = frozenset(catches)
        elif name.startswith("const-string"):
            arity(1)
            if not isinstance(op.literal, str):
                self.fail("const-string lacks its decoded string")
            put(regs[0], "Ljava/lang/String;", string(op.literal))
        elif name == "const-class":
            arity(1)
            put(regs[0], "Ljava/lang/Class;", type_name(op.reference, self.classes) + ".class")
        elif name.startswith("const"):
            arity(1)
            if type(op.literal) is not int:
                self.fail("constant has no exact integer bits")
            wide = name.startswith("const-wide")
            put(regs[0], "bits64" if wide else "zero" if op.literal == 0 else "bits32", _literal(op.literal, 64 if wide else 32))
        elif base.startswith("return"):
            typ = self.method.reference.returns
            if name == "return-void":
                arity(0)
                if typ != "V":
                    self.fail("void return disagrees with method signature")
                lines.append("break dispatch;" if self.method.reference.name == "<clinit>" else "return;")
            else:
                arity(1)
                expected = "return-object" if _ref(typ) else "return-wide" if width(typ) == 2 else "return"
                if typ == "V" or name != expected:
                    self.fail("return carrier disagrees with method signature")
                lines.append("return " + read(regs[0], typ) + ";")
        elif name == "array-length":
            arity(2)
            typ = self._array_type(incoming, regs[1], strict=strict)
            put(regs[0], "I", read(regs[1], typ) + ".length")
        elif name == "new-array":
            arity(2)
            typ = op.reference
            if not isinstance(typ, str) or not typ.startswith("["):
                self.fail("new-array has no array type")
            dimension = len(typ) - len(typ.lstrip("["))
            put(regs[0], typ, "new " + type_name(typ[dimension:], self.classes) + "[" + read(regs[1], "I") + "]" + "[]" * (dimension - 1))
        elif name in {"filled-new-array", "filled-new-array/range"}:
            typ = op.reference
            if not isinstance(typ, str) or not typ.startswith("[") or typ[1:] in {"J", "D"}:
                self.fail("filled-new-array requires single-word elements")
            values = ", ".join(read(reg, typ[1:]) for reg in regs)
            lines.append("resultObject = new " + type_name(typ, self.classes) + " {" + values + "};")
        elif name == "new-instance":
            arity(1)
            if not isinstance(op.reference, str) or not op.reference.startswith("L"):
                self.fail("new-instance requires a class descriptor")
            index = self.by_pc[op.pc]
            if index + 1 >= len(self.code):
                self.fail("new-instance has no initializing invocation")
            following = self.code[index + 1]
            if (following.opcode not in {"invoke-direct", "invoke-direct/range"}
                    or not isinstance(following.reference, MethodRef) or following.reference.name != "<init>"
                    or not following.registers or following.registers[0] != regs[0]
                    or following.reference.owner != op.reference
                    or self.handlers(op.pc) != self.handlers(following.pc)):
                self.fail("allocation requires an adjacent initializer in the same exception region")
            self.write(state, regs[0], f"new:{op.pc}:{op.reference}", "null")
            self.constructors[following.pc] = op.pc
        elif name == "fill-array-data":
            arity(1)
            typ = self._array_type(incoming, regs[0], strict=strict)
            element = typ[1:]
            expected_width = {"Z": 1, "B": 1, "C": 2, "S": 2, "I": 4, "F": 4, "J": 8, "D": 8}.get(element)
            if op.element_width != expected_width:
                self.fail("array payload element width disagrees with its array type")
            address = read(regs[0], typ)
            # Check the complete range before writing, as the bytecode payload
            # operation does; an oversized payload must not partially modify it.
            lines.append(f"if ({address}.length < {len(op.data)}) throw new java.lang.ArrayIndexOutOfBoundsException();")
            for index, value in enumerate(op.data):
                self.budget.tick()
                if element == "F":
                    expr = "((java.lang.Float) null).intBitsToFloat(" + _literal(value, 32) + ")"
                elif element == "D":
                    expr = "((java.lang.Double) null).longBitsToDouble(" + _literal(value, 64) + ")"
                elif element == "Z":
                    if value not in (0, 1):
                        self.fail("boolean array payload contains a non-boolean value")
                    expr = "true" if value else "false"
                else:
                    expr = f"({_PRIMITIVES[element]}) " + _literal(value, 64 if element == "J" else 32)
                lines.append(f"{address}[{index}] = {expr};")
        elif name.startswith(("aget", "aput")):
            arity(3)
            typ = self._array_type(incoming, regs[1], strict=strict)
            element = typ[1:]
            suffix = "-object" if _ref(element) else "-wide" if element in {"J", "D"} else {"Z": "-boolean", "B": "-byte", "C": "-char", "S": "-short"}.get(element, "")
            if name != name[:4] + suffix and strict:
                self.fail("array opcode disagrees with its element width/type")
            address = read(regs[1], typ) + "[" + read(regs[2], "I") + "]"
            if name.startswith("aget"):
                put(regs[0], element, address)
            else:
                lines.append(address + " = " + read(regs[0], element) + ";")
        elif name == "check-cast":
            arity(1)
            read(regs[0], "Ljava/lang/Object;")
            put(regs[0], op.reference, f"(({type_name(op.reference, self.classes)}) o{regs[0]})")
        elif name == "instance-of":
            arity(2)
            put(regs[0], "Z", read(regs[1], "Ljava/lang/Object;") + " instanceof " + type_name(op.reference, self.classes))
        elif name.startswith(("iget", "iput", "sget", "sput")):
            ref = op.reference
            if not isinstance(ref, FieldRef):
                self.fail("field instruction has no field reference")
            static = name.startswith("s")
            arity(1 if static else 2)
            suffix = "-object" if _ref(ref.type) else "-wide" if width(ref.type) == 2 else {"Z": "-boolean", "B": "-byte", "C": "-char", "S": "-short"}.get(ref.type, "")
            if name[4:] != suffix:
                self.fail("field opcode disagrees with its type")
            declaration = _member(ref, self.classes, self.budget)
            if declaration and static != ("static" in declaration.access):
                self.fail("field opcode disagrees with the declared static/instance kind")
            address = (self.static_owner(ref.owner, field=True) if static else read(regs[1], ref.owner)) + "." + identifier(ref.name)
            if name[1:4] == "get":
                put(regs[0], ref.type, address)
            else:
                if declaration and "final" in declaration.access:
                    self.fail("final field assignment requires a structured initialization proof")
                lines.append(address + " = " + read(regs[0], ref.type) + ";")
        elif name.startswith("invoke-"):
            ref = op.reference
            if not isinstance(ref, MethodRef) or ref.name.startswith("<") and ref.name != "<init>":
                self.fail("unmodelled constructor or dynamic invocation")
            self.validate_invocation(op)
            static = name.startswith("invoke-static")
            cursor, args = (0 if static else 1), []
            if not static and not regs:
                self.fail("instance invocation lacks its receiver")
            for typ in ref.parameters:
                if cursor >= len(regs):
                    self.fail("invocation has too few argument words")
                args.append(read(regs[cursor], typ))
                if width(typ) == 2 and (cursor + 1 >= len(regs) or regs[cursor + 1] != regs[cursor] + 1):
                    self.fail("invocation has a broken wide argument")
                cursor += width(typ)
            if cursor != len(regs):
                self.fail("invocation has too many argument words")
            if ref.name == "<init>":
                new_pc = self.constructors.get(op.pc)
                expected = f"new:{new_pc}:{ref.owner}"
                if (new_pc is None or incoming[regs[0]] != frozenset({expected})
                        or ref.returns != "V"):
                    self.fail("initializer is not bound to its uninitialized allocation")
                put(regs[0], ref.owner, "new " + type_name(ref.owner, self.classes) + "(" + ", ".join(args) + ")")
                return tuple(state), lines
            if name.startswith("invoke-super"):
                if ("static" in self.method.access
                        or incoming[regs[0]] != frozenset({"self:" + self.method.reference.owner})):
                    self.fail("super invocation lacks its actual receiver")
                owner = "super"
            else:
                owner = self.static_owner(ref.owner) if static else read(regs[0], ref.owner)
            call = owner + "." + identifier(ref.name) + "(" + ", ".join(args) + ")"
            if ref.returns == "V":
                lines.append(call + ";")
            else:
                lines.append(self._result_write(ref.returns, call))
        elif name.startswith(("goto", "if-")) or "switch" in name:
            if name.startswith("if-"):
                self.condition(op, incoming, strict=strict)
            elif "switch" in name:
                arity(1)
                read(regs[0], "I")
        elif name == "throw":
            arity(1)
            value = read(regs[0], "Ljava/lang/Object;")
            lines.append(f"throw {self.helper_name()}((java.lang.Throwable) {value});")
        elif re.fullmatch(r"(?:neg|not)-(?:int|long|float|double)", name):
            arity(2)
            typ = {"int": "I", "long": "J", "float": "F", "double": "D"}[name.split("-")[1]]
            if name.startswith("not") and typ not in {"I", "J"}:
                self.fail("invalid floating bitwise negation")
            put(regs[0], typ, ("~" if name.startswith("not") else "-") + "(" + read(regs[1], typ) + ")")
        elif re.fullmatch(r"(?:int|long|float|double)-to-(?:int|long|float|double|byte|char|short)", name):
            arity(2)
            types = {"int": "I", "long": "J", "float": "F", "double": "D", "byte": "B", "char": "C", "short": "S"}
            source, dest = (types[x] for x in name.split("-to-"))
            put(regs[0], dest, f"(({_PRIMITIVES[dest]}) ({read(regs[1], source)}))")
        elif name.startswith(("cmp-long", "cmpl-", "cmpg-")):
            arity(3)
            typ = "J" if name == "cmp-long" else "F" if name.endswith("float") else "D"
            a, b = read(regs[1], typ), read(regs[2], typ)
            expr = f"(({a}) > ({b}) ? 1 : ({a}) == ({b}) ? 0 : -1)"
            if name.startswith("cmpg"):
                expr = f"(({a}) < ({b}) ? -1 : ({a}) == ({b}) ? 0 : 1)"
            put(regs[0], "I", expr)
        else:
            match = re.fullmatch(r"(add|sub|rsub|mul|div|rem|and|or|xor|shl|shr|ushr)-(int|long|float|double)(/2addr|/lit8|/lit16)?", name)
            if not match:
                self.fail(f"unsupported instruction: {name}")
            operator, typename, form = match.groups()
            if operator == "rsub" and form is None and op.literal is not None:
                form = "/lit16"
            typ = {"int": "I", "long": "J", "float": "F", "double": "D"}[typename]
            if operator in {"and", "or", "xor", "shl", "shr", "ushr", "rsub"} and typ not in {"I", "J"}:
                self.fail("invalid arithmetic carrier")
            arity(2 if form else 3)
            left = read(regs[0] if form == "/2addr" else regs[1], typ)
            if form in {"/lit8", "/lit16"}:
                if typ != "I" or type(op.literal) is not int:
                    self.fail("literal operation has no signed integer literal")
                right = _literal(op.literal, 32)
            else:
                right = read(regs[1] if form == "/2addr" else regs[2], "I" if operator in {"shl", "shr", "ushr"} else typ)
            if operator == "rsub":
                left, right = right, left
            symbol = {"add": "+", "sub": "-", "rsub": "-", "mul": "*", "div": "/", "rem": "%",
                      "and": "&", "or": "|", "xor": "^", "shl": "<<", "shr": ">>", "ushr": ">>>"}[operator]
            put(regs[0], typ, f"(({left}) {symbol} ({right}))")
        return tuple(state), lines

    def condition(self, op, state, *, strict):
        name, regs = op.opcode, op.registers
        zero = name.endswith("z")
        if len(regs) != (1 if zero else 2):
            self.fail("conditional branch register count disagrees")
        relation = name[3:-1] if zero else name[3:]
        symbol = {"eq": "==", "ne": "!=", "lt": "<", "ge": ">=", "gt": ">", "le": "<="}.get(relation)
        if not symbol:
            self.fail("unsupported conditional relation")
        kinds = state[regs[0]]
        if not zero:
            kinds = kinds | state[regs[1]]
        reference = any(_ref(k) or k == "null" for k in kinds)
        if reference:
            if relation not in {"eq", "ne"}:
                self.fail("ordered comparison of object references")
            left = self.read(state, regs[0], "Ljava/lang/Object;", strict=strict)
            right = "null" if zero else self.read(state, regs[1], "Ljava/lang/Object;", strict=strict)
        else:
            left = self.read(state, regs[0], "I", strict=strict)
            right = "0" if zero else self.read(state, regs[1], "I", strict=strict)
        return f"{left} {symbol} {right}"

    def initial(self):
        result = [_UNDEF] * self.method.registers
        reg = self.method.registers - self.method.incoming_words
        if "static" not in self.method.access:
            result[reg] = frozenset({"self:" + self.method.reference.owner})
            reg += 1
        for typ in self.method.reference.parameters:
            result[reg] = frozenset({typ})
            if width(typ) == 2:
                result[reg + 1] = frozenset({"wide-high"})
            reg += width(typ)
        return tuple(result)

    def verify(self):
        start = self.code[self.first].pc
        self.states[start] = self.initial()
        queue, queued = deque([start]), {start}
        normal_predecessors = {}
        while queue:
            self.budget.tick(self.method.registers + 1)
            pc = queue.popleft()
            queued.remove(pc)
            index, incoming = self.by_pc[pc], self.states[pc]
            outgoing, _ = self.operation(self.code[index], incoming, strict=False)
            edges = [(target, outgoing, True) for target in self.successors(index)]
            if self.code[index].opcode.startswith(_THROWING):
                edges += [(target, incoming, False) for _, target in self.handlers(pc)]
            for target, state, normal in edges:
                if target == self.code[0].pc and self.first:
                    self.fail("control flow re-enters its constructor prefix")
                if normal:
                    normal_predecessors.setdefault(target, set()).add(pc)
                old = self.states.get(target)
                merged = state if old is None else tuple(a | b for a, b in zip(old, state))
                if merged != old:
                    self.states[target] = merged
                    if target not in queued:
                        queue.append(target)
                        queued.add(target)
        for pc, state in self.states.items():
            index = self.by_pc[pc]
            op = self.code[index]
            if op.opcode == "move-exception" and (pc == start or normal_predecessors.get(pc)):
                self.fail("normal flow enters an exception-only value")
            if op.opcode.startswith("move-result") and normal_predecessors.get(pc) != {self.code[index - 1].pc}:
                self.fail("branch bypasses an invocation result producer")
            if pc in self.constructors and normal_predecessors.get(pc) != {self.constructors[pc]}:
                self.fail("branch bypasses an uninitialized allocation")
            self.operation(op, state, strict=True)
        if any(op.pc not in self.states and op.opcode != "nop" for op in self.code[self.first:]):
            self.fail("unreachable instructions have no verified source projection")

    @staticmethod
    def _result_read(typ):
        return ("resultObject" if _ref(typ) else "((java.lang.Float) null).intBitsToFloat(result32)" if typ == "F"
                else "((java.lang.Double) null).longBitsToDouble(result64)" if typ == "D"
                else "result64" if typ == "J" else "(result32 != 0)" if typ == "Z" else "result32")

    @staticmethod
    def _result_write(typ, value):
        if _ref(typ):
            return f"resultObject = {value};"
        if typ == "F":
            return f"result32 = ((java.lang.Float) null).floatToRawIntBits({value});"
        if typ == "D":
            return f"result64 = ((java.lang.Double) null).doubleToRawLongBits({value});"
        return f"{'result64' if typ == 'J' else 'result32'} = " + (f"({value}) ? 1 : 0" if typ == "Z" else value) + ";"

    def helper_name(self):
        return _helper_name(self.classes[self.method.reference.owner], "__neverdThrow")

    def emit(self):
        self.verify()
        lines = _Lines(self.budget, [self.prefix] if self.prefix else [], cumulative=True)
        # JVM definite assignment cannot follow a reconstructed dispatch loop.
        # Zero initializers only make declarations legal: the verifier above
        # independently rejects every read not defined on all reaching paths.
        for reg in range(self.method.registers):
            lines += [f"int v{reg} = 0;", f"long w{reg} = 0L;", f"java.lang.Object o{reg} = null;"]
        reg = self.method.registers - self.method.incoming_words
        initial = list(self.initial())
        if "static" not in self.method.access:
            lines.append(f"o{reg} = this;")
            reg += 1
        for index, typ in enumerate(self.method.reference.parameters):
            lines.append(self.write(initial, reg, typ, f"arg{index}"))
            reg += width(typ)
        lines += ["int result32 = 0;", "long result64 = 0L;", "java.lang.Object resultObject = null;",
                  "java.lang.Throwable caught = null;", f"int pc = {self.code[self.first].pc};",
                  "dispatch: while (true) {", "  try {", "    switch (pc) {"]
        for index in range(self.first, len(self.code)):
            op = self.code[index]
            if op.pc not in self.states:
                continue  # A decoded, unreachable no-op has no executable effect.
            _, body = self.operation(op, self.states[op.pc], strict=True)
            lines.append(f"      case {op.pc}: {{")
            lines.extend("        " + s for s in body)
            if op.opcode.startswith("if-"):
                condition = self.condition(op, self.states[op.pc], strict=True)
                lines.append(f"        pc = ({condition}) ? {op.target} : {self.code[index + 1].pc};")
            elif "switch" in op.opcode:
                lines.append(f"        switch (v{op.registers[0]}) {{")
                for key, target in zip(op.keys, op.targets):
                    lines.append(f"          case {_literal(key, 32)}: pc = {target}; break;")
                lines += [f"          default: pc = {self.code[index + 1].pc};", "        }"]
            elif op.opcode.startswith("goto"):
                lines.append(f"        pc = {op.target};")
            elif not op.opcode.startswith(("return", "throw")):
                lines.append(f"        pc = {self.code[index + 1].pc};")
            if not op.opcode.startswith(("return", "throw")):
                lines.append("        continue dispatch;")
            lines.append("      }")
        lines += ["      default: throw new java.lang.AssertionError(\"Invalid recovered control flow\");",
                  "    }", "  } catch (java.lang.Throwable failure) {"]
        for region in self.method.tries:
            lines.append(f"    if (pc >= {region.start} && pc < {region.end}) {{")
            for typ, target in region.handlers:
                test = "true" if typ is None else "failure instanceof " + type_name(typ, self.classes)
                lines.append(f"      if ({test}) {{ caught = failure; pc = {target}; continue dispatch; }}")
            lines.append("    }")
        lines += [f"    throw {self.helper_name()}(failure);", "  }", "}"]
        return "\n".join(lines)


def _field_value(value, typ, classes):
    if value is None:
        return None
    if isinstance(value, dict):
        if value.get("kind") == "float-bits" and typ == "F":
            return "((java.lang.Float) null).intBitsToFloat(" + _literal(value["bits"], 32) + ")"
        if value.get("kind") == "double-bits" and typ == "D":
            return "((java.lang.Double) null).longBitsToDouble(" + _literal(value["bits"], 64) + ")"
        raise MobileError("unsupported encoded static field value")
    if typ == "Ljava/lang/String;" and isinstance(value, str):
        return string(value)
    if typ == "Z" and isinstance(value, (bool, int)):
        return "true" if value else "false"
    if typ in {"B", "C", "S", "I", "J"} and type(value) is int:
        text = _literal(value, 64 if typ == "J" else 32)
        return f"({_PRIMITIVES[typ]}) {text}" if typ in {"B", "C", "S"} else text
    raise MobileError("static field value disagrees with its declared type")


def recover_java(classes: dict[str, Class], *, budget: Budget) -> dict:
    units, methods, recovered, declared = [], [], 0, 0
    enclosing = {}
    for name, cls in classes.items():
        budget.tick()
        if cls.enclosing:
            if cls.enclosing == name or cls.enclosing not in classes or not cls.inner_name:
                raise MobileError("unresolved or recursive nested class")
            enclosing.setdefault(cls.enclosing, []).append(cls)
    # Validate ownership before any field/signature resolves a nested type.
    # Such references can reach a cycle before the class emitter's active-set
    # guard, and even an acyclic chain needs a bounded source nesting depth.
    for name in classes:
        current, seen = name, set()
        while current:
            budget.tick()
            if current in seen:
                raise MobileError("recursive nested-class ownership")
            seen.add(current)
            if len(seen) > 128:
                raise MobileError("nested-class ownership exceeds the source depth limit")
            current = classes[current].enclosing
    emitting = set()

    def emit_class(cls: Class, nested=False):
        nonlocal recovered, declared
        budget.tick()
        if cls.name in emitting:
            raise MobileError("recursive nested-class ownership")
        emitting.add(cls.name)
        if cls.access & {"annotation", "enum"}:
            raise MobileError(f"unsupported Java declaration shape: {cls.name}")
        flags = cls.inner_access if nested else cls.access
        if nested and "static" not in flags:
            raise MobileError("non-static inner class requires an outer-instance initialization proof")
        name = identifier(cls.inner_name if nested else cls.name[1:-1].rsplit("/", 1)[-1])
        if name == "java":
            raise MobileError("class name shadows the required Java runtime package")
        mods = [s for s in ("public", "protected", "private", "static", "abstract", "final", "strictfp") if s in flags and (nested or s != "static")]
        kind = "interface" if "interface" in cls.access else "class"
        header = " ".join([*mods, kind, name])
        if kind == "class" and cls.superclass and cls.superclass != "Ljava/lang/Object;":
            header += " extends " + type_name(cls.superclass, classes)
        if cls.interfaces:
            header += (" extends " if kind == "interface" else " implements ") + ", ".join(type_name(t, classes) for t in cls.interfaces)
        body = _Lines(budget, [header + " {"], cumulative=True)
        member_names = set()
        constant_types = set()
        for fld in cls.fields:
            budget.tick()
            if fld.reference.name in member_names:
                raise MobileError("Java cannot represent field names overloaded only by type")
            member_names.add(fld.reference.name)
            mods = [s for s in ("public", "protected", "private", "static", "final", "volatile", "transient") if s in fld.access]
            value = _field_value(fld.value, fld.reference.type, classes)
            if "final" in fld.access and value is None:
                raise MobileError("final field needs a verified initialization expression")
            if value is not None and {"static", "final"} <= fld.access:
                # Dalvik sget initializes the declaring class even for encoded
                # constants. A Java constant expression would be inlined by
                # javac and silently drop that observable initialization.
                constant_types.add(fld.reference.type)
                value = _helper_name(cls, "__neverdConstant") + "(" + value + ")"
            declaration = " ".join([*mods, type_name(fld.reference.type, classes), identifier(fld.reference.name)])
            body.append("  " + declaration + (" = " + value if value is not None else "") + ";")
        java_signatures = set()
        for method in cls.methods:
            budget.tick()
            ref = method.reference
            signature = (ref.name, ref.parameters)
            if signature in java_signatures:
                raise MobileError("Java cannot represent methods overloaded only by return type")
            java_signatures.add(signature)
            row = {"identity": ref.identity, "class": cls.name, "name": ref.name, "prototype": ref.signature,
                   "input": cls.source_id, "instruction_count": len(method.instructions)}
            mods = [s for s in ("public", "protected", "private", "static", "final", "synchronized", "native", "abstract", "strictfp") if s in method.access]
            params = ", ".join(type_name(typ, classes) + f" arg{i}" for i, typ in enumerate(ref.parameters))
            if ref.name == "<clinit>":
                if ref.parameters or ref.returns != "V" or "static" not in method.access:
                    raise MobileError("invalid class initializer signature")
                declaration = "static"
            elif ref.name == "<init>":
                if ref.returns != "V" or "static" in method.access:
                    raise MobileError("invalid instance initializer signature")
                declaration = " ".join([*mods, name]) + "(" + params + ")"
                if _constructor_throws(method, classes, budget):
                    declaration += " throws java.lang.Throwable"
            else:
                declaration = " ".join([*mods, type_name(ref.returns, classes), identifier(ref.name)]) + "(" + params + ")"
            if method.access & {"abstract", "native"}:
                body.append("  " + declaration + ";")
                row.update(status="declaration-only", reason="original abstract/native declaration has no Dalvik body")
                declared += 1
            else:
                if kind == "interface":
                    raise MobileError("interface method body requires a compatible Java default/static declaration")
                emitter = Body(method, classes, budget)
                source = emitter.emit()
                body.append("  " + declaration + " {\n" + "\n".join("    " + line for line in source.splitlines()) + "\n  }")
                row["status"] = "recovered"
                recovered += 1
            methods.append(row)
        if kind == "class":
            if not any(m.reference.name == "<init>" for m in cls.methods):
                body.append(f"  private {name}() {{}}")
            helper = _helper_name(cls, "__neverdThrow")
            body += ['  @java.lang.SuppressWarnings("unchecked")',
                     f"  private static <E extends java.lang.Throwable> java.lang.RuntimeException {helper}(java.lang.Throwable failure) throws E {{",
                     "    if (failure == null) throw new java.lang.NullPointerException();",
                     "    throw (E) failure;", "  }"]
        for typ in sorted(constant_types):
            name_type = type_name(typ, classes)
            helper = _helper_name(cls, "__neverdConstant")
            body.append(f"  private static {name_type} {helper}({name_type} value) {{ return value; }}")
        for child in sorted(enclosing.get(cls.name, []), key=lambda c: c.name):
            body.extend("  " + line for line in emit_class(child, True).splitlines())
        body.append("}")
        emitting.remove(cls.name)
        return "\n".join(body)

    for cls in sorted(classes.values(), key=lambda c: c.name):
        if cls.enclosing:
            continue
        name = cls.name[1:-1]
        package, _, simple = name.rpartition("/")
        prefix = "package " + ".".join(identifier(p) for p in package.split("/")) + ";\n\n" if package else ""
        source = prefix + emit_class(cls) + "\n"
        units.append({"path": name + ".java", "class": cls.name, "source": source})
    if len(methods) != sum(len(c.methods) for c in classes.values()):
        raise MobileError("Android class ownership omitted declared methods")
    if not units:
        raise MobileError("Android input contains no source classes")
    return {"schema_version": 1, "status": "recovered", "class_count": len(classes), "method_count": len(methods),
            "recovered_method_count": recovered, "declaration_only_method_count": declared,
            "unrecovered_method_count": 0, "methods": methods, "source_units": units}
