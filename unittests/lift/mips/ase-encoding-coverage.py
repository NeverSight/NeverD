#!/usr/bin/env python3
"""Assemble and lift every documented MIPS32 DSP/R2 and MSA mnemonic/format."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

# Independent tables from MIPS DSP Volume IV-e revision 3.01 (pre-R6) and
# MIPS SIMD Volume IV-j revision 1.12. Each row supplies assembly operands,
# legal element widths and mnemonic stems. DSP spellings already include width.
DSP = (
    (
        "$2,$5",
        "",
        "absq_s.ph absq_s.w bitrev preceq.w.phl preceq.w.phr precequ.ph.qbl "
        "precequ.ph.qbla precequ.ph.qbr precequ.ph.qbra preceu.ph.qbl preceu.ph.qbla "
        "preceu.ph.qbr preceu.ph.qbra replv.ph replv.qb",
    ),
    (
        "$2,$4,$5",
        "",
        "addq.ph addq_s.ph addq_s.w addsc addu.qb addu_s.qb addwc cmpgu.eq.qb "
        "cmpgu.lt.qb cmpgu.le.qb modsub muleq_s.w.phl muleq_s.w.phr muleu_s.ph.qbl "
        "muleu_s.ph.qbr mulq_rs.ph packrl.ph pick.ph pick.qb precrq.ph.w "
        "precrq.qb.ph precrqu_s.qb.ph precrq_rs.ph.w subq.ph subq_s.ph subq_s.w "
        "subu.qb subu_s.qb",
    ),
    (".+8", "", "bposge32"),
    ("$4,$5", "", "cmp.eq.ph cmp.lt.ph cmp.le.ph cmpu.eq.qb cmpu.lt.qb cmpu.le.qb"),
    (
        "$ac2,$4,$5",
        "",
        "dpaq_s.w.ph dpaq_sa.l.w dpau.h.qbl dpau.h.qbr dpsq_s.w.ph dpsq_sa.l.w "
        "dpsu.h.qbl dpsu.h.qbr madd maddu maq_s.w.phl maq_sa.w.phl maq_s.w.phr "
        "maq_sa.w.phr msub msubu mulsaq_s.w.ph mult multu",
    ),
    ("$5,$ac2,3", "", "extp extpdp extr.w extr_r.w extr_rs.w extr_s.h"),
    ("$5,$ac2,$4", "", "extpdpv extpv extrv.w extrv_r.w extrv_rs.w extrv_s.h"),
    ("$2,$5($4)", "", "lbux lhx lwx"),
    ("$2,$ac2", "", "mfhi mflo"),
    ("$4,$ac2", "", "mthi mthlip mtlo"),
    ("$2,$4", "", "raddu.w.qb"),
    ("$2,63", "", "rddsp"),
    ("$2,3", "", "repl.ph repl.qb"),
    ("$ac2,-3", "", "shilo"),
    ("$ac2,$4", "", "shilov"),
    (
        "$2,$5,3",
        "",
        "shll.ph shll_s.ph shll.qb shll_s.w shra.ph shra_r.ph shra_r.w shrl.qb",
    ),
    (
        "$2,$5,$4",
        "",
        "shllv.ph shllv_s.ph shllv.qb shllv_s.w shrav.ph shrav_r.ph shrav_r.w shrlv.qb",
    ),
    ("$4,63", "", "wrdsp"),
    ("$5,$4", "", "insv"),
)

DSPR2 = (
    ("$2,$5", "", "absq_s.qb"),
    (
        "$2,$4,$5",
        "",
        "addqh.ph addqh_r.ph addqh.w addqh_r.w addu.ph addu_s.ph adduh.qb adduh_r.qb "
        "cmpgdu.eq.qb cmpgdu.lt.qb cmpgdu.le.qb mul.ph mul_s.ph mulq_rs.w mulq_s.ph "
        "mulq_s.w precr.qb.ph subqh.ph subqh_r.ph subqh.w subqh_r.w subu.ph "
        "subu_s.ph subuh.qb subuh_r.qb",
    ),
    ("$5,$4,3", "", "append balign precr_sra.ph.w precr_sra_r.ph.w prepend"),
    (
        "$ac2,$4,$5",
        "",
        "dpa.w.ph dpaqx_s.w.ph dpaqx_sa.w.ph dpax.w.ph dps.w.ph dpsqx_s.w.ph "
        "dpsqx_sa.w.ph dpsx.w.ph mulsa.w.ph",
    ),
    ("$2,$5,3", "", "shra.qb shra_r.qb shrl.ph"),
    ("$2,$5,$4", "", "shrav.qb shrav_r.qb shrlv.ph"),
)

MSA = (
    (
        "$w2,$w0,$w1",
        "bhwd",
        "add_a adds_a adds_s adds_u addv asub_s asub_u ave_s ave_u aver_s aver_u "
        "bclr binsl binsr bneg bset ceq cle_s cle_u clt_s clt_u div_s div_u ilvev "
        "ilvl ilvod ilvr maddv max_a max_s max_u min_a min_s min_u mod_s mod_u msubv "
        "mulv pckev pckod sll sra srar srl srlr subs_s subs_u subsus_u subsuu_s subv "
        "vshf",
    ),
    ("$w2,$w0,5", "bhwd", "addvi clei_u clti_u maxi_u mini_u subvi"),
    ("$w2,$w0,$w1", "v", "and bmnz bmz bsel nor or xor"),
    ("$w2,$w0,5", "b", "andi bmnzi bmzi bseli nori ori xori"),
    (
        "$w2,$w0,3",
        "bhwd",
        "bclri binsli binsri bnegi bseti sat_s sat_u slli srai srari srli srlri",
    ),
    ("$w1,.+8", "bhwdv", "bnz bz"),
    ("$w2,$w0,-5", "bhwd", "ceqi clei_s clti_s maxi_s mini_s"),
    ("$2,$1", "", "cfcmsa"),
    ("$2,$w0[1]", "bhw", "copy_s"),
    ("$2,$w0[1]", "bh", "copy_u"),
    ("$1,$5", "", "ctcmsa"),
    (
        "$w2,$w0,$w1",
        "hwd",
        "dotp_s dotp_u dpadd_s dpadd_u dpsub_s dpsub_u hadd_s hadd_u hsub_s hsub_u",
    ),
    (
        "$w2,$w0,$w1",
        "wd",
        "fadd fcaf fceq fcle fclt fcne fcor fcueq fcule fcult fcun fcune fdiv fexp2 "
        "fmadd fmax fmax_a fmin fmin_a fmsub fmul fsaf fseq fsle fslt fsne fsor fsub "
        "fsueq fsule fsult fsun fsune",
    ),
    (
        "$w2,$w0",
        "wd",
        "fclass fexupl fexupr ffint_s ffint_u ffql ffqr flog2 frcp frint frsqrt "
        "fsqrt ftint_s ftint_u ftrunc_s ftrunc_u",
    ),
    ("$w2,$w0,$w1", "hw", "fexdo ftq madd_q maddr_q msub_q msubr_q mul_q mulr_q"),
    ("$w2,$5", "bhw", "fill"),
    ("$w2[1],$5", "bhw", "insert"),
    ("$w2[1],$w0[0]", "bhwd", "insve"),
    ("$w2,0($4)", "bhwd", "ld st"),
    ("$w2,-5", "bhwd", "ldi"),
    ("$2,$4,$5,3", "", "lsa"),
    ("$w2,$w0", "v", "move"),
    ("$w2,$w0", "bhwd", "nloc nlzc pcnt"),
    ("$w2,$w0,5", "bhw", "shf"),
    ("$w2,$w0[$5]", "bhwd", "sld splat"),
    ("$w2,$w0[1]", "bhwd", "sldi splati"),
)


def check(args, work, feature, cpu, table, count):
    assembly = []
    for operands, widths, names in table:
        for name in names.split():
            for width in widths or ("",):
                mnemonic = f"{name}.{width}" if width else name
                assembly.append(f"{mnemonic} {operands}")
    if len(set(assembly)) != count or len(assembly) != count:
        raise ValueError(f"expected {count} distinct {feature} forms")
    source, obj, raw = (work / name for name in ("fixture.s", "fixture.o", "raw.bin"))
    # NOP supplies a real slot for branches and equal spacing for all forms.
    source.write_text(
        ".set noreorder\n.text\n" + "".join(f"{insn}\nnop\n" for insn in assembly)
    )
    records = []
    for target, suffix, endian in (("mips", "", "big"), ("mipsel", "el", "little")):
        profile = "mips" + feature + suffix
        subprocess.run(
            [
                args.llvm_mc,
                "-triple=" + target,
                "-mcpu=" + cpu,
                "-mattr=+" + feature,
                "-filetype=obj",
                source,
                "-o",
                obj,
            ],
            check=True,
        )
        subprocess.run(
            [args.llvm_objcopy, "-O", "binary", "--only-section=.text", obj, raw],
            check=True,
        )
        data = raw.read_bytes()
        if len(data) != count * 8:
            raise ValueError(
                f"{profile} assembler expanded or dropped a physical instruction"
            )
        words = [
            int.from_bytes(data[i : i + 4], endian) for i in range(0, len(data), 8)
        ]
        if len(set(words)) != count or any(
            data[i : i + 4] != bytes(4) for i in range(4, len(data), 8)
        ):
            raise ValueError(
                f"{profile} contains duplicate encodings or an unexpected delay slot"
            )
        result = json.loads(
            subprocess.check_output([args.bench, profile, "lift", raw, "1"], text=True)
        )
        if result["instructions_per_repeat"] != 2 * count:
            raise ValueError(f"{profile} lift did not consume the complete input")
        records.append(
            dict(
                profile=profile,
                documented_forms=count,
                physical_instructions=result["instructions_per_repeat"],
                operations=result["operations_per_repeat"],
                digest=result["digest"],
                sha256=hashlib.sha256(data).hexdigest(),
            )
        )
    if records[0]["digest"] != records[1]["digest"]:
        raise ValueError(
            f"{feature} physical decode and LowIR fingerprints differ by byte order"
        )
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--llvm-mc", default="llvm-mc")
    parser.add_argument("--llvm-objcopy", default="llvm-objcopy")
    args = parser.parse_args()
    records = []
    with tempfile.TemporaryDirectory(prefix="neverd-mips-ase-") as directory:
        for feature, cpu, table, count in (
            ("dsp", "mips32r2", DSP, 113),
            ("dspr2", "mips32r2", DSPR2, 46),
            ("msa", "mips32r5", MSA, 526),
        ):
            records.extend(check(args, Path(directory), feature, cpu, table, count))
    print(json.dumps(records, indent=2))


if __name__ == "__main__":
    main()
