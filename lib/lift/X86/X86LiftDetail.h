//===- X86LiftDetail.h - x86/x64 lifter internals ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared by the x86/x64 instruction-lifter translation
/// units: the per-instruction-family handlers that the X86Lifter::lift*
/// members chain through, plus the few helpers used by more than one of them.
///
/// Each handler has exactly the shape of the switch it was carved out of: it
/// returns true when it recognized (and lifted) the instruction and false when
/// the instruction is not one of its cases, so the dispatchers can chain them
/// with `||` and keep the original dispatch order.
///
/// Handlers that need X86Lifter's private cross-instruction state (the
/// CQO/CDQ + DIV tracking, the get-PC thunk state, the x87 stack top, the
/// target architecture) could not be carved out and stay in the member
/// function next to the chain.
///
/// This header is an implementation detail of lib/lift/X86/ and must NOT be
/// included by code outside that directory.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86_X86LIFTDETAIL_H
#define NEVERD_LIFT_X86_X86LIFTDETAIL_H

#include "neverd/lift/X86Lifter.h"

#include <capstone/capstone.h>

namespace neverd {

//===----------------------------------------------------------------------===//
// Helpers shared by more than one handler translation unit
//===----------------------------------------------------------------------===//

/// Element size (bytes) of a MOVS/STOS/LODS/SCAS/CMPS variant.
/// Defined in X86LiftString.cpp.
unsigned stringElemSize(unsigned InsnId);

/// Address space selected for an implicit x86 string source.  Capstone exposes
/// the source as a memory operand for MOVS/LODS/CMPS, but XLAT has no explicit
/// operands, so the legacy-prefix bytes are the authoritative fallback.
NdMemoryAddressSpace stringSourceAddressSpace(const cs_x86 &X86);

/// Snapshot the incoming carry flag widened to \p Size bytes.  ADC/SBB and
/// RCL/RCR still consume the incoming carry while computing the flags they
/// have already overwritten, so they take this copy first.  Defined in
/// X86LiftCore.cpp.
NdVar snapshotCarryAtWidth(X86Lifter::LiftState &S, uint16_t Size);

//===----------------------------------------------------------------------===//
// X86Lifter::liftCore handlers
//===----------------------------------------------------------------------===//

/// NOP, the MOV family, scalar integer/float conversion, LEA and XCHG.
bool liftCoreMove(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                  const cs_x86 &X86);

/// Integer arithmetic, comparison, bitwise logic and multiply.
bool liftCoreArith(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86);

/// Shifts, rotates, double-precision shifts and rotate-through-carry.
bool liftCoreShift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86);

/// Bit scan, byte swap and the EFLAGS transfer instructions.
bool liftCoreBit(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                 const cs_x86 &X86);

//===----------------------------------------------------------------------===//
// X86Lifter::liftString handlers
//===----------------------------------------------------------------------===//

/// MOVS/STOS/LODS (with and without REP) and XLATB.
bool liftStringMove(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86);

/// Traps, CPUID/timestamp, serializing fences, cache hints and system calls.
bool liftSystem(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                const cs_x86 &X86);

//===----------------------------------------------------------------------===//
// X86Lifter::liftExt handlers
//===----------------------------------------------------------------------===//

/// BMI1/BMI2/ADX bit-manipulation extensions.
bool liftExtBMI(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                const cs_x86 &X86);

//===----------------------------------------------------------------------===//
// X86Lifter::liftSIMD handlers
//===----------------------------------------------------------------------===//

/// Vector data movement, lane extract/insert, broadcast and vector state.
bool liftSIMDMove(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                  const cs_x86 &X86);

/// Shuffle, permute, blend and masked vector load/store.
bool liftSIMDShuffle(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86);

/// Packed and scalar comparison, PTEST and the packed string compares.
bool liftSIMDCompare(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86);

/// Packed integer add/subtract, average and minimum/maximum.
bool liftSIMDIntArith(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86);

/// Packed integer multiply and multiply-accumulate.
bool liftSIMDIntMul(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86);

/// Horizontal add/subtract, sum of absolute differences, absolute value and
/// sign application.
bool liftSIMDIntHorizontal(X86Lifter &L, X86Lifter::LiftState &S,
                           const cs_insn *Insn, const cs_x86 &X86);

/// Packed and scalar floating-point arithmetic, minimum/maximum, square root
/// and reciprocal approximation.
bool liftSIMDFloatArith(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86);

/// Bulk bitwise logic and packed shifts.
bool liftSIMDLogicShift(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86);

/// Pack, unpack, lane widening and integer/float conversion.
bool liftSIMDConvert(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86);

/// AES/SHA/PCLMULQDQ/CRC32 plus the TSX and MPX extensions.
bool liftSIMDCrypto(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86);

//===----------------------------------------------------------------------===//
// X86Lifter::liftSIMDAVX handlers
//===----------------------------------------------------------------------===//

/// Fused multiply-add (FMA3 and FMA4).
bool liftSIMDAVXFMA(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86);

/// SSE3/SSSE3/SSE4 instructions reached through the AVX dispatcher.
bool liftSIMDAVXSSE(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86);

/// AVX-512 opmask (k0-k7) register operations.
bool liftSIMDAVXMask(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86);

/// AVX-512 packed integer (EVEX VP*) instructions.
bool liftSIMDAVXInt(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86);

/// VEX/EVEX integer/float and float-width conversions.
bool liftSIMDAVXConvert(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86);

/// The remaining VEX/EVEX float and lane-move instructions.
bool liftSIMDAVXFloat(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86);

//===----------------------------------------------------------------------===//
// X86Lifter::liftSIMDLegacy handlers
//===----------------------------------------------------------------------===//

/// BCD adjust, BOUND, INTO, SALC and the far-pointer loads.
bool liftLegacyBCD(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86);

/// 3DNow!, TBM, CET, VIA PadLock, GFNI, LWP and the other minor extensions.
bool liftLegacyExt(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86);

/// AMD XOP plus the AVX-512 instructions that share this dispatcher.
bool liftLegacyXOP(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86);

/// Baseline SSE integer logic, add/subtract, shift and shuffle.
bool liftLegacySSEInt(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86);

/// Baseline SSE compare, float arithmetic, conversion and MXCSR.
bool liftLegacySSEFloat(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86);

} // namespace neverd

#endif // NEVERD_LIFT_X86_X86LIFTDETAIL_H
