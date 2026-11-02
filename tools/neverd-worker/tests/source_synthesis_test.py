#!/usr/bin/env python3
"""GUI source requests and CLI flags reach the real semantic synthesis engine."""
import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from transport_test import Client


ENTRY = 0x400078
WEIGHTED_MASKS = (0x7A6D35C1, 0xB53C678A, 0x4EF0A903, 0xD2195A6C)
WEIGHTED_UNION = 0
for _mask in WEIGHTED_MASKS:
    WEIGHTED_UNION |= _mask
PRODUCT_MASKS = (0x91A2C45E, 0x6F3C92A1, 0xD8743BC6, 0x25E9A70D)
PRODUCT_UNION = 0
for _mask in PRODUCT_MASKS:
    PRODUCT_UNION |= _mask
VIEW_MASKS = tuple(0x10001 << bit for bit in range(16))
VIEW_KEYS = tuple((0x13579BDF * (bit + 1)) & 0xFFFFFFFF for bit in range(16))
VIEW_KEY = 0
for _mask, _key in zip(VIEW_MASKS, VIEW_KEYS):
    VIEW_KEY |= _mask & _key


def affine_elf():
    # System V x64: 182*x + 73*y + 211*z + 37
    #                + (x >> 4) - ((x >> 2) >> 2), all in 32-bit registers.
    # Keep the original shifts as distinct instructions, independent of a
    # fixture compiler's optimization and instruction-selection policies.
    code = bytes.fromhex(
        "69c7b6000000 6bce49 01c8 69cad3000000 01c8 83c025 "
        "89f9 c1e904 89fa c1ea02 c1ea02 29d1 01c8 c3")
    return elf(code)


def quadratic_elf():
    # System V x64: 11*x*x + 73*x*y + 182*y + 211*z + 37, plus the
    # same right-shift residual as the affine fixture.
    code = bytes.fromhex(
        "89f8 0fafc7 6bc00b 89f9 0fafce 6bc949 01c8 "
        "69ceb6000000 01c8 69cad3000000 01c8 83c025 "
        "89f9 c1e904 89fa c1ea02 c1ea02 29d1 01c8 c3")
    return elf(code)


def masked_elf():
    # System V x64: 37 * (((x ^ 0x39) | (y ^ 0x6d)) +
    #                    (x ^ ~0x39)) + 37, then OR 0x55 and add z.
    # The equivalent two-XOR form is (37*((x ^ ~0x39)&(y ^ 0x6d))|0x55)+z.
    code = bytes.fromhex(
        "89f8 3539000000 89f1 83f16d 09c8 83f7c6 01f8 "
        "6bc025 83c025 83c855 01d0 c3")
    return elf(code)


def frame_elf():
    # Store all three incoming registers. The unused middle store leaves an
    # empty source block; the two surviving homes feed (z|~x)+z+1+(z&~x).
    code = bytes.fromhex(
        "55 4889e5 897dfc 8975f8 8955f4 8b4dfc f7d1 "
        "8b45f4 09c8 0345f4 ffc0 234df4 01c8 5d c3")
    return elf(code)


def weighted_elf():
    # Fifteen overlapping masks form an inclusion-exclusion union over x^y.
    # Keep every AND/add/sub in machine code, then scale the sum and add z.
    code = bytearray.fromhex("89f9 31f1 31c0")
    for subset in range(1, 1 << len(WEIGHTED_MASKS)):
        mask = 0xFFFFFFFF
        for bit, value in enumerate(WEIGHTED_MASKS):
            if subset & (1 << bit):
                mask &= value
        code += bytes.fromhex("4189c8 4181e0") + struct.pack("<I", mask)
        code += bytes.fromhex("4401c0" if subset.bit_count() % 2 else "4429c0")
    code += bytes.fromhex("6bc025 01d0 c3")
    return elf(code)


def bitwise_elf():
    # Independently flipped, disjoint pieces of x^y recombine into one XOR.
    # Encode every piece directly so a fixture compiler cannot simplify it.
    code = bytearray.fromhex("89f9 31f1 31c0")
    for mask, key in zip(VIEW_MASKS, VIEW_KEYS):
        code += bytes.fromhex("4189c8 4181f0") + struct.pack("<I", key)
        code += bytes.fromhex("4181e0") + struct.pack("<I", mask)
        code += bytes.fromhex("4401c0")
    code += bytes.fromhex("6bc025 01d0 c3")
    return elf(code)


def interleaved_elf():
    # Interleave two fifteen-term inclusion-exclusion sums over x^y and x*y.
    # Both words share the positive/negative accumulators and enclosing scale.
    code = bytearray.fromhex("89f9 31f1 0faffe 31c0 4531d2")
    for subset in range(1, 16):
        masks = [0xFFFFFFFF, 0xFFFFFFFF]
        for group, values in enumerate((WEIGHTED_MASKS, PRODUCT_MASKS)):
            for bit, value in enumerate(values):
                if subset & (1 << bit):
                    masks[group] &= value
        code += bytes.fromhex("4189c8 4181e0") + struct.pack("<I", masks[0])
        code += bytes.fromhex("4189f9 4181e1") + struct.pack("<I", masks[1])
        code += bytes.fromhex("4501c8")
        code += bytes.fromhex("4401c0" if subset.bit_count() % 2 else "4501c2")
    code += bytes.fromhex("4429d0 6bc007 01d0 c3")
    return elf(code)


def partitioned_elf(mixed=False):
    # Multiply all four independently partitioned pieces of A=x^y and B=y|z.
    # r8d/r9d retain A/B; r10d/r11d form each product. The three arguments
    # remain intact, and the final word is (x^y)*(y|z)+x+y modulo 2^32.
    code = bytearray.fromhex("4189f8 4131f0 4189f1 4109d1 31c0")
    for invert_a in (False, True):
        for invert_b in (False, True):
            code += bytes.fromhex("4589c2 4589cb")
            code += bytes.fromhex(
                "89d1 f7d1 4121ca" if invert_a else "4121d2")
            code += bytes.fromhex(
                "89f9 f7d1 4121cb" if invert_b else "4121fb")
            code += bytes.fromhex("450fafd3 4401d0")
    if mixed:
        # A scaled cubic residual used to exhaust the search allowance before
        # the enclosing quadratic/linear expression could finish.
        code += bytes.fromhex("69c0b6000000 89f9 0fafce 0fafca 6bc925 01c8")
    code += bytes.fromhex("01f8 01f0 c3")
    return elf(code)


def cubic_elf(degree=3, perturbed=False, missing=False):
    # Eight products of separately partitioned A=x^y, B=y|z and C=x|z.
    # Every original instruction remains in the fixture; no host optimizer
    # can fold the expansion before NeverD sees it. Keep a high-bit remainder.
    code = bytearray.fromhex("4189f8 4131f0 4189f1 4109d1 31c0")
    for invert_a in (False, True):
        for invert_b in (False, True):
            for invert_c in (False, True):
                if missing and not (invert_a or invert_b or invert_c):
                    continue
                code += bytes.fromhex("4589c2 4589cb")
                code += bytes.fromhex(
                    "89d1 f7d1 4121ca" if invert_a else "4121d2")
                code += bytes.fromhex(
                    "89f9 f7d1 4121cb" if invert_b else "4121fb")
                code += bytes.fromhex("450fafd3 89f9 09d1")
                code += bytes.fromhex(
                    "4189f3 41f7d3 4421d9" if invert_c else "21f1")
                code += bytes.fromhex("440fafd1")
                tails = (
                    "4189fb 4121f3",  # r11d = x & y
                    "4189fb 4131d3",  # r11d = x ^ z
                    "4189f3 4131d3",  # r11d = y ^ z
                )
                for index in range(degree - 3):
                    factor = tails[index % len(tails)]
                    code += bytes.fromhex(factor + " 450fafd3")
                if perturbed and not (invert_a or invert_b or invert_c):
                    code += bytes.fromhex("456bd203")  # r10d *= 3
                code += bytes.fromhex("4401d0")
    code += bytes.fromhex(
        "6bc025 89f9 31f1 01c8 89f9 21f1 d1e1 01c8 "
        "89f9 81e100000080 01c8 c3")
    return elf(code)


def residue_elf(degree):
    # r8d = x+y. Keep the product of degree consecutive offsets in original
    # machine instructions. Its valuation is at least v2(degree!), so shifting
    # it by the remaining bits makes the entire product zero modulo 2^32.
    # Keep x+y+z as an independent tail and retain all three ABI inputs.
    code = bytearray.fromhex("448d0437 4489c0")
    for offset in range(1, degree):
        code += bytes.fromhex("418d48") + bytes((offset,))
        code += bytes.fromhex("0fafc1")
    shift = 32 - (degree - degree.bit_count())
    code += bytes.fromhex("c1e0") + bytes((shift,))
    code += bytes.fromhex("01f8 01f0 01d0 c3")
    return elf(code)


def affine_residue_elf(degree, neighbor=False):
    # Original x64: r8d = x+3*y+5*z; each IMUL has a different odd unit.
    # The neighbor repeats the penultimate offset and must remain nonzero.
    units = (0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D, 0x27D4EB2F,
             0x165667B1, 0xD3A2646D, 0xA24BAED5, 0x9FB21C65)
    scale = 1 << (32 - (degree - degree.bit_count()))
    code = bytearray.fromhex("4189f8 448d0c76 4501c8 448d0c92 4501c8 b8")
    code += struct.pack("<I", scale)
    factors = []
    for i, unit in enumerate(units[:degree]):
        offset = i - 1 if neighbor and i == degree - 1 else i
        constant = unit * offset & 0xFFFFFFFF
        code += bytes.fromhex("4169c8") + struct.pack("<I", unit)
        code += bytes.fromhex("81c1") + struct.pack("<I", constant)
        code += bytes.fromhex("0fafc1")
        factors.append(f"({unit}U*(x+3U*y+5U*z)+{constant}U)")
    code += bytes.fromhex("01f8 01f0 01d0 c3")
    expected = "x+y+z"
    if neighbor:
        expected += f"+{scale}U*" + "*".join(factors)
    return elf(code), expected


def prefix_residue_elf(degree, neighbor=False, *, prefix_bits=None,
                       distributed_zeros=False):
    # Independently authored x64 words. Each factor changes its high bits;
    # only the common low-prefix relation justifies removing the product.
    units = (0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D, 0x27D4EB2F,
             0x165667B1, 0xD3A2646D, 0xA24BAED5, 0x9FB21C65)
    bits = prefix_bits or (2 if distributed_zeros else (degree - 1).bit_length())
    assert not distributed_zeros or bits == 2
    zeros = sum(degree >> bit for bit in range(1, bits + 1))
    scale = 1 << (32 - zeros)
    code = bytearray.fromhex("4189f8 448d0c76 4501c8 448d0c92 4501c8 b8")
    code += struct.pack("<I", scale)
    factors = []
    for i, unit in enumerate(units[:degree]):
        offset = min(i, degree - 2) if neighbor else i
        constant = unit * offset & 0xFFFFFFFF
        code += bytes.fromhex("4169c8") + struct.pack("<I", unit)
        code += bytes.fromhex("81c1") + struct.pack("<I", constant)
        # Preserve ABI inputs while building the independent disturbance.
        code += bytes.fromhex("4469cf") + struct.pack("<I", 2 * i + 1)
        if distributed_zeros:
            # r9d=(x*(2*i+1))&~1, r10d=(z+2*i+1)&~1. Each is only
            # known even; their product supplies the complete two-bit prefix.
            code += bytes.fromhex("4183e1fe 4189d2 4181c2")
            code += struct.pack("<I", 2 * i + 1)
            code += bytes.fromhex("4183e2fe 450fafca")
            high = f"(((x*{2*i+1}U)&0xfffffffeU)*((z+{2*i+1}U)&0xfffffffeU))"
        else:
            # r9d=(x*(2*i+1))<<bits, r10d=z<<bits.
            code += bytes.fromhex("41c1e1") + bytes((bits,))
            code += bytes.fromhex("4189d2 41c1e2") + bytes((bits,))
            code += bytes.fromhex("4509d1")
            high = f"(((x*{2*i+1}U)<<{bits})|(z<<{bits}))"
        if neighbor:
            code += bytes.fromhex("4401c9")  # ecx += high
            factor = f"({unit}U*(x+3U*y+5U*z)+{constant}U+{high})"
        else:
            code += bytes.fromhex("4431c9")  # ecx ^= high
            factor = f"(({unit}U*(x+3U*y+5U*z)+{constant}U)^{high})"
        code += bytes.fromhex("0fafc1")
        factors.append(factor)
    code += bytes.fromhex("01f8 01f0 01d0 c3")
    expected = "x+y+z"
    if neighbor:
        expected += f"+{scale}U*" + "*".join(factors)
    return elf(code), expected



def multiple_residual_elf(degree, neighbor=False):
    # Every odd x row retains several independent high-bit disturbances.
    # The lower scale in the neighbor keeps cross terms observable.
    scale = 1 << (32 - (5 if neighbor else 3))
    code = bytearray.fromhex("b8") + struct.pack("<I", scale)
    factors = []
    for i in range(degree):
        code += bytes.fromhex("89f9 81c1") + struct.pack("<I", 2*i)
        code += bytes.fromhex("4189f0 4181c0") + struct.pack("<I", i)
        code += bytes.fromhex("4183e0fc 4431c1 0fafc1")
        factors.append(f"((x+{2*i}U)^((y+{i}U)&0xfffffffcU))")
    code += bytes.fromhex("01f8 01f0 01d0 c3")
    return elf(code), "x+y+z+" + str(scale) + "U*" + "*".join(factors)


def carry_product_elf(degree, product=True, needed=7, high_bits=None, additive=False,
                      *, interleaved=False, complemented=False):
    # Retain the requested low bits, including observable residual cross terms.
    high_bits = high_bits or (3 if product else 2)
    mask = (0xffffffff << (high_bits - int(product))) & 0xffffffff
    scale = 3 << (32 - needed)
    code = bytearray.fromhex("b8") + struct.pack("<I", scale)
    factors = [None] * degree
    order = list(range(degree))
    if interleaved:
        order = order[::2] + order[1::2]
    for i in order:
        code += bytes.fromhex("89f9")
        if complemented:
            code += bytes.fromhex("83f101")
        code += bytes.fromhex("81c1") + struct.pack("<I", 2*i)
        code += bytes.fromhex("4189f0 4181c0") + struct.pack("<I", i)
        if product:
            code += bytes.fromhex("4183e0fe 4189d1 4181c1") + struct.pack("<I", 2*i)
            code += bytes.fromhex("4183e1") + bytes([mask & 0xff])
            code += bytes.fromhex("450fafc1")
            high = f"(((y+{i}U)&0xfffffffeU)*((z+{2*i}U)&{mask}U))"
        else:
            code += bytes.fromhex("4183e0") + bytes([mask & 0xff])
            high = f"((y+{i}U)&{mask}U)"
        code += bytes.fromhex("4401c1" if additive else "4431c1")
        code += bytes.fromhex("0fafc1")
        # Keep the independent C oracle in natural order even when the
        # original machine instructions construct the factors differently.
        base = "(x^1U)" if complemented else "x"
        factors[i] = f"(({base}+{2*i}U){'+' if additive else '^'}{high})"
    code += bytes.fromhex("01f8 01f0 01d0 c3")
    return elf(code), f"x+y+z+{scale}U*" + "*".join(factors)


def residue_power_elf(degree, observed, perturb=0, residual=False):
    # Original x64 instructions keep z independent of the repeated x group.
    # The scalar C oracle multiplies every original factor at complete width.
    split = (observed + 2) // 3
    scale = 3 << (32 - observed)
    code = bytearray.fromhex("b8") + struct.pack("<I", scale)
    code += bytes.fromhex("89f1 c1e1") + bytes([split])
    factors = []
    for i in range(degree):
        offset = (i << split) + (perturb if i == 0 else 0)
        code += bytes.fromhex("4189f8 4181c0") + struct.pack("<I", offset)
        code += bytes.fromhex("4131c8 410fafc0")
        factors.append(f"((x+{offset}U)^(y<<{split}))")
    code += bytes.fromhex("0fafc2")
    if residual:
        code += bytes.fromhex("4189d0 4183c003 410fafc0")
        factors.append("(z+3U)")
    code += bytes.fromhex("01f8 01f0 01d0 c3")
    return elf(code), f"x+y+z+{scale}U*z*" + "*".join(factors)


def elf(code):
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, 62, 1, ENTRY,
                         64, 0, 0, 64, 56, 1, 64, 0, 0)
    segment = struct.pack("<IIQQQQQQ", 1, 5, 0, 0x400000, 0x400000,
                          120 + len(code), 120 + len(code), 4096)
    return header + segment + code


def ok(client, operation, payload):
    reply = client.call(operation, payload)
    assert reply["status"] == "ok", reply
    return reply["payload"]


def source(client, representation, symbolic):
    return ok(client, "decompile", {
        "address": hex(ENTRY), "representation": representation, "limit": 2048,
        "source_options": {"symbolic": True} if symbolic else {},
    })["text"]


def function_name(text):
    # The worker recognizes the ELF entry as start; CLI's --no-debug mode may
    # retain its address-derived name. Both must identify this exact function.
    match = re.search(r"^\s*u?int(?:32|64)_t\s+(start|sub_400078)\s*\(", text, re.M)
    assert match, text
    return match.group(1)


HARNESS = r"""
#include <stdint.h>
int main(void) {
  static const uint32_t edges[] = {
    0, 1, 15, 16, 127, 128, 255, 256, 0x7fffffffU, 0x80000000U, 0xffffffffU
  };
  for (unsigned i = 0; i < sizeof(edges) / sizeof(*edges); ++i)
    for (unsigned j = 0; j < sizeof(edges) / sizeof(*edges); ++j)
      for (unsigned k = 0; k < sizeof(edges) / sizeof(*edges); ++k) {
        uint32_t x = edges[i], y = edges[j], z = edges[k];
        uint32_t want = EXPECTED_RESULT;
        if ((uint32_t)neverd_synthesis_subject(x, y, z) != want)
          return 1;
      }
  uint32_t state = 0x12345678U;
  for (unsigned i = 0; i < 4096; ++i) {
    uint32_t x = (state = 1664525U*state + 1013904223U);
    uint32_t y = (state = 1664525U*state + 1013904223U);
    uint32_t z = (state = 1664525U*state + 1013904223U);
    if ((uint32_t)neverd_synthesis_subject(x, y, z) != EXPECTED_RESULT)
      return 2;
  }
  return 0;
}
"""


def execute_source(root, name, text, compiler, expected):
    path = root / (name + ".c")
    alias = f"\n#define neverd_synthesis_subject {function_name(text)}\n"
    harness = HARNESS.replace("EXPECTED_RESULT", expected)
    path.write_text("#include <stdint.h>\n" + text + alias + harness,
                    encoding="utf-8")
    for level in ("-O0", "-O2"):
        binary = root / (name + level + ".exe")
        subprocess.run([compiler, "-std=c11", level, "-Werror=return-type",
                        "-Werror=implicit-function-declaration", str(path),
                        "-o", str(binary)], check=True, capture_output=True,
                       text=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)


def run_fixture(worker, cli, compiler, kind, image, expected):
    def enhanced_shape(text):
        if kind in ("affine-residue-neighbor", "prefix-residue-neighbor"):
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
            assert ">>" in code, text
        elif kind == "coarse-residue-neighbor":
            # A nonzero frontier can become a predicate or a lookup. Execute
            # its original product oracle; allow the lookup's literal scale.
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
            assert code.count("*") <= 1 and "<<" not in code, text
        elif kind.startswith("multiple-residual-"):
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
            if not kind.endswith("neighbor"):
                assert code.count("*") <= 3, text
        elif kind.startswith("carry-product-partial-"):
            # Partial blocks keep complete unpaired words. Their original
            # degree-N product has two multiplications per factor; require
            # less than half that chain, then execute its full-word oracle.
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
            assert code.count("*") < int(kind.rsplit("-", 1)[1]), text
        elif kind.startswith("carry-product-"):
            # Keep the proved bit selectors, including higher-bit shifts,
            # while eliminating the original long multiplication chain.
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
            # A highest-bit coefficient absorbs a selector's mask, leaving
            # an exact complement that may be printed as XOR with -1.
            without_complements = re.sub(r"\^\s*-1\b", "", code)
            assert code.count("*") <= 3 and "^" not in without_complements, text
        elif kind.startswith("carry-cofactor-"):
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
            assert code.count("*") <= 3 and code.count("^") <= 2, text
        elif kind == "distributed-residue-neighbor":
            # The original-product execution oracle checks the retained
            # nonzero residual without prescribing a particular spelling.
            pass
        elif kind.startswith("power-group-"):
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
            assert code.count("*") < int(kind.rsplit("-", 1)[1]) // 2, text
        elif kind.startswith("residue-"):
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
            assert "*" not in code and "<<" not in code, text
        elif kind == "masked":
            assert text.count("^") <= 2, text
        elif kind == "frame":
            assert all(op not in text for op in ("|", "^", "&")), text
        elif kind == "weighted":
            assert text.count("&") <= 1 and text.count("^") <= 1, text
        elif kind == "bitwise":
            assert "&" not in text and text.count("^") <= 2, text
        elif kind == "interleaved":
            assert text.count("&") <= 2 and text.count("^") <= 1, text
        elif kind in ("partitioned", "mixed"):
            assert "&" not in text and text.count("^") <= 1, text
        elif kind == "cubic":
            assert text.count("&") <= 1 and text.count("^") <= 1, text
        elif kind in ("quartic", "sextic"):
            assert text.count("&") <= 2, text
            assert text.count("^") <= (1 if kind == "quartic" else 3), text
        elif kind == "missing-cubic":
            # Retain the missing corner's exact residual, while collapsing
            # the seven separately partitioned products. LLVM-to-C spells a
            # word complement as XOR -1; count it separately from binary XOR.
            complements = len(re.findall(r"\^\s*-1\b", text))
            assert text.count("&") <= 8, text
            assert text.count("^") - complements <= 3, text
        elif kind in ("missing-degree12", "missing-degree24", "missing-degree48",
                      "missing-degree96"):
            complements = len(re.findall(r"\^\s*-1\b", text))
            limit = int(kind.removeprefix("missing-degree"))
            assert text.count("&") <= limit, text
            assert text.count("^") - complements <= limit, text
        elif kind in ("perturbed-quartic", "perturbed-sextic",
                      "missing-quartic", "missing-sextic"):
            # Keep the changed original product, but eliminate the remaining
            # complemented partitions. The sextic tail adds two XOR factors.
            assert text.count("&") <= 5 and "~" not in text, text
            assert text.count("^") <= (
                3 if kind.endswith("quartic") else 5), text
        else:
            assert ">>" not in text, text

    with tempfile.TemporaryDirectory(prefix="neverd-source-synthesis-") as directory:
        root = Path(directory)
        fixture = root / (kind + ".elf")
        fixture.write_bytes(image)
        outputs = {}
        with Client(worker) as client:
            ok(client, "open", {"path": str(fixture), "read_only": True})
            for representation in ("source", "llvmc"):
                ordinary = source(client, representation, False)
                enhanced = source(client, representation, True)
                function_name(enhanced)
                enhanced_shape(enhanced)
                if representation == "source":
                    if kind.startswith(("residue-", "multiple-residual-", "carry-product-", "carry-cofactor-", "power-group-")) or kind in ("affine-residue-neighbor", "prefix-residue-neighbor", "coarse-residue-neighbor", "distributed-residue-neighbor"):
                        code = re.sub(r"/\*.*?\*/|//[^\n]*", "", ordinary,
                                      flags=re.S)
                        assert "*" in code, ordinary
                    elif kind == "masked":
                        assert ordinary.count("^") >= 3, ordinary
                    elif kind == "frame":
                        assert "|" in ordinary, ordinary
                    elif kind in ("weighted", "bitwise", "interleaved"):
                        assert ordinary.count("&") >= 7, ordinary
                    elif kind in ("partitioned", "mixed", "cubic", "quartic",
                                  "sextic", "perturbed-quartic",
                                  "perturbed-sextic", "missing-quartic",
                                  "missing-sextic", "missing-cubic",
                                  "missing-degree12", "missing-degree24",
                                  "missing-degree48", "missing-degree96"):
                        assert ordinary.count("&") >= 4, ordinary
                    else:
                        assert ">>" in ordinary, ordinary
                    if kind not in ("distributed-residue-neighbor", "multiple-residual-neighbor"):
                        assert enhanced != ordinary
                # Turning the menu off restores its independent ordinary page.
                assert source(client, representation, False) == ordinary
                assert source(client, representation, True) == enhanced
                outputs["worker-" + representation] = enhanced

        if cli:
            for llvm in (False, True):
                for single in (False, True):
                    name = f"cli-{llvm}-{single}"
                    output = root / (name + ".c")
                    args = [cli, "decompile", "--no-debug", "--symbolic-simplify"]
                    if llvm:
                        args.append("--llvm")
                    if single:
                        args.append("--func=" + hex(ENTRY))
                    subprocess.run(args + ["-o", str(output), str(fixture)],
                                   check=True, capture_output=True, text=True, timeout=90)
                    text = output.read_text(encoding="utf-8")
                    enhanced_shape(text)
                    outputs[name] = text

        if compiler:
            for name, text in outputs.items():
                execute_source(root, name, text, compiler, expected)
            print(f"{kind}: {len(outputs)} source outputs executed at O0/O2; "
                  "1,331 boundary triples and 4,096 random triples per run")
        else:
            print("SKIP generated-C execution: clang is unavailable")
        print("Real worker symbolic menu requests, toggle/cache isolation and CLI synthesis passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("worker")
    parser.add_argument("--cli")
    parser.add_argument("--compiler", default=shutil.which("clang"))
    options = parser.parse_args()
    try:
        for kind, image, expected in (
            ("residue-prefix-quartic", *prefix_residue_elf(4)),
            ("residue-prefix-octic", *prefix_residue_elf(8)),
            ("residue-coarse-quartic", *prefix_residue_elf(4, prefix_bits=1)),
            ("residue-coarse-octic", *prefix_residue_elf(8, prefix_bits=1)),
            ("coarse-residue-neighbor", *prefix_residue_elf(4, True, prefix_bits=1)),
            ("prefix-residue-neighbor", *prefix_residue_elf(4, True)),
            ("residue-distributed-quartic", *prefix_residue_elf(4, distributed_zeros=True)),
            ("residue-distributed-octic", *prefix_residue_elf(8, distributed_zeros=True)),
            ("distributed-residue-neighbor", *prefix_residue_elf(4, True, distributed_zeros=True)),
            ("multiple-residual-quartic", *multiple_residual_elf(4)),
            ("multiple-residual-octic", *multiple_residual_elf(8)),
            ("multiple-residual-neighbor", *multiple_residual_elf(4, True)),
            ("carry-product-32", *carry_product_elf(32)),
            ("carry-product-partial-23", *carry_product_elf(23, True, 6, 2)),
            ("carry-product-partial-narrow-23", *carry_product_elf(23, True, 6, 3)),
            ("carry-product-64", *carry_product_elf(64, False)),
            ("carry-cofactor-8", *carry_product_elf(8, True, 5, 2)),
            ("carry-cofactor-projection-24", *carry_product_elf(24, True, 5, 2)),
            ("carry-cofactor-complement-4", *carry_product_elf(4, product=False, needed=4, high_bits=2, additive=True)),
            ("carry-cofactor-partial-32", *carry_product_elf(32, True, 7, 2)),
            ("carry-product-paired-64", *carry_product_elf(64, True, 7, 2)),
            ("carry-product-paired-128", *carry_product_elf(128, True, 8, 3)),
            ("carry-product-wide-128", *carry_product_elf(128, False, 8, 2)),
            ("carry-product-wide-mask-128", *carry_product_elf(128, False, 8, 3)),
            ("carry-product-interleaved-128", *carry_product_elf(128, False, 8, 2, interleaved=True)),
            ("carry-product-interleaved-mask-128", *carry_product_elf(128, False, 8, 3, interleaved=True)),
            ("carry-cofactor-zero-witness-96", *carry_product_elf(96, True, 6, 2, complemented=True)),
            ("carry-cofactor-zero-witness-mask-96", *carry_product_elf(96, True, 6, 3, complemented=True)),
            ("carry-cofactor-zero-witness-256", *carry_product_elf(256, True, 6, 2, complemented=True)),
            ("power-group-16", *residue_power_elf(16, 4)),
            ("power-group-64", *residue_power_elf(64, 6)),
            ("power-group-perturbed-128", *residue_power_elf(128, 4, 1)),
            ("power-group-512", *residue_power_elf(512, 8)),
            ("power-group-513", *residue_power_elf(513, 8)),
            ("power-group-residual-512", *residue_power_elf(512, 8, 1, True)),
            ("residue-affine-quartic", *affine_residue_elf(4)),
            ("residue-affine-octic", *affine_residue_elf(8)),
            ("affine-residue-neighbor", *affine_residue_elf(8, True)),
            ("residue-quartic", residue_elf(4), "x+y+z"),
            ("residue-octic", residue_elf(8), "x+y+z"),
            ("affine", affine_elf(), "182U*x + 73U*y + 211U*z + 37U"),
            ("quadratic", quadratic_elf(), "11U*x*x + 73U*x*y + 182U*y + 211U*z + 37U"),
            ("masked", masked_elf(), "(37U*((x ^ ~0x39U)&(y ^ 0x6dU))|0x55U)+z"),
            ("frame", frame_elf(), "2U*z-x"),
            ("weighted", weighted_elf(), f"37U*((x^y)&{WEIGHTED_UNION}U)+z"),
            ("bitwise", bitwise_elf(), f"37U*(x^y^{VIEW_KEY}U)+z"),
            ("interleaved", interleaved_elf(),
             f"7U*(((x^y)&{WEIGHTED_UNION}U)+((x*y)&{PRODUCT_UNION}U))+z"),
            ("partitioned", partitioned_elf(), "(x^y)*(y|z)+x+y"),
            ("mixed", partitioned_elf(mixed=True),
             "182U*(x^y)*(y|z)+37U*x*y*z+x+y"),
            ("cubic", cubic_elf(),
             "37U*(x^y)*(y|z)*(x|z)+x+y+(x&0x80000000U)"),
            ("missing-cubic", cubic_elf(missing=True),
             "37U*(x^y)*(y|z)*(x|z)"
             "-37U*((x^y)&z)*((y|z)&x)*((x|z)&y)"
             "+x+y+(x&0x80000000U)"),
            ("quartic", cubic_elf(degree=4),
             "37U*(x^y)*(y|z)*(x|z)*(x&y)+x+y+(x&0x80000000U)"),
            ("sextic", cubic_elf(degree=6),
             "37U*(x^y)*(y|z)*(x|z)*(x&y)*(x^z)*(y^z)"
             "+x+y+(x&0x80000000U)"),
            ("perturbed-quartic", cubic_elf(degree=4, perturbed=True),
             "37U*(x^y)*(y|z)*(x|z)*(x&y)"
             "+74U*((x^y)&z)*((y|z)&x)*((x|z)&y)*(x&y)"
             "+x+y+(x&0x80000000U)"),
            ("perturbed-sextic", cubic_elf(degree=6, perturbed=True),
             "37U*(x^y)*(y|z)*(x|z)*(x&y)*(x^z)*(y^z)"
             "+74U*((x^y)&z)*((y|z)&x)*((x|z)&y)*(x&y)*(x^z)*(y^z)"
             "+x+y+(x&0x80000000U)"),
            ("missing-quartic", cubic_elf(degree=4, missing=True),
             "37U*(x^y)*(y|z)*(x|z)*(x&y)"
             "-37U*((x^y)&z)*((y|z)&x)*((x|z)&y)*(x&y)"
             "+x+y+(x&0x80000000U)"),
            ("missing-sextic", cubic_elf(degree=6, missing=True),
             "37U*(x^y)*(y|z)*(x|z)*(x&y)*(x^z)*(y^z)"
             "-37U*((x^y)&z)*((y|z)&x)*((x|z)&y)*(x&y)*(x^z)*(y^z)"
             "+x+y+(x&0x80000000U)"),
            ("missing-degree12", cubic_elf(degree=12, missing=True),
             "(37U*(x^y)*(y|z)*(x|z)"
             "-37U*((x^y)&z)*((y|z)&x)*((x|z)&y))"
             "*(x&y)*(x^z)*(y^z)*(x&y)*(x^z)*(y^z)*(x&y)*(x^z)*(y^z)"
             "+x+y+(x&0x80000000U)"),
            ("missing-degree24", cubic_elf(degree=24, missing=True),
             "(37U*(x^y)*(y|z)*(x|z)"
             "-37U*((x^y)&z)*((y|z)&x)*((x|z)&y))"
             + "*(x&y)*(x^z)*(y^z)" * 7 + "+x+y+(x&0x80000000U)"),
            ("missing-degree48", cubic_elf(degree=48, missing=True),
             "(37U*(x^y)*(y|z)*(x|z)"
             "-37U*((x^y)&z)*((y|z)&x)*((x|z)&y))"
             + "*(x&y)*(x^z)*(y^z)" * 15 + "+x+y+(x&0x80000000U)"),
            ("missing-degree96", cubic_elf(degree=96, missing=True),
             "(37U*(x^y)*(y|z)*(x|z)"
             "-37U*((x^y)&z)*((y|z)&x)*((x|z)&y))"
             + "*(x&y)*(x^z)*(y^z)" * 31 + "+x+y+(x&0x80000000U)"),
        ):
            run_fixture(options.worker, options.cli, options.compiler, kind,
                        image, expected)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            sys.stderr.write(error.stdout)
        if error.stderr:
            sys.stderr.write(error.stderr)
        raise
