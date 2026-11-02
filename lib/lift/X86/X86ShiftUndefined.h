//===- X86ShiftUndefined.h - Legacy shift/rotate effect audit -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86SHIFTUNDEFINED_H
#define NEVERD_LIB_LIFT_X86_X86SHIFTUNDEFINED_H

#include "neverd/lift/X86/X86Lifter.h"

#include <array>

namespace neverd::shiftundefined {

inline bool isSingleShift(unsigned Id) {
  return Id == X86_INS_SHL || Id == X86_INS_SAL || Id == X86_INS_SHR ||
         Id == X86_INS_SAR;
}

inline bool isRotate(unsigned Id) {
  return Id == X86_INS_ROL || Id == X86_INS_ROR;
}

// An instruction table owns the arbitrary output slice and its masked-count
// threshold. Emission and verification share the saved-count/placement rules.
struct Rule {
  NdVar Output;
  unsigned BitCount;
  unsigned MinimumCount;
};
// Intel SDM 093, Vol. 2B, SAL/SAR/SHL/SHR, Flags Affected. Counts here
// are already masked to five or six bits. SAR keeps a defined carry even
// when its count exceeds a narrow operand's width. Each single-shift threshold
// describes one fresh bit, not an arbitrary byte or selected hardware value.
inline auto rules(unsigned Id, unsigned Bytes) {
  // ROL/ROR use the architectural masked count, before any narrow-width
  // modulo. Only OF is fresh for counts above one; every other flag either
  // remains unchanged or has a defined value, including whole rotations.
  if (isRotate(Id))
    return std::array<Rule, 3>{{{NdVar::reg(x86reg::AF, 1), 1, 64},
                                {NdVar::reg(x86reg::OF, 1), 1, 2},
                                {NdVar::reg(x86reg::CF, 1), 1, 64}}};
  return std::array<Rule, 3>{
      {{NdVar::reg(x86reg::AF, 1), 1, 1},
       {NdVar::reg(x86reg::OF, 1), 1, 2},
       {NdVar::reg(x86reg::CF, 1), 1, Id == X86_INS_SAR ? 64 : Bytes * 8}}};
}

inline void recordRules(X86Lifter::LiftState &S, NdVar Count,
                        llvm::ArrayRef<Rule> Rules) {
  for (const auto &R : Rules) {
    // A masked count cannot reach 64; omit impossible producers entirely.
    if (R.MinimumCount > (Count.Size == 8 ? 63u : 31u))
      continue;
    if (Count.isConst()) {
      if (Count.Offset >= R.MinimumCount)
        S.recordUndefinedBits(R.Output, 0, R.BitCount);
      continue;
    }
    const auto Guard = S.makeTemp(1);
    // Emit identically with or without a sidecar. The immutable count temp
    // precedes destination writes, including an overlapping CL/ECX/RCX.
    S.emit(NdOp::INT_LESSEQUAL, Guard,
           {NdVar::scalar(R.MinimumCount, Count.Size), Count});
    S.recordUndefinedBits(R.Output, 0, R.BitCount, Guard);
  }
}

inline void record(X86Lifter::LiftState &S, unsigned Id, NdVar Count) {
  recordRules(S, Count, rules(Id, Count.Size));
}

inline bool overlaps(NdVar A, NdVar B) {
  return A.Space == B.Space && A.Size && B.Size &&
         (A.Offset <= B.Offset ? B.Offset - A.Offset < A.Size
                               : A.Offset - B.Offset < B.Size);
}

// Match exact rules and their canonical Boolean guards before publishing a
// transaction. A mutable register must never stand in for the saved count.
inline bool matchesRules(unsigned Width, const cs_x86_op &CountOperand,
                         NdVar CountRegister, llvm::ArrayRef<Rule> Rules,
                         llvm::ArrayRef<LowOp> Ops,
                         const LowInstructionUndefinedEffects &Effects) {
  const unsigned Mask = Width == 8 ? 63 : 31;
  const bool Immediate = CountOperand.type == X86_OP_IMM;
  const uint64_t Count =
      Immediate ? static_cast<uint64_t>(CountOperand.imm) & Mask : 0;
  size_t Index = 0;
  std::optional<NdVar> SavedCount;
  for (const auto &R : Rules) {
    if (R.MinimumCount > Mask || (Immediate && Count < R.MinimumCount))
      continue;
    if (Index == Effects.Effects.size())
      return false;
    const auto &E = Effects.Effects[Index++];
    if (E.Output != R.Output || E.BitOffset != 0 || E.BitCount != R.BitCount ||
        E.AfterOp == 0 || E.AfterOp > Ops.size())
      return false;
    if (Immediate) {
      if (E.When)
        return false;
    } else {
      if (!E.When || !E.When->isTemp() || E.When->Size != 1)
        return false;
      const auto &Guard = Ops[E.AfterOp - 1];
      if (Guard.Opcode != NdOp::INT_LESSEQUAL || Guard.NumInputs != 2 ||
          Guard.Output != *E.When ||
          Guard.Inputs[0] != NdVar::scalar(R.MinimumCount, Width) ||
          !Guard.Inputs[1].isTemp() || Guard.Inputs[1].Size != Width)
        return false;
      if (SavedCount && *SavedCount != Guard.Inputs[1])
        return false;
      SavedCount = Guard.Inputs[1];
      unsigned Definitions = 0;
      bool FoundCount = false;
      for (size_t I = 0; I < Ops.size(); ++I) {
        const auto &Op = Ops[I];
        if (overlaps(Op.Output, *E.When) &&
            (Op.Output != *E.When || ++Definitions != 1))
          return false;
        if (!overlaps(Op.Output, *SavedCount))
          continue;
        if (Op.Output != *SavedCount || FoundCount || I >= E.AfterOp - 1 ||
            Op.Opcode != NdOp::INT_AND || Op.NumInputs != 2 ||
            Op.Inputs[0] != CountRegister ||
            Op.Inputs[1] != NdVar::scalar(Mask, Width))
          return false;
        FoundCount = true;
        // Before taking the snapshot, no physical write may alter CL.
        for (const auto &Earlier : Ops.take_front(I))
          if (overlaps(Earlier.Output, NdVar::reg(x86reg::RCX, 1)))
            return false;
      }
      if (!FoundCount || Definitions != 1)
        return false;
    }
    // A producer follows its final architectural write. No remaining
    // instruction-local computation may consume or kill any overlapping byte.
    for (const auto &Later : Ops.drop_front(E.AfterOp)) {
      if (overlaps(Later.Output, E.Output))
        return false;
      for (unsigned I = 0; I < Later.NumInputs; ++I)
        if (overlaps(Later.Inputs[I], E.Output))
          return false;
    }
  }
  return Index == Effects.Effects.size();
}

inline bool matches(unsigned Id, const cs_x86 &X, llvm::ArrayRef<LowOp> Ops,
                    const LowInstructionUndefinedEffects &Effects) {
  return X.op_count == 2 &&
         matchesRules(X.operands[0].size, X.operands[1],
                      NdVar::reg(x86reg::RCX, isRotate(Id) ? 1 : 4),
                      rules(Id, X.operands[0].size), Ops, Effects);
}

// Admit documented legacy /0, /1, /4, /5 and /7 encodings and the /6 SAL/SHL
// alias specified by Intel SDM 093, Vol. 3B, section 25.15. Widths and count
// operands must match. General scalar/address validation remains in the shared
// lifter audit. Duplicate, LOCK/REP, APX and vector prefixes stay unaudited.
inline bool form(const cs_insn *Insn, Arch Target) {
  const auto &X = Insn->detail->x86;
  if (X.op_count != 2)
    return false;
  const bool Long = Target == Arch::X64;
  uint8_t Segment = 0, OperandPrefix = 0, AddressPrefix = 0, Rex = 0;
  size_t Pos = 0;
  for (; Pos < Insn->size; ++Pos) {
    const uint8_t B = Insn->bytes[Pos];
    if (B == 0x66) {
      if (OperandPrefix || Rex)
        return false;
      OperandPrefix = B;
    } else if (B == 0x67) {
      if (AddressPrefix || Rex)
        return false;
      AddressPrefix = B;
    } else if (B == 0x26 || B == 0x2e || B == 0x36 || B == 0x3e || B == 0x64 ||
               B == 0x65) {
      if (Segment || Rex)
        return false;
      Segment = B;
    } else if (Long && B >= 0x40 && B <= 0x4f) {
      if (Rex || (B & 4)) // REX.R cannot extend an opcode group.
        return false;
      Rex = B;
    } else {
      break;
    }
  }
  if (Pos + 2 > Insn->size)
    return false;
  const uint8_t Opcode = Insn->bytes[Pos];
  const uint8_t ModRM = Insn->bytes[Pos + 1];
  if (Opcode != 0xc0 && Opcode != 0xc1 && Opcode != 0xd0 && Opcode != 0xd1 &&
      Opcode != 0xd2 && Opcode != 0xd3)
    return false;
  const unsigned Group = Insn->id == X86_INS_ROL   ? 0
                         : Insn->id == X86_INS_ROR ? 1
                         : Insn->id == X86_INS_SAR ? 7
                         : Insn->id == X86_INS_SHR ? 5
                                                   : 4;
  const unsigned Width = !(Opcode & 1)   ? 1
                         : (Rex & 8)     ? 8
                         : OperandPrefix ? 2
                                         : 4;
  const unsigned AddressWidth =
      Long ? (AddressPrefix ? 4 : 8) : (AddressPrefix ? 2 : 4);
  const unsigned EncodedGroup = (ModRM >> 3) & 7;
  const bool LeftAlias =
      EncodedGroup == 6 && (Insn->id == X86_INS_SAL || Insn->id == X86_INS_SHL);
  if ((EncodedGroup != Group && !LeftAlias) || X.modrm != ModRM ||
      X.encoding.modrm_offset != Pos + 1 || X.opcode[0] != Opcode ||
      X.opcode[1] || X.opcode[2] || X.opcode[3] || X.rex != Rex ||
      X.prefix[0] || X.prefix[1] != Segment || X.prefix[2] != OperandPrefix ||
      X.prefix[3] != AddressPrefix || X.addr_size != AddressWidth ||
      X.operands[0].size != Width)
    return false;
  const auto &Destination = X.operands[0];
  const auto &Count = X.operands[1];
  const bool Immediate = Opcode == 0xc0 || Opcode == 0xc1;
  const bool CL = Opcode == 0xd2 || Opcode == 0xd3;
  if (Count.size != (CL || Immediate ? 1 : Width))
    return false;
  if (CL ? Count.type != X86_OP_REG || Count.reg != X86_REG_CL ||
               Count.size != 1
         : Count.type != X86_OP_IMM ||
               Count.imm != (Immediate ? Insn->bytes[Insn->size - 1] : 1))
    return false;
  if (X.encoding.imm_size != (Immediate ? 1 : 0) ||
      X.encoding.imm_offset != (Immediate ? Insn->size - 1 : 0))
    return false;
  if ((ModRM >> 6) == 3) {
    if (Destination.type != X86_OP_REG || Segment || AddressPrefix ||
        Insn->size != Pos + 2 + Immediate || (Rex & 2))
      return false;
    const unsigned Number = (ModRM & 7) + ((Rex & 1) ? 8 : 0);
    const uint64_t Offset =
        Width == 1 && !Rex && Number >= 4 ? (Number - 4) * 8 + 1 : Number * 8;
    const auto Reg = mapCapstoneReg(static_cast<x86_reg>(Destination.reg));
    return Reg.Offset == Offset && Reg.Size == Width;
  }
  if (Destination.type != X86_OP_MEM)
    return false;
  // Check the encoded length including SIB and displacement. The common
  // operand audit additionally checks valid base/index/segment registers.
  size_t Cursor = Pos + 2;
  unsigned Disp = 0;
  const unsigned Mod = ModRM >> 6, RM = ModRM & 7;
  if (AddressWidth == 2) {
    Disp = Mod == 1 ? 1 : Mod == 2 || RM == 6 ? 2 : 0;
  } else {
    if (RM == 4) {
      if (Cursor >= Insn->size)
        return false;
      const auto SIB = Insn->bytes[Cursor++];
      if (X.sib != SIB)
        return false;
      if (Mod == 0 && (SIB & 7) == 5)
        Disp = 4;
    } else if (Mod == 0 && RM == 5) {
      Disp = 4;
    }
    if (Mod == 1)
      Disp = 1;
    if (Mod == 2)
      Disp = 4;
  }
  return X.encoding.disp_size == Disp &&
         X.encoding.disp_offset == (Disp ? Cursor : 0) &&
         Cursor + Disp + Immediate == Insn->size;
}

} // namespace neverd::shiftundefined

#endif
