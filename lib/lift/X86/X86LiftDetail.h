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
#include <cstddef>

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

/// True when \p Operand is an AVX-512 opmask register operand.
bool isX86OpmaskOperand(const cs_x86_op &Operand);

/// Reject EVEX modifiers whose value or memory semantics require dedicated
/// handling. Ordinary vector writemasks are intentionally not rejected here.
bool hasUnsupportedEvexValueModifier(const cs_x86 &X86);

/// Decode the EVEX ModRM.reg vector-register number. EVEX P0 bit 7 extends
/// register bit 3 and P0 bit 4 extends register bit 4; both are inverted.
unsigned decodeEvexVectorRegIndex(uint8_t P0, uint8_t ModRM);

/// Decode the EVEX register-form ModRM.rm vector-register number. P0 bits 5
/// and 6 provide the inverted bit-3 and bit-4 extensions respectively.
unsigned decodeEvexVectorRMIndex(uint8_t P0, uint8_t ModRM);

/// Decode the inverted EVEX vvvv/V' vector-register number.
unsigned decodeEvexVectorVvvvIndex(uint8_t P1, uint8_t P2);

/// Expand one compact bit per vector lane into the sign-bit mask consumed by
/// the x86 masked-load/store intrinsics.  The compact value may be a K-register
/// view or an all-active constant; callers retain ownership of writemask
/// merge/zero policy.
NdVar expandCompactLaneMask(X86Lifter::LiftState &S, NdVar CompactMask,
                            uint16_t VectorSize, uint16_t ElementSize);

/// Select the exact lane-granular masked-load intrinsic for an x86 element
/// width. Returns Intrinsic::None for unsupported widths.
Intrinsic maskedVectorLoadIntrinsic(uint16_t ElementSize);

/// Canonical raw EVEX prefix and addressing metadata.  Parsing this structure
/// is validation-only: callers must still check their family-specific map,
/// opcode, W/PP, vector length, mask, broadcast, and immediate contracts.
struct CanonicalEvexEncodingInfo {
  size_t Offset = 0;
  uint8_t SegmentPrefix = 0;
  bool AddressOverride = false;
  bool Is64Bit = false;
  uint16_t AddressSize = 0;
  uint8_t P0 = 0;
  uint8_t P1 = 0;
  uint8_t P2 = 0;
  uint8_t Opcode = 0;
  uint8_t ModRM = 0;
};

/// Canonical three-byte VEX prefix and addressing metadata. FMA4 uses this
/// encoding exclusively and owns one trailing is4 byte after the ordinary
/// ModRM/SIB/displacement tail.
struct CanonicalVex3EncodingInfo {
  size_t Offset = 0;
  uint8_t SegmentPrefix = 0;
  bool AddressOverride = false;
  bool Is64Bit = false;
  uint16_t AddressSize = 0;
  uint8_t P0 = 0;
  uint8_t P1 = 0;
  uint8_t Opcode = 0;
  uint8_t ModRM = 0;
};

/// Parse the only legacy prefixes accepted before an EVEX instruction and
/// prove that Capstone's prefix/opcode/ModRM/address-size detail matches the
/// raw bytes exactly.  Duplicate and unrelated prefixes fail closed.
bool parseCanonicalEvexEncodingInfo(const cs_insn *Insn, const cs_x86 &X86,
                                    Arch TargetArch,
                                    CanonicalEvexEncodingInfo &Encoding);

/// Three-byte VEX counterpart of parseCanonicalEvexEncodingInfo. Only segment
/// and address-size prefixes may precede C4; family-specific map, W/L/pp,
/// opcode, register and immediate rules remain the caller's responsibility.
bool parseCanonicalVex3EncodingInfo(const cs_insn *Insn, const cs_x86 &X86,
                                    Arch TargetArch,
                                    CanonicalVex3EncodingInfo &Encoding);

/// Prove that an EVEX memory tail and Capstone's structured address detail are
/// identical. TupleScale applies the architectural disp8*N compression;
/// TrailingBytes reserves family-owned immediate bytes at the end.
bool validateCanonicalEvexMemoryTail(const cs_insn *Insn, const cs_x86 &X86,
                                     const CanonicalEvexEncodingInfo &Encoding,
                                     const cs_x86_op &Operand,
                                     uint16_t TupleScale,
                                     size_t TrailingBytes = 0);

/// Register-form counterpart of validateCanonicalEvexMemoryTail.
bool validateCanonicalEvexRegisterTail(
    const cs_insn *Insn, const cs_x86 &X86,
    const CanonicalEvexEncodingInfo &Encoding, size_t TrailingBytes = 0);

/// Validate an ordinary (uncompressed) VEX3 memory tail against Capstone's
/// structured address detail. TrailingBytes reserves family-owned bytes such
/// as FMA4's is4 register selector.
bool validateCanonicalVex3MemoryTail(
    const cs_insn *Insn, const cs_x86 &X86,
    const CanonicalVex3EncodingInfo &Encoding, const cs_x86_op &Operand,
    size_t TrailingBytes = 0);

/// Register-form counterpart of validateCanonicalVex3MemoryTail.
bool validateCanonicalVex3RegisterTail(
    const cs_insn *Insn, const cs_x86 &X86,
    const CanonicalVex3EncodingInfo &Encoding, size_t TrailingBytes = 0);

/// Load an EVEX vector memory source under a compact K-register mask.  Full
/// tuples fault-suppress each inactive lane.  Broadcast tuples perform at most
/// one scalar load when any destination lane is active and then replicate it.
/// A scalar tuple can be represented by passing a mask whose only possible set
/// bit is bit zero, ResultSize == 16 and Broadcast == false.
NdVar emitEvexMaskedMemoryLoad(X86Lifter::LiftState &S,
                               const cs_x86_op &MemoryOperand,
                               NdVar CompactMask, uint16_t ResultSize,
                               uint16_t ElementSize, uint16_t MemoryTupleSize,
                               bool Broadcast);

/// Apply an ordinary EVEX vector writemask to a fully computed register
/// result. K destinations, masked memory and topology-changing operations need
/// dedicated implementations instead.
bool emitMaskedVectorResult(X86Lifter &L, X86Lifter::LiftState &S,
                            const cs_x86_op &DestinationOperand,
                            const cs_x86_op &MaskOperand, NdVar RawResult,
                            uint16_t ElementSize);

/// Lift a register-only EVEX compress/expand operation.  The helper owns the
/// topology-changing writemask semantics; masked memory forms require a
/// separate fault-suppressing implementation and are rejected by its caller.
bool liftEvexCompressExpandRegister(X86Lifter &L, X86Lifter::LiftState &S,
                                    const cs_insn *Insn, const cs_x86 &X86,
                                    uint16_t ElementSize, bool Compress,
                                    uint8_t Opcode, bool W);

/// Interleave the selected half of two packed inputs independently within each
/// architectural 64-bit MMX or 128-bit vector lane.
bool emitPackedUnpack(X86Lifter::LiftState &S, NdVar Destination, NdVar Left,
                      NdVar Right, uint16_t ElementSize, bool HighHalf);

/// Translate an unpack destination writemask into the expanded source-element
/// mask needed to fault-suppress a full-tuple memory input. Only the odd
/// interleaved result elements consume the right-hand source.
NdVar emitPackedUnpackMemoryMask(X86Lifter::LiftState &S,
                                 const cs_x86_op *MaskOperand,
                                 uint16_t VectorSize, uint16_t ElementSize,
                                 bool HighHalf);

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

/// AMX tile matrix compute operations.
bool liftAMX(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
             const cs_x86 &X86);

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

/// EVEX packed integer narrowing with register destinations.
bool liftSIMDAVXNarrow(X86Lifter &L, X86Lifter::LiftState &S,
                       const cs_insn *Insn, const cs_x86 &X86);

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
