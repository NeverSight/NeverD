//===- EHFrameCFI.cpp - DWARF call-frame instruction decoding -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decodes the call-frame instruction program carried by a CIE's initial
/// instructions and by every FDE, normalizing each `DW_CFA_*` opcode into a
/// \ref CFIInstruction and tracking the frame location the program advances
/// through.
///
//===----------------------------------------------------------------------===//

#include "EHFrameDetail.h"

#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Support/MathExtras.h"

#include <limits>
#include <utility>

#define DEBUG_TYPE "neverd-dwarf-eh"

namespace neverd::dwarf_eh {
namespace {

/// Largest call-frame instruction expression block accepted.  A DWARF
/// expression describing a CFA rule is a handful of bytes in practice; a
/// length beyond this is a corrupt or hostile record, not a real rule.
constexpr uint64_t kMaxExpressionBytes = 64 * 1024;

} // namespace

namespace detail {

bool decodeCFIProgram(const uint8_t *Buf, size_t Size, size_t Cursor,
                      size_t End, const DwarfCIE &CIE, uint8_t FDEEncoding,
                      va_t BufVA, const PointerBases &Bases, unsigned PtrSize,
                      const BinaryImage *Img, const ParseLimits &Limits,
                      std::vector<CFIInstruction> &Out,
                      ExceptionParseStatus &Status,
                      std::vector<std::string> &Diagnostics) {
  using namespace llvm::dwarf;

  uint64_t Location = 0;
  const uint64_t CodeAlign = CIE.CodeAlignmentFactor;
  const int64_t DataAlign = CIE.DataAlignmentFactor;

  auto fail = [&](const char *Message) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Malformed);
    Diagnostics.emplace_back(Message);
    return false;
  };

  auto advance = [&](uint64_t Delta) {
    // The scaled advance is bounded by the entry's own address range in
    // practice; guard the multiply so a crafted delta cannot wrap.
    if (CodeAlign != 0 &&
        Delta > std::numeric_limits<uint64_t>::max() / CodeAlign)
      return false;
    const uint64_t Scaled = Delta * CodeAlign;
    if (Scaled > std::numeric_limits<uint64_t>::max() - Location)
      return false;
    Location += Scaled;
    return true;
  };

  // The data alignment factor is negative on every stack-down architecture, so
  // a division-based range check would have to flip its comparisons with the
  // sign.  LLVM's portable overflow helper is exact for either sign and also
  // works with compilers that do not provide __builtin_mul_overflow.
  auto scaleOffset = [&](int64_t Factored, int64_t &Scaled) {
    return !llvm::MulOverflow(Factored, DataAlign, Scaled);
  };

  while (Cursor < End) {
    if (Out.size() >= Limits.MaxInstructionsPerEntry) {
      Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
      Diagnostics.emplace_back(
          "DWARF call-frame program exceeds decode budget");
      return true;
    }

    const uint8_t Op = Buf[Cursor++];
    CFIInstruction Insn;
    Insn.CodeOffset = Location;

    const uint8_t Primary = Op & 0xc0;
    const uint8_t Low = Op & 0x3f;

    if (Primary == DW_CFA_advance_loc) {
      Insn.Kind = CFIOpKind::AdvanceLoc;
      Insn.Offset = static_cast<int64_t>(Low);
      if (!advance(Low))
        return fail("DWARF advance_loc overflows the frame location");
      Out.push_back(std::move(Insn));
      continue;
    }
    if (Primary == DW_CFA_offset) {
      uint64_t Factored = 0;
      if (!readULEB128(Buf, End, Cursor, Factored))
        return fail("truncated DWARF offset operand");
      int64_t Scaled = 0;
      if (!scaleOffset(static_cast<int64_t>(Factored), Scaled))
        return fail("DWARF offset operand overflows its data alignment");
      Insn.Kind = CFIOpKind::Offset;
      Insn.Register = Low;
      Insn.Offset = Scaled;
      Out.push_back(std::move(Insn));
      continue;
    }
    if (Primary == DW_CFA_restore) {
      Insn.Kind = CFIOpKind::Restore;
      Insn.Register = Low;
      Out.push_back(std::move(Insn));
      continue;
    }

    switch (Low) {
    case DW_CFA_nop:
      Insn.Kind = CFIOpKind::Nop;
      break;
    case DW_CFA_set_loc: {
      va_t Address = 0;
      size_t Save = Cursor;
      if (!readEncodedPointer(Buf, End, Cursor, BufVA, FDEEncoding, Bases,
                              PtrSize, Img, Address)) {
        Cursor = Save;
        return fail("unreadable DWARF set_loc address");
      }
      Insn.Kind = CFIOpKind::SetLoc;
      Insn.Offset = static_cast<int64_t>(Address);
      break;
    }
    case DW_CFA_advance_loc1:
    case DW_CFA_advance_loc2:
    case DW_CFA_advance_loc4: {
      const size_t Width = Low == DW_CFA_advance_loc1   ? 1
                           : Low == DW_CFA_advance_loc2 ? 2
                                                        : 4;
      if (!rangeInBounds(Cursor, Width, End))
        return fail("truncated DWARF advance_loc operand");
      uint64_t Delta = Width == 1   ? Buf[Cursor]
                       : Width == 2 ? readLE<uint16_t>(Buf + Cursor)
                                    : readLE<uint32_t>(Buf + Cursor);
      Cursor += Width;
      Insn.Kind = CFIOpKind::AdvanceLoc;
      Insn.Offset = static_cast<int64_t>(Delta);
      if (!advance(Delta))
        return fail("DWARF advance_loc overflows the frame location");
      break;
    }
    case DW_CFA_offset_extended:
    case DW_CFA_val_offset: {
      uint64_t Reg = 0, Factored = 0;
      if (!readULEB128(Buf, End, Cursor, Reg) ||
          !readULEB128(Buf, End, Cursor, Factored))
        return fail("truncated DWARF offset_extended operands");
      int64_t Scaled = 0;
      if (!scaleOffset(static_cast<int64_t>(Factored), Scaled))
        return fail("DWARF offset operand overflows its data alignment");
      Insn.Kind = Low == DW_CFA_offset_extended ? CFIOpKind::Offset
                                                : CFIOpKind::ValOffset;
      Insn.Register = Reg;
      Insn.Offset = Scaled;
      break;
    }
    case DW_CFA_offset_extended_sf:
    case DW_CFA_val_offset_sf: {
      uint64_t Reg = 0;
      int64_t Factored = 0;
      if (!readULEB128(Buf, End, Cursor, Reg) ||
          !readSLEB128(Buf, End, Cursor, Factored))
        return fail("truncated DWARF signed offset operands");
      int64_t Scaled = 0;
      if (!scaleOffset(Factored, Scaled))
        return fail("DWARF offset operand overflows its data alignment");
      Insn.Kind = Low == DW_CFA_offset_extended_sf ? CFIOpKind::Offset
                                                   : CFIOpKind::ValOffset;
      Insn.Register = Reg;
      Insn.Offset = Scaled;
      break;
    }
    case DW_CFA_restore_extended:
    case DW_CFA_undefined:
    case DW_CFA_same_value:
    case DW_CFA_def_cfa_register: {
      uint64_t Reg = 0;
      if (!readULEB128(Buf, End, Cursor, Reg))
        return fail("truncated DWARF register operand");
      Insn.Kind = Low == DW_CFA_restore_extended ? CFIOpKind::Restore
                  : Low == DW_CFA_undefined      ? CFIOpKind::Undefined
                  : Low == DW_CFA_same_value     ? CFIOpKind::SameValue
                                                 : CFIOpKind::DefCFARegister;
      Insn.Register = Reg;
      break;
    }
    case DW_CFA_register: {
      uint64_t Reg = 0, Reg2 = 0;
      if (!readULEB128(Buf, End, Cursor, Reg) ||
          !readULEB128(Buf, End, Cursor, Reg2))
        return fail("truncated DWARF register-pair operands");
      Insn.Kind = CFIOpKind::Register;
      Insn.Register = Reg;
      Insn.Register2 = Reg2;
      break;
    }
    case DW_CFA_remember_state:
      Insn.Kind = CFIOpKind::RememberState;
      break;
    case DW_CFA_restore_state:
      Insn.Kind = CFIOpKind::RestoreState;
      break;
    case DW_CFA_def_cfa: {
      uint64_t Reg = 0, Offset = 0;
      if (!readULEB128(Buf, End, Cursor, Reg) ||
          !readULEB128(Buf, End, Cursor, Offset))
        return fail("truncated DWARF def_cfa operands");
      Insn.Kind = CFIOpKind::DefCFA;
      Insn.Register = Reg;
      Insn.Offset = static_cast<int64_t>(Offset);
      break;
    }
    case DW_CFA_def_cfa_sf: {
      uint64_t Reg = 0;
      int64_t Factored = 0;
      if (!readULEB128(Buf, End, Cursor, Reg) ||
          !readSLEB128(Buf, End, Cursor, Factored))
        return fail("truncated DWARF def_cfa_sf operands");
      int64_t Scaled = 0;
      if (!scaleOffset(Factored, Scaled))
        return fail("DWARF def_cfa_sf offset overflows its data alignment");
      Insn.Kind = CFIOpKind::DefCFA;
      Insn.Register = Reg;
      Insn.Offset = Scaled;
      break;
    }
    case DW_CFA_def_cfa_offset: {
      uint64_t Offset = 0;
      if (!readULEB128(Buf, End, Cursor, Offset))
        return fail("truncated DWARF def_cfa_offset operand");
      Insn.Kind = CFIOpKind::DefCFAOffset;
      Insn.Offset = static_cast<int64_t>(Offset);
      break;
    }
    case DW_CFA_def_cfa_offset_sf: {
      int64_t Factored = 0;
      if (!readSLEB128(Buf, End, Cursor, Factored))
        return fail("truncated DWARF def_cfa_offset_sf operand");
      int64_t Scaled = 0;
      if (!scaleOffset(Factored, Scaled))
        return fail("DWARF def_cfa_offset_sf overflows its data alignment");
      Insn.Kind = CFIOpKind::DefCFAOffset;
      Insn.Offset = Scaled;
      break;
    }
    case DW_CFA_def_cfa_expression: {
      uint64_t Length = 0;
      if (!readULEB128(Buf, End, Cursor, Length))
        return fail("truncated DWARF def_cfa_expression length");
      if (Length > kMaxExpressionBytes || !rangeInBounds(Cursor, Length, End))
        return fail("invalid DWARF def_cfa_expression block");
      Insn.Kind = CFIOpKind::DefCFAExpression;
      Insn.Expression.assign(Buf + Cursor, Buf + Cursor + Length);
      Cursor += static_cast<size_t>(Length);
      break;
    }
    case DW_CFA_expression:
    case DW_CFA_val_expression: {
      uint64_t Reg = 0, Length = 0;
      if (!readULEB128(Buf, End, Cursor, Reg) ||
          !readULEB128(Buf, End, Cursor, Length))
        return fail("truncated DWARF expression operands");
      if (Length > kMaxExpressionBytes || !rangeInBounds(Cursor, Length, End))
        return fail("invalid DWARF expression block");
      Insn.Kind = Low == DW_CFA_expression ? CFIOpKind::Expression
                                           : CFIOpKind::ValExpression;
      Insn.Register = Reg;
      Insn.Expression.assign(Buf + Cursor, Buf + Cursor + Length);
      Cursor += static_cast<size_t>(Length);
      break;
    }
    case DW_CFA_GNU_args_size: {
      uint64_t Size2 = 0;
      if (!readULEB128(Buf, End, Cursor, Size2))
        return fail("truncated DWARF GNU_args_size operand");
      Insn.Kind = CFIOpKind::GnuArgsSize;
      Insn.Offset = static_cast<int64_t>(Size2);
      break;
    }
    case DW_CFA_AARCH64_negate_ra_state:
      // 0x2d is DW_CFA_GNU_window_save on SPARC.  Both are operand-free and
      // both toggle a per-frame boolean, so one normalized kind is exact for
      // the architectures NeverD lifts.
      Insn.Kind = CFIOpKind::NegateRAState;
      break;
    case DW_CFA_AARCH64_negate_ra_state_with_pc:
      Insn.Kind = CFIOpKind::NegateRAStateWithPC;
      break;
    default:
      // An unmodeled vendor opcode has an unknown operand encoding, so the
      // rest of the program cannot be located.  Retain the opcode byte and
      // report the record as partial rather than misreading the tail.
      Insn.Kind = CFIOpKind::Opaque;
      Insn.OperandBytes.assign(1, Op);
      Out.push_back(std::move(Insn));
      Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
      Diagnostics.emplace_back("unknown DWARF call-frame opcode");
      return true;
    }
    Out.push_back(std::move(Insn));
  }
  return true;
}

} // namespace detail
} // namespace neverd::dwarf_eh
