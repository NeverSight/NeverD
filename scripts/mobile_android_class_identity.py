"""Independent compiler class-file facts for the owned local-class oracle.

This reads JDK output, never DEX or NeverD's implementation metadata. A proven
lexical mapping does not imply that the two binary class names are equal.
"""
from __future__ import annotations

import hashlib
from pathlib import Path


DEPRECATED_TYPE = "Ljava/lang/Deprecated;"
ANNOTATION_ATTRIBUTES = frozenset({
    "Deprecated", "RuntimeVisibleAnnotations", "RuntimeInvisibleAnnotations",
    "RuntimeVisibleParameterAnnotations", "RuntimeInvisibleParameterAnnotations",
    "RuntimeVisibleTypeAnnotations", "RuntimeInvisibleTypeAnnotations", "AnnotationDefault",
})


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def prototype_parts(prototype):
    require(isinstance(prototype, str) and prototype.startswith("("),
            "Invalid compiler method descriptor")

    def typ(position, void=False):
        start = position
        while position < len(prototype) and prototype[position] == "[":
            position += 1
        require(position < len(prototype), "Truncated compiler descriptor")
        char = prototype[position]
        if char == "L":
            end = prototype.find(";", position)
            require(end > position + 1 and not any(
                part in prototype[position + 1:end] for part in ".[()\0"),
                "Invalid compiler reference descriptor")
            position = end + 1
        else:
            require(char in "ZBCSIJFD" or (void and start == position and char == "V"),
                    "Invalid compiler scalar descriptor")
            position += 1
        return prototype[start:position], position

    parameters, position = [], 1
    while position < len(prototype) and prototype[position] != ")":
        value, position = typ(position)
        parameters.append(value)
    require(position < len(prototype), "Truncated compiler method descriptor")
    returns, position = typ(position + 1, True)
    require(position == len(prototype), "Trailing compiler method descriptor")
    return parameters, returns


class Bytes:
    def __init__(self, data):
        self.data, self.offset = data, 0

    def take(self, count):
        require(type(count) is int and 0 <= count <= len(self.data) - self.offset,
                "Truncated compiler class file")
        value = self.data[self.offset:self.offset + count]
        self.offset += count
        return value

    def number(self, count):
        return int.from_bytes(self.take(count), "big")

    def finish(self):
        require(self.offset == len(self.data), "Compiler attribute has trailing data")


class ClassFile:
    def __init__(self, data):
        require(len(data) <= 16 * 1024 * 1024, "Compiler class file exceeds its byte limit")
        self.reader, self.constants, self.result = Bytes(data), {}, None

    def constant(self, index, tag):
        entry = self.constants.get(index)
        require(entry is not None and entry[0] == tag, "Invalid compiler constant reference")
        return entry[1]

    def text(self, index):
        return self.constant(index, 1)

    def owner(self, index):
        value = self.text(self.constant(index, 7))
        require(value and not any(char in value for char in ".;[\0"),
                "Invalid compiler class identity")
        return "L" + value + ";"

    def attributes(self, reader):
        return [(self.text(reader.number(2)), reader.take(reader.number(4)))
                for _ in range(reader.number(2))]

    def signature(self, attributes):
        values = [data for kind, data in attributes if kind == "Signature"]
        require(len(values) <= 1, "Duplicate compiler Signature attribute")
        if not values:
            return None
        attribute = Bytes(values[0])
        value = self.text(attribute.number(2))
        attribute.finish()
        require(value and "\0" not in value, "Invalid compiler Signature text")
        return value

    def deprecation(self, attributes, context, *, allowed=True):
        """The Java 8 marker and the zero-length attribute are distinct facts.

        This oracle's supported annotation grammar is intentionally narrow.
        A nonempty element list or another annotation attachment is rejected,
        never skipped and then accepted as equivalent compiler identity.
        """
        deprecated, visible, seen = False, [], set()
        for kind, data in attributes:
            if kind not in ANNOTATION_ATTRIBUTES:
                continue
            require(allowed, "Unsupported compiler annotation attribute in Code of " + context)
            require(kind not in seen, "Duplicate compiler annotation attribute " + kind + " on " + context)
            seen.add(kind)
            if kind == "Deprecated":
                require(not data, "Compiler Deprecated attribute must have zero length on " + context)
                deprecated = True
                continue
            require(kind == "RuntimeVisibleAnnotations",
                    "Unsupported compiler annotation attribute " + kind + " on " + context)
            attribute = Bytes(data)
            types = set()
            for _ in range(attribute.number(2)):
                descriptor = self.text(attribute.number(2))
                require(descriptor not in types, "Duplicate compiler runtime annotation on " + context)
                types.add(descriptor)
                require(descriptor == DEPRECATED_TYPE,
                        "Unsupported compiler runtime annotation " + descriptor + " on " + context)
                require(attribute.number(2) == 0,
                        "Compiler Deprecated annotation elements are unsupported on " + context)
                visible.append({"type": descriptor, "elements": []})
            attribute.finish()
        return {"deprecated_attribute": deprecated, "runtime_visible_annotations": visible}

    def code(self, data, context):
        reader = Bytes(data)
        reader.take(4)  # max_stack and max_locals
        length = reader.number(4)
        require(0 < length <= 65535, "Invalid compiler Code length")
        reader.take(length)
        for _ in range(reader.number(2)):
            start, end, handler, catch = (reader.number(2) for _ in range(4))
            require(start < end <= length and handler < length, "Invalid compiler Code handler")
            if catch:
                self.owner(catch)
        self.deprecation(self.attributes(reader), context, allowed=False)
        reader.finish()

    def facts(self):
        if self.result is not None:
            return self.result
        reader = self.reader
        require(reader.take(4) == b"\xca\xfe\xba\xbe", "Invalid compiler class file")
        minor, major = reader.number(2), reader.number(2)
        count, index = reader.number(2), 1
        while index < count:
            tag = reader.number(1)
            if tag == 1:
                raw = reader.take(reader.number(2))
                value = raw.replace(b"\xc0\x80", b"\0").decode("utf-8", errors="surrogatepass")
            elif tag in (7, 8, 16, 19, 20):
                value = reader.number(2)
            elif tag in (9, 10, 11, 12, 17, 18):
                value = (reader.number(2), reader.number(2))
            elif tag in (3, 4):
                value = reader.take(4)
            elif tag in (5, 6):
                value = reader.take(8)
                require(index + 1 < count, "Truncated wide compiler constant")
            elif tag == 15:
                value = reader.take(3)
            else:
                raise RuntimeError(f"Unsupported compiler constant-pool tag {tag}")
            self.constants[index] = tag, value
            index += 2 if tag in (5, 6) else 1
        access, owner, parent = reader.number(2), self.owner(reader.number(2)), reader.number(2)
        superclass = self.owner(parent) if parent else None
        interfaces = [self.owner(reader.number(2)) for _ in range(reader.number(2))]
        fields, methods = {}, {}
        for _ in range(reader.number(2)):
            flags, name, descriptor = reader.number(2), self.text(reader.number(2)), self.text(reader.number(2))
            parameters, _ = prototype_parts("(" + descriptor + ")V")
            require(len(parameters) == 1, "Invalid compiler field descriptor")
            identity = owner + "->" + name + ":" + descriptor
            require(identity not in fields, "Compiler emitted a duplicate field")
            attrs = self.attributes(reader)
            signature = self.signature(attrs)
            deprecation = self.deprecation(attrs, identity)
            values = [data for kind, data in attrs if kind == "ConstantValue"]
            require(len(values) <= 1, "Duplicate compiler field ConstantValue")
            constant_value = None
            if values:
                attribute = Bytes(values[0])
                index = attribute.number(2)
                attribute.finish()
                tag = {"B": 3, "C": 3, "S": 3, "Z": 3, "I": 3, "J": 5,
                       "F": 4, "D": 6, "Ljava/lang/String;": 8}.get(descriptor)
                require(tag is not None, "Invalid compiler field ConstantValue descriptor")
                value = self.constant(index, tag)
                constant_value = {"tag": tag, "value": self.text(value) if tag == 8 else value.hex()}
            fields[identity] = {"name": name, "descriptor": descriptor, "access": flags,
                                "constant_value": constant_value, "signature": signature, **deprecation}
        for _ in range(reader.number(2)):
            flags, name, descriptor = reader.number(2), self.text(reader.number(2)), self.text(reader.number(2))
            prototype_parts(descriptor)
            attrs = self.attributes(reader)
            bodies = [data for kind, data in attrs if kind == "Code"]
            declaration = bool(flags & (0x100 | 0x400))
            require(len(bodies) == (0 if declaration else 1),
                    "Compiler method has inconsistent Code/access metadata")
            identity = owner + "->" + name + descriptor
            for data in bodies:
                self.code(data, identity)
            require(identity not in methods, "Compiler emitted a duplicate method")
            methods[identity] = {"name": name, "prototype": descriptor, "access": flags,
                                 "code": bool(bodies), "signature": self.signature(attrs),
                                 **self.deprecation(attrs, identity)}
        enclosing, own_inner, source_file = None, None, None
        seen = set()
        attrs = self.attributes(reader)
        signature = self.signature(attrs)
        deprecation = self.deprecation(attrs, owner)
        for kind, data in attrs:
            if kind not in {"EnclosingMethod", "InnerClasses", "SourceFile"}:
                continue
            require(kind not in seen, "Duplicate compiler class identity attribute")
            seen.add(kind)
            attribute = Bytes(data)
            if kind == "EnclosingMethod":
                outer, method = self.owner(attribute.number(2)), attribute.number(2)
                if method:
                    name, descriptor = self.constant(method, 12)
                    name, descriptor = self.text(name), self.text(descriptor)
                    prototype_parts(descriptor)
                else:
                    name, descriptor = None, None
                enclosing = {"owner": outer, "name": name, "prototype": descriptor}
            elif kind == "InnerClasses":
                inner_seen = set()
                for _ in range(attribute.number(2)):
                    inner, outer, name, flags = (attribute.number(2) for _ in range(4))
                    inner = self.owner(inner)
                    outer = self.owner(outer) if outer else None
                    name = self.text(name) if name else None
                    require(inner not in inner_seen, "Duplicate compiler InnerClasses identity")
                    inner_seen.add(inner)
                    if inner == owner:
                        own_inner = {"inner": inner, "outer": outer, "name": name, "access": flags}
            else:
                source_file = self.text(attribute.number(2))
            attribute.finish()
        reader.finish()
        self.result = {"name": owner, "access": access, "superclass": superclass,
                       "interfaces": interfaces, "fields": fields, "methods": methods,
                       "enclosing_method": enclosing, "inner_class": own_inner,
                       "source_file": source_file, "major": major, "minor": minor,
                       "signature": signature, **deprecation}
        return self.result

    def inventory(self):
        facts = self.facts()
        return facts["name"], {identity: "body" if row["code"] else "declaration"
                               for identity, row in facts["methods"].items()}


def compiler_classes(directory):
    classes = {}
    for path in sorted(Path(directory).rglob("*.class")):
        require(not path.is_symlink(), "Compiler class inventory contains a symlink")
        data = path.read_bytes()
        facts = ClassFile(data).facts()
        owner = facts["name"]
        require(owner not in classes, "Duplicate compiler inventory")
        require(path.relative_to(directory).as_posix() == owner[1:-1] + ".class",
                "Compiler class path disagrees with its identity")
        facts.update(path=path.relative_to(directory).as_posix(), size=len(data),
                     sha256=hashlib.sha256(data).hexdigest())
        classes[owner] = facts
    require(classes and any(row["methods"] for row in classes.values()),
            "Original compiler emitted no declaration inventory")
    return classes


THROW_HELPER_PROTOTYPE = "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
THROW_HELPER_SIGNATURE = "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;"


def deprecation_facts(row):
    """Validate explicit facts, with absence only for old handwritten fixtures."""
    require(isinstance(row, dict), "Malformed compiler deprecation facts")
    keys = {"deprecated_attribute", "runtime_visible_annotations"}
    present = keys.intersection(row)
    require(not present or present == keys, "Incomplete compiler deprecation facts")
    deprecated = row.get("deprecated_attribute", False)
    visible = row.get("runtime_visible_annotations", [])
    require(type(deprecated) is bool and isinstance(visible, list) and len(visible) <= 1,
            "Malformed compiler deprecation facts")
    for marker in visible:
        require(isinstance(marker, dict) and set(marker) == {"type", "elements"}
                and marker["type"] == DEPRECATED_TYPE and isinstance(marker["elements"], list)
                and not marker["elements"], "Unsupported compiler deprecation facts")
    return {"deprecated_attribute": deprecated, "runtime_visible_annotations": visible}


def with_deprecation_facts(row):
    facts = deprecation_facts(row)
    return {**row, **facts}


def match_generic_recompiled(original, rebuilt):
    """Exact owned top-level identities; only the known runtime helper is extra.

    The expected original Signature contents are checked separately against the
    handwritten fixture contract. These facts come from javac, not NeverD JSON.
    """
    require(original and set(original) == set(rebuilt), "Generic class inventory changed")
    signatures = {"classes": 0, "fields": 0, "methods": 0}
    method_count = 0
    for owner, before in original.items():
        after = rebuilt[owner]
        for facts in (before, after):
            require(facts["major"] == 52 and facts["minor"] == 0,
                    "Generic oracle requires Java 8 compiler output")
            require(facts["enclosing_method"] is None and facts["inner_class"] is None,
                    "Generic fixture unexpectedly acquired a nested scope")
        for key in ("name", "access", "superclass", "interfaces", "signature"):
            require(before[key] == after[key], "Generic class declaration changed: " + owner + ":" + key)
        require(deprecation_facts(before) == deprecation_facts(after),
                "Generic class deprecation changed: " + owner)
        require({key: with_deprecation_facts(row) for key, row in before["fields"].items()}
                == {key: with_deprecation_facts(row) for key, row in after["fields"].items()},
                "Generic field declaration changed: " + owner)
        signatures["classes"] += before["signature"] is not None
        signatures["fields"] += sum(row["signature"] is not None for row in before["fields"].values())
        for identity, method in before["methods"].items():
            require(method["code"] and not (method["access"] & (0x40 | 0x1000)),
                    "Generic fixture contains a bridge, synthetic or declaration-only method")
            rebuilt_method = after["methods"].get(identity)
            require(isinstance(rebuilt_method, dict)
                    and with_deprecation_facts(rebuilt_method) == with_deprecation_facts(method),
                    "Generic original method or Signature changed: " + identity)
            method_count += 1
            signatures["methods"] += method["signature"] is not None
        helper = owner + "->__neverdThrow" + THROW_HELPER_PROTOTYPE
        require(helper not in before["methods"], "Generic fixture collides with its fixed auxiliary helper")
        require(set(after["methods"]) - set(before["methods"]) == {helper},
                "Generic generated helper inventory changed: " + owner)
        require(with_deprecation_facts(after["methods"][helper]) == {
            "name": "__neverdThrow", "prototype": THROW_HELPER_PROTOTYPE,
            "access": 0xA, "code": True, "signature": THROW_HELPER_SIGNATURE,
            "deprecated_attribute": False, "runtime_visible_annotations": []},
            "Generic generated helper declaration changed: " + owner)
    return {"scope": "owned-generic-signature-and-body", "class_count": len(original),
            "original_method_count": method_count, "signature_count": signatures,
            "generated_helper_count": len(original)}


def match_deprecated_recompiled(original, rebuilt):
    """Exact Java 8 owned declarations; count only original annotation owners."""
    result = match_generic_recompiled(original, rebuilt)
    counts = {key: {scope: 0 for scope in ("classes", "fields", "constructors", "methods")}
              for key in ("deprecated_attribute_count", "runtime_visible_deprecated_count")}
    for before in original.values():
        rows = [("classes", before)]
        rows.extend(("fields", row) for row in before["fields"].values())
        rows.extend(("constructors" if row["name"] == "<init>" else "methods", row)
                    for row in before["methods"].values())
        for scope, row in rows:
            facts = deprecation_facts(row)
            counts["deprecated_attribute_count"][scope] += int(facts["deprecated_attribute"])
            counts["runtime_visible_deprecated_count"][scope] += len(facts["runtime_visible_annotations"])
    return {**result, "scope": "owned-deprecated-attributes-and-body", **counts}


def local_key(facts):
    enclosing, inner = facts["enclosing_method"], facts["inner_class"]
    require(enclosing and enclosing["name"] not in (None, "<init>", "<clinit>")
            and inner and inner["outer"] is None and isinstance(inner["name"], str)
            and inner["name"] and not (inner["access"] & 8),
            "Local compiler identity is anonymous, a member, or an initializer")
    return enclosing["owner"], enclosing["name"], enclosing["prototype"], inner["name"]


def local_classes(classes):
    return {owner: facts for owner, facts in classes.items() if facts["enclosing_method"] is not None}


def projected_methods(classes):
    projected = set()
    scopes = set()
    for facts in local_classes(classes).values():
        key = local_key(facts)
        require(key not in scopes, "Ambiguous original local compiler identity")
        scopes.add(key)
        owner, name, prototype, _ = key
        enclosing = owner + "->" + name + prototype
        require(owner in classes and enclosing in classes[owner]["methods"],
                "Missing exact enclosing compiler method")
        require(classes[owner]["methods"][enclosing]["code"]
                and classes[owner]["methods"][enclosing]["access"] & 8,
                "Enclosing compiler method is not a static body")
        require(not facts["fields"] and facts["superclass"] == "Ljava/lang/Object;"
                and not facts["interfaces"], "Local compiler class has unsupported storage or parents")
        require(facts["inner_class"]["access"] & ~0x1010 == 0,
                "Local compiler declaration has unsupported access")
        constructors = [row for row in facts["methods"].values() if row["name"] == "<init>"]
        require(len(constructors) == 1 and constructors[0]["prototype"] == "()V"
                and constructors[0]["code"] and not (constructors[0]["access"] & 8),
                "Local compiler constructor is missing or captures arguments")
        for row in [*facts["methods"].values(), classes[owner]["methods"][enclosing]]:
            parameters, returns = prototype_parts(row["prototype"])
            require(all(value in "ZBCSIJFD" for value in parameters)
                    and returns in "VZBCSIJFD" and row["code"],
                    "Local fixture methods must have scalar bodies")
        require(all(not (row["access"] & 8) and row["name"] != "<clinit>"
                    for row in facts["methods"].values()),
                "Local fixture acquired a static method or initializer")
        projected.add(enclosing)
        projected.update(facts["methods"])
    require(projected, "Missing expected local compiler declarations")
    return projected


def source_unit(owner, classes):
    seen = set()
    while True:
        require(owner in classes and owner not in seen, "Unresolved compiler source ownership")
        seen.add(owner)
        facts = classes[owner]
        if facts["enclosing_method"]:
            owner = facts["enclosing_method"]["owner"]
        elif facts["inner_class"] and facts["inner_class"]["outer"]:
            owner = facts["inner_class"]["outer"]
        else:
            return owner[1:-1] + ".java"


def validate_bindings(coverage, original, inputs):
    locals_ = local_classes(original)
    rows = coverage.get("class_source_bindings")
    require(isinstance(rows, list) and len(rows) == len(locals_), "Incomplete local class bindings")
    seen = set()
    keys = {"class", "input", "enclosing_method", "source_unit", "source_name",
            "binding_kind", "binary_name_status"}
    for row in rows:
        require(isinstance(row, dict) and set(row) == keys, "Malformed or guessed local class binding")
        owner = row["class"]
        require(isinstance(owner, str) and owner in locals_ and owner not in seen,
                "Duplicate or unexpected local class binding")
        seen.add(owner)
        enclosing, name, descriptor, inner = local_key(locals_[owner])
        parameters, returns = prototype_parts(descriptor)
        expected = {"identity": enclosing + "->" + name + descriptor, "owner": enclosing,
                    "name": name, "prototype": descriptor, "parameters": parameters, "returns": returns}
        require(row["enclosing_method"] == expected and row["input"] == inputs[owner]
                and row["source_name"] == inner and row["source_unit"] == source_unit(owner, original)
                and row["binding_kind"] == "named-method-local" and row["binary_name_status"] == "unverified",
                "Local class binding disagrees with independent compiler scope")


def match_recompiled(original, rebuilt, helpers):
    projected_methods(original)
    locals_ = local_classes(original)
    rebuilt_locals = local_classes(rebuilt)
    ordinary = set(original) - set(locals_)
    require(ordinary == set(rebuilt) - set(rebuilt_locals), "Recompiled ordinary class inventory changed")
    mapping = {owner: owner for owner in ordinary}
    by_key = {}
    for owner, facts in rebuilt_locals.items():
        key = local_key(facts)
        require(key not in by_key, "Ambiguous recompiled local compiler identity")
        by_key[key] = owner
    used = set()
    for owner, facts in locals_.items():
        enclosing, name, descriptor, inner = local_key(facts)
        require(enclosing in mapping, "Unsupported nested enclosing compiler scope")
        target = by_key.get((mapping[enclosing], name, descriptor, inner))
        require(target is not None and target not in used, "Missing or many-to-one local compiler mapping")
        used.add(target)
        mapping[owner] = target
    require(used == set(rebuilt_locals), "Unexpected recompiled local class")
    require(isinstance(helpers, list), "Missing exact generated helper inventory")
    declared_helpers = {}
    for helper in helpers:
        require(isinstance(helper, dict) and set(helper) == {
            "class", "name", "prototype", "static", "source_unit", "kind"}, "Malformed generated helper")
        owner, name, descriptor = helper["class"], helper["name"], helper["prototype"]
        require(isinstance(owner, str) and owner in mapping and isinstance(name, str)
                and type(helper["static"]) is bool and helper["source_unit"] == source_unit(owner, original),
                "Generated helper owner or source unit disagrees")
        parameters, returns = prototype_parts(descriptor)
        kind = helper["kind"]
        if kind == "throw-helper":
            require(descriptor == "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                    and helper["static"] == (owner not in locals_) and not name.startswith("<"),
                    "Generated throw helper has the wrong ABI")
        elif kind == "constant-helper":
            require(owner not in locals_ and helper["static"] and parameters == [returns]
                    and returns in {"B", "C", "D", "F", "I", "J", "S", "Z", "Ljava/lang/String;"}
                    and not name.startswith("<"), "Generated constant helper has the wrong ABI")
        elif kind == "default-constructor":
            require(name == "<init>" and descriptor == "()V" and not helper["static"]
                    and not any(row["name"] == "<init>" for row in original[owner]["methods"].values()),
                    "Generated constructor cannot replace an original constructor")
        elif kind == "field-initializer":
            require(name == "<clinit>" and descriptor == "()V" and helper["static"]
                    and not any(row["name"] == "<clinit>" for row in original[owner]["methods"].values())
                    and any(row["access"] & 8 and row.get("constant_value") is not None
                            for row in original[owner]["fields"].values()),
                    "Generated field initializer lacks original ConstantValue evidence or replaces an original initializer")
        else:
            raise RuntimeError("Unknown generated helper kind")
        original_id = owner + "->" + name + descriptor
        target_id = mapping[owner] + "->" + name + descriptor
        require(original_id not in original[owner]["methods"] and target_id not in declared_helpers,
                "Generated helper duplicates an original or auxiliary method")
        declared_helpers[target_id] = helper
    actual_extras = {}
    for owner, target in mapping.items():
        before, after = original[owner], rebuilt[target]
        require(before["major"] == after["major"] == 52 and before["minor"] == after["minor"] == 0,
                "Local oracle requires Java 8 compiler output")
        require(before["access"] == after["access"] and before["superclass"] == after["superclass"]
                and before["interfaces"] == after["interfaces"], "Recompiled class access or parents changed")
        require(deprecation_facts(before) == deprecation_facts(after), "Recompiled class deprecation changed")
        if owner in locals_:
            require(before["inner_class"]["access"] == after["inner_class"]["access"],
                    "Recompiled InnerClasses access changed")
        def storage(row):
            return {key: value for key, value in with_deprecation_facts(row).items() if key != "constant_value"}
        wanted_fields = {target + "->" + row["name"] + ":" + row["descriptor"]: storage(row)
                         for row in before["fields"].values()}
        require(wanted_fields == {key: storage(row) for key, row in after["fields"].items()},
                "Recompiled fields changed or acquired capture storage")
        for row in before["fields"].values():
            target_id = target + "->" + row["name"] + ":" + row["descriptor"]
            old, new = row.get("constant_value"), after["fields"][target_id].get("constant_value")
            if old != new:
                initializer = declared_helpers.get(target + "-><clinit>()V")
                require(old is not None and new is None and initializer
                        and initializer["kind"] == "field-initializer",
                        "Recompiled ConstantValue changed without a measured field initializer")
        wanted = set()
        for identity, row in before["methods"].items():
            target_id = target + "->" + row["name"] + row["prototype"]
            wanted.add(target_id)
            rebuilt_method = after["methods"].get(target_id)
            require(isinstance(rebuilt_method, dict)
                    and with_deprecation_facts(rebuilt_method) == with_deprecation_facts(row),
                    "Recompiled original method or Code/access role changed: " + identity)
        for identity in after["methods"].keys() - wanted:
            actual_extras[identity] = after["methods"][identity]
    require(set(actual_extras) == set(declared_helpers), "Generated helper inventory does not match exact extra methods")
    for identity, row in actual_extras.items():
        helper = declared_helpers[identity]
        flags = 8 if helper["kind"] == "field-initializer" else (2 | (8 if helper["static"] else 0))
        require(row["code"] and row["access"] == flags,
                "Generated helper Code/access role changed")
        require(deprecation_facts(row) == {"deprecated_attribute": False, "runtime_visible_annotations": []},
                "Generated helper acquired an annotation or Deprecated attribute")
    return {"schema_version": 1, "scope": "owned-local-source-projection",
            "binary_identity_equivalence": False,
            "all_measured_binary_names_equal": all(owner == target for owner, target in mapping.items()),
            "classes": [{"original": owner, "recompiled": target, "binary_name_equal": owner == target}
                        for owner, target in sorted(mapping.items())],
            "original_method_count": sum(len(row["methods"]) for row in original.values()),
            "generated_helper_count": len(declared_helpers)}
