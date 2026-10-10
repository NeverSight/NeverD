//===- X86OpaqueStateAudit.h - Independent opaque bank audit ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIFT_X86_OPAQUE_STATE_AUDIT_H
#define NEVERD_LIFT_X86_OPAQUE_STATE_AUDIT_H

#include <capstone/capstone.h>

namespace neverd::opaqueaudit {

// Intel SDM vol. 2, rev. 093: on normal completion, these legacy scalar
// families affect scalar destinations, memory, flags or control, not the
// closed LegacyIntegerOpaqueV1 banks. This separate frozen instruction-ID
// audit is consumed ONLY after the shared strict legacy form check. It is
// not undefined-output coverage. New forms require both audits separately;
// adding an undefined-output instruction cannot silently extend this list.
inline bool hasLegacyIntegerBankAudit(unsigned Id) {
  switch (Id) {
  case X86_INS_ADC:
  case X86_INS_ADD:
  case X86_INS_AND:
  case X86_INS_BSWAP:
  case X86_INS_BT:
  case X86_INS_BTC:
  case X86_INS_BTR:
  case X86_INS_BTS:
  case X86_INS_CALL:
  case X86_INS_CBW:
  case X86_INS_CDQ:
  case X86_INS_CDQE:
  case X86_INS_CLC:
  case X86_INS_CLD:
  case X86_INS_CMC:
  case X86_INS_CMOVA:
  case X86_INS_CMOVAE:
  case X86_INS_CMOVB:
  case X86_INS_CMOVBE:
  case X86_INS_CMOVE:
  case X86_INS_CMOVG:
  case X86_INS_CMOVGE:
  case X86_INS_CMOVL:
  case X86_INS_CMOVLE:
  case X86_INS_CMOVNE:
  case X86_INS_CMOVNO:
  case X86_INS_CMOVNP:
  case X86_INS_CMOVNS:
  case X86_INS_CMOVO:
  case X86_INS_CMOVP:
  case X86_INS_CMOVS:
  case X86_INS_CMP:
  case X86_INS_CQO:
  case X86_INS_CWD:
  case X86_INS_CWDE:
  case X86_INS_DEC:
  case X86_INS_INC:
  case X86_INS_JA:
  case X86_INS_JAE:
  case X86_INS_JB:
  case X86_INS_JBE:
  case X86_INS_JCXZ:
  case X86_INS_JE:
  case X86_INS_JECXZ:
  case X86_INS_JG:
  case X86_INS_JGE:
  case X86_INS_JL:
  case X86_INS_JLE:
  case X86_INS_JMP:
  case X86_INS_JNE:
  case X86_INS_JNO:
  case X86_INS_JNP:
  case X86_INS_JNS:
  case X86_INS_JO:
  case X86_INS_JP:
  case X86_INS_JRCXZ:
  case X86_INS_JS:
  case X86_INS_LAHF:
  case X86_INS_LEA:
  case X86_INS_MOV:
  case X86_INS_MOVABS:
  case X86_INS_MOVSX:
  case X86_INS_MOVSXD:
  case X86_INS_MOVZX:
  case X86_INS_NEG:
  case X86_INS_NOP:
  case X86_INS_NOT:
  case X86_INS_OR:
  case X86_INS_POP:
  case X86_INS_POPF:
  case X86_INS_POPFD:
  case X86_INS_POPFQ:
  case X86_INS_PUSH:
  case X86_INS_PUSHF:
  case X86_INS_PUSHFD:
  case X86_INS_PUSHFQ:
  case X86_INS_RET:
  case X86_INS_ROL:
  case X86_INS_ROR:
  case X86_INS_SAHF:
  case X86_INS_SAL:
  case X86_INS_SAR:
  case X86_INS_SBB:
  case X86_INS_SETA:
  case X86_INS_SETAE:
  case X86_INS_SETB:
  case X86_INS_SETBE:
  case X86_INS_SETE:
  case X86_INS_SETG:
  case X86_INS_SETGE:
  case X86_INS_SETL:
  case X86_INS_SETLE:
  case X86_INS_SETNE:
  case X86_INS_SETNO:
  case X86_INS_SETNP:
  case X86_INS_SETNS:
  case X86_INS_SETO:
  case X86_INS_SETP:
  case X86_INS_SETS:
  case X86_INS_SHL:
  case X86_INS_SHLD:
  case X86_INS_SHR:
  case X86_INS_SHRD:
  case X86_INS_STC:
  case X86_INS_STD:
  case X86_INS_SUB:
  case X86_INS_TEST:
  case X86_INS_XADD:
  case X86_INS_XCHG:
  case X86_INS_XOR:
    return true;
  default:
    return false;
  }
}

} // namespace neverd::opaqueaudit
#endif
