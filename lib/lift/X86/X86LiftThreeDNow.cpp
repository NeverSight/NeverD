//===- X86LiftThreeDNow.cpp - Owned packed MMX lifting ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep the original packed instruction and memory access inside the owned
/// x87 context. Scalar arithmetic cannot represent this instruction family.
//===----------------------------------------------------------------------===//

#include "X86LiftThreeDNow.h"

#include "X86LiftAPXValidation.h"
#include "X86LiftDetail.h"

#include "neverd/ir/X86/X87Context.h"
#include "neverd/support/ISAEncoding.h"

namespace neverd {
bool liftThreeDNow(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X) {
  if (!L.ownsX87Context() || !Insn || !Insn->detail || Insn->size < 4 ||
      Insn->size > x86::kMaxInstructionLen || X.op_count != 2 || X.eflags ||
      X.rex2 || X.encoding.imm_offset || X.encoding.imm_size || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID)
    return false;
  unsigned Suffix;
  switch (Insn->id) {
#define NEVERD_3DNOW(Name, Opcode, Reads, Enhanced)                            \
  case X86_INS_##Name:                                                         \
    Suffix = Opcode;                                                           \
    break;
#include "neverd/ir/X86/X86ThreeDNow.def"
#undef NEVERD_3DNOW
  default:
    return false;
  }
  size_t Offset = 0;
  uint8_t Rex = 0, Segment = 0;
  bool Operand16 = false, Address32 = false;
  while (Offset < Insn->size) {
    const uint8_t Byte = Insn->bytes[Offset];
    if ((Byte & 0xf0) == 0x40) {
      Rex = Byte;
    } else if (apxvalidation::isSegmentPrefix(Byte)) {
      Segment = Byte;
      Rex = 0;
    } else if (Byte == 0x66 || Byte == 0x67 || Byte == 0xf2 || Byte == 0xf3) {
      // AMD defines operand-size and repeat prefixes as ignored. A later
      // legacy prefix cancels REX; the last adjacent REX supplies addresses.
      Operand16 |= Byte == 0x66;
      Address32 |= Byte == 0x67;
      Rex = 0;
    } else {
      break;
    }
    ++Offset;
  }
  const unsigned AddressSize = Address32 ? 4 : 8;
  if (Offset + 4 > Insn->size || Insn->bytes[Offset] != 0x0f ||
      Insn->bytes[Offset + 1] != 0x0f ||
      Insn->bytes[Insn->size - 1] != Suffix || X.opcode[0] != 0x0f ||
      X.opcode[1] != Suffix || X.opcode[2] || X.opcode[3] ||
      X.modrm != Insn->bytes[Offset + 2] ||
      X.encoding.modrm_offset != Offset + 2 || X.prefix[0] ||
      X.prefix[1] != Segment || X.prefix[2] != (Operand16 ? 0x66 : 0) ||
      X.prefix[3] != (Address32 ? 0x67 : 0) || X.rex != Rex ||
      X.addr_size != AddressSize || S.AddressSize != AddressSize)
    return false;
  const auto Register = [](const cs_x86_op &Op, unsigned Index) {
    return Op.type == X86_OP_REG && Op.size == 8 &&
           Op.reg == X86_REG_MM0 + Index && !Op.avx_zero_opmask &&
           Op.avx_bcast == X86_AVX_BCAST_INVALID;
  };
  if (!Register(X.operands[0], (X.modrm >> 3) & 7))
    return false;
  const auto &Source = X.operands[1];
  const bool Memory = (X.modrm & 0xc0) != 0xc0;
  if (Memory ? Source.type != X86_OP_MEM || Source.size != 8 ||
                   Source.avx_zero_opmask ||
                   Source.avx_bcast != X86_AVX_BCAST_INVALID
             : !Register(Source, X.modrm & 7))
    return false;
  if (!validateCanonicalScalarConversionTail(Insn, X, Offset + 3, Segment, true,
                                             AddressSize, (Rex & 1) ? 8 : 0,
                                             (Rex & 2) ? 8 : 0, Source, 1))
    return false;

  uint32_t Selector = X87ContextThreeDNow | 0x0f |
                      (Suffix << X87ContextThreeDNowOpcodeShift) |
                      (uint32_t(Memory ? X.modrm & 0x38 : X.modrm) << 8);
  auto Space = NdMemoryAddressSpace::Default;
  if (Memory) {
    Selector |= X87ContextMemory;
    Space = S.memoryAddressSpace(Source);
    if (AddressSize == 8 && Space == NdMemoryAddressSpace::Default &&
        isStackDefaultBase(Source.mem.base))
      Selector |= X87ContextStackReference;
  }
  // All byte/detail and ownership checks precede EA temporaries or state.
  const NdVar Address = Memory ? S.computeEA(Source) : NdVar::cst(0, 8);
  S.emitIntrinsic(Intrinsic::X87ContextStep, {},
                  {NdVar::reg(x86reg::X87ContextHandle, 8),
                   NdVar::cst(Selector, 4), Address, NdVar::cst(0, 8),
                   NdVar::cst(S.Addr, 8)},
                  NdMemoryOrdering::None, Space);
  return true;
}
} // namespace neverd
