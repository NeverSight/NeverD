//===- X86NativeStateAudit.h - Native state-effect audit -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Undefined-output admission for bounded string and control-state owners.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86NATIVESTATEAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86NATIVESTATEAUDIT_H

#include "X86BitTestUndefined.h"
#include "X86DescriptorQueryAudit.h"
#include "X86LegacyMemoryAudit.h"
#include "X86LockedLogicAudit.h"
#include "X86LockedUnary.h"
#include "X86XaddAudit.h"

namespace neverd::nativestateaudit {

inline bool loadFenceForm(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      Insn->id != X86_INS_LFENCE || Insn->size != 3 || Insn->bytes[0] != 0x0f ||
      Insn->bytes[1] != 0xae || Insn->bytes[2] != 0xe8)
    return false;
  const auto &X = Insn->detail->x86;
  return !X.prefix[0] && !X.prefix[1] && !X.prefix[2] && !X.prefix[3] &&
         X.opcode[0] == 0x0f && X.opcode[1] == 0xae && !X.opcode[2] &&
         !X.opcode[3] && !X.rex && X.addr_size == 8 && X.modrm == 0xe8 &&
         !X.sib && !X.disp && X.encoding.modrm_offset == 2 &&
         !X.encoding.disp_offset && !X.encoding.disp_size &&
         !X.encoding.imm_offset && !X.encoding.imm_size && !X.op_count &&
         !X.avx_sae && X.avx_rm == X86_AVX_RM_INVALID &&
         X.avx_cc == X86_AVX_CC_INVALID && !X.eflags;
}

inline bool cpuQueryForm(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      (Insn->id != X86_INS_CPUID && Insn->id != X86_INS_RDTSC &&
       Insn->id != X86_INS_RDTSCP))
    return false;
  const bool Auxiliary = Insn->id == X86_INS_RDTSCP;
  const uint8_t Opcode = Auxiliary                   ? 0x01
                         : Insn->id == X86_INS_CPUID ? 0xa2
                                                     : 0x31;
  if (Insn->size != (Auxiliary ? 3 : 2) || Insn->bytes[0] != 0x0f ||
      Insn->bytes[1] != Opcode || (Auxiliary && Insn->bytes[2] != 0xf9))
    return false;
  const auto &X = Insn->detail->x86;
  return !X.prefix[0] && !X.prefix[1] && !X.prefix[2] && !X.prefix[3] &&
         X.opcode[0] == 0x0f && X.opcode[1] == Opcode && !X.opcode[2] &&
         !X.opcode[3] && !X.rex && X.addr_size == 8 &&
         X.modrm == (Auxiliary ? 0xf9 : 0) && !X.sib && !X.disp &&
         X.encoding.modrm_offset == (Auxiliary ? 2 : 0) &&
         !X.encoding.disp_offset && !X.encoding.disp_size &&
         !X.encoding.imm_offset && !X.encoding.imm_size && !X.op_count &&
         !X.avx_sae && X.avx_rm == X86_AVX_RM_INVALID &&
         X.avx_cc == X86_AVX_CC_INVALID && !X.eflags;
}

inline bool legacyMsrForm(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      (Insn->id != X86_INS_RDMSR && Insn->id != X86_INS_WRMSR))
    return false;
  const uint8_t Opcode = Insn->id == X86_INS_RDMSR ? 0x32 : 0x30;
  if (Insn->size != 2 || Insn->bytes[0] != 0x0f || Insn->bytes[1] != Opcode)
    return false;
  const auto &X = Insn->detail->x86;
  return !X.prefix[0] && !X.prefix[1] && !X.prefix[2] && !X.prefix[3] &&
         X.opcode[0] == 0x0f && X.opcode[1] == Opcode && !X.opcode[2] &&
         !X.opcode[3] && !X.rex && X.addr_size == 8 && !X.modrm && !X.sib &&
         !X.disp && !X.encoding.modrm_offset && !X.encoding.disp_offset &&
         !X.encoding.disp_size && !X.encoding.imm_offset &&
         !X.encoding.imm_size && !X.op_count && !X.avx_sae &&
         X.avx_rm == X86_AVX_RM_INVALID && X.avx_cc == X86_AVX_CC_INVALID &&
         !X.eflags;
}

inline bool sysretqForm(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      Insn->id != X86_INS_SYSRETQ || Insn->size != 3 ||
      Insn->bytes[0] != 0x48 || Insn->bytes[1] != 0x0f ||
      Insn->bytes[2] != 0x07)
    return false;
  const auto &X = Insn->detail->x86;
  return !X.prefix[0] && !X.prefix[1] && !X.prefix[2] && !X.prefix[3] &&
         X.opcode[0] == 0x0f && X.opcode[1] == 0x07 && !X.opcode[2] &&
         !X.opcode[3] && X.rex == 0x48 && X.addr_size == 8 && !X.modrm &&
         !X.sib && !X.disp && !X.encoding.modrm_offset &&
         !X.encoding.disp_offset && !X.encoding.disp_size &&
         !X.encoding.imm_offset && !X.encoding.imm_size && !X.op_count &&
         !X.avx_sae && X.avx_rm == X86_AVX_RM_INVALID &&
         X.avx_cc == X86_AVX_CC_INVALID &&
         X.eflags == (X86_EFLAGS_MODIFY_AF | X86_EFLAGS_MODIFY_CF |
                      X86_EFLAGS_MODIFY_SF | X86_EFLAGS_MODIFY_ZF |
                      X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_OF |
                      X86_EFLAGS_MODIFY_TF | X86_EFLAGS_MODIFY_IF |
                      X86_EFLAGS_MODIFY_DF | X86_EFLAGS_MODIFY_NT |
                      X86_EFLAGS_MODIFY_RF);
}

inline bool longModeEventForm(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      (Insn->id != X86_INS_IRETQ && Insn->id != X86_INS_INT3 &&
       Insn->id != X86_INS_INT))
    return false;
  const bool Return = Insn->id == X86_INS_IRETQ;
  const bool Immediate = Insn->id == X86_INS_INT;
  const uint8_t Opcode = Return ? 0xcf : Immediate ? 0xcd : 0xcc;
  if (Insn->size != (Return || Immediate ? 2 : 1) ||
      (Return && Insn->bytes[0] != 0x48) ||
      Insn->bytes[Return ? 1 : 0] != Opcode)
    return false;
  const auto &X = Insn->detail->x86;
  if (X.op_count != (Immediate ? 1 : 0) ||
      X.encoding.imm_offset != (Immediate ? 1 : 0) ||
      X.encoding.imm_size != (Immediate ? 1 : 0))
    return false;
  if (Immediate) {
    const auto &Operand = X.operands[0];
    if (Operand.type != X86_OP_IMM || Operand.size != 1 || Operand.access ||
        Operand.imm != Insn->bytes[1] || Operand.avx_zero_opmask ||
        Operand.avx_bcast != X86_AVX_BCAST_INVALID)
      return false;
  }
  const uint64_t ReturnFlags =
      X86_EFLAGS_MODIFY_AF | X86_EFLAGS_MODIFY_CF | X86_EFLAGS_MODIFY_SF |
      X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_OF |
      X86_EFLAGS_MODIFY_TF | X86_EFLAGS_MODIFY_IF | X86_EFLAGS_MODIFY_DF |
      X86_EFLAGS_MODIFY_NT | X86_EFLAGS_MODIFY_RF;
  const uint64_t TrapFlags = X86_EFLAGS_MODIFY_IF | X86_EFLAGS_RESET_TF |
                             X86_EFLAGS_MODIFY_NT | X86_EFLAGS_RESET_RF;
  return !X.prefix[0] && !X.prefix[1] && !X.prefix[2] && !X.prefix[3] &&
         X.opcode[0] == Opcode && !X.opcode[1] && !X.opcode[2] &&
         !X.opcode[3] && X.rex == (Return ? 0x48 : 0) && X.addr_size == 8 &&
         !X.modrm && !X.sib && !X.disp && !X.encoding.modrm_offset &&
         !X.encoding.disp_offset && !X.encoding.disp_size && !X.avx_sae &&
         X.avx_rm == X86_AVX_RM_INVALID && X.avx_cc == X86_AVX_CC_INVALID &&
         X.eflags == (Return ? ReturnFlags : TrapFlags);
}

inline bool swapgsForm(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      Insn->id != X86_INS_SWAPGS || Insn->size != 3 || Insn->bytes[0] != 0x0f ||
      Insn->bytes[1] != 0x01 || Insn->bytes[2] != 0xf8)
    return false;
  const auto &X = Insn->detail->x86;
  return !X.prefix[0] && !X.prefix[1] && !X.prefix[2] && !X.prefix[3] &&
         X.opcode[0] == 0x0f && X.opcode[1] == 0x01 && !X.opcode[2] &&
         !X.opcode[3] && !X.rex && X.addr_size == 8 && X.modrm == 0xf8 &&
         !X.sib && !X.disp && X.encoding.modrm_offset == 2 &&
         !X.encoding.disp_offset && !X.encoding.disp_size &&
         !X.encoding.imm_offset && !X.encoding.imm_size && !X.op_count &&
         !X.avx_sae && X.avx_rm == X86_AVX_RM_INVALID &&
         X.avx_cc == X86_AVX_CC_INVALID && !X.eflags;
}

inline bool writePrefetchForm(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      Insn->id != X86_INS_PREFETCHW || !Insn->size || Insn->size > 15)
    return false;
  const auto &X = Insn->detail->x86;
  size_t Pos = 0;
  uint8_t Address = 0, Segment = 0, Rex = 0;
  for (; Pos < Insn->size; ++Pos) {
    const uint8_t B = Insn->bytes[Pos];
    if (B == 0x67 && !Address)
      Address = B;
    else if ((B == 0x64 || B == 0x65) && !Segment)
      Segment = B;
    else
      break;
  }
  if (Pos < Insn->size && Insn->bytes[Pos] >= 0x40 && Insn->bytes[Pos] <= 0x43)
    Rex = Insn->bytes[Pos++];
  if (Pos + 3 > Insn->size || Insn->bytes[Pos] != 0x0f ||
      Insn->bytes[Pos + 1] != 0x0d)
    return false;
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const unsigned AddressSize = Address ? 4 : 8;
  if ((ModRM >> 6) == 3 || ((ModRM >> 3) & 7) != 1 || X.op_count != 1 ||
      X.modrm != ModRM || X.encoding.modrm_offset != Pos + 2 ||
      X.opcode[0] != 0x0f || X.opcode[1] != 0x0d || X.opcode[2] ||
      X.opcode[3] || X.rex != Rex || X.prefix[0] || X.prefix[1] != Segment ||
      X.prefix[2] || X.prefix[3] != Address || X.addr_size != AddressSize ||
      X.encoding.imm_size || X.encoding.imm_offset || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.avx_cc != X86_AVX_CC_INVALID)
    return false;
  // PREFETCHW leaves flags unchanged (Intel SDM, Flags Affected: None).
  // The bundled Capstone table incorrectly reports all EFLAGS modified for
  // this opcode. Its hint flag bitmap is not semantic evidence; authenticate
  // the native encoding and the complete effective address instead.
  const auto &Memory = X.operands[0];
  if (Memory.avx_bcast != X86_AVX_BCAST_INVALID || Memory.avx_zero_opmask)
    return false;
  const auto SegmentReg = Segment == 0x64   ? X86_REG_FS
                          : Segment == 0x65 ? X86_REG_GS
                                            : X86_REG_INVALID;
  return legacyaudit::memoryOperand(Insn, Memory, 1, AddressSize, true, Rex,
                                    Pos + 3, 0, SegmentReg);
}

inline bool accessCheckForm(const cs_insn *Insn, Arch Target) {
  if (!Insn || !Insn->detail || Target != Arch::X64 ||
      (Insn->id != X86_INS_CLAC && Insn->id != X86_INS_STAC) ||
      Insn->size != 3 || Insn->bytes[0] != 0x0f || Insn->bytes[1] != 0x01)
    return false;
  const bool Clear = Insn->id == X86_INS_CLAC;
  const uint8_t ModRM = Clear ? 0xca : 0xcb;
  const auto &X = Insn->detail->x86;
  // Capstone exposes RESET_AC but no SET_AC flag. The canonical byte and
  // instruction ID remain the authority for STAC's architectural AC write.
  return Insn->bytes[2] == ModRM && !X.prefix[0] && !X.prefix[1] &&
         !X.prefix[2] && !X.prefix[3] && X.opcode[0] == 0x0f &&
         X.opcode[1] == 0x01 && !X.opcode[2] && !X.opcode[3] && !X.rex &&
         X.addr_size == 8 && X.modrm == ModRM && !X.sib && !X.disp &&
         X.encoding.modrm_offset == 2 && !X.encoding.disp_offset &&
         !X.encoding.disp_size && !X.encoding.imm_offset &&
         !X.encoding.imm_size && !X.op_count && !X.avx_sae &&
         X.avx_rm == X86_AVX_RM_INVALID && X.avx_cc == X86_AVX_CC_INVALID &&
         X.eflags == (Clear ? X86_EFLAGS_RESET_AC : 0);
}

// Intel SDM Vol. 2, MOVS/REP, LDMXCSR, SWAPGS and CPUID: these forms introduce
// no undefined output bits. This does not certify nonfaulting execution, the
// MXCSR writable mask, a repeated transfer's count/direction, or opaque-state
// preservation. Their existing semantic owners retain those separate
// obligations.
inline bool form(const cs_insn *Insn, Arch Target) {
  if (Insn->detail->x86.prefix[0] == X86_PREFIX_LOCK) {
    if (Insn->id == X86_INS_XADD)
      return xaddaudit::form(Insn, Target);
    if (bitundefined::isBitTest(Insn->id))
      return bitundefined::form(Insn, Target);
    if (Insn->id == X86_INS_AND || Insn->id == X86_INS_OR ||
        Insn->id == X86_INS_XOR)
      return lockedlogicaudit::form(Insn, Target);
  }
  if (sysretqForm(Insn, Target) || longModeEventForm(Insn, Target))
    return true;
  if (descriptorqueryaudit::form(Insn, Target))
    return true;
  if (Insn->id == X86_INS_INC || Insn->id == X86_INS_DEC)
    return lockedunary::form(Insn, Target);
  if (Insn->id == X86_INS_LFENCE)
    return loadFenceForm(Insn, Target);
  if (Insn->id == X86_INS_CPUID || Insn->id == X86_INS_RDTSC ||
      Insn->id == X86_INS_RDTSCP)
    return cpuQueryForm(Insn, Target);
  if (Insn->id == X86_INS_RDMSR || Insn->id == X86_INS_WRMSR)
    return legacyMsrForm(Insn, Target);
  if (Insn->id == X86_INS_SWAPGS)
    return swapgsForm(Insn, Target);
  if (Insn->id == X86_INS_PREFETCHW)
    return writePrefetchForm(Insn, Target);
  if (Insn->id == X86_INS_CLAC || Insn->id == X86_INS_STAC)
    return accessCheckForm(Insn, Target);
  const auto &X = Insn->detail->x86;
  if (Target != Arch::X64 || !Insn->size || Insn->size > 15 || X.avx_sae ||
      X.avx_rm != X86_AVX_RM_INVALID || X.avx_cc != X86_AVX_CC_INVALID ||
      X.encoding.imm_size || X.encoding.imm_offset)
    return false;
  if (Insn->id == X86_INS_MOVSQ) {
    // Keep the first admission exactly F3 48 A5: 64-bit count and pointers,
    // no source segment, address-size override or redundant prefix.
    if (Insn->size != 3 || Insn->bytes[0] != 0xf3 || Insn->bytes[1] != 0x48 ||
        Insn->bytes[2] != 0xa5 || X.prefix[0] != 0xf3 || X.prefix[1] ||
        X.prefix[2] || X.prefix[3] || X.opcode[0] != 0xa5 || X.opcode[1] ||
        X.opcode[2] || X.opcode[3] || X.rex != 0x48 || X.addr_size != 8 ||
        X.modrm || X.sib || X.disp || X.encoding.modrm_offset ||
        X.encoding.disp_size || X.encoding.disp_offset || X.op_count != 2)
      return false;
    for (unsigned I = 0; I != 2; ++I) {
      const auto &Operand = X.operands[I];
      if (Operand.type != X86_OP_MEM || Operand.size != 8 ||
          Operand.mem.segment != X86_REG_INVALID ||
          Operand.mem.base != (I ? X86_REG_RSI : X86_REG_RDI) ||
          Operand.mem.index != X86_REG_INVALID || Operand.mem.scale != 1 ||
          Operand.mem.disp || Operand.avx_bcast != X86_AVX_BCAST_INVALID ||
          Operand.avx_zero_opmask)
        return false;
    }
    return true;
  }
  if ((Insn->id != X86_INS_LDMXCSR && Insn->id != X86_INS_STMXCSR) ||
      X.op_count != 1)
    return false;

  size_t Pos = 0;
  uint8_t AddressPrefix = 0, SegmentPrefix = 0, Rex = 0;
  for (; Pos < Insn->size; ++Pos) {
    const uint8_t B = Insn->bytes[Pos];
    if (B == 0x67 && !AddressPrefix)
      AddressPrefix = B;
    else if ((B == 0x64 || B == 0x65) && !SegmentPrefix)
      SegmentPrefix = B;
    else
      break;
  }
  // REX.B/X extend address registers. W/R are outside this closed form.
  if (Pos < Insn->size && Insn->bytes[Pos] >= 0x40 && Insn->bytes[Pos] <= 0x43)
    Rex = Insn->bytes[Pos++];
  if (Pos + 3 > Insn->size || Insn->bytes[Pos] != 0x0f ||
      Insn->bytes[Pos + 1] != 0xae)
    return false;
  const uint8_t ModRM = Insn->bytes[Pos + 2];
  const unsigned AddressSize = AddressPrefix ? 4 : 8;
  const auto &Operand = X.operands[0];
  const unsigned Extension = Insn->id == X86_INS_LDMXCSR ? 2 : 3;
  if ((ModRM >> 6) == 3 || ((ModRM >> 3) & 7) != Extension ||
      X.modrm != ModRM || X.encoding.modrm_offset != Pos + 2 ||
      X.opcode[0] != 0x0f || X.opcode[1] != 0xae || X.opcode[2] ||
      X.opcode[3] || X.rex != Rex || X.prefix[0] ||
      X.prefix[1] != SegmentPrefix || X.prefix[2] ||
      X.prefix[3] != AddressPrefix || X.addr_size != AddressSize ||
      Operand.avx_bcast != X86_AVX_BCAST_INVALID || Operand.avx_zero_opmask)
    return false;
  const x86_reg Segment = SegmentPrefix == 0x64   ? X86_REG_FS
                          : SegmentPrefix == 0x65 ? X86_REG_GS
                                                  : X86_REG_INVALID;
  return legacyaudit::memoryOperand(Insn, Operand, 4, AddressSize, true, Rex,
                                    Pos + 3, 0, Segment);
}

} // namespace neverd::nativestateaudit

#endif
