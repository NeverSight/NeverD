#!/usr/bin/env python3
"""Preserve SDK declaration owners and receiver types for source call analysis.

This supplements selector-wide agreement with compiler-observed class,
category and protocol identities. Each architecture requires agreement between
the same macOS/iOS preprocessing profiles as the existing declaration catalogs.
No framework implementation or inferred dynamic receiver is included.
"""

import argparse
import ctypes
import json
from pathlib import Path
import tempfile

try:
    from .generate_objc_declarations import CXCursor, CXString, TARGETS, VISITOR
    from .generate_objc_framework_declarations import (
        DEFAULT_FRAMEWORKS, FrameworkDeclarations, framework_header, module_paths, owns_header, sdk_module_paths,
    )
except ImportError:
    from generate_objc_declarations import CXCursor, CXString, TARGETS, VISITOR
    from generate_objc_framework_declarations import (
        DEFAULT_FRAMEWORKS, FrameworkDeclarations, framework_header, module_paths, owns_header, sdk_module_paths,
    )


class CXType(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_uint), ("data", ctypes.c_void_p * 2)]


class ReceiverDeclarations(FrameworkDeclarations):
    def __init__(self, library, headers, include_dependencies=False):
        super().__init__(library, headers)
        self.include_dependencies = include_dependencies
        self.bind("clang_getCursorSemanticParent", CXCursor, CXCursor)
        self.bind("clang_getCursorReferenced", CXCursor, CXCursor)
        self.bind("clang_getCursorDefinition", CXCursor, CXCursor)
        self.bind("clang_isCursorDefinition", ctypes.c_uint, CXCursor)
        self.bind("clang_getCursorResultType", CXType, CXCursor)
        self.bind("clang_Cursor_getNumArguments", ctypes.c_int, CXCursor)
        self.bind("clang_Cursor_getArgument", CXCursor, CXCursor, ctypes.c_uint)
        self.bind("clang_getCursorType", CXType, CXCursor)
        self.bind("clang_getCanonicalType", CXType, CXType)
        self.bind("clang_getPointeeType", CXType, CXType)
        self.bind("clang_getTypeDeclaration", CXCursor, CXType)
        self.bind("clang_getTypeSpelling", CXString, CXType)

    def object_pointer_parameter_classes(self, cursor):
        result = []
        count = self.clang_Cursor_getNumArguments(cursor)
        if count < 0:
            return ()
        for index in range(count):
            argument = self.clang_Cursor_getArgument(cursor, index)
            canonical = self.clang_getCanonicalType(
                self.clang_getCursorType(argument))
            if canonical.kind != 101:  # CXType_Pointer
                continue
            pointee = self.clang_getCanonicalType(
                self.clang_getPointeeType(canonical))
            if pointee.kind != 109:  # CXType_ObjCObjectPointer
                continue
            declaration = self.clang_getTypeDeclaration(
                self.clang_getPointeeType(pointee))
            if declaration.kind != 11:
                continue
            name = self.string(self.clang_getCursorSpelling(declaration))
            if name:
                # Source signatures include self and _cmd before explicit
                # Objective-C method arguments.
                result.append((index + 2, name))
        return tuple(result)

    def eligible(self, cursor):
        if self.include_dependencies:
            return True
        file = ctypes.c_void_p()
        self.clang_getSpellingLocation(self.clang_getCursorLocation(cursor),
                                       ctypes.byref(file), None, None, None)
        return bool(file.value) and owns_header(
            self.string(self.clang_getFileName(file)), self.headers)

    def owner(self, cursor):
        key = (cursor.kind, *cursor.data)
        if key in self.owner_cache:
            return self.owner_cache[key]
        name = self.string(self.clang_getCursorSpelling(cursor))
        references = []

        @VISITOR
        def visit(child, parent, data):
            # Only immediate superclass/protocol/class references belong to
            # this owner. References inside method signatures are unrelated.
            if child.kind in (40, 41, 42):
                referenced = self.clang_getCursorReferenced(child)
                references.append((child.kind, self.string(
                    self.clang_getCursorSpelling(referenced))))
            return 1  # CXChildVisit_Continue, without recursion.

        self.clang_visitChildren(cursor, visit, None)
        classes = [value for kind, value in references if kind == 42]
        parents = tuple(value for kind, value in references if kind == 40)
        protocols = tuple(sorted({value for kind, value in references if kind == 41}))
        if len(parents) > 1 or any(not value for _, value in references):
            raise ValueError("incomplete or ambiguous Objective-C owner references")
        if cursor.kind == 11 and name:
            result = ("class", name, "", parents, protocols)
        elif cursor.kind == 13 and name:
            result = ("protocol", name, "", (), protocols)
        elif cursor.kind == 12 and len(classes) == 1:
            result = ("category", classes[0], name, (), protocols)
        else:
            raise ValueError(f"unresolved Objective-C owner: {cursor.kind} {name}")
        self.owner_cache[key] = result
        return result

    def declaration(self, cursor):
        if not self.eligible(cursor):
            return None
        if cursor.kind in (40, 42):
            # For a class reference libclang returns its @interface. Applied
            # directly to an interface, isCursorDefinition instead asks for
            # an @implementation, which SDK declarations do not contain.
            definition = self.clang_getCursorDefinition(cursor)
            if definition.kind == 11 and self.eligible(definition):
                self.owners.add(self.owner(definition))
            return None
        if cursor.kind in (11, 12, 13):
            owner = self.owner(cursor)
            if (cursor.kind == 12 or
                    (cursor.kind == 13 and self.clang_isCursorDefinition(cursor)) or
                    (cursor.kind == 11 and (owner[3] or owner[4]))):
                self.owners.add(owner)
            return None
        if cursor.kind not in (16, 17):
            return None
        parent = self.clang_getCursorSemanticParent(cursor)
        owner = self.owner(parent)
        # A member declaration independently establishes a class interface;
        # a bare @class forward declaration cannot contain members.
        self.owners.add(owner)
        selector = self.string(self.clang_getCursorSpelling(cursor))
        encoding = self.string(self.clang_getDeclObjCTypeEncoding(cursor))
        if self.clang_Cursor_isVariadic(cursor):
            encoding = ""
        result = self.clang_getCursorResultType(cursor)
        spelling = self.string(self.clang_getTypeSpelling(result))
        canonical = self.clang_getCanonicalType(result)
        return_class = ""
        if canonical.kind == 109:  # CXType_ObjCObjectPointer
            declaration = self.clang_getTypeDeclaration(
                self.clang_getPointeeType(canonical))
            if declaration.kind == 11:
                return_class = self.string(self.clang_getCursorSpelling(declaration))
        self.methods.add((*owner[:3], cursor.kind == 17, selector, encoding,
                          return_class, spelling == "instancetype",
                          self.object_pointer_parameter_classes(cursor)))
        return selector, encoding

    def extract_owned(self, source, sdk, target, extra_arguments=()):
        # Cursor addresses can be reused after a translation unit is disposed.
        self.owner_cache = {}
        self.owners = set()
        self.methods = set()
        self.extract(source, sdk, target, extra_arguments)
        return {"owners": sorted(self.owners), "methods": sorted(self.methods)}


def owner_profiles(profile):
    result = {}
    for kind, name, category, parents, protocols in profile["owners"]:
        edges = "|".join(["C:" + value for value in parents] +
                         ["P:" + value for value in protocols])
        result.setdefault((kind, name, category), set()).add(edges)
    return result


def method_profiles(profile):
    result = {}
    for record in profile["methods"]:
        *identity, encoding, return_class, return_self, parameters = record
        result.setdefault(tuple(identity), set()).add(
            (encoding, return_class, return_self, tuple(parameters)))
    return result


def parameter_profiles(profile):
    result = {}
    for record in profile["methods"]:
        *identity, encoding, return_class, return_self, parameters = record
        for index, class_name in parameters:
            result.setdefault((*identity, index), set()).add(class_name)
    return result


def common_owner(first, second, identity):
    left, right = first.get(identity), second.get(identity)
    if not left or not right:
        return None
    if left != right or len(left) != 1:
        return "!"  # Negative hierarchy evidence; an empty string is a root.
    return next(iter(left))


def common_methods(first, second, identity):
    left, right = first.get(identity), second.get(identity)
    if not left or not right:
        return [(None, "", False)]
    encodings = {record[0] for record in left}
    if encodings != {record[0] for record in right} or "" in encodings:
        return [("", "", False)]
    result = []
    for encoding in sorted(encodings):
        types = {(record[1], record[2]) for record in left | right
                 if record[0] == encoding}
        return_type = next(iter(types)) if len(types) == 1 else ("", False)
        result.append((encoding, *return_type))
    return result


def common_parameter(first, second, identity):
    left, right = first.get(identity), second.get(identity)
    if not left or not right or left != right or len(left) != 1:
        return None
    return next(iter(left))


def literal(value):
    return "nullptr" if value is None else json.dumps(value)


def render(frameworks, version, compiler):
    lines = ["// clang-format off",
             "// Generated by scripts/generate_objc_receiver_declarations.py.",
             f"// Compiler-derived declaration ownership: MacOSX SDK {version}.",
             f"// {compiler}", "// Target profiles: " + ", ".join(TARGETS),
             "// Public imports: " + ", ".join(framework_header(name) for name, _, _ in sorted(frameworks)),
             "// No framework implementation is included.",
             '// Owner "!" and empty method encodings are negative evidence.',
             "// nullptr denotes an absent cross-platform declaration."]
    for framework, modules, profiles in sorted(frameworks):
        if len(profiles) != len(TARGETS):
            raise ValueError("all four target profiles are required")
        owners = [owner_profiles(profile) for profile in profiles]
        methods = [method_profiles(profile) for profile in profiles]
        parameters = [parameter_profiles(profile) for profile in profiles]
        for identity in sorted(set().union(*owners)):
            arm = common_owner(owners[0], owners[1], identity)
            x64 = common_owner(owners[2], owners[3], identity)
            if arm is None and x64 is None:
                continue
            values = (framework, modules, *identity, arm, x64)
            lines.append("ND_OBJC_OWNER(" + ", ".join(map(literal, values)) + ")")
        for identity in sorted(set().union(*methods)):
            arm = common_methods(methods[0], methods[1], identity)
            x64 = common_methods(methods[2], methods[3], identity)
            if arm[0][0] is None and x64[0][0] is None:
                continue
            for index in range(max(len(arm), len(x64))):
                values = (framework, modules, *identity,
                          *arm[min(index, len(arm) - 1)],
                          *x64[min(index, len(x64) - 1)])
                lines.append("ND_OBJC_MEMBER(" + ", ".join(map(literal, values)) + ")")
        for identity in sorted(set().union(*parameters)):
            arm = common_parameter(parameters[0], parameters[1], identity)
            x64 = common_parameter(parameters[2], parameters[3], identity)
            if arm is None and x64 is None:
                continue
            values = (framework, modules, *identity, arm, x64)
            lines.append("ND_OBJC_OUT_PARAMETER(" +
                         ", ".join(map(literal, values)) + ")")
    return "\n".join(lines + ["// clang-format on", ""])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--libclang", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--framework", action="append")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    frameworks = []
    for framework in sorted(set(args.framework or ("Foundation", *DEFAULT_FRAMEWORKS))):
        module_paths(framework,
                     f"/System/Library/Frameworks/{framework}.framework/{framework}")
        headers = sdk / f"System/Library/Frameworks/{framework}.framework/Headers"
        # Match the existing Foundation catalog's public Objective-C base
        # declarations. Other frameworks retain their own header boundary.
        clang = ReceiverDeclarations(args.libclang, headers.resolve(strict=True),
                                     include_dependencies=framework == "Foundation")
        with tempfile.TemporaryDirectory(prefix="neverd-receiver-declarations-") as work:
            source = Path(work) / "declarations.m"
            source.write_text(f"#import <{framework_header(framework)}>\n")
            profiles = [clang.extract_owned(source, sdk, target) for target in TARGETS]
        frameworks.append((framework, sdk_module_paths(sdk, framework), profiles))
    version = json.loads((sdk / "SDKSettings.json").read_text())["Version"]
    output = render(frameworks, version, clang.string(clang.clang_getClangVersion()))
    if args.check:
        if args.output.read_text() != output:
            parser.error("generated receiver declarations differ")
    else:
        args.output.write_text(output)
    print(f"verified {sum(line.startswith('ND_OBJC_') for line in output.splitlines())} "
          "owned declaration records")


if __name__ == "__main__":
    main()
