#!/usr/bin/env python3
"""Generate a scaled MBA product or its independently accumulated reference."""

import argparse
import random
from collections import Counter


def distributed_product(width, case, *, reference=False, counted=False, joined_offsets=False):
    """Return one input, its affine reference, or its counted evaluation form."""
    variant = case % 4
    bits = ((5, 7, 9, 11) if width <= 32 else (17, 37, 65, 97))[variant]
    if width < 2 * bits + 2:
        raise ValueError("width must accommodate the observed field and coefficient")
    degree = ((103, 149, 211, 277) if width <= 32 else
              (283, 389, 461, 557) if width <= 513 else
              (12011, 14009, 16001, 18013))[variant]
    if joined_offsets:
        degree = ((127, 179, 229, 317) if width <= 32 else
                  (307, 419, 503, 601) if width <= 513 else
                  (20011, 23003, 26003, 29009))[variant]
    shift = bits + case % 2
    precision = shift + bits
    mask = (1 << precision) - 1
    field_mask = (1 << bits) - 1
    unit = (3, 5, 7, -3)[variant]
    scale = (unit << (width - precision)) % (1 << width)
    rng = random.Random(0xAD947 + 17 * width + 103 * case)
    sources = ["x", "(y+z)", "(u^v)", "((x&w)+z)", "((u|y)^x)", "((v-x)&w)",
               "((x+y)^u)", "((v&z)-w)", "((u^w)+x)", "((z-v)&u)",
               "((w^z)|y)", "((x-u)^v)"]
    fields = []
    for j, source in enumerate(sources):
        start = width - 1 - j % 4 if j % 3 == 0 else (j * 11 + case + 2) % (width - bits + 1)
        if start + bits > width:
            source = f"ror({source},{start})"
            start = 0
        fields.append(f"extract({source},{start},{bits})")

    pool = []
    for j in range((31, 47, 67, 89)[variant]):
        offset = rng.choice((-9, -5, -1, 1, 3, 7, 11))
        if joined_offsets:
            offset = abs(offset)
            if j % 3 == 0:
                offset |= 1 << (precision + 1 + j % 5)
        row = []
        for axis in rng.sample(range(len(fields)), 2 + j % 3):
            weight = rng.choice((-11, -7, -3, 1, 3, 5, 9)) << rng.randrange(4)
            field = fields[axis]
            spelling = (j + axis) % 3
            if spelling == 2:
                extended = f"concat(extract((u+{j % 11 + 1}*v),{bits},{width - bits}),{field})"
            else:
                extended = f"{('zext', 'sext')[spelling]}({field},{width})"
            row.append((axis, weight, extended))
        pool.append((offset, row))

    factors, residuals = [], []
    constant = 1
    linear = [0] * len(fields)
    for j in range(degree):
        if j % len(pool) == 0:
            rng.shuffle(pool)
        offset, row = pool[j % len(pool)]
        if variant == 3 and j == degree // 3:
            offset = 6
        overlapping = joined_offsets and variant >= 2 and j == degree // 4
        if overlapping:
            offset |= 1 << shift
        live_overlap = overlapping and any(weight & 1 for _, weight, _ in row)
        if variant and j == degree // 2:
            field = fields[(j + case) % len(fields)]
            factor = f"(3+12*sext({field},{width}))"
            residuals.append(factor)
        elif reference and not live_overlap:
            # Multiply the old polynomial by this factor in Z/(2^precision).
            # Every product of two shifted weights vanishes in this ring.
            linear = [v * offset & field_mask for v in linear]
            for axis, weight, _ in row:
                linear[axis] = (linear[axis] + constant * weight) & field_mask
            constant = constant * offset & mask
            continue
        else:
            terms = [f"({weight << shift})*{word}" for _, weight, word in row]
            if joined_offsets:
                op = ("|", "^")[(row[0][0] + offset) % 2]
                factor = f"({offset}{op}({'+'.join(terms)}))"
                if live_overlap:
                    residuals.append(factor)
            else:
                factor = f"({offset}+{'+'.join(terms)})"
        factors.append(factor)

    if reference:
        value = unit * constant & mask
        terms = [f"{value >> shift}#{bits}"]
        for coefficient, field in zip(linear, fields):
            live = (unit * coefficient) & field_mask
            if live:
                terms.append(f"{live}#{bits}*{field}")
        tail = (value % (1 << shift)) << (width - precision)
        packed = f"concat(({'+'.join(terms)}),{tail:#x}#{width - bits})"
        return "(" + "*".join([packed, *residuals]) + ")+(x+y)+(u|v)"
    if counted:
        terms = ",".join(f"{term},{copies}" for term, copies in Counter(factors).items())
        product = f"scaled_product({scale:#x},{terms})"
    else:
        product = f"({scale:#x}*" + "*".join(factors) + ")"
    return product + "+(x^y)+2*(x&y)+(u|v)"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", required=True, type=int)
    parser.add_argument("--case", type=int, default=0)
    parser.add_argument("--joined-offsets", action="store_true",
                        help="join constants to weighted fields with OR and XOR")
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--reference", action="store_true",
                        help="emit the independently calculated reference expression")
    output.add_argument("--counted", action="store_true",
                        help="emit repeated factors for the benchmark modular evaluator")
    args = parser.parse_args()
    try:
        print(distributed_product(args.width, args.case, reference=args.reference, counted=args.counted,
                                  joined_offsets=args.joined_offsets))
    except ValueError as error:
        parser.error(str(error))
