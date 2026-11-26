#!/usr/bin/env python3
"""Generate a scaled MBA product or its independently accumulated reference."""

import argparse
import random
from collections import Counter


def distributed_product(width, case, *, reference=False, counted=False,
                        joined_offsets=False, masked_offsets=False, field_masks=False,
                        scaled_fields=False, result_masks=False, carry_masks=False,
                        sparse_masks=False):
    """Return one input, its affine reference, or its counted evaluation form."""
    if sum((joined_offsets, masked_offsets, field_masks, scaled_fields, result_masks, carry_masks, sparse_masks)) > 1:
        raise ValueError("choose one bitwise offset family")
    carry_masks = carry_masks or sparse_masks
    output_mask = result_masks or carry_masks
    scaled = scaled_fields or output_mask
    gated = field_masks or scaled
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
    if masked_offsets:
        degree = ((137, 193, 251, 347) if width <= 32 else
                  (349, 457, 569, 677) if width <= 513 else
                  (31013, 34019, 39019, 43003))[variant]
    if field_masks:
        degree = ((163, 211, 283, 373) if width <= 32 else
                  (379, 503, 613, 733) if width <= 513 else
                  (49157, 53003, 61001, 70001))[variant]
    if scaled_fields:
        degree = ((179, 239, 313, 431) if width <= 32 else
                  (421, 557, 683, 809) if width <= 513 else
                  (79013, 85021, 91009, 100003))[variant]
    if output_mask:
        degree = ((197, 263, 353, 463) if width <= 32 else
                  (487, 601, 743, 911) if width <= 513 else
                  (105019, 111029, 121021, 131071))[variant]
    if carry_masks:
        degree += degree // 4
    if sparse_masks:
        degree += (degree + 1) // 2
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
        if joined_offsets or masked_offsets or gated:
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
        sign = 1
        if masked_offsets:
            op = ("&~", "^", "|")[row[0][0] % 3]
            high = field_mask if (row[-1][0] + offset) % 2 else 0
            if variant >= 2 and j == degree // 4:
                high = field_mask // 3
            offset |= high << shift
            live_overlap = high not in (0, field_mask)
            # On the live bits a uniform mask passes, negates or erases the
            # weighted sum. A partial mask stays as an independent factor.
            if high == field_mask:
                sign = 0 if op == "|" else -1
            elif op == "&~":
                sign = 0

        if gated:
            arithmetic = "-" if case % 8 >= 4 else "+"
            op = (arithmetic, "|", "&~", "^")[(row[0][0] + row[-1][0]) % 4]
            high = field_mask if row[1][0] % 3 == 0 else 0
            offset |= high << shift
            gate = 0 if row[-1][0] % 5 == 0 else field_mask
            if variant >= 2 and j == degree // 4:
                gate = field_mask // 3
                op = arithmetic
            if carry_masks:
                trailing = 1 + row[0][0] % min(bits - 1, 4)
                # These holes leave the observed word when the inner scale shifts.
                gate = field_mask ^ (1 << (bits - trailing + row[1][0] % trailing))
                if row[-1][0] % 7 == 0:
                    gate &= field_mask ^ ((1 << (bits - trailing)) - 1)
                if variant >= 2 and j == degree // 4:
                    gate ^= 1 << (bits - trailing - 1)
            if op == arithmetic:
                sign = -1 if op == "-" else 1
            elif high == field_mask:
                sign = 0 if op == "|" else -1
            elif op == "&~":
                sign = 0
            if not gate:
                sign = 0
            live_overlap = gate not in (0, field_mask)
            lane_mask = (1 << (precision + 3 + row[0][0])) | (gate << shift) | 5
            if scaled:
                multipliers = (3, -5, 6, (1 << (bits - 1)) + 5, -(1 << bits) + 9)
                multiplier = multipliers[(row[0][0] + row[1][0]) % len(multipliers)]
                if carry_masks:
                    multiplier = (multiplier | 1) << trailing
                sign *= multiplier
            if output_mask:
                # The second gate selects bits after multiplication. Carries can
                # enter a hole in the input mask or leave an output-selected bit.
                result_gate = 0 if row[1][0] % 5 == 0 else field_mask
                if variant >= 2 and j == degree // 4:
                    result_gate = (field_mask // 3) ^ (field_mask // 7)
                visible = field_mask
                input_bits = gate
                if carry_masks:
                    visible >>= trailing
                    input_bits &= visible
                    result_gate = field_mask ^ (1 << (row[-1][0] % trailing))
                    if row[1][0] % 7 == 0:
                        result_gate &= (1 << trailing) - 1
                    if variant >= 2 and j == degree // 4:
                        result_gate ^= 1 << trailing
                output_bits = result_gate >> trailing if carry_masks else result_gate
                if not input_bits or not output_bits:
                    sign, live_overlap = 0, False
                else:
                    live_overlap = input_bits != visible or output_bits != visible
                if sparse_masks:
                    mode = (row[0][0] + 2 * row[-1][0]) % 8
                    near = variant >= 2 and j == degree // 5
                    if near:
                        mode = 0
                    if mode < 5 and not (variant >= 2 and j == degree // 4):
                        stripe = max(1, visible // 3)
                        if (row[1][0] + case) % 2:
                            stripe = visible ^ stripe or 1
                        if mode < 2:
                            # The non-shift part disappears at the observed width.
                            multiplier = ((-1 if mode else 1) << bits) + (1 << trailing)
                            gate = stripe
                            result_gate = (visible ^ stripe) << trailing
                            if near:
                                result_gate |= (stripe & -stripe) << trailing
                        elif mode == 2:
                            # Even with carries, no output below the first set
                            # input bit and the multiplier's shift can survive.
                            split = max(1, visible.bit_length() // 2)
                            gate = visible & ~((1 << split) - 1)
                            result_gate = (1 << (split + trailing)) - 1
                        else:
                            gate = visible
                            result_gate = stripe << trailing
                            high = result_gate if mode == 3 else field_mask ^ result_gate
                            offset = (offset & ~(field_mask << shift)) | (high << shift)
                            op = "|" if mode == 3 else "&~"
                        lane_mask = (1 << (precision + 3 + row[0][0])) | (gate << shift) | 5
                        sign, live_overlap = 0, near
                result_mask = (1 << (precision + 2 + row[1][0])) | (result_gate << shift) | 3

        if variant and j == degree // 2:
            field = fields[(j + case) % len(fields)]
            factor = f"(3+12*sext({field},{width}))"
            residuals.append(factor)
        elif reference and not live_overlap:
            # Multiply the old polynomial by this factor in Z/(2^precision).
            # Every product of two shifted weights vanishes in this ring.
            linear = [v * offset & field_mask for v in linear]
            for axis, weight, _ in row:
                linear[axis] = (linear[axis] + constant * sign * weight) & field_mask
            constant = constant * offset & mask
            continue
        else:
            terms = [f"({weight << shift})*{word}" for _, weight, word in row]
            if joined_offsets or masked_offsets or gated:
                if joined_offsets:
                    op = ("|", "^")[(row[0][0] + offset) % 2]
                delta = "+".join(terms)
                if gated:
                    delta = f"{lane_mask}&({delta})"
                    if scaled:
                        delta = f"({multiplier})*({delta})"
                    if output_mask:
                        delta = f"{result_mask}&({delta})"
                factor = f"({offset}{op}({delta}))"
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
    offsets = parser.add_mutually_exclusive_group()
    offsets.add_argument("--masked-offsets", action="store_true",
                        help="mix masks that clear, invert or set weighted fields")
    offsets.add_argument("--joined-offsets", action="store_true",
                        help="join constants to weighted fields with OR and XOR")
    offsets.add_argument("--field-masks", action="store_true",
                        help="mask weighted sums before combining them with an offset")
    offsets.add_argument("--scaled-fields", action="store_true",
                        help="multiply masked weighted sums before combining them with an offset")
    offsets.add_argument("--result-masks", action="store_true",
                        help="mask weighted sums both before and after multiplication")
    offsets.add_argument("--carry-masks", action="store_true",
                         help="mix observable mask holes with bits lost to even multipliers")
    offsets.add_argument("--sparse-masks", action="store_true",
                         help="interleave sparse selections, modular shifts and live carry controls")
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--reference", action="store_true",
                        help="emit the independently calculated reference expression")
    output.add_argument("--counted", action="store_true",
                        help="emit repeated factors for the benchmark modular evaluator")
    args = parser.parse_args()
    try:
        print(distributed_product(args.width, args.case, reference=args.reference, counted=args.counted,
                                  joined_offsets=args.joined_offsets, masked_offsets=args.masked_offsets,
                                  field_masks=args.field_masks, scaled_fields=args.scaled_fields,
                                  result_masks=args.result_masks,
                                  carry_masks=args.carry_masks, sparse_masks=args.sparse_masks))
    except ValueError as error:
        parser.error(str(error))
