#!/usr/bin/env python3
"""Authored DEX: a loadLibrary initializer and retained Java/native call chains."""
import hashlib
import struct
import zlib
from benchmark_mobile_inventory import uleb128


def resume_dex(binder_only=False, dispatch=False):
    owner, obj = 'Lresumption/Library;', 'Ljava/lang/Object;'
    string, system = 'Ljava/lang/String;', 'Ljava/lang/System;'
    # owner, name, result, parameters, access, registers, code factory
    declarations = []

    def add(name, result, params, flags=9, regs=1, code=None, cls=owner):
        declarations.append((cls, name, result, tuple(params), flags, regs, code))

    def invoke(mid, name, regs=(0,), cls=owner):
        index = mid[(cls, name)]
        return [0x71 | (len(regs) << 12), index,
                sum(r << (4*i) for i, r in enumerate(regs))]

    def init(mid, si):
        return ([0x1012, 0x0067, 0, 0x001a, si['resumption']] +
                invoke(mid, 'loadLibrary', cls=system) +
                [0x0060, 0, 0x00d8, 0x0100, 0x0067, 0, 0x000e])

    add('<clinit>', 'V', [], 0x10008, code=init)
    add('<init>', 'V', [], 0x10001, regs=2,
        code=lambda m, s: [0x1070, m[(obj, '<init>')], 1, 0x5012] +
        invoke(m, 'apply') + [0x000a, 0x000e])
    add('run', 'I', ['I'], code=lambda m, s: invoke(m, 'apply') +
        [0x000a, 0x00d8, 0x0300, 0x000f])
    add('step', 'I', ['I'], code=lambda m, s: invoke(m, 'helper') +
        [0x000a, 0x00d8, 0x0700, 0x000f])
    add('again', 'V', [], code=lambda m, s:
        [0x001a, s['resumption']] + invoke(m, 'loadLibrary', cls=system) + [0xe])
    add('loop', 'V', [], regs=2, code=lambda m, s:
        [0x001a, s['resumption'], 0x0113, 4096] +
        invoke(m, 'loadLibrary', cls=system) +
        [0x01d8, 0xff01, 0x0139, 0xfffb, 0xe])
    add('reference', obj, [obj], code=lambda m, s: invoke(m, 'echo') + [0xc, 0x11])
    add('throwing', 'I', [], code=lambda m, s: invoke(m, 'fail', ()) + [0xa, 0xf])
    for name, result, params in [('apply', 'I', ['I']), ('helper', 'I', ['I']),
                                 ('echo', obj, [obj]), ('fail', 'I', [])]:
        add(name, result, params, 0x109, 0)
    add('loadLibrary', 'V', [string], cls=system)
    add('<init>', 'V', [], cls=obj)
    if binder_only:
        declarations.clear()
        owner = 'Lresumption/Binder;'
        binder, parcel = 'Landroid/os/IBinder;', 'Landroid/os/Parcel;'
        def transactions(mid, si):
            tx = [0x5172, mid[(binder, 'transact')], 0x6504, 0x020a]
            return ([0xf012, 0x0112] + tx + tx +
                    [0x1071, mid[(owner, 'step')], 2, 0x020a] + tx + [0x020f])
        add('run', 'I', [binder, parcel, parcel], regs=7,
            code=transactions, cls=owner)
        add('step', 'I', ['I'], 0x109, 0, cls=owner)
        add('transact', 'Z', ['I', parcel, parcel, 'I'], cls=binder)
    parent = obj
    if dispatch:
        declarations.clear()
        owner, parent = 'Lresumption/Dispatch;', 'Landroid/os/Binder;'
        parcel = 'Landroid/os/Parcel;'
        add('<init>', 'V', [], 0x10001, 1,
            lambda m, s: [0x1070, m[(parent, '<init>')], 0, 0xe], cls=owner)
        add('<init>', 'V', [], cls=parent)
        add('onTransact', 'Z', ['I', parcel, parcel, 'I'], 4, 6,
            lambda m, s: [0x5571, m[(owner, 'step')], 0x4321, 0xa, 0xf], cls=owner)
        add('step', 'Z', [owner, 'I', parcel, parcel, 'I'], 0x109, 0, cls=owner)
    types = sorted({owner, parent, obj, string, system, 'I', *[d[2] for d in declarations],
                    *[t for d in declarations for t in d[3]]})
    ti = {t: i for i, t in enumerate(types)}
    protos = sorted({(d[2], d[3]) for d in declarations},
                    key=lambda p: (ti[p[0]], tuple(ti[t] for t in p[1])))

    def shorty(p):
        return ''.join('L' if t.startswith(('L', '[')) else t for t in (p[0], *p[1]))

    strings = sorted({*types, 'count', 'resumption', *[d[1] for d in declarations],
                      *[shorty(p) for p in protos]})
    si = {s: i for i, s in enumerate(strings)}
    methods = sorted(declarations, key=lambda d:
                     (ti[d[0]], si[d[1]], protos.index((d[2], d[3]))))
    mi = {(d[0], d[1]): i for i, d in enumerate(methods)}
    data = bytearray(112)
    sections = [(0, 1, 0)]

    def table(kind, count, size):
        at = len(data)
        data.extend(bytes(count * size))
        sections.append((kind, count, at))
        return at

    so, to = table(1, len(strings), 4), table(2, len(types), 4)
    po, fo = table(3, len(protos), 12), table(4, 1, 8)
    mo, co = table(5, len(methods), 8), table(6, 1, 32)
    start = len(data)
    sections.append((0x2002, len(strings), len(data)))
    for i, s in enumerate(strings):
        struct.pack_into('<I', data, so + 4*i, len(data))
        data.extend(uleb128(len(s)) + s.encode() + b'\0')
    for i, t in enumerate(types):
        struct.pack_into('<I', data, to + 4*i, si[t])
    lists = {}
    for _, args in protos:
        if args and args not in lists:
            data.extend(bytes(-len(data) % 4))
            lists[args] = len(data)
            data.extend(struct.pack('<I', len(args)))
            data.extend(struct.pack('<' + 'H'*len(args), *[ti[t] for t in args]))
    sections.append((0x1001, len(lists), min(lists.values())))
    for i, p in enumerate(protos):
        struct.pack_into('<III', data, po + 12*i, si[shorty(p)], ti[p[0]], lists.get(p[1], 0))
    struct.pack_into('<HHI', data, fo, ti[owner], ti['I'], si['count'])
    code = {}
    for i, (cls, name, ret, args, flags, regs, factory) in enumerate(methods):
        struct.pack_into('<HHI', data, mo + 8*i, ti[cls], protos.index((ret, args)), si[name])
        if factory:
            words = factory(mi, si)
            data.extend(bytes(-len(data) % 4))
            code[i] = len(data)
            incoming = len(args) + (not flags & 8)
            data.extend(struct.pack('<HHHHII', regs, incoming, 5 if binder_only or dispatch else 1, 0, 0, len(words)))
            data.extend(struct.pack('<' + 'H'*len(words), *words))
    sections.append((0x2001, len(code), min(code.values())))
    at = len(data)
    direct = [i for i, m in enumerate(methods) if m[0] == owner and (m[4] & 8 or m[1] == '<init>')]
    virtual = [i for i, m in enumerate(methods) if m[0] == owner and i not in direct]
    data.extend(uleb128(1) + b'\0' + uleb128(len(direct)) + uleb128(len(virtual)))
    data.extend(uleb128(0) + uleb128(9))  # public static count
    for group in (direct, virtual):
        last = 0
        for i in group:
            data.extend(uleb128(i-last) + uleb128(methods[i][4]) + uleb128(code.get(i, 0)))
            last = i
    sections.append((0x2000, 1, at))
    struct.pack_into('<8I', data, co, ti[owner], 1, ti[parent], 0, 0xffffffff, 0, at, 0)
    data.extend(bytes(-len(data) % 4))
    map_at = len(data)
    sections.append((0x1000, 1, map_at))
    data.extend(struct.pack('<I', len(sections)))
    for kind, count, at in sorted(sections, key=lambda s: s[2]):
        data.extend(struct.pack('<HHII', kind, 0, count, at))
    data[:8] = b'dex\n035\0'
    struct.pack_into('<20I', data, 32, len(data), 112, 0x12345678, 0, 0, map_at,
                     len(strings), so, len(types), to, len(protos), po, 1, fo,
                     len(methods), mo, 1, co, len(data)-start, start)
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into('<I', data, 8, zlib.adler32(data[12:]) & 0xffffffff)
    return data
