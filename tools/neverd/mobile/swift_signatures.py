"""Decode structured Swift demangler nodes into conservative source hints.

This module never derives a physical layout from a pretty-printed signature.
The native loader must cross-check the symbol address and establish ABI binding.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
import re

from .common import Limits, MobileError, run_tool

_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z_0-9]*\Z")
_NODE = re.compile(r'(?P<indent> *)(?:kind=(?P<kind>[A-Za-z_0-9]+))(?P<attrs>(?:, (?:text="(?:[^"\\]|\\.)*"|index=[0-9]+))*)\Z')
_CALLABLE_NODES = frozenset({
    'Function', 'Allocator', 'Constructor', 'Destructor', 'Deallocator',
    'Getter', 'Setter', 'GlobalGetter', 'ModifyAccessor', 'ReadAccessor',
    'MaterializeForSet', 'WillSet', 'DidSet', 'ExplicitClosure', 'ImplicitClosure',
    'DefaultArgumentInitializer', 'Initializer', 'IVarInitializer', 'IVarDestroyer',
    'UnsafeAddressor', 'UnsafeMutableAddressor', 'OwningAddressor', 'OwningMutableAddressor',
    'NativeOwningAddressor', 'NativeOwningMutableAddressor', 'NativePinningAddressor',
    'NativePinningMutableAddressor', 'ProtocolWitness', 'ReabstractionThunk',
    'ReabstractionThunkHelper', 'CurryThunk', 'VTableThunk', 'DispatchThunk',
    'MethodLookupFunction', 'TypeMetadataAccessFunction',
})
_METADATA_NODES = frozenset({
    'ModuleDescriptor', 'NominalTypeDescriptor', 'ProtocolDescriptor',
    'TypeMetadata', 'FullTypeMetadata', 'TypeMetadataPattern', 'Metaclass',
    'ClassMetadataBaseOffset', 'PropertyDescriptor', 'FieldOffset',
    'MethodDescriptor', 'ProtocolWitnessTable', 'ValueWitnessTable',
    'ReflectionMetadataFieldDescriptor', 'ReflectionMetadataBuiltinDescriptor',
    'ReflectionMetadataAssocTypeDescriptor', 'ReflectionMetadataSuperclassDescriptor',
})
_ATTRIBUTE = re.compile(r', (text="(?:[^"\\]|\\.)*"|index=[0-9]+)')


class _Unsupported(ValueError):
    pass


@dataclass
class _Node:
    kind: str
    text: str | None = None
    index: int | None = None
    children: list[_Node] = field(default_factory=list)


def _tree(text: str) -> _Node:
    root = None
    stack: list[_Node] = []
    count = 0
    for line in text.splitlines():
        if not line:
            continue
        match = _NODE.fullmatch(line)
        if not match or len(match['indent']) % 2:
            raise _Unsupported("malformed Swift demangling tree")
        depth = len(match['indent']) // 2
        count += 1
        if depth > 64 or count > 10000 or depth > len(stack):
            raise _Unsupported("invalid or excessive Swift demangling tree depth")
        node = _Node(match['kind'])
        seen = set()
        for attribute in _ATTRIBUTE.findall(match['attrs']):
            key, value = attribute.split('=', 1)
            if key in seen:
                raise _Unsupported("duplicate Swift demangling node attribute")
            seen.add(key)
            try:
                setattr(node, key, json.loads(value) if key == 'text' else int(value))
            except (ValueError, UnicodeError) as error:
                raise _Unsupported("invalid Swift demangling node attribute") from error
        if depth == 0:
            if root is not None:
                raise _Unsupported("multiple Swift demangling roots")
            root = node
        else:
            stack[depth - 1].children.append(node)
        stack[depth:] = [node]
    if root is None:
        raise _Unsupported("empty Swift demangling tree")
    return root


def _one(node: _Node, kind: str | None = None) -> _Node:
    if len(node.children) != 1 or (kind is not None and node.children[0].kind != kind):
        raise _Unsupported("unsupported Swift type or declaration shape")
    return node.children[0]


def _identifier(node: _Node, kind: str = 'Identifier') -> str:
    if node.kind != kind or node.children or node.text is None or not _IDENTIFIER.fullmatch(node.text):
        raise _Unsupported("unsafe or unsupported Swift declaration identifier")
    return node.text


def _nominal(node: _Node) -> tuple[str, str]:
    if len(node.children) != 2:
        raise _Unsupported("nested or generic Swift declaration contexts are unsupported")
    return _identifier(node.children[0], 'Module'), _identifier(node.children[1])


def _context(node: _Node) -> tuple[str, str, str]:
    if node.kind == 'Module':
        return _identifier(node, 'Module'), '', 'global'
    if node.kind in {'Class', 'Structure'}:
        module, name = _nominal(node)
        return module, name, 'class' if node.kind == 'Class' else 'struct'
    raise _Unsupported("unsupported Swift declaration context")


def _property(node: _Node, is_static: bool, pointer_size: int) -> tuple:
    variable = _one(node, 'Variable')
    if len(variable.children) != 3:
        raise _Unsupported("unsupported Swift property declaration shape")
    context, property_name, property_type = variable.children
    module, context_name, context_kind = _context(context)
    if property_type.kind != 'Type':
        raise _Unsupported("Swift property type wrapper is missing")
    if context_kind == 'global' or is_static:
        raise _Unsupported("Swift global and static property storage conventions are not established")
    value_type = _type(property_type, pointer_size)
    if value_type['kind'] == 'void':
        raise _Unsupported("Swift scalar property has a void type")
    return module, context_name, context_kind, _identifier(property_name), value_type


def _accessor(row: dict, node: _Node, is_static: bool, pointer_size: int) -> None:
    module, context_name, context_kind, name, value_type = _property(node, is_static, pointer_size)
    setter = node.kind == 'Setter'
    row.update(declaration_kind='setter' if setter else 'getter', module=module,
               context_kind=context_kind, context_name=context_name, name=name,
               labels=['_'] if setter else [],
               parameters=[{'name': 'arg0', 'type': value_type}] if setter else [],
               return_type={'kind': 'void', 'name': 'Void'} if setter else value_type,
               is_static=False, is_mutating=False, is_mutating_known=context_kind == 'class')
    if context_kind == 'struct':
        row['requires_self_abi_proof'] = True
        raise _Unsupported("value-type accessor self layout and mutating convention require native ABI proof")
    row['status'] = 'supported'


def _runtime_declaration(row: dict, node: _Node, is_static: bool, pointer_size: int,
                         *, continuation: bool = False) -> None:
    if node.kind == 'ModifyAccessor':
        module, context_name, context_kind, name, value_type = _property(node, is_static, pointer_size)
        row['property_type'] = value_type
        kind = 'modify_resume' if continuation else 'modify_accessor'
    else:
        if is_static or continuation:
            raise _Unsupported("unsupported Swift runtime declaration wrapper")
        nominal = _one(node)
        if node.kind == 'TypeMetadataAccessFunction':
            nominal = _one(nominal) if nominal.kind == 'Type' else nominal
        module, context_name, context_kind = _context(nominal)
        if context_kind == 'global' or (node.kind != 'TypeMetadataAccessFunction' and context_kind != 'class'):
            raise _Unsupported("unsupported Swift runtime declaration context")
        kind = {'Destructor': 'destructor', 'Deallocator': 'deallocator',
                'TypeMetadataAccessFunction': 'type_metadata_accessor'}[node.kind]
        name = 'typeMetadata' if node.kind == 'TypeMetadataAccessFunction' else 'deinit'
    row.update(declaration_kind='runtime', runtime_source_kind=kind, module=module,
               context_kind=context_kind, context_name=context_name, name=name,
               parameters=[], labels=[], return_type={'kind': 'void', 'name': 'Void'},
               is_static=False, is_mutating=False, is_mutating_known=False,
               requires_runtime_source_proof=True)
    raise _Unsupported("Swift compiler entry requires native effect and recovered source dependency proof")


def _type(node: _Node, pointer_size: int, depth: int = 0) -> dict:
    if depth > 8:
        raise _Unsupported("Swift pointer type is too deep")
    if node.kind == 'Type':
        return _type(_one(node), pointer_size, depth)
    if node.kind == 'Tuple' and not node.children:
        return {'kind': 'void', 'name': 'Void'}
    if node.kind == 'BoundGenericStructure':
        if len(node.children) != 2 or node.children[0].kind != 'Type' or node.children[1].kind != 'TypeList':
            raise _Unsupported("unsupported Swift generic type")
        nominal = _one(node.children[0], 'Structure')
        module, name = _nominal(nominal)
        if module != 'Swift' or name not in {'UnsafePointer', 'UnsafeMutablePointer'}:
            raise _Unsupported("Swift generic types other than ordinary pointers are unsupported")
        pointee = _type(_one(node.children[1], 'Type'), pointer_size, depth + 1)
        if pointee['kind'] == 'void':
            raise _Unsupported("typed Swift pointer has a void pointee")
        return {'kind': 'pointer', 'name': name, 'pointee': pointee}
    if node.kind != 'Structure':
        raise _Unsupported("unsupported Swift signature type: " + node.kind)
    module, name = _nominal(node)
    if module != 'Swift':
        raise _Unsupported("user-defined Swift value layouts are not established")
    if name in {'Float', 'Double'}:
        return {'kind': 'float', 'name': name, 'bits': 32 if name == 'Float' else 64}
    if name == 'Bool':
        return {'kind': 'bool', 'name': name, 'bits': 1}
    if name in {'UnsafeRawPointer', 'UnsafeMutableRawPointer'}:
        return {'kind': 'pointer', 'name': name}
    match = re.fullmatch(r'(U?Int)(8|16|32|64)?', name)
    if not match:
        raise _Unsupported("unsupported Swift scalar type: " + name)
    return {'kind': 'integer', 'name': name, 'bits': int(match[2]) if match[2] else pointer_size * 8,
            'signed': match[1] == 'Int'}


def parse_swift_signature(mangled_symbol: str, entry: str, tree: str, *, pointer_size: int = 8) -> dict:
    """Return one inventory row, retaining unsupported symbols and reasons."""
    row = {'entry': entry, 'mangled_symbol': mangled_symbol, 'status': 'unsupported', 'classification': 'unknown'}
    try:
        if (not isinstance(mangled_symbol, str) or len(mangled_symbol) > 65536 or
                not mangled_symbol.lstrip('_').startswith(('$s', '$S', 'T0')) or
                not isinstance(entry, str) or not re.fullmatch(r'0x[0-9a-fA-F]+', entry) or
                type(pointer_size) is not int or pointer_size != 8):
            raise _Unsupported("invalid Swift symbol identity or unsupported pointer size")
        node = _tree(tree)
        if node.kind != 'Global':
            raise _Unsupported("Swift demangling root is not a global symbol")
        if (len(node.children) == 2 and node.children[0].kind in _CALLABLE_NODES and
                node.children[1].kind == 'Suffix' and not node.children[1].children and
                isinstance(node.children[1].text, str) and re.fullmatch(r'\.resume\.[0-9]+', node.children[1].text)):
            row.update(classification='callable', node_kind='CoroutineContinuation',
                       continuation_of=node.children[0].kind, compiler_suffix=node.children[1].text)
            if node.children[0].kind == 'ModifyAccessor':
                _runtime_declaration(row, node.children[0], False, pointer_size, continuation=True)
            raise _Unsupported("Swift coroutine continuation requires its suspended-frame calling convention")
        node = _one(node)
        is_static = node.kind == 'Static'
        if is_static:
            node = _one(node)
        row['node_kind'] = node.kind
        row['classification'] = 'callable' if node.kind in _CALLABLE_NODES else 'metadata' if node.kind in _METADATA_NODES else 'unknown'
        if node.kind in {'Getter', 'Setter'}:
            _accessor(row, node, is_static, pointer_size)
            return row
        if node.kind in {'Destructor', 'Deallocator', 'TypeMetadataAccessFunction', 'ModifyAccessor'}:
            _runtime_declaration(row, node, is_static, pointer_size)
        is_initializer = node.kind in {'Constructor', 'Allocator'}
        if is_initializer and len(node.children) == 3:
            context, labels, function_type = node.children
            function_name = _Node('Identifier', text='init')
        elif node.kind == 'Function' and len(node.children) == 4:
            context, function_name, labels, function_type = node.children
        else:
            raise _Unsupported("Swift symbol is not a plain fixed-signature function or initializing constructor")
        module, context_name, context_kind = _context(context)
        if is_static and context_kind == 'global':
            raise _Unsupported("global Swift function has a static marker")
        row.update(declaration_kind='initializer' if is_initializer else 'function', module=module,
                   context_kind=context_kind, context_name=context_name, name=_identifier(function_name))
        if is_initializer and (context_kind not in {'class', 'struct'} or is_static):
            raise _Unsupported("unsupported Swift initializing-constructor context")
        name = _identifier(function_name)
        if labels.kind != 'LabelList' or function_type.kind != 'Type':
            raise _Unsupported("unsupported Swift parameter labels or function type")
        function_type = _one(function_type, 'FunctionType')
        if len(function_type.children) != 2 or [child.kind for child in function_type.children] != ['ArgumentTuple', 'ReturnType']:
            raise _Unsupported("async, throwing, generic or other Swift function conventions are unsupported")
        argument = _one(function_type.children[0], 'Type')
        argument_value = _one(argument)
        if argument_value.kind == 'Tuple':
            arguments = []
            for element in argument_value.children:
                if element.kind != 'TupleElement':
                    raise _Unsupported("unsupported Swift argument tuple")
                arguments.append(_type(_one(element, 'Type'), pointer_size))
        else:
            arguments = [_type(argument, pointer_size)]
        if any(value['kind'] == 'void' for value in arguments):
            raise _Unsupported("Swift function has a void argument")
        parameter_labels = []
        for label in labels.children:
            if label.kind == 'FirstElementMarker' and not label.children and label.text is None:
                parameter_labels.append('_')
            else:
                parameter_labels.append(_identifier(label))
        if not parameter_labels:
            parameter_labels = ['_'] * len(arguments)
        if len(parameter_labels) != len(arguments):
            raise _Unsupported("Swift parameter labels disagree with the argument count")
        returned_node = _one(function_type.children[1], 'Type')
        if is_initializer:
            nominal_return = _one(returned_node, 'Class' if context_kind == 'class' else 'Structure')
            if _nominal(nominal_return) != (module, context_name):
                raise _Unsupported("Swift initializer return identity disagrees with its context")
            returned_type = ({'kind': 'pointer', 'name': 'UnsafeMutableRawPointer'} if context_kind == 'class' else
                             {'kind': 'nominal', 'module': module, 'name': context_name, 'context_kind': 'struct'})
        else:
            returned_type = _type(returned_node, pointer_size)
        row.update(declaration_kind='initializer' if is_initializer else 'function', module=module, context_kind=context_kind, context_name=context_name, name=name,
                   labels=parameter_labels, parameters=[{'name': f'arg{index}', 'type': value} for index, value in enumerate(arguments)],
                   return_type=returned_type,
                   is_static=is_static, is_mutating=False, is_mutating_known=is_initializer or context_kind != 'struct' or is_static)
        if node.kind == 'Allocator' and context_kind == 'class':
            row.update(declaration_kind='runtime', runtime_source_kind='allocating_initializer',
                       requires_runtime_source_proof=True)
            raise _Unsupported("Swift allocating constructor requires native allocation and initializer effect proof")
        if is_initializer and context_kind == 'struct':
            row['requires_storage_abi_proof'] = True
            raise _Unsupported("value-type initializer return layout is not established by mangling")
        if context_kind == 'struct' and not is_static:
            row['requires_self_abi_proof'] = True
            raise _Unsupported("value-type self layout and mutating convention are not established by mangling")
        row['status'] = 'supported'
    except _Unsupported as error:
        row['reason'] = str(error)
    return row


def recover_swift_signatures(symbols: list[dict], *, demangler: str, log_directory: Path,
                              limits: Limits, pointer_size: int = 8) -> dict:
    """Run a separately installed demangler using bounded argument vectors."""
    if not isinstance(symbols, list) or len(symbols) > limits.max_files:
        raise MobileError("Swift symbol inventory exceeds the file limit")
    log_directory.mkdir(parents=True, exist_ok=True)
    inventory = []
    logs = []
    # Bounded command lines also work with CreateProcess's 32767-character cap.
    batches: list[list[tuple[str, str]]] = [[]]
    command_bytes = 0
    for symbol in symbols:
        if not isinstance(symbol, dict) or not isinstance(symbol.get('name'), str) or not isinstance(symbol.get('address'), str):
            raise MobileError("invalid Swift symbol inventory entry")
        name, address = symbol['name'], symbol['address']
        if not name.lstrip('_').startswith(('$s', '$S', 'T0')):
            continue
        if len(name) > 8000 or any(character in name for character in '\r\n\x00'):
            inventory.append({'entry': address, 'mangled_symbol': name, 'status': 'unsupported', 'classification': 'unknown', 'reason': 'Swift symbol exceeds safe demangler input limits'})
            continue
        if command_bytes + len(name) > 12000 or len(batches[-1]) == 64:
            batches.append([])
            command_bytes = 0
        batches[-1].append((name, address))
        command_bytes += len(name) + 3
    for index, batch in enumerate(batches):
        if not batch:
            continue
        log = log_directory / f'swift-demangle-{index:04d}.log'
        run_tool([demangler, '--expand', '--tree-only', *[name for name, _ in batch]], log, limits.timeout)
        logs.append(log.name)
        text = log.read_text(encoding='utf-8')
        sections = re.split(r'^Demangling for ', text, flags=re.M)
        if sections[0].strip() or len(sections) != len(batch) + 1:
            raise MobileError("Swift demangler returned an unexpected structured output")
        for section, (name, address) in zip(sections[1:], batch):
            title, separator, tree = section.partition('\n')
            if not separator or title != name:
                raise MobileError("Swift demangler symbol identity disagrees with its input")
            inventory.append(parse_swift_signature(name, address, tree, pointer_size=pointer_size))
    methods = [row for row in inventory if row['classification'] == 'callable']
    other_symbols = [row for row in inventory if row['classification'] != 'callable']
    supported = sum(method['status'] == 'supported' for method in methods)
    return {'schema_version': 1, 'methods': methods, 'symbols': other_symbols,
            'method_count': len(methods), 'symbol_count': len(inventory),
            'unclassified_symbol_count': sum(row['classification'] == 'unknown' for row in other_symbols),
            'supported_signature_count': supported, 'unsupported_signature_count': len(methods) - supported,
            'logs': logs, 'limitations': [
                'Mangled symbols describe source types, not authenticated ABI or complete native decoding.',
                'Swift value-type self layout and mutating convention require evidence beyond a function mangling.',
                'Compiler runtime entries retain their context and require native effect and recovered source dependency proofs; demangling alone never recovers their bodies.',
                'Stripped symbols, generic, async, throwing and aggregate signatures are not recovered by this signature reader.',
                'Pure metadata symbols are excluded from method coverage; unclassified symbols may conceal additional callable declarations.',
            ]}
