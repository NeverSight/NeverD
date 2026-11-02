//===- X86FPPackedConvertState.h - Packed conversion lifter -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticated SSE/VEX packed conversion, source access and MXCSR completion.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_LIFT_X86_X86FPPACKEDCONVERTSTATE_H
#define NEVERD_LIB_LIFT_X86_X86FPPACKEDCONVERTSTATE_H

#include "X86FPEncoding.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86/X86FPState.h"
#include "neverd/lift/X86/X86Lifter.h"

#include <array>
#include <optional>

namespace neverd {
inline std::optional<unsigned> x86PackedConvertKind(unsigned Id, bool Vex) {
#define PACKED_CONVERT(NAME, KIND)                                             \
  case X86_INS_##NAME:                                                         \
    return Vex ? std::nullopt : std::optional<unsigned>(KIND);                 \
  case X86_INS_V##NAME:                                                        \
    return Vex ? std::optional<unsigned>(KIND) : std::nullopt;
  switch (Id) {
    PACKED_CONVERT(CVTDQ2PS, 0)
    PACKED_CONVERT(CVTDQ2PD, 1)
    PACKED_CONVERT(CVTPS2PD, 2)
    PACKED_CONVERT(CVTPD2PS, 3)
    PACKED_CONVERT(CVTPS2DQ, 4)
    PACKED_CONVERT(CVTPD2DQ, 5)
    PACKED_CONVERT(CVTTPS2DQ, 6)
    PACKED_CONVERT(CVTTPD2DQ, 7)
  default:
    return std::nullopt;
  }
#undef PACKED_CONVERT
}
inline bool liftPackedConvertState(X86Lifter &L, X86Lifter::LiftState &S,
                                   const cs_insn *Insn, const cs_x86 &X86,
                                   unsigned Kind, bool Vex) {
  const auto Refuse = [&]() -> bool {
    throw UnliftedInstruction(Insn ? Insn->address : InvalidVA,
                              Insn ? Insn->mnemonic : nullptr,
                              Insn ? Insn->op_str : nullptr);
  };
  if (!Insn || !Insn->size || Insn->size > 15 || Kind > 7 ||
      (L.targetArch() != Arch::X86 && L.targetArch() != Arch::X64) ||
      X86.op_count != 2 || X86.operands[0].type != X86_OP_REG ||
      X86.encoding.imm_offset || X86.encoding.imm_size || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || X86.operands[0].avx_zero_opmask ||
      X86.operands[1].avx_zero_opmask ||
      X86.operands[0].avx_bcast != X86_AVX_BCAST_INVALID ||
      X86.operands[1].avx_bcast != X86_AVX_BCAST_INVALID)
    return Refuse();
  constexpr std::array<uint8_t, 8> Opcodes{0x5b, 0xe6, 0x5a, 0x5a,
                                           0x5b, 0xe6, 0x5b, 0xe6};
  constexpr std::array<uint8_t, 8> Prefixes{0, 2, 0, 1, 1, 3, 2, 1};
  FPEncodingInfo E;
  if (!parseFPEncoding(Insn, X86, L.targetArch(), Vex, 1, Prefixes[Kind], E) ||
      E.Opcode != Opcodes[Kind] || !E.unusedVvvv())
    return Refuse();
  const unsigned Control = Kind | (Vex ? 8 : 0) | (E.L ? 16 : 0);
  const unsigned SourceBytes = x86FPPackedConvertSourceBytes(Control);
  const unsigned DestinationBytes = x86FPPackedConvertDestinationBytes(Control);
  if (X86.operands[0].size != DestinationBytes ||
      X86.operands[0].reg !=
          (DestinationBytes == 32 ? X86_REG_YMM0 : X86_REG_XMM0) +
              E.vectorRegIndex())
    return Refuse();
  const auto &Operand = X86.operands[1];
  if ((E.ModRM >> 6) == 3) {
    const unsigned RegisterBytes = SourceBytes == 32 ? 32 : 16;
    if (Operand.type != X86_OP_REG || Operand.size != RegisterBytes ||
        Operand.reg != (RegisterBytes == 32 ? X86_REG_YMM0 : X86_REG_XMM0) +
                           E.vectorRMIndex())
      return Refuse();
  } else if (Operand.type != X86_OP_MEM || Operand.size != SourceBytes)
    return Refuse();
  if (!validateFPEncodingTail(Insn, X86, E, Operand))
    return Refuse();
  const bool Memory = Operand.type == X86_OP_MEM;
  auto Source = Memory ? S.computeEA(Operand) : L.operandRead(S, Operand);
  if (!Memory && Source.Size != SourceBytes) {
    auto Narrow = S.makeTemp(SourceBytes);
    S.emit(NdOp::SUBBYTES, Narrow, {Source, NdVar::cst(0, 4)});
    Source = Narrow;
  }
  auto Incoming = S.makeTemp(4), Completed = S.makeTemp(DestinationBytes + 4);
  S.emitIntrinsic(Intrinsic::X86ReadMXCSR, Incoming);
  S.emitIntrinsic(Memory ? Intrinsic::X86FPPackedConvertMemoryState
                         : Intrinsic::X86FPPackedConvertState,
                  Completed, {Source, NdVar::cst(Control, 1), Incoming},
                  NdMemoryOrdering::None,
                  Memory ? S.memoryAddressSpace(Operand)
                         : NdMemoryAddressSpace::Default);
  auto Number = S.makeTemp(DestinationBytes), Outgoing = S.makeTemp(4);
  S.emit(NdOp::SUBBYTES, Number, {Completed, NdVar::cst(0, 4)});
  S.emit(NdOp::SUBBYTES, Outgoing,
         {Completed, NdVar::cst(DestinationBytes, 4)});
  S.emitVoidIntrinsic(Intrinsic::X86WriteMXCSR, {Outgoing});
  S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Number});
  return true;
}
} // namespace neverd
#endif
