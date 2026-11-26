#!/usr/bin/env python3
"""Generate repeated MBA factors with one or two observable square bits."""

import argparse
import random


def paired_product(width, case, *, reference=False, counted=False):
    if width < 8 or not 0 <= case < 8:
        raise ValueError("expected a width of at least 8 and a case from 0 through 7")
    bits = min(
        (width - 2) // 2,
        (5, 7, 9, 11)[case % 4] if width <= 32 else (9, 17, 37, 65)[case % 4],
    )
    shift = bits + (case % 2)
    k = shift - 1
    precision = shift + bits
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
    factors = []
    rows = []
    pool_size = 109 + 11 * (case % 4) if width > 65536 else 7 + case % 4
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
    pairs = (degree - 1) // 2
    counts = [pairs // len(rows) + (j < pairs % len(rows)) for j in range(len(rows))]
    if case % 4 == 2:
        # One squared even offset leaves a nonzero cofactor at small precisions.
        counts[1] += counts[0] - 1
        counts[0] = 1
    # Odd copies outside the pairs retain products of independent fields.
    leftovers = [1, 3][: 1 + int(case % 4 >= 2)]
    outside = [factors[i] for i in leftovers]
    scale = f"{unit << (width - precision):#x}"
    if counted:
        powers = [
            f"{factor},{2 * counts[j] + leftovers.count(j)}"
            for j, factor in enumerate(factors)
        ]
        return f"scaled_product({scale},{','.join(powers)})+(x^y)+2*(x&y)"
    if not reference:
        seq = [f for f, c in zip(factors, counts) for _ in range(2 * c)]
        rng = random.Random(0x931958 + case)
        rng.shuffle(seq)
        return "(" + "*".join([scale, *seq, *outside]) + ")+(x^y)+2*(x&y)"
    # Evaluate the product of pairs at 0, e_i, 2 e_i and e_i+e_j.
    # Different pairs have square-zero perturbations after the external scale.
    # Their product has degree at most two. Finite differences reconstruct
    # that polynomial directly from the original modular powers.
    modulus = 1 << precision

    def probe(values):
        product = unit
        for (offset, terms), copies in zip(rows, counts):
            value = offset + (sum(w * values[i] for i, w in terms) << k)
            product = product * pow(value, 2 * copies, modulus) % modulus
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
        assert first % (1 << shift) == 0 and second in (0, modulus // 2)
        if first:
            pieces.append(f"{first >> shift}#{bits}*{name}")
        if second:
            pieces.append(f"{second >> shift}#{bits}*zext(extract({name},1,1),{bits})")
        for j in range(i):
            cross = (at_indices(i, j) - ones[i] - ones[j] + origin) % modulus
            assert cross in (0, modulus // 2)
            if cross:
                pieces.append(f"{cross >> shift}#{bits}*({name}&{names[j]})")
    low = (origin % (1 << shift)) << (width - precision)
    packed = f"concat(({'+'.join(pieces)}),{low:#x}#{width - bits})"
    return "(" + "*".join([packed, *outside]) + ")+(x+y)"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", required=True, type=int)
    parser.add_argument("--case", type=int, choices=range(8), default=0)
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
                args.width, args.case, reference=args.reference, counted=args.counted
            )
        )
    except ValueError as error:
        parser.error(str(error))
