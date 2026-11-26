#!/usr/bin/env python3
"""Generate repeated MBA factors with observable quadratic terms at several repetition depths."""

import argparse
import random


def paired_product(width, case, *, reference=False, counted=False, group_log=1):
    if width < 8 or not 0 <= case < 8:
        raise ValueError("expected a width of at least 8 and a case from 0 through 7")
    bits = min(
        (width - 2) // 2,
        (5, 7, 9, 11)[case % 4] if width <= 32 else (9, 17, 37, 65)[case % 4],
    )
    shift = bits + (case % 2)
    precision = shift + bits
    if not 1 <= group_log <= 4:
        raise ValueError("expected group_log from 1 through 4")
    k = shift - group_log
    if group_log > 1 and (
        k < 1 or min(3 * k + group_log, 4 * k + group_log - 2) < precision
    ):
        raise ValueError(
            "the width is too small for a quadratic reference at this depth"
        )
    group = 1 << group_log
    unit = (3, 5, 7, 11)[case % 4]
    axes = ["x", "(y^z)", "((u&v)+x)", "(v|z)", "(u-y)", "(x^v)"]
    names = []
    for i, axis in enumerate(axes):
        start = (i * 7 + case) % (width - bits + 1)
        names.append(f"extract({axis},{start},{bits})")
    if width <= 513:
        degree = (97, 129, 165, 209)[case % 4]
    elif width <= 12289:
        degree = (601, 837, 1057, 1361)[case % 4]
    elif width <= 65536:
        degree = (2039, 3079, 4099, 5153)[case % 4]
    else:
        degree = (260001, 286013, 327701, 360017)[case % 4]
    if group_log > 1:
        if width > 65536:
            degree = (393253, 434081, 487073, 524321)[case % 4]
        else:
            degree = 2 * degree + 4 * group + 1
    factors = []
    rows = []
    pool_size = 109 + 11 * (case % 4) if width > 65536 else 7 + case % 4
    if group_log > 1 and width > 65536:
        pool_size += 47
    for j in range(pool_size):
        offset = (1, 3, 5, 9, -3, 7, 11)[j % 7]
        if j >= 10:
            offset *= 2 * (j // 7) + 1
        if case % 4 == 2 and j == 0:
            offset = 2
        terms = [(j % 6, (1, 3, -5)[j % 3]), ((j + 1) % 6, (3, -7, 5)[j % 3])]
        if j % 3 == 2:
            terms.append(((j + 4) % 6, 3 if case >= 4 else 2))
        if case >= 4 and j % 4 == 1:
            terms.append(((j + 3) % 6, -5))
        if j >= 10:
            terms[0] = (terms[0][0], terms[0][1] * (2 * (j // 6) + 1))
        weighted_terms = []
        for axis, weight in terms:
            ext = ("zext", "sext")[(j + axis) % 2]
            weighted_terms.append(f"({weight << k})*{ext}({names[axis]},{width})")
        factors.append("(" + str(offset) + "+" + "+".join(weighted_terms) + ")")
        rows.append((offset, terms))
    blocks = (degree - 1) // group
    counts = [blocks // len(rows) + (j < blocks % len(rows)) for j in range(len(rows))]
    if case % 4 == 2:
        # One block with an even offset leaves a nonzero cofactor at small precisions.
        counts[1] += counts[0] - 1
        counts[0] = 1
    # Incomplete blocks retain products of independent fields.
    leftovers = [1, 3][: 1 + int(case % 4 >= 2)]
    if group_log > 1:
        leftovers.extend([2] * (case % group))
    outside = [factors[i] for i in leftovers]
    scale = f"{unit << (width - precision):#x}"
    if counted:
        powers = [
            f"{factor},{group * counts[j] + leftovers.count(j)}"
            for j, factor in enumerate(factors)
        ]
        return f"scaled_product({scale},{','.join(powers)})+(x^y)+2*(x&y)"
    if not reference:
        seq = [f for f, c in zip(factors, counts) for _ in range(group * c)]
        rng = random.Random(0x931958 + case)
        rng.shuffle(seq)
        return "(" + "*".join([scale, *seq, *outside]) + ")+(x^y)+2*(x&y)"
    # Evaluate the product of complete blocks at 0, e_i, 2 e_i and e_i+e_j.
    # Different blocks have square-zero perturbations after the external scale.
    # Their product has degree at most two. Finite differences reconstruct
    # that polynomial directly from the original modular powers.
    modulus = 1 << precision

    def probe(values):
        product = unit
        for (offset, terms), copies in zip(rows, counts):
            value = offset + (sum(w * values[i] for i, w in terms) << k)
            product = product * pow(value, group * copies, modulus) % modulus
        return product

    def at_indices(*indices):
        values = [0] * len(names)
        for i in indices:
            values[i] += 1
        return probe(values)

    origin = at_indices()
    ones = [at_indices(i) for i in range(len(names))]
    pieces = [f"{origin >> shift}#{bits}"]
    for i, name in enumerate(names):
        second = (at_indices(i, i) - 2 * ones[i] + origin) % modulus
        # x(x-1)/2 has parity ((x>>1)&1) when the second difference is half the ring.
        first = (ones[i] - origin) % modulus
        assert first % (1 << shift) == 0 and second % (2 << shift) == 0
        if group_log > 1:
            # The Newton term second*x*(x-1)/2 is integral. Its coefficient
            # loses no bits on division since second is a multiple of 2^(shift+1).
            quadratic = second >> (shift + 1)
            linear = ((first >> shift) - quadratic) % (1 << bits)
            if linear:
                pieces.append(f"{linear}#{bits}*{name}")
            if quadratic:
                pieces.append(f"{quadratic}#{bits}*{name}*{name}")
        else:
            assert second in (0, modulus // 2)
            if first:
                pieces.append(f"{first >> shift}#{bits}*{name}")
            if second:
                pieces.append(
                    f"{second >> shift}#{bits}*zext(extract({name},1,1),{bits})"
                )
        for j in range(i):
            cross = (at_indices(i, j) - ones[i] - ones[j] + origin) % modulus
            assert cross % (1 << shift) == 0
            if cross:
                if group_log > 1:
                    pieces.append(f"{cross >> shift}#{bits}*{name}*{names[j]}")
                else:
                    assert cross == modulus // 2
                    pieces.append(f"{cross >> shift}#{bits}*({name}&{names[j]})")
    low = (origin % (1 << shift)) << (width - precision)
    packed = f"concat(({'+'.join(pieces)}),{low:#x}#{width - bits})"
    return "(" + "*".join([packed, *outside]) + ")+(x+y)"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", required=True, type=int)
    parser.add_argument("--case", type=int, choices=range(8), default=0)
    parser.add_argument(
        "--group-log",
        type=int,
        choices=range(1, 5),
        default=1,
        help="log2 of the repeated block size",
    )
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument(
        "--reference",
        action="store_true",
        help="emit the independently interpolated polynomial",
    )
    modes.add_argument(
        "--counted",
        action="store_true",
        help="compress repeated factors for the modular evaluator",
    )
    args = parser.parse_args()
    try:
        print(
            paired_product(
                args.width,
                args.case,
                reference=args.reference,
                counted=args.counted,
                group_log=args.group_log,
            )
        )
    except ValueError as error:
        parser.error(str(error))
