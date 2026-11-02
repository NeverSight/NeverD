#!/usr/bin/env python3
"""Compare two builds on independently generated full-width MBA identities.

Keep each CLI beside its own shared SDK library. The benchmark pins library
lookup to that directory; copying only the executable would compare the same
engine twice. Timing includes parsing and proof, excludes process startup, and
is descriptive rather than a machine-dependent pass/fail threshold.
"""

import argparse
import ast
import hashlib
import itertools
import json
import os
import operator
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def engine(path):
    path = Path(path).resolve()
    libraries = [
        p for p in path.parent.glob("*neverd*") if p.suffix in (".dylib", ".so", ".dll")
    ]
    if not libraries:
        raise ValueError(f"Put the matching shared SDK beside {path}")
    return {
        "path": str(path),
        "sha256": digest(path),
        "libraries": {p.name: digest(p) for p in libraries},
    }


def corpus(width, count):
    rng = random.Random(0x4D424100 + width)
    full = (1 << width) - 1
    rows = []
    for index in range(count):
        a, b, k, tail = (rng.getrandbits(width) for _ in range(4))
        k |= 1
        # Both complements and deliberately noncomplementary neighbours occur.
        opposite = full ^ a ^ (index % 4 == 3)
        right = "y" if index % 2 == 0 else f"(y^{b})"
        rows.append(f"({k}*((((x^{a})|{right})+(x^{opposite})))+{k})|{tail}")
    return rows


def weighted_corpus(width, count):
    rng = random.Random(0x4D424132 + width)
    full = (1 << width) - 1
    rows = []
    for index in range(count):
        carrier = ("(x^y)", "(x+y)", "(x*y)", "((x+y)^(x*y))")[(index // 6) % 4]
        size = (3, 4, 5, 4, 8, 16)[index % 6]
        if index % 6 < 3:
            masks = [rng.getrandbits(width) for _ in range(size)]
            terms = []
            positive, negative = [], []
            for subset in range(1, 1 << size):
                mask = full
                for bit in range(size):
                    if subset & (1 << bit):
                        mask &= masks[bit]
                # A nearby nonidentity must retain its changed high-bit term.
                if index % 4 == 3 and subset == 1:
                    mask ^= 1 << (width - 1)
                sign = "+" if subset.bit_count() % 2 else "-"
                terms.append(f"{sign}({carrier}&{mask})")
                (positive if sign == "+" else negative).append(f"({carrier}&{mask})")
            if index % 24 >= 12:
                # Compiler reassociation groups subtractions and hoists scales.
                grouped = f"({'+'.join(positive)})-({'+'.join(negative)})"
                rows.append(f"(3*({grouped}))+(x|y)")
                continue
        else:
            masks = [0] * size
            for bit in range(width):
                masks[rng.randrange(size)] |= 1 << bit
            scale = (37, -11, 182)[(index // 6) % 3]
            terms = [f"+({scale}*({carrier}&{mask}))" for mask in masks]
        rows.append("0" + "".join(terms))
    return rows


def bitwise_corpus(width, count):
    """Literal flips, forced bits and nested views of opaque arithmetic words."""
    rng = random.Random(0x4D424133 + width)
    full = (1 << width) - 1
    rows = []
    for index in range(count):
        carrier = ("(x^y)", "(x+y)", "(x*y)", "((x+y)^(x*y))")[(index // 6) % 4]
        kind = index % 6
        weight = (37, -11, 182, 1)[(index // 6) % 4]
        if kind in (0, 1, 4):
            masks = [0] * 16
            for bit in range(width):
                masks[rng.randrange(len(masks))] |= 1 << bit
            terms = []
            for mask in masks:
                key = rng.getrandbits(width)
                view = f"({carrier}^{key})"
                if kind == 4:
                    view = f"(~{view}|{rng.getrandbits(width)})"
                terms.append(f"({weight}*({view}&{mask}))")
                if kind == 1:
                    terms.extend((
                        f"({2*weight}*({carrier}&{key&mask}))",
                        str(-weight * (key & mask)),
                    ))
            rows.append("+".join(terms))
        else:
            positive, negative = [], []
            for part in range(8):
                key, forced = rng.getrandbits(width), rng.getrandbits(width)
                view = f"({carrier}^{key})"
                if kind == 3:
                    view = f"~{view}"
                    key ^= full
                compensation = key
                # A high-bit error must survive even if most terms collapse.
                if kind == 5 and part == 0:
                    compensation ^= 1 << (width - 2)
                positive.extend((
                    f"({view}|{forced})", f"({view}&{forced})",
                    f"(2*({carrier}&{compensation}))",
                ))
                negative.extend((str(key), str(forced)))
            rows.append(
                f"(-5*(({'+'.join(positive)})-({'+'.join(negative)})))+(x|y)"
            )
    return rows


def interleaved_corpus(width, count):
    """Several opaque words mixed inside shared scales and subtractions."""
    rng = random.Random(0x4D424134 + width)
    full = (1 << width) - 1
    families = (
        ("(x^y)", "(x*y)"),
        ("(x+y)", "(x*y)"),
        ("(x&y)", "(x^y)", "(x*y)"),
        ("(x+y)", "(x*y)", "((x+y)^(x*y))", "(x|y)"),
    )
    rows = []
    for index in range(count):
        carriers = families[(index // 6) % len(families)]
        pieces = (4, 8, 16)[(index % 6) // 2]
        if index % 2 == 0:
            groups = []
            for carrier in carriers:
                masks = [0] * pieces
                for bit in range(width):
                    masks[rng.randrange(pieces)] |= 1 << bit
                groups.append([
                    f"(({carrier}^{rng.getrandbits(width)})&{mask})"
                    for mask in masks
                ])
            rows.append("+".join(
                f"(37*({'+'.join(group[part] for group in groups)}))"
                for part in range(pieces)
            ))
        else:
            positive, negative = [], []
            for group, carrier in enumerate(carriers):
                masks = [rng.getrandbits(width) for _ in range(4)]
                for subset in range(1, 16):
                    mask = full
                    for bit in range(4):
                        if subset & (1 << bit):
                            mask &= masks[bit]
                    if index % 6 == 5 and group == 0 and subset == 1:
                        mask ^= 1 << (width - 1)
                    destination = positive if subset.bit_count() % 2 else negative
                    destination.append(f"({carrier}&{mask})")
            rows.append(
                f"(7*(({'+'.join(positive)})-({'+'.join(negative)})))+(x|y)"
            )
    return rows


def quadratic_corpus(width, count):
    """Expanded products with three to five inputs and unrelated partitions."""
    rng = random.Random(0x4D424135 + width)
    full = (1 << width) - 1
    families = (
        ("(x^y)", "(y|z)", ("z",), ("x",)),
        ("(x^y)", "(x|y)", ("z",), ("w",)),
        ("(x|z)", "(y^w)", ("w",), ("z",)),
        ("(x^y)", "(y|v)", ("z", "w"), ("x", "z")),
    )
    rows = []
    for index in range(count):
        left, right, axes_a, axes_b = families[(index // 6) % len(families)]
        kind = index % 6
        scale = (1, 7, -11, 182, 1 << (width - 1), 37)[kind]

        def partition(word, axes):
            return [
                "(" + "&".join(
                    [word] + [axis if mask & (1 << bit) else f"~{axis}"
                              for bit, axis in enumerate(axes)]
                ) + ")"
                for mask in range(1 << len(axes))
            ]

        terms = [f"({a}*{b})"
                 for a in partition(left, axes_a)
                 for b in partition(right, axes_b)]
        rng.shuffle(terms)
        expression = f"({scale}*({' + '.join(terms)}))"
        if kind in (1, 2, 3, 5):
            expression += "+(x^y)+2*(x&y)"
        if kind == 5:
            # A changed high bit remains an observable extra term.
            expression += f"+(x&{1 << (width - 1)})"
        if kind == 3:
            # A second, different degree must survive the quadratic recovery.
            expression += f"+({(rng.getrandbits(width) | 1) & full}*x*y*z)"
        rows.append(expression)
    return rows


def cubic_corpus(width, count):
    """Three Boolean factors, independently partitioned, with mixed degrees."""
    rng = random.Random(0x4D424136 + width)
    families = (
        ("(x^y)", "(y|z)", "(x|z)", ("z",), ("x",), ("y",)),
        ("(x^y)", "(x|y)", "(x|z)", ("z",), ("w",), ()),
        ("(x|z)", "(y^w)", "(x|w)", ("w",), ("z",), ()),
        ("(x^y)", "(y|v)", "(x|v)", ("z", "w"), ("x", "z"), ()),
    )
    rows = []
    for index in range(count):
        a, b, c, axes_a, axes_b, axes_c = families[(index // 6) % len(families)]
        kind = index % 6
        scale = (1, 7, -11, 182, 1 << (width - 2), 37)[kind]

        def partition(word, axes):
            return [
                "(" + "&".join(
                    [word] + [axis if mask & (1 << bit) else f"~{axis}"
                              for bit, axis in enumerate(axes)]
                ) + ")"
                for mask in range(1 << len(axes))
            ]

        terms = [f"({left}*{right}*{third})"
                 for left in partition(a, axes_a)
                 for right in partition(b, axes_b)
                 for third in partition(c, axes_c)]
        rng.shuffle(terms)
        expression = f"({scale}*({' + '.join(terms)}))"
        if kind in (1, 2, 3, 5):
            expression += "+(x^y)+2*(x&y)"
        if kind == 3:
            # Quadratic and cubic coefficients must both be retained.
            expression += f"+({rng.getrandbits(width) | 1}*x*y)"
        if kind == 5:
            # Nearby error survives every factor reconstruction.
            expression += f"+(x&{1 << (width - 1)})"
        rows.append(expression)
    return rows


def higher_corpus(width, count):
    """Fourth through eighth degrees, mixed products and nonlinear quotients."""
    rng = random.Random(0x4D424137 + width)
    words = ("(x^y)", "(y|z)", "(x|z)", "(x&y)", "(x^z)", "(y^z)",
             "(x|y)", "~(x&z)")
    axes = ("z", "x", "y", "w", "v", "z")
    rows = []
    for index in range(count):
        degree = (4, 5, 6, 8)[(index // 16) % 4]
        kind = index % 16
        scale = (1, 7, -11, 182, 1 << (width - 2), 37)[kind % 6]
        if 6 <= kind < 10:
            # Neither quadratic identity exposes a shared single factor.
            # Their distributed product requires a two-factor quotient.
            pairs = (("(x&y)", "(x|y)"), ("(x&~y)", "(~x&y)"))
            other = (("(z&w)", "(z|w)"), ("(z&~w)", "(~z&w)"))
            terms = ["(" + "*".join(a + b + words[4:degree]) + ")"
                     for a in pairs for b in other]
        else:
            pieces = (min(degree, kind - 9) if kind >= 14
                      else 2 + (kind % 6) // 2)
            choices = [
                (f"({word}&{axes[i]})", f"({word}&~{axes[i]})")
                if i < pieces else (word,)
                for i, word in enumerate(words[:degree])
            ]
            terms = ["(" + "*".join(fs) + ")"
                     for fs in itertools.product(*choices)]
        if kind == 9:
            # Alter one nonlinear coefficient; a partial identity must not
            # silently remove this difference.
            terms[0] = f"(3*{terms[0]})"
        rng.shuffle(terms)
        expression = f"({scale}*({' + '.join(terms)}))+(x^y)+2*(x&y)"
        if kind % 3 == 2:
            expression += "+13*x*y*z"
        if kind % 4 == 3:
            expression += f"+(x&{1 << (width - 1)})"
        if 10 <= kind < 14:
            factors = ("x", "y", "z", "(x|y)") + ("x",) * (degree - 4)
            if kind != 13:
                factors += ("y",)
            expression += "+19*" + "*".join(factors)
        rows.append(expression)
    return rows


def perturbed_corpus(width, count):
    """Sparse coefficient differences in fourth through twelfth degree sums."""
    rng = random.Random(0x4D424138 + width)
    words = ("(x^y)", "(y|z)", "(x|z)", "(x&y)", "(x^z)", "(y^z)",
             "(x|y)", "~(x&z)")
    axes = ("z", "x", "y", "w", "v")
    rows = []
    for index in range(count):
        degree = (4, 6, 8, 12)[(index // 16) % 4]
        kind = index % 16
        scale = (1, 37, -11, 182, 1 << (width - 2), -7)[kind % 6]
        delta = (2 * scale, -scale, 1 << (width - 1), 13)[kind % 4]
        if kind < 8:
            pairs = (("(x&y)", "(x|y)"), ("(x&~y)", "(~x&y)"))
            other = (("(z&w)", "(z|w)"), ("(z&~w)", "(~z&w)"))
            tail = tuple(words[i % len(words)] for i in range(4, degree))
            factors = [a + b + tail for a in pairs for b in other]
        else:
            pieces = 3 + (kind % 3)
            whole = tuple(words[i % len(words)] for i in range(degree))
            choices = [
                (f"({word}&{axes[i]})", f"({word}&~{axes[i]})")
                if i < pieces else (word,)
                for i, word in enumerate(whole)
            ]
            factors = list(itertools.product(*choices))
        # Some coefficients wrap to zero or change sign. Additional independent
        # differences ensure that recovering a nearby identity is insufficient.
        changed = {0: delta}
        if kind % 4 == 3:
            changed[len(factors) - 1] = -3 * delta
        terms = [f"({scale + changed.get(i, 0)}*{'*'.join(fs)})"
                 for i, fs in enumerate(factors)]
        rng.shuffle(terms)
        expression = "+".join(terms) + "+(x^y)+2*(x&y)"
        if kind % 3 == 2:
            expression += "+19*x*y*z+(x&" + str(1 << (width - 1)) + ")"
        if kind % 4 == 3:
            expression += "+23*" + "*".join(("x", "y") * (degree // 2))
        rows.append(expression)
    return rows


def completion_corpus(width, count):
    """Missing corners in larger products, including opaque arithmetic words."""
    rng = random.Random(0x4D424139 + width)
    words = ("(x^y)", "(y|z)", "(x|z)", "(x&y)", "(x^z)", "(y^z)",
             "(x|y)", "~(x&z)")
    axes = ("z", "x", "y", "w", "v", "z")
    rows = []
    for index in range(count):
        degree = (4, 8, 12, 16)[(index // 16) % 4]
        kind = index % 16
        pieces = min(degree, 3 + kind % 4)
        scale = (1, 37, 182, -11, 1 << (width - 2))[kind % 5]
        whole = list(words[i % len(words)] for i in range(degree))
        if kind >= 8:
            whole[:3] = ("((x+y)^z)", "((y*z)|x)", "((x+z)&(y|w))")
        choices = [(f"({word}&{axes[i]})", f"({word}&~{axes[i]})")
                   if i < pieces else (word,)
                   for i, word in enumerate(whole)]
        missing = {0}
        if kind % 3 >= 1:
            missing.add(1 << (pieces - 1))
        if kind % 3 == 2:
            missing.add((1 << pieces) - 1)
        terms = []
        for number, factors in enumerate(itertools.product(*choices)):
            if number in missing:
                continue
            coefficient = scale
            if number == 1 and kind % 2:
                coefficient += (1 << (width - 1)) if kind % 4 == 1 else 13
            terms.append(f"({coefficient}*{'*'.join(factors)})")
        rng.shuffle(terms)
        expression = "+".join(terms) + "+(x^y)+2*(x&y)+19*x*y*z"
        if kind % 4 == 3:
            expression += "+23*" + "*".join(("x", "y") * (degree // 2))
        rows.append(expression)
    return rows


def residual_corpus(width, count, *, deep=False):
    """Sparse products through degree 32, including two interleaved blocks."""
    rng = random.Random((0x4D42413B if deep else 0x4D42413A) + width)
    axes = ("z", "x", "y", "w", "v", "z")
    words = ("(x^y)", "(y|z)", "(x|z)", "(x&y)", "(x^z)",
             "(y^z)", "(x|y)", "~(x&z)")
    opaque = ("((x+y)^z)", "((y*z)|w)", "((x+z)&(y|v))",
              "(x&~y)", "(z|v)", "(y^w)", "~(x&z)", "(x|v)")
    rows = []
    for index in range(count):
        degree = ((24, 32, 48, 64)[(index // 8) % 4] if deep
                  else (8, 16, 24, 32)[(index // 16) % 4])
        kind = index % 16
        pieces = 3 + kind % (3 if deep else 4)
        blocks = 2 if kind % 4 == 2 and (not deep or pieces <= 4) else 1
        terms = []
        for block in range(blocks):
            basis = opaque if block or kind >= 8 else words
            scale = (37, -11, 182, 1 << (width - 2))[(kind + block) % 4]
            whole = [basis[(i + block) % len(basis)] for i in range(degree)]
            choices = [
                (f"({word}&{axes[i]})", f"({word}&~{axes[i]})")
                if i < pieces else (word,)
                for i, word in enumerate(whole)
            ]
            missing = {0, (1 << pieces) - 1, 3 + block}
            for number, factors in enumerate(itertools.product(*choices)):
                if number in missing:
                    continue
                coefficient = scale
                if number == 1:
                    coefficient += 13 if kind % 2 else 1 << (width - 1)
                if number == 2 and kind % 3 == 0:
                    coefficient -= 2 * scale
                terms.append(f"({coefficient}*{'*'.join(factors)})")
        rng.shuffle(terms)
        expression = "+".join(terms) + "+(x^y)+2*(x&y)+19*x*y*z"
        if kind % 4 == 1:
            expression += "+23*" + "*".join(("x", "y") * (degree // 2))
        rows.append(expression)
    return rows


def deep_residual_corpus(width, count):
    """Sparse products through degree 64 with nonlinear opaque factor words."""
    return residual_corpus(width, count, deep=True)


def row_residual_corpus(width, count):
    """Degree 8..96 products with wide, overlapping quadratic quotients."""
    rng = random.Random(0x4D42413C + width)
    words = ("((x+y)^z)", "((y*z)|w)", "((x+z)&(y|v))", "(~x|v)")
    common = ("(x|y)", "~(x&z)", "((x+y)|1)", "((y*z)|w|1)")
    rows = []
    for index in range(count):
        degree = (8, 16, 32, 64, 96)[index % 5]
        a, b, c, d = words[index % 4:] + words[:index % 4]
        pairs = [(f"({a}&x)", f"({b}&z)"),
                 (f"({a}&~x)", f"({b}&z)"),
                 (f"({a}&x)", f"({b}&~z)"),
                 (f"({a}&~x)", f"({b}&~z)")]
        if index % 3:
            pairs[-1] = (f"({c}&~x)", f"({d}|z)")
        scale = (37, -11, 182, 1 << (width - 2))[index % 4]
        prefix = [common[i % len(common)] for i in range(degree - 2)]
        terms = []
        for number, pair in enumerate(pairs):
            coefficient = scale
            if number == 2 and index % 2:
                coefficient += 13 if index % 4 == 1 else 1 << (width - 1)
            terms.append(f"({coefficient}*{'*'.join(prefix + list(pair))})")
        if index % 4 == 2:
            terms += [f"(17*{'*'.join(prefix)}*{c}*{d})",
                      f"(-17*{'*'.join(prefix)}*{c}*{d})"]
        rng.shuffle(terms)
        rows.append("+".join(terms) + "+(x^y)+2*(x&y)+19*x*y*z")
    return rows


def mixed_frontier_corpus(width, count):
    """Sparse mixed degrees through 144 with odd, nonlinear common powers."""
    rng = random.Random(0x4D42413D + width)
    words = ("((x+y)^z)", "((y*z)|w)", "((x+z)&(y|v))",
             "((w+v)^x)", "((x*w)|y)")
    common = ("((x+y)|1)", "((z*w)|1)", "((x^v)|1)")
    axes = ("x", "z", "v")
    rows = []
    for index in range(count):
        degree = (12, 36, 72, 108, 144)[index % 5]
        blocks = 2 if index % 3 and degree <= 72 else 1
        terms = []
        for block in range(blocks):
            current = degree - 3 * block
            scale = (37, -11, 182, 1 << (width - 2))[(index + block) % 4]
            prefix = [common[(i + block) % len(common)]
                      for i in range(current - 3)]
            choices = [(f"({words[(i + block) % len(words)]}&{axis})",
                        f"({words[(i + block) % len(words)]}&~{axis})")
                       for i, axis in enumerate(axes)]
            for corner, factors in enumerate(itertools.product(*choices)):
                if corner == 0 or (index % 2 and corner == 7):
                    continue
                coefficient = scale
                if corner == 1:
                    coefficient += 13 if index % 2 else 1 << (width - 1)
                if corner == 4 and index % 3 == 2:
                    coefficient -= 2 * scale
                terms.append(f"({coefficient}*{'*'.join(prefix + list(factors))})")
        rng.shuffle(terms)
        rows.append("+".join(terms)
                    + "+(x^y)+2*(x&y)+(z|w)+(z&w)+19*x*y*z")
    return rows



def extreme_frontier_corpus(width, count):
    """Degree 192/256, three mixed blocks and multiple changed coefficients."""
    rng = random.Random(0x4D42413E + width)
    words = ("((x+y)^z)", "((y*z)|w)", "((x+z)&(y|v))", "((w+v)^x)")
    common = ("((x+y)|1)", "((z*w)|1)", "~((x^v)&z)", "((y+v)|1)")
    layouts = ((192,), (256,), (96, 128), (48, 64, 80), (128,), (112,))
    rows = []
    for index in range(count):
        kind = index % len(layouts)
        axes = ("x", "z", "v", "y")[:4 if kind == 4 else 3]
        terms = []
        for block, degree in enumerate(layouts[kind]):
            prefix = [common[(i + block) % len(common)]
                      for i in range(degree - len(axes))]
            choices = [(f"({words[(i + block) % len(words)]}&{axis})",
                        f"({words[(i + block) % len(words)]}&~{axis})")
                       for i, axis in enumerate(axes)]
            scale = (37, -11, 182, 1 << (width - 2))[(index + block) % 4]
            for corner, factors in enumerate(itertools.product(*choices)):
                if corner in (0, (1 << len(axes)) - 1):
                    continue
                coefficient = scale
                if corner == 1:
                    coefficient += 13
                if corner == 4:
                    coefficient -= 2 * scale
                if corner == 2 and kind % 2:
                    coefficient += 1 << (width - 1)
                terms.append(f"({coefficient}*{'*'.join(prefix + list(factors))})")
        rng.shuffle(terms)
        rows.append("+".join(terms)
                    + "+(x^y)+2*(x&y)+(z|w)+(z&w)+19*x*y*z")
    return rows

def residue_product_corpus(width, count):
    """Correlated factor residues, modular annihilation and nearby nonzeros."""
    bases = ("x", "((x+y)^z)", "(x+y)")
    tail = "+(x^y)+2*(x&y)+(z|w)+(z&w)"
    rows = []
    for index in range(count):
        kind = index % 80
        if kind < 63:
            degree = (4, 8, 16, 32, 64, 128, 256)[kind // 9]
            base = bases[kind % 3]
            variant = kind % 9 // 3
            start, step = ((0, 1), (11, 5),
                           ((1 << (width - 1)) - 7, -3))[variant]
            zeros = degree - degree.bit_count()
            scale = (1 << max(width - zeros, 0)) * (1, 37, -11)[variant]
            factors = [f"({base}+({start + step * i}))" for i in range(degree)]
        elif kind < 66:
            degree = (4, 8, 16)[kind - 63]
            zeros = 2 * (degree - degree.bit_count())
            scale = 1 << max(width - zeros, 0)
            factors = [f"({base}+{i})" for base in ("(x+y)", "((x*z)^w)")
                       for i in range(degree)]
        else:
            degree = (4, 8)[kind % 2]
            base = bases[(kind // 2) % 3]
            zeros = degree - degree.bit_count()
            scale = 1 << max(width - zeros, 0)
            control = (kind - 66) % 3
            offsets = list(range(degree - (control == 0)))
            if control == 1:
                offsets[-1] -= 1
            if control == 2:
                scale += 1
            factors = [f"({base}+{offset})" for offset in offsets]
        rows.append(f"({scale}*{'*'.join(factors)})" + tail)
    return rows


def affine_residue_corpus(width, count):
    """Independent odd units, even factors and partial-width affine bases."""
    rng = random.Random(0x41464652 + width)
    mask = (1 << width) - 1
    tail = "+(x^y)+2*(x&y)+(z|w)+(z&w)"
    rows = []
    for index in range(count):
        kind = index % 96
        control = None
        if kind < 63:
            degree = (4, 8, 16, 32, 64, 128, 256)[kind // 9]
            flavor = kind % 9
        elif kind < 72:
            degree = (4, 8, 16)[(kind - 63) // 3]
            flavor = (0, 3, 7)[kind % 3]
        else:
            degree = (4, 8)[kind % 2]
            flavor = (0, 1, 3)[(kind - 72) // 8]
            control = (kind - 72) % 4
        zeros = degree - degree.bit_count()
        if flavor == 8:
            zeros = degree - 2
        fixed = 1 if flavor == 5 or control == 3 else (2 if flavor == 6 else 0)
        scale = 1 << max(width - zeros - fixed, 0)
        if control == 2:
            scale += 1
        factors = []
        for i in range(degree - (control == 0)):
            offset = (i if control is not None else 11 + 5 * i)
            if control == 1 and i == degree - 1:
                offset -= 1
            if flavor == 8:
                offset = 11 + 5 * (i % (degree // 2))
            unit = (rng.getrandbits(width) | 1)
            a = (unit << (fixed if i == 0 else 0)) & mask
            b = (a * offset) & mask
            if control == 3 and i == 0:
                b = (b + 1) & mask
            if flavor in (0,):
                body = f"{a}*x"
            elif flavor == 1:
                body = f"{a}*((x+y)^z)"
            elif flavor in (2, 5):
                body = f"{a}*(x+y)"
            elif flavor == 3:
                body = f"{a}*x+{3*a & mask}*y+{5*a & mask}*z"
            elif flavor == 4:
                body = f"{a}*x*y"
            elif flavor == 6:
                body = f"{a}*x+{2*a & mask}*y+{3*a & mask}*z"
            elif flavor == 7:
                body = f"{a}*(x+3*(y+5*z))"
            else:
                body = f"{a}*x+{(a if i < degree//2 else -a) & mask}*y"
            if 63 <= kind < 72:
                # These perturbations preserve only the bounded low-bit
                # affine base, not a full-width common scalar factor.
                perturb = (rng.getrandbits(width) << (degree + 1).bit_length()) & mask
                body += f"+{perturb}*w"
            factors.append(f"({body}+{b})")
        rows.append(f"({scale}*{'*'.join(factors)})" + tail)
    return rows


def prefix_residue_corpus(width, count):
    """High-bit disturbances preserving a complete low-residue product proof."""
    rng = random.Random(0x50524546 + width)
    mask = (1 << width) - 1
    tail = "+(x^y)+2*(x&y)+(z|w)+(z&w)"
    rows = []
    for index in range(count):
        kind = index % 128
        control = None
        if kind < 112:
            degree = (4, 8, 16, 32, 64, 128, 256)[kind // 16]
            flavor = kind % 16
        else:
            degree = 4
            flavor = (kind - 112) % 8
            control = (kind - 112) // 8
        bits = min(width, (degree - 1).bit_length())
        low = (1 << bits) - 1
        zeros = degree - degree.bit_count()
        if flavor == 13:
            zeros = degree - 2
        if flavor == 14:
            zeros += degree // 2
        scale = 1 << max(width - zeros, 0)
        factors = []
        for i in range(degree - (control == 1)):
            offset = i - (control == 0 and i + 1 == degree)
            base = "(x+3*y+5*z)"
            if flavor == 13:
                base = "(x+3*y+5*z)" if i < degree // 2 else "(x-3*y+5*z)"
                offset = i % (degree // 2)
            if flavor == 15:
                base = "((x+3*(y+5*z))^v)"
            fixed = int(flavor == 14 and i % 2 == 0)
            a = ((rng.getrandbits(width) | 1) << fixed) & mask
            b = (a * offset - (flavor == 6)) & mask
            high_bits = min(width, bits + fixed)
            high = f"((u*{2*i+1})<<{high_bits})"
            f = f"({a}*{base}+{b})"
            if flavor in (0, 13, 14, 15):
                f = f"({f}^{high})"
            elif flavor == 1:
                f = f"({f}|{high})"
            elif flavor == 2:
                f = f"({f}&({high}|{low}))"
            elif flavor == 3:
                f = f"(({a}*({base}&{low})+{b})^{high})"
            elif flavor == 4:
                f = f"({f}+{high})"
            elif flavor == 5:
                f = f"({f}+({high}|(v<<{bits})))"
            elif flavor == 6:
                f = f"({f}^({high}|{low}))"
            elif flavor == 7:
                f = f"({f}&{low})"
            elif flavor == 8:
                f = f"zext(extract({f},0,{bits}),{width})"
            elif flavor == 9:
                f = f"sext(extract({f},0,{bits}),{width})"
            elif flavor == 10:
                f = f"((({f}^{high})&((v<<{bits})|{low}))|{high})"
            elif flavor == 11:
                f = f"({f}^(u&{mask ^ low}))"
            else:
                f = f"({f}^{rng.getrandbits(width) & (mask ^ low)})"
            factors.append(f)
        rows.append(f"({scale}*{'*'.join(factors)})" + tail)
    return rows


def residue_frontier_corpus(width, count):
    """Large products whose useful congruence ends before their degree bound.

    At precision b, a balanced degree-n product guarantees n - n/2**b low
    zeros even when every higher bit is independently disturbed. Even units,
    correlated bases and narrow views exercise different precision choices.
    The last sixteen cases duplicate an offset in an unmasked quartic; their
    nonzero residual must survive. The generator is independent of the solver.
    """
    rng = random.Random(0x46524F4E + width)
    mask = (1 << width) - 1
    rows = []
    for index in range(count):
        kind = index % 128
        control = kind >= 112
        flavor = kind % 16
        degree = 4 if control else (4, 8, 16, 32, 64, 128, 256)[kind // 16]
        bits = min((degree - 1).bit_length() - 1, 1 + flavor % 3)
        low = (1 << bits) - 1
        zeros = degree - (degree >> bits)
        if flavor == 7 and not control:
            zeros += degree // 2
        scale = 1 << max(width - zeros, 0)
        factors = []
        if control:
            flavor %= 2
        for i in range(degree):
            offset = i - (control and i + 1 == degree)
            base = "(x+3*y+5*z)"
            if flavor == 8:
                base = base if i < degree // 2 else "(x-3*y+5*z)"
                offset = i % (degree // 2)
            if flavor == 9:
                base = "((x+3*y)^(z*w))"
            if flavor == 12:
                base = f"({base}&{low})"
            fixed = int(flavor == 7 and i % 2 == 0)
            a = ((rng.getrandbits(width) | 1) << fixed) & mask
            b = (a * offset - (flavor == 5)) & mask
            high = f"((u*{i*2+1})<<{bits+fixed})"
            f = f"({a}*{base}+{b})"
            if flavor in (0, 7, 8, 9, 12, 15):
                f = f"({f}^{high})"
            elif flavor == 1:
                f = f"({f}|{high})"
            elif flavor == 2:
                f = f"({f}&({high}|{low}))"
            elif flavor == 3:
                f = f"({f}+{high})"
            elif flavor == 4:
                f = f"zext(extract({f},0,{bits}),{width})"
            elif flavor == 5:
                f = f"(~{f}^{high})"
            elif flavor == 6:
                f = f"((({f}^{high})&((v<<{bits})|{low}))|{high})"
            elif flavor == 10:
                f = f"({f}^(u&{mask ^ low}))"
            elif flavor == 11:
                f = f"({f}+((u*v)<<{bits}))"
            elif flavor == 13:
                f = f"({f}^((v+u)<<{bits+i%2}))"
            else:
                f = f"sext(extract({f},0,{bits}),{width})"
            factors.append(f)
        rows.append(f"({scale}*{'*'.join(factors)})"
                    "+(x^y)+2*(x&y)+(z|w)+(z&w)")
    return rows


def distributed_prefix_corpus(width, count):
    """Low-prefix proofs requiring information from several partial operands.

    Each disturbance is divisible by 2**bits, although its individual operands
    need not have a constant low prefix. A consecutive useful block supplies
    sum(block >> bit) guaranteed zeros. The remaining odd factors make degrees
    up to 256 without supplying a stronger parity-only proof. Controls either
    repeat an offset or halve the enclosing scale and have nonzero witnesses.
    """
    rng = random.Random(0x44505246 + width)
    rows = []
    for index in range(count):
        kind = index % 128
        control = kind >= 112
        flavor = kind % 16
        degree = 4 if control else (4, 8, 16, 32, 64, 128, 256)[kind // 16]
        block = min(degree, max(4, width // 2))
        bits = 2 + (flavor % 3 == 2)
        low = (1 << bits) - 1
        zeros = sum(block >> bit for bit in range(1, bits + 1))
        scale = 1 << max(width - zeros - (control and flavor >= 8), 0)
        factors = []
        for i in range(block):
            offset = i - (control and flavor < 8 and i + 1 == block)
            u, v = f"(u+{2*i})", f"(v+{4*i})"
            distributed = f"(({u}&-2)*({v}&{-1 << (bits-1)}))"
            odd_product = f"(({u}|{low})*({v}|{low}))"
            if flavor == 0:
                high = distributed
            elif flavor == 1:
                high = f"({distributed}*(w|1))"
            elif flavor == 2:
                high = f"({odd_product}-1)"
            elif flavor == 3:
                high = f"({odd_product}+{low})"
            elif flavor == 4:
                high = f"({distributed}^((v&-2)*(w&{-1 << (bits-1)})))"
            elif flavor == 5:
                high = f"({distributed}|((v&-2)*(w&{-1 << (bits-1)})))"
            elif flavor == 6:
                high = f"({distributed}&(w|{low}))"
            elif flavor == 7:
                high = f"((({u}|{low})+({v}|{low})+2)*(w|1))"
            elif flavor == 8:
                high = f"(({u}&-2)*({v}&-2)*(w&-2))"
            elif flavor == 9:
                high = f"(-{distributed})"
            elif flavor == 10:
                high = f"({distributed}+({odd_product}-1))"
            elif flavor == 11:
                high = f"((({u}&-2)*(v|1)+(w&-2))*({v}&-4))"
            elif flavor == 12:
                high = f"zext(extract({distributed},0,{bits}),{width})"
            elif flavor == 13:
                high = f"({distributed}<<1)"
            elif flavor == 14:
                high = f"(({odd_product}&{low})-1)"
            else:
                high = f"(({distributed}*(w|1))|({odd_product}-1))"
            base = "(x+3*y)" if flavor % 4 else "((x+3*y)^(z*w))"
            unit = (rng.getrandbits(width) | 1)
            affine = f"({unit}*({base}+{offset}))"
            if flavor % 4 == 1:
                factor = f"({affine}|{high})"
            elif flavor % 4 == 2:
                factor = f"({affine}&({high}|{low}))"
            elif flavor % 4 == 3:
                factor = f"({affine}+{high})"
            else:
                factor = f"({affine}^{high})"
            factors.append(factor)
        factors.extend(f"((w+{2*i})|1)" for i in range(block, degree))
        rows.append(f"({scale}*{'*'.join(factors)})"
                    "+(x^y)+2*(x&y)+(z|w)+(z&w)")
    return rows



def multiple_residual_corpus(width, count):
    """Nonzero products with simultaneous unobserved high-bit factors.

    The even-offset factors share a prefix but the odd residue rows retain
    several independent disturbances. Nearby controls observe one extra bit,
    so a first-order expansion may no longer discard the cross terms.
    """
    rows = []
    for index in range(count):
        kind = index % 128
        control = kind >= 112
        flavor = kind % 16
        degree = 4 if control else (4, 8, 16, 24, 32, 48, 64)[kind // 16]
        bits = 2 + (flavor % 4 == 3)
        needed = bits + 1 if flavor < 8 else 2 * bits
        if control:
            needed = 2 * bits + 1
        factors = []
        for i in range(degree):
            u, v = f"(y+{i})", f"(z+{2*i})"
            high = (f"({u}&{-1 << bits})",
                    f"({u}*{v}&{-1 << bits})",
                    f"({u}<<{bits})",
                    f"(({u}&-2)*({v}&{-1 << (bits-1)}))")[flavor % 4]
            base = f"(x+{2*i})"
            op = "+" if flavor % 8 >= 4 else "^"
            factors.append(f"({base}{op}{high})")
        rows.append(f"({3 << (width-needed)}*{'*'.join(factors)})"
                    "+(x^y)+2*(x&y)+(z|w)+(z&w)")
    return rows


def _carry_product_parts(kind):
    degree = (8, 16, 24, 32, 48, 64, 96, 128)[kind // 16]
    needed = 5 + kind % 16 // 4
    flavor = kind % 4
    bits = 3 if flavor % 2 else 2
    factors = []
    for i in range(degree):
        high = (f"((y+{i})&{-1 << bits})" if flavor < 2 else
                f"(((y+{i})&-2)*((z+{2*i})&{-1 << (bits-1)}))")
        factors.append(f"((x+{2*i})^{high})")
    return needed, factors


def carry_product_corpus(width, count):
    """Higher-degree nonzero products with five to eight observed low bits."""
    rows = []
    for index in range(count):
        needed, factors = _carry_product_parts(index % 128)
        rows.append(f"({3 << (width-needed)}*{'*'.join(factors)})"
                    "+(x^y)+2*(x&y)+(z|w)+(z&w)")
    return rows


def permuted_carry_product_corpus(width, count):
    """Equivalent degree-48–128 products with four factor construction orders."""
    kinds = (72, 73, 88, 89, 92, 93, 100, 101, 104, 105, 124, 125)
    rows = []
    for index in range(count):
        kind = kinds[(index // 4) % len(kinds)]
        needed, factors = _carry_product_parts(kind)
        order = index % 4
        if order == 0:
            factors.reverse()
        elif order == 1:
            factors = factors[3:] + factors[:3]
        elif order == 2:
            factors = factors[::2] + factors[1::2]
        else:
            random.Random(0x4D4241 + kind).shuffle(factors)
        rows.append(f"({3 << (width-needed)}*{'*'.join(factors)})"
                    "+(x^y)+2*(x&y)+(z|w)+(z&w)")
    return rows


def complemented_carry_product_corpus(width, count):
    """Degree-48–256 products with complemented inputs and parity near misses."""
    rows = []
    for index in range(count):
        kind = index % 108
        degree = (48, 65, 96, 128, 192, 256)[kind // 18]
        observed = 6 + kind % 18 // 6
        high = 2 + kind % 6 // 3
        mode = kind % 3
        base = "(~x)" if mode == 1 else "(x^1)"
        factors = []
        for i in range(degree):
            offset = 2*i + int(mode == 2 and i == 0)
            upper = f"(((y+{i})&-2)*((z+{2*i})&-{1 << (high-1)}))"
            factors.append(f"(({base}+{offset})^{upper})")
        if mode == 1:
            factors.reverse()
        rows.append(f"({3 << (width-observed)}*{'*'.join(factors)})"
                    "+(x^y)+2*(x&y)+(z|w)+(z&w)")
    return rows


def narrow_product_cross_terms_corpus(width, count):
    """Partial products with three effective nonlinear bits and deeper carries."""
    rows = []
    configurations = ((6, 23, 3), (6, 30, 3), (7, 47, 4),
                      (7, 62, 4), (8, 95, 5), (8, 126, 5))
    for index in range(count):
        kind = index % 36
        observed, degree, high = configurations[kind // 6]
        mode, neighbor = kind % 6 // 2, bool(kind % 2)
        base = ("x", "(x^1)", "(~x)")[mode]
        factors = []
        for i in range(degree):
            offset = 2*i + int(neighbor and i == 0)
            upper = f"(((y+{i})&-2)*((z+{2*i})&-{1 << (high-1)}))"
            factors.append(f"(({base}+{offset})^{upper})")
        if mode == 2:
            factors.reverse()
        rows.append(f"({3 << (width-observed)}*{'*'.join(factors)})"
                    "+(x^y)+2*(x&y)+(z|w)+(z&w)")
    return rows


def residue_power_corpus(width, count):
    """Degree-16–512 grouped powers with an independent-parity multiplier.

    Period boundaries and their immediate neighbors retain all even inputs;
    the final flavor changes the low residue of exactly one factor.
    """
    rows = []
    for index in range(count):
        kind = index % 81
        degree = (16, 17, 32, 33, 64, 65, 128, 129, 512)[kind // 9]
        observed = (4, 6, 8)[kind % 9 // 3]
        split = (observed + 2) // 3
        mode = kind % 3
        base = ("x", "(~x)", "(x^1)")[mode]
        factors = [f"(({base}+{(i << split) + int(mode == 2 and i == 0)})"
                   f"^(y<<{split}))" for i in range(degree)]
        if mode == 1:
            factors.reverse()
        rows.append(f"({3 << (width-observed)}*z*{'*'.join(factors)})"
                    "+(x^y)+2*(x&y)+(z|w)+(z&w)")
    return rows


def residue_quotient_corpus(width, count):
    """Degree-512–1025 products with nested high offsets and low disturbances.

    The nested flavor changes every full-width factor while preserving its
    observed residue. Immediate degree neighbors and a one-factor low change
    prevent repeated-residue assumptions from becoming unconditional rewrites.
    """
    rows = []
    for index in range(count):
        kind = index % 72
        degree = (512, 513, 640, 641, 768, 769, 1024, 1025)[kind // 9]
        observed = (4, 6, 8)[kind % 9 // 3]
        split = (observed + 2) // 3
        mode = kind % 3
        factors = []
        for i in range(degree):
            offset = (i << split) + int(mode == 2 and i == 0)
            value = f"((x+{offset})^(y<<{split}))"
            if mode == 1:
                value = f"(({value}+{(2*i+1) << observed})^{(3*i+1) << observed})"
            factors.append(value)
        if mode == 2:
            factors.reverse()
        rows.append(f"({3 << (width-observed)}*z*{'*'.join(factors)})"
                    "+(x^y)+2*(x&y)+(z|w)+(z&w)")
    return rows


def residue_subgroup_corpus(width, count):
    """Degree-511–1026 groups with multiple independent residual factors.

    Off-period counts, two disturbed low prefixes, complemented bases and
    nested high offsets retain all original factors in the modular oracle.
    """
    rows = []
    for index in range(count):
        kind = index % 54
        degree = (511, 514, 767, 770, 1023, 1026)[kind // 9]
        observed = (4, 6, 8)[kind % 9 // 3]
        split = (observed + 2) // 3
        mode = kind % 3
        base = ("x", "(~x)", "(x^1)")[mode]
        factors = []
        for i in range(degree):
            disturbance = (int(i == 0) + 2 * int(i == degree - 1)) if mode == 2 else 0
            value = f"(({base}+{(i << split) + disturbance})^(y<<{split}))"
            if mode == 1:
                value = f"(({value}+{(2*i+1) << observed})^{(3*i+1) << observed})"
            factors.append(value)
        if mode:
            factors.reverse()
        residual = "((z+3)^(y&7))" if mode == 2 else "(z+3)"
        rows.append(f"({3 << (width-observed)}*z*{residual}*{'*'.join(factors)})"
                    "+(x^y)+2*(x&y)+(z|w)+(z&w)")
    return rows


def sparse_linear_mixed_degree_corpus(width, count):
    """Degree-4–18 products with disjoint constant, linear and perturbed tails.

    The 135-case cycle varies degree, mixed/partitioned products, tail support,
    and factor order. Product-only variables must not enlarge the linear table.
    """
    rows = []
    for index in range(count):
        degree = (4, 8, 12, 16, 18)[(index // 27) % 5]
        kind, tail, order = (index // 9) % 3, (index // 3) % 3, index % 3
        variables = [f"v{i}" for i in range(2, degree + 2)]
        if order == 1:
            variables.reverse()
        elif order == 2:
            variables = variables[::2] + variables[1::2]
        body = f"3*{'*'.join(variables)}"
        if kind == 1:
            body += f"+5*{'*'.join(variables[:degree//2])}"
        elif kind == 2:
            left, right = variables[:2]
            common = '*'.join(variables[2:])
            body = (f"3*({left}&{right})*({left}|{right})*{common}"
                    f"+3*({left}&~{right})*(~{left}&{right})*{common}")
        linear = ("13", "13+7*(v0^v1)+14*(v0&v1)",
                  f"13+7*(v0^v1)+14*(v0&v1)+(v0&{1 << (width-1)})")[tail]
        rows.append(f"({body})+({linear})")
    return rows


def check_semantics(expressions, results, width):
    """Independent modular evaluator; never execute parsed source as Python."""
    mask = (1 << width) - 1
    binary = {
        ast.Add: operator.add,
        ast.Sub: operator.sub,
        ast.Mult: operator.mul,
        ast.BitAnd: operator.and_,
        ast.BitOr: operator.or_,
        ast.BitXor: operator.xor,
        ast.LShift: operator.lshift,
        ast.RShift: operator.rshift,
    }
    unary = {ast.USub: operator.neg, ast.UAdd: operator.pos, ast.Invert: operator.invert}

    def parse(expression):
        # SymPrint's explicit widths are lexical suffixes, not Python comments.
        expression = re.sub(r"\b(0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+|[A-Za-z_]\w*)#([0-9]+)\b",
                            r"word(\1,\2)", expression)
        return ast.parse(expression, mode="eval")

    def typed(node, values, evaluated):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value & mask, width
        if isinstance(node, ast.Name) and node.id in values:
            return values[node.id], width
        if isinstance(node, ast.BinOp) and type(node.op) in binary:
            left, left_width = evaluated[node.left]
            right, right_width = evaluated[node.right]
            bits = max(left_width, right_width)
            word_mask = (1 << bits) - 1
            left &= word_mask
            right &= (1 << (right_width or bits)) - 1
            if isinstance(node.op, (ast.LShift, ast.RShift)) and right >= bits:
                return 0, bits
            return binary[type(node.op)](left, right) & word_mask, bits
        if isinstance(node, ast.UnaryOp) and type(node.op) in unary:
            value, bits = evaluated[node.operand]
            bits = bits or width
            return unary[type(node.op)](value) & ((1 << bits) - 1), bits
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
            name = node.func.id
            if name == "word" and len(node.args) == 2:
                value, bits = node.args
                if (isinstance(bits, ast.Constant) and type(bits.value) is int
                        and bits.value > 0):
                    if isinstance(value, ast.Constant) and type(value.value) is int:
                        return value.value & ((1 << bits.value) - 1), bits.value
                    if isinstance(value, ast.Name) and value.id in values:
                        return values[value.id] & ((1 << bits.value) - 1), bits.value
            if name == "extract" and len(node.args) == 3:
                value, _ = evaluated[node.args[0]]
                offset, bits = node.args[1:]
                if (isinstance(offset, ast.Constant) and type(offset.value) is int
                        and isinstance(bits, ast.Constant) and type(bits.value) is int
                        and offset.value >= 0 and bits.value > 0):
                    return (value >> offset.value) & ((1 << bits.value) - 1), bits.value
            if name in ("zext", "sext", "trunc") and len(node.args) == 2:
                value, source_bits = evaluated[node.args[0]]
                target = node.args[1]
                if (isinstance(target, ast.Constant) and type(target.value) is int
                        and target.value > 0):
                    source_bits = source_bits or width
                    value &= (1 << source_bits) - 1
                    if name == "sext" and value & (1 << (source_bits - 1)):
                        value -= 1 << source_bits
                    return value & ((1 << target.value) - 1), target.value
        raise ValueError(f"Unsupported benchmark expression: {ast.dump(node)}")

    def evaluation_order(node):
        pending = [(node, False)]
        order = []
        while pending:
            current, visited = pending.pop()
            if visited:
                order.append(current)
                continue
            pending.append((current, True))
            if isinstance(current, ast.BinOp):
                pending.extend(((current.right, False), (current.left, False)))
            elif isinstance(current, ast.UnaryOp):
                pending.append((current.operand, False))
            elif (isinstance(current, ast.Call)
                  and isinstance(current.func, ast.Name)
                  and current.func.id in ("extract", "zext", "sext", "trunc")
                  and current.args):
                pending.append((current.args[0], False))
        return order

    def evaluate(order, values):
        evaluated = {}
        for node in order:
            evaluated[node] = typed(node, values, evaluated)
        return evaluated[order[-1]][0] & mask

    rng = random.Random(0x53454D00 + width)
    edges = (0, 1, mask, mask >> 1, 1 << (width - 1), mask // 3)
    names = sorted({
        node.id for expression in [*expressions, *(r["output"] for r in results)]
        for node in ast.walk(parse(expression))
        if isinstance(node, ast.Name) and node.id not in ("word", "extract", "zext", "sext", "trunc")
    })
    assignments = [
        {name: (x if index % 2 == 0 else y)
         for index, name in enumerate(names)}
        for x in edges for y in edges
    ]
    for name in names:
        for base in (0, mask):
            for edge in edges:
                assignments.append({**dict.fromkeys(names, base), name: edge})
    assignments += [
        {name: rng.getrandbits(width) for name in names}
        for _ in range(128)
    ]
    if len(expressions) != len(results):
        raise AssertionError("Engine returned an incomplete corpus")
    for expression, result in zip(expressions, results):
        before = evaluation_order(parse(expression).body)
        after = evaluation_order(parse(result["output"]).body)
        for values in assignments:
            if evaluate(before, values) != evaluate(after, values):
                raise AssertionError((width, expression, result["output"], values))


def run(engine_info, corpus_path, width):
    executable = engine_info["path"]
    environment = os.environ.copy()
    variable = (
        "DYLD_LIBRARY_PATH" if platform.system() == "Darwin" else "LD_LIBRARY_PATH"
    )
    environment[variable] = str(Path(executable).parent)
    command = [
        executable,
        "simplify",
        "--json",
        "--stats",
        f"--width={width}",
        "--max-work=65536",
        "-f",
        str(corpus_path),
    ]
    result = subprocess.run(
        command, env=environment, capture_output=True, text=True, timeout=120
    )
    # A bounded search may legitimately decline. Malformed queries may not.
    report = json.loads(result.stdout)
    if any(not row["ok"] for row in report["results"]):
        raise RuntimeError(result.stderr or result.stdout)
    return {"returncode": result.returncode, **report}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--count", type=int, default=32)
    parser.add_argument(
        "--family",
        choices=("complement", "weighted", "bitwise", "interleaved", "quadratic",
                 "cubic", "higher", "perturbed", "completion", "residual", "deep-residual",
                 "row-residual", "mixed-frontier", "extreme-frontier",
                 "residue-product", "affine-residue", "prefix-residue", "residue-frontier",
                 "distributed-prefix", "multiple-residual", "carry-product",
                 "permuted-carry-product", "complemented-carry-product",
                 "narrow-product-cross-terms",
                 "residue-power", "residue-quotient", "residue-subgroup",
                 "sparse-linear-mixed-degree"),
        default="complement",
    )
    args = parser.parse_args()
    if args.rounds < 1 or args.count < 1:
        parser.error("rounds and count must be positive")
    engines = {"baseline": engine(args.baseline), "candidate": engine(args.candidate)}
    report = {
        "schemaVersion": 1,
        "platform": platform.platform(),
        "engines": engines,
        "rounds": args.rounds,
        "family": args.family,
        "widths": [],
    }
    with tempfile.TemporaryDirectory(prefix="neverd-mask-bench-") as directory:
        path = Path(directory) / "expressions.txt"
        for width in (8, 16, 32, 64):
            generate = {
                "complement": corpus, "weighted": weighted_corpus,
                "bitwise": bitwise_corpus, "interleaved": interleaved_corpus,
                "quadratic": quadratic_corpus,
                "cubic": cubic_corpus,
                "higher": higher_corpus,
                "perturbed": perturbed_corpus,
                "completion": completion_corpus,
                "residual": residual_corpus,
                "deep-residual": deep_residual_corpus,
                "row-residual": row_residual_corpus,
                "mixed-frontier": mixed_frontier_corpus,
                "extreme-frontier": extreme_frontier_corpus,
                "residue-product": residue_product_corpus,
                "affine-residue": affine_residue_corpus,
                "prefix-residue": prefix_residue_corpus,
                "residue-frontier": residue_frontier_corpus,
                "distributed-prefix": distributed_prefix_corpus,
                "multiple-residual": multiple_residual_corpus,
                "carry-product": carry_product_corpus,
                "permuted-carry-product": permuted_carry_product_corpus,
                "complemented-carry-product": complemented_carry_product_corpus,
                "narrow-product-cross-terms": narrow_product_cross_terms_corpus,
                "residue-power": residue_power_corpus,
                "residue-quotient": residue_quotient_corpus,
                "residue-subgroup": residue_subgroup_corpus,
                "sparse-linear-mixed-degree": sparse_linear_mixed_degree_corpus,
            }[args.family]
            expressions = generate(width, args.count)
            path.write_text("\n".join(expressions) + "\n", encoding="utf-8")
            row = {
                "width": width,
                "corpusSha256": digest(path),
                "expressions": expressions,
                "runs": {name: [] for name in engines},
            }
            # Warm both processes, then alternate order to reduce thermal/load bias.
            for info in engines.values():
                warm = run(info, path, width)
                check_semantics(expressions, warm["results"], width)
            for iteration in range(args.rounds):
                for name in (
                    list(engines) if iteration % 2 == 0 else list(reversed(engines))
                ):
                    row["runs"][name].append(run(engines[name], path, width))
            row["medians"] = {
                name: {
                    key: statistics.median(r["summary"][key] for r in runs)
                    for key in (
                        "p50Micros",
                        "p95Micros",
                        "work",
                        "costAfter",
                        "simplified",
                    )
                }
                for name, runs in row["runs"].items()
            }
            report["widths"].append(row)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(
                json.dumps(report, indent=2) + "\n", encoding="utf-8"
            )
            print(width, json.dumps(row["medians"]), flush=True)


if __name__ == "__main__":
    main()
