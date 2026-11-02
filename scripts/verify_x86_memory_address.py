"""Independent decoder for the native vector probes' ordinary SIB carriers."""


def memory_address(code, end, p0, modrm, bits):
    mod, base, index, scale = modrm >> 6, modrm & 7, None, 1
    assert mod != 3, "expected a memory operand"
    if base == 4:
        assert end < len(code), "truncated SIB"
        sib = code[end]
        end += 1
        base, index, scale = sib & 7, (sib >> 3) & 7, 1 << (sib >> 6)
        index += ((~p0 >> 6) & 1) * 8
        if index == 4:
            index = None
    assert not (mod == 0 and base == 5), "unexpected absolute/RIP carrier"
    base += ((~p0 >> 5) & 1) * 8
    size = 1 if mod == 1 else 4 if mod == 2 else 0
    assert end + size <= len(code), "truncated displacement"
    displacement = int.from_bytes(code[end:end + size], 'little', signed=True)
    end += size
    assert displacement == 0, "native transport introduced a displacement"
    stack = base in (4, 5)
    if stack:
        assert bits == 64 and base == 5 and index in (0, 1, 2, 3) and scale == 1
    else:
        assert mod == 0 and base in (0, 1, 2, 3) and index is None
    return end, stack, base, index, scale
