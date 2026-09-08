"""Typed declarations and bounded work shared by the Android readers."""
from __future__ import annotations

from dataclasses import dataclass, field
import re
import time

from .common import Limits, MobileError


@dataclass
class Budget:
    limits: Limits
    deadline: float = field(init=False)
    remaining: int = field(init=False)
    output_bytes: int = field(init=False, default=0)

    def __post_init__(self):
        self.deadline = time.monotonic() + self.limits.timeout
        self.remaining = min(20_000_000, max(100_000, self.limits.max_bytes * 4))

    def tick(self, count: int = 1) -> None:
        if count < 0:
            raise MobileError("invalid Android analysis budget charge")
        self.remaining -= count
        if self.remaining < 0 or time.monotonic() > self.deadline:
            raise MobileError("Android analysis exceeded its work or time budget")

    def output(self, count: int) -> None:
        """Charge UTF-8 generation bytes before accumulating another fragment."""
        if type(count) is not int or count < 0:
            raise MobileError("invalid Android output budget charge")
        self.output_bytes += count
        if self.output_bytes > self.limits.max_bytes:
            raise MobileError("Android output exceeded its byte budget")


def descriptor(value: str, *, allow_void: bool = False) -> str:
    if not isinstance(value, str) or len(value) > 65535:
        raise MobileError("invalid Dalvik type descriptor")
    depth = len(value) - len(value.lstrip("["))
    leaf = value[depth:]
    if depth > 255 or not leaf or (leaf == "V" and (depth or not allow_void)):
        raise MobileError("invalid Dalvik type descriptor")
    if leaf in ("V" if allow_void else "") or leaf in "ZBCSIJFD" and len(leaf) == 1:
        return value
    if not re.fullmatch(r"L[^;\[.\x00\s]+;", leaf) or any(not p for p in leaf[1:-1].split("/")):
        raise MobileError(f"invalid Dalvik type descriptor: {value!r}")
    return value


def prototype(value: str) -> tuple[tuple[str, ...], str]:
    if not value.startswith("(") or ")" not in value:
        raise MobileError("invalid Dalvik method prototype")
    body, result = value[1:].split(")", 1)
    parts = []
    while body:
        match = re.match(r"\[*(?:[ZBCSIJFD]|L[^;]+;)", body)
        if not match:
            raise MobileError("invalid Dalvik parameter descriptor")
        parts.append(descriptor(match[0]))
        body = body[len(match[0]):]
    return tuple(parts), descriptor(result, allow_void=True)


def width(typ: str) -> int:
    return 2 if typ in ("J", "D") else 1


@dataclass(frozen=True)
class MethodRef:
    owner: str
    name: str
    parameters: tuple[str, ...]
    returns: str

    @property
    def signature(self) -> str:
        return "(" + "".join(self.parameters) + ")" + self.returns

    @property
    def identity(self) -> str:
        return self.owner + "->" + self.name + self.signature


@dataclass(frozen=True)
class FieldRef:
    owner: str
    name: str
    type: str


def method_ref(value: str) -> MethodRef:
    try:
        owner, tail = value.split("->", 1)
        name, sig = tail.split("(", 1)
    except ValueError as error:
        raise MobileError("invalid Dalvik method reference") from error
    if not name:
        raise MobileError("empty Dalvik method name")
    params, returns = prototype("(" + sig)
    return MethodRef(descriptor(owner), name, params, returns)


def field_ref(value: str) -> FieldRef:
    try:
        owner, tail = value.split("->", 1)
        name, typ = tail.split(":", 1)
    except ValueError as error:
        raise MobileError("invalid Dalvik field reference") from error
    if not name:
        raise MobileError("empty Dalvik field name")
    return FieldRef(descriptor(owner), name, descriptor(typ))


@dataclass(frozen=True)
class Instruction:
    pc: int
    opcode: str
    registers: tuple[int, ...] = ()
    literal: int | str | None = None
    target: int | None = None
    reference: MethodRef | FieldRef | str | None = None
    keys: tuple[int, ...] = ()
    targets: tuple[int, ...] = ()
    data: tuple[int, ...] = ()
    element_width: int = 0


@dataclass(frozen=True)
class TryRegion:
    start: int
    end: int
    handlers: tuple[tuple[str | None, int], ...]


@dataclass
class Method:
    reference: MethodRef
    access: frozenset[str]
    registers: int
    instructions: list[Instruction] = field(default_factory=list)
    tries: list[TryRegion] = field(default_factory=list)
    code_end: int = 0

    @property
    def incoming_words(self) -> int:
        return sum(width(p) for p in self.reference.parameters) + ("static" not in self.access)


@dataclass
class Field:
    reference: FieldRef
    access: frozenset[str]
    value: object = None


@dataclass
class Class:
    name: str
    superclass: str | None
    access: frozenset[str]
    source_id: str
    interfaces: list[str] = field(default_factory=list)
    fields: list[Field] = field(default_factory=list)
    methods: list[Method] = field(default_factory=list)
    enclosing: str | None = None
    inner_name: str | None = None
    inner_access: frozenset[str] = frozenset()


ACCESS_FLAGS = {0x1: "public", 0x2: "private", 0x4: "protected", 0x8: "static",
                0x10: "final", 0x20: "synchronized", 0x40: "volatile", 0x80: "transient",
                0x100: "native", 0x200: "interface", 0x400: "abstract", 0x800: "strictfp",
                0x1000: "synthetic", 0x2000: "annotation", 0x4000: "enum",
                0x10000: "constructor", 0x20000: "declared-synchronized"}


def access_flags(value: int) -> frozenset[str]:
    known = sum(ACCESS_FLAGS)
    if value < 0 or value & ~known:
        raise MobileError("unknown Dalvik declaration access flags")
    return frozenset(name for flag, name in ACCESS_FLAGS.items() if value & flag)


_VISIBILITY = frozenset({"public", "private", "protected"})
_CLASS_FLAGS = _VISIBILITY | {"static", "final", "interface", "abstract", "synthetic", "annotation", "enum"}
_FIELD_FLAGS = _VISIBILITY | {"static", "final", "volatile", "transient", "synthetic", "enum"}
_METHOD_FLAGS = _VISIBILITY | {"static", "final", "synchronized", "bridge", "varargs", "native", "abstract",
                               "strictfp", "synthetic", "constructor", "declared-synchronized"}


def _check_flags(flags, allowed, identity: str) -> None:
    if flags - allowed:
        raise MobileError(f"invalid declaration access flags: {identity}")
    if len(flags & _VISIBILITY) > 1:
        raise MobileError(f"conflicting declaration visibility: {identity}")


def _check_class(cls: Class, budget: Budget) -> None:
    _check_flags(cls.access, _CLASS_FLAGS, cls.name)
    if not descriptor(cls.name).startswith("L"):
        raise MobileError("class declaration requires a class descriptor")
    if cls.superclass is None:
        if cls.name != "Ljava/lang/Object;":
            raise MobileError(f"class has no superclass: {cls.name}")
    elif not descriptor(cls.superclass).startswith("L"):
        raise MobileError(f"invalid superclass descriptor: {cls.name}")
    if len(set(cls.interfaces)) != len(cls.interfaces):
        raise MobileError(f"duplicate declared interface: {cls.name}")
    for interface in cls.interfaces:
        budget.tick()
        if not descriptor(interface).startswith("L"):
            raise MobileError(f"invalid interface descriptor: {cls.name}")
    declarations = [cls.access]
    if cls.enclosing:
        _check_flags(cls.inner_access, _CLASS_FLAGS, cls.name)
        declarations.append(cls.inner_access)
    elif cls.access & {"private", "protected", "static"}:
        raise MobileError(f"top-level class has nested-only access flags: {cls.name}")
    for flags in declarations:
        if "final" in flags and flags & {"abstract", "interface"}:
            raise MobileError(f"class cannot be both final and abstract/interface: {cls.name}")
        if "interface" in flags and "abstract" not in flags:
            raise MobileError(f"interface must be abstract: {cls.name}")
        if "annotation" in flags and "interface" not in flags:
            raise MobileError(f"annotation must be an interface: {cls.name}")
    interface = "interface" in cls.access
    if interface and cls.superclass != "Ljava/lang/Object;":
        raise MobileError(f"interface has a non-Object superclass: {cls.name}")
    for member in cls.fields:
        budget.tick()
        ref, flags = member.reference, member.access
        _check_flags(flags, _FIELD_FLAGS, f"{cls.name}->{ref.name}")
        if ref.owner != cls.name:
            raise MobileError(f"field owner disagrees with its declaration: {cls.name}")
        descriptor(ref.type)
        if {"final", "volatile"} <= flags:
            raise MobileError(f"field cannot be both final and volatile: {cls.name}->{ref.name}")
        if interface and (not {"public", "static", "final"} <= flags or flags & {"volatile", "transient"}):
            raise MobileError(f"invalid interface field declaration: {cls.name}->{ref.name}")
    for method in cls.methods:
        budget.tick()
        ref, flags = method.reference, method.access
        _check_flags(flags, _METHOD_FLAGS, ref.identity)
        if ref.owner != cls.name:
            raise MobileError(f"method owner disagrees with its declaration: {ref.identity}")
        descriptor(ref.returns, allow_void=True)
        for parameter in ref.parameters:
            budget.tick()
            descriptor(parameter)
        if "abstract" in flags:
            if "abstract" not in cls.access:
                raise MobileError(f"concrete class declares an abstract method: {ref.identity}")
            if flags & {"static", "private", "final", "native", "synchronized", "strictfp", "constructor", "declared-synchronized"}:
                raise MobileError(f"incompatible abstract method flags: {ref.identity}")
        if {"native", "strictfp"} <= flags:
            raise MobileError(f"native method cannot be strictfp: {ref.identity}")
        if interface and flags & {"protected", "final", "synchronized", "native", "declared-synchronized"}:
            raise MobileError(f"invalid interface method flags: {ref.identity}")
        if ref.name == "<init>":
            if ref.returns != "V" or flags - (_VISIBILITY | {"constructor", "synthetic", "varargs"}) or interface:
                raise MobileError(f"invalid instance initializer declaration: {ref.identity}")
        elif ref.name == "<clinit>":
            if ref.parameters or ref.returns != "V" or "static" not in flags or flags - {"static", "constructor", "synthetic", "strictfp"}:
                raise MobileError(f"invalid class initializer declaration: {ref.identity}")
        elif ref.name.startswith("<") or "constructor" in flags:
            raise MobileError(f"invalid constructor identity: {ref.identity}")


def _check_inheritance(classes: dict[str, Class], budget: Budget) -> None:
    parents = {}
    for name, cls in classes.items():
        budget.tick()
        parent = classes.get(cls.superclass)
        if parent and ("interface" in parent.access or "final" in parent.access):
            raise MobileError(f"invalid interface or final superclass: {name}")
        for interface in cls.interfaces:
            budget.tick()
            if interface in classes and "interface" not in classes[interface].access:
                raise MobileError(f"implemented type is not an interface: {name}")
        parents[name] = tuple(p for p in (cls.superclass, *cls.interfaces) if p in classes)
    complete, active = set(), set()
    for root in classes:
        pending = [(root, False)]
        while pending:
            budget.tick()
            name, finish = pending.pop()
            if finish:
                active.remove(name)
                complete.add(name)
            elif name not in complete:
                if name in active:
                    raise MobileError(f"cyclic class/interface inheritance: {name}")
                active.add(name)
                pending.append((name, True))
                pending.extend((parent, False) for parent in parents[name])


def link_classes(classes: list[Class], budget: Budget) -> dict[str, Class]:
    result = {}
    for cls in classes:
        budget.tick()
        if cls.name in result:
            raise MobileError(f"duplicate Android class definition: {cls.name}")
        _check_class(cls, budget)
        result[cls.name] = cls
        identities = [m.reference.identity for m in cls.methods]
        fields = [(f.reference.name, f.reference.type) for f in cls.fields]
        if len(set(identities)) != len(identities) or len(set(fields)) != len(fields):
            raise MobileError(f"duplicate member definition in {cls.name}")
        for method in cls.methods:
            budget.tick()
            no_code = bool(method.access & {"abstract", "native"})
            if no_code == bool(method.instructions):
                raise MobileError(f"method body/access mismatch: {method.reference.identity}")
            if not no_code and not method.incoming_words <= method.registers <= 65535:
                raise MobileError(f"invalid register frame: {method.reference.identity}")
    _check_inheritance(result, budget)
    return result
