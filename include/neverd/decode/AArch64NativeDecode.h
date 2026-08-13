//===- AArch64NativeDecode.h - Native fixed-width operand decode -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Capstone-free operand-level decode of a single 32-bit AArch64 instruction
/// word into a `cs_insn` the existing AArch64Lifter can consume unchanged.
///
/// Capstone's decode DFA is the throughput ceiling of the front end: on this
/// machine it sustains only ~1.6M insn/s single-threaded and — being
/// table-driven with a large working set — it does not scale across cores
/// (aggregate throughput peaks at ~4 threads and then regresses), while a
/// fixed-width native scan over the very same bytes runs at ~150M insn/s and
/// scales ~9x to 12 threads.  AArch64 is fixed 4-byte width and 4-byte
/// aligned, so the common instruction classes can be decoded with a handful of
/// mask+shift extractions and the operands written straight into a `cs_insn`,
/// bypassing the DFA entirely.
///
/// \b Correctness \b contract.  This decoder never has to be a full AArch64
/// disassembler.  It handles a fixed, growing set of high-frequency classes
/// and \e declines everything else (returns false), so the caller falls back
/// to Capstone.  Two invariants make the fast path safe, and both are locked
/// by a differential test (AArch64_NativeDecodeParityTests) that uses Capstone
/// as the oracle over exhaustive field sweeps, millions of random words, and
/// whole real binaries:
///
///   1. \b Strict \b subset.  For every word `tryDecode` accepts, Capstone
///      also accepts it (every reserved-bit constraint of the class is
///      checked here), so routing a word through the fast path never turns a
///      Capstone \e reject into an accept — the property function-entry
///      verification relies on when walking untrusted bytes.
///   2. \b Lift \b parity.  Feeding the `cs_insn` produced here to
///      AArch64Lifter yields byte-identical LowIR to feeding the `cs_insn`
///      Capstone would have produced for the same word.  The synthesized form
///      is chosen to lift identically (e.g. the canonical MOVZ form lifts the
///      same as Capstone's `mov` alias), so exact reproduction of Capstone's
///      mnemonic text / alias flags is unnecessary — only the operand detail
///      the lifter reads must match.
///
/// The produced `cs_insn` intentionally leaves `mnemonic`/`op_str` empty: the
/// lift and CFG paths read operands and the instruction id, never the rendered
/// text (the SDK disassembly path, which does read the text, keeps using
/// Capstone).  `id` is set to the canonical instruction id for the class.
///
/// This is an umbrella header: it is the include every caller uses, and the
/// decoder itself is split across the headers below.  AArch64DecodeDispatch.h
/// holds tryDecode and the ordered table of encoding-class masks; the
/// AArch64Decode*.h class headers hold the per-class operand decode; and
/// AArch64DecodeOperands.h / AArch64DecodeImm.h hold the register-mapping,
/// operand-population and immediate-expansion helpers they share.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64NATIVEDECODE_H
#define NEVERD_DECODE_AARCH64NATIVEDECODE_H

#include "neverd/decode/AArch64DecodeArith.h"
#include "neverd/decode/AArch64DecodeBranch.h"
#include "neverd/decode/AArch64DecodeCond.h"
#include "neverd/decode/AArch64DecodeDispatch.h"
#include "neverd/decode/AArch64DecodeFP.h"
#include "neverd/decode/AArch64DecodeImm.h"
#include "neverd/decode/AArch64DecodeLoadStore.h"
#include "neverd/decode/AArch64DecodeLogical.h"
#include "neverd/decode/AArch64DecodeMove.h"
#include "neverd/decode/AArch64DecodeMulDiv.h"
#include "neverd/decode/AArch64DecodeOperands.h"

#endif // NEVERD_DECODE_AARCH64NATIVEDECODE_H
