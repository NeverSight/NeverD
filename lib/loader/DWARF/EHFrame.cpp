//===- EHFrame.cpp - DWARF call frame information decoding ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/DWARF/EHFrame.h"

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/DwarfEH.h"

#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <limits>

#define DEBUG_TYPE "neverd-dwarf-eh"

namespace neverd::dwarf_eh {
namespace {

using namespace dweh;

/// Largest call-frame instruction expression block accepted.  A DWARF
/// expression describing a CFA rule is a handful of bytes in practice; a
/// length beyond this is a corrupt or hostile record, not a real rule.
constexpr uint64_t kMaxExpressionBytes = 64 * 1024;

/// Largest augmentation string accepted.  The defined characters are a short
/// fixed set, so anything longer indicates the string is not NUL terminated
/// inside the entry.
constexpr size_t kMaxAugmentationLength = 32;

} // namespace

bool readULEB128(const uint8_t *Buf, size_t Size, size_t &Cursor,
                 uint64_t &Out) {
  uint64_t Result = 0;
  unsigned Shift = 0;
  size_t Start = Cursor;
  while (Cursor < Size) {
    uint8_t Byte = Buf[Cursor++];
    // Ten 7-bit groups is the most that can contribute to a 64-bit value; the
    // tenth may only carry the single remaining bit.  Rejecting past that
    // keeps a padded or hostile encoding from silently wrapping.
    if (Shift >= 64) {
      if ((Byte & 0x7f) != 0) {
        Cursor = Start;
        return false;
      }
    } else {
      Result |= static_cast<uint64_t>(Byte & 0x7f) << Shift;
    }
    Shift += 7;
    if ((Byte & 0x80) == 0) {
      Out = Result;
      return true;
    }
    if (Cursor - Start > 10) {
      Cursor = Start;
      return false;
    }
  }
  Cursor = Start;
  return false;
}

bool readSLEB128(const uint8_t *Buf, size_t Size, size_t &Cursor,
                 int64_t &Out) {
  uint64_t Result = 0;
  unsigned Shift = 0;
  size_t Start = Cursor;
  uint8_t Byte = 0;
  while (Cursor < Size) {
    Byte = Buf[Cursor++];
    if (Shift < 64)
      Result |= static_cast<uint64_t>(Byte & 0x7f) << Shift;
    Shift += 7;
    if ((Byte & 0x80) == 0) {
      if (Shift < 64 && (Byte & 0x40))
        Result |= ~uint64_t(0) << Shift;
      Out = static_cast<int64_t>(Result);
      return true;
    }
    if (Cursor - Start > 10) {
      Cursor = Start;
      return false;
    }
  }
  Cursor = Start;
  return false;
}

bool readEncodedPointer(const uint8_t *Buf, size_t Size, size_t &Cursor,
                        va_t BufVA, uint8_t Encoding, const PointerBases &Bases,
                        unsigned PtrSize, const BinaryImage *Img, va_t &Out,
                        va_t *SlotOut) {
  if (SlotOut)
    *SlotOut = 0;
  if (Encoding == Omit)
    return false;

  const uint8_t Format = getFormat(Encoding);
  const uint8_t Application = getApplication(Encoding);
  const bool IsIndirect = (Encoding & dweh::Indirect) != 0;

  // DW_EH_PE_aligned rounds the *position* up to a pointer boundary and then
  // reads an absolute pointer, ignoring the format nibble.
  if (Application == dweh::Aligned) {
    if (PtrSize == 0 || BufVA > std::numeric_limits<va_t>::max() - Cursor)
      return false;
    uint64_t Pos = BufVA + Cursor;
    uint64_t Skip = alignUp(Pos, PtrSize) - Pos;
    if (!rangeInBounds(Cursor, Skip, Size))
      return false;
    Cursor += static_cast<size_t>(Skip);
  }

  const va_t FieldVA = BufVA + Cursor;
  // Sign-extended to 64 bits by the narrow signed formats, so a negative
  // displacement is already correct for modular addition below.
  uint64_t Raw = 0;

  switch (Format) {
  case Absptr: {
    if (PtrSize != 4 && PtrSize != 8)
      return false;
    if (!rangeInBounds(Cursor, PtrSize, Size))
      return false;
    Raw = PtrSize == 4 ? uint64_t(readLE<uint32_t>(Buf + Cursor))
                       : readLE<uint64_t>(Buf + Cursor);
    Cursor += PtrSize;
    break;
  }
  case Uleb128: {
    if (!readULEB128(Buf, Size, Cursor, Raw))
      return false;
    break;
  }
  case Sleb128: {
    int64_t Value = 0;
    if (!readSLEB128(Buf, Size, Cursor, Value))
      return false;
    Raw = static_cast<uint64_t>(Value);
    break;
  }
  case Udata2:
  case Sdata2: {
    if (!rangeInBounds(Cursor, 2, Size))
      return false;
    uint16_t Value = readLE<uint16_t>(Buf + Cursor);
    Raw = Format == Sdata2
              ? static_cast<uint64_t>(static_cast<int64_t>(int16_t(Value)))
              : Value;
    Cursor += 2;
    break;
  }
  case Udata4:
  case Sdata4: {
    if (!rangeInBounds(Cursor, 4, Size))
      return false;
    uint32_t Value = readLE<uint32_t>(Buf + Cursor);
    Raw = Format == Sdata4
              ? static_cast<uint64_t>(static_cast<int64_t>(int32_t(Value)))
              : Value;
    Cursor += 4;
    break;
  }
  case Udata8:
  case Sdata8: {
    if (!rangeInBounds(Cursor, 8, Size))
      return false;
    Raw = readLE<uint64_t>(Buf + Cursor);
    Cursor += 8;
    break;
  }
  default:
    return false;
  }

  // A zero value means "no pointer" for every application except a genuine
  // absolute zero, and every producer spells "absent" as zero.  Resolving it
  // against a base would manufacture an address that is not in the file.
  if (Raw == 0) {
    Out = 0;
    return true;
  }

  uint64_t Base = 0;
  switch (Application) {
  case AbsoluteApp:
  case dweh::Aligned:
    Base = 0;
    break;
  case PCRel:
    Base = FieldVA;
    break;
  case TextRel:
    if (Bases.Text == 0)
      return false;
    Base = Bases.Text;
    break;
  case DataRel:
    if (Bases.Data == 0)
      return false;
    Base = Bases.Data;
    break;
  case FuncRel:
    if (Bases.Func == 0)
      return false;
    Base = Bases.Func;
    break;
  default:
    return false;
  }

  // A relative value is a displacement, and a displacement applied to a base
  // is computed modulo the address size.  The format nibble does not have to
  // be a signed one for the displacement to be negative: ld64 emits FDE
  // initial locations as `DW_EH_PE_pcrel | DW_EH_PE_absptr`, and `__eh_frame`
  // sits above `__text`, so nearly every one of them wraps.  Rejecting a wrap
  // would discard the whole table, and the runtime unwinders do not reject it
  // either -- what makes a resolved address trustworthy is that it lands in a
  // mapped range, which the caller checks.
  uint64_t Resolved = Base + Raw;
  if (PtrSize == 4)
    Resolved &= 0xffffffffull;

  if (IsIndirect) {
    if (SlotOut)
      *SlotOut = static_cast<va_t>(Resolved);
    if (!Img || PtrSize == 0)
      return false;
    const uint8_t *Slot = Img->readVA(Resolved, PtrSize);
    if (!Slot)
      return false;
    Resolved = PtrSize == 4 ? uint64_t(readLE<uint32_t>(Slot))
                            : readLE<uint64_t>(Slot);
  }

  Out = static_cast<va_t>(Resolved);
  return true;
}

namespace {

/// Decode the call-frame instruction program of one entry.
///
/// Returns false only when the program could not be decoded at all; a
/// truncated tail is reported through \p Status so the instructions that were
/// proven remain available.
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
    uint64_t Scaled = 0;
    if (__builtin_mul_overflow(Delta, CodeAlign, &Scaled))
      return false;
    if (Scaled > std::numeric_limits<uint64_t>::max() - Location)
      return false;
    Location += Scaled;
    return true;
  };

  // The data alignment factor is negative on every stack-down architecture, so
  // a division-based range check would have to flip its comparisons with the
  // sign.  Asking the compiler for the overflow bit is exact for either sign.
  auto scaleOffset = [&](int64_t Factored, int64_t &Scaled) {
    return !__builtin_mul_overflow(Factored, DataAlign, &Scaled);
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

/// Parse one CIE body.  \p Cursor points just past the CIE id field.
bool parseCIE(const uint8_t *Buf, size_t Size, size_t Cursor, size_t End,
              va_t BufVA, const PointerBases &Bases, unsigned PtrSize,
              const BinaryImage *Img, const ParseLimits &Limits, DwarfCIE &CIE,
              ExceptionParseStatus &Status,
              std::vector<std::string> &Diagnostics) {
  auto fail = [&](const char *Message) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Malformed);
    Diagnostics.emplace_back(Message);
    return false;
  };

  if (Cursor >= End)
    return fail("truncated DWARF CIE");
  CIE.Version = Buf[Cursor++];
  if (CIE.Version != 1 && CIE.Version != 3 && CIE.Version != 4)
    return fail("unsupported DWARF CIE version");

  size_t AugStart = Cursor;
  while (Cursor < End && Buf[Cursor] != 0)
    ++Cursor;
  if (Cursor >= End)
    return fail("unterminated DWARF CIE augmentation string");
  if (Cursor - AugStart > kMaxAugmentationLength)
    return fail("DWARF CIE augmentation string is implausibly long");
  CIE.Augmentation.assign(reinterpret_cast<const char *>(Buf + AugStart),
                          Cursor - AugStart);
  ++Cursor;

  if (CIE.Version >= 4) {
    if (!rangeInBounds(Cursor, 2, End))
      return fail("truncated DWARF CIE address-size fields");
    CIE.AddressSize = Buf[Cursor++];
    CIE.SegmentSelectorSize = Buf[Cursor++];
    if (CIE.AddressSize != 4 && CIE.AddressSize != 8)
      return fail("unsupported DWARF CIE address size");
    if (CIE.SegmentSelectorSize != 0)
      return fail("segmented DWARF CIE addresses are not modeled");
  }

  // The legacy "eh" augmentation places a pointer-sized language-specific
  // field immediately after the string, outside any 'z' block.
  if (CIE.Augmentation == "eh") {
    if (PtrSize == 0 || !rangeInBounds(Cursor, PtrSize, End))
      return fail("truncated legacy DWARF CIE eh field");
    Cursor += PtrSize;
  }

  if (!readULEB128(Buf, End, Cursor, CIE.CodeAlignmentFactor))
    return fail("truncated DWARF CIE code alignment factor");
  if (!readSLEB128(Buf, End, Cursor, CIE.DataAlignmentFactor))
    return fail("truncated DWARF CIE data alignment factor");
  if (CIE.CodeAlignmentFactor == 0)
    return fail("zero DWARF CIE code alignment factor");

  if (CIE.Version == 1) {
    if (Cursor >= End)
      return fail("truncated DWARF CIE return address register");
    CIE.ReturnAddressRegister = Buf[Cursor++];
  } else if (!readULEB128(Buf, End, Cursor, CIE.ReturnAddressRegister)) {
    return fail("truncated DWARF CIE return address register");
  }

  size_t ProgramStart = Cursor;
  if (!CIE.Augmentation.empty() && CIE.Augmentation[0] == 'z') {
    CIE.HasAugmentationData = true;
    uint64_t AugLength = 0;
    if (!readULEB128(Buf, End, Cursor, AugLength))
      return fail("truncated DWARF CIE augmentation length");
    if (!rangeInBounds(Cursor, AugLength, End))
      return fail("DWARF CIE augmentation data leaves its entry");
    const size_t AugEnd = Cursor + static_cast<size_t>(AugLength);
    ProgramStart = AugEnd;

    for (size_t I = 1; I < CIE.Augmentation.size(); ++I) {
      switch (CIE.Augmentation[I]) {
      case 'L':
        if (Cursor >= AugEnd)
          return fail("truncated DWARF CIE LSDA encoding");
        CIE.LSDAPointerEncoding = Buf[Cursor++];
        break;
      case 'P': {
        if (Cursor >= AugEnd)
          return fail("truncated DWARF CIE personality encoding");
        CIE.PersonalityEncoding = Buf[Cursor++];
        va_t Personality = 0;
        va_t Slot = 0;
        if (!readEncodedPointer(Buf, AugEnd, Cursor, BufVA,
                                CIE.PersonalityEncoding, Bases, PtrSize, Img,
                                Personality, &Slot)) {
          Status =
              mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
          Diagnostics.emplace_back("unresolved DWARF CIE personality pointer");
          Cursor = AugEnd;
        } else {
          CIE.PersonalityVA = Personality;
        }
        // A dynamically bound personality slot holds nothing in the file
        // image, so the slot address is what names the routine later.
        CIE.PersonalitySlotVA = Slot;
        break;
      }
      case 'R':
        if (Cursor >= AugEnd)
          return fail("truncated DWARF CIE FDE pointer encoding");
        CIE.FDEPointerEncoding = Buf[Cursor++];
        break;
      case 'S':
        CIE.IsSignalFrame = true;
        break;
      case 'B':
        CIE.HasPointerAuth = true;
        break;
      case 'G':
        CIE.HasMTETaggedFrame = true;
        break;
      default:
        // An unknown augmentation character makes the remaining augmentation
        // bytes uninterpretable.  The 'z' length still bounds them exactly, so
        // the entry stays parseable; only the augmentation is unknown.
        Status =
            mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
        Diagnostics.emplace_back("unknown DWARF CIE augmentation character");
        I = CIE.Augmentation.size();
        break;
      }
    }
    Cursor = AugEnd;
  } else if (!CIE.Augmentation.empty() && CIE.Augmentation != "eh") {
    // Without 'z' there is no length prefix, so an augmentation this decoder
    // does not know makes the rest of the entry unparseable.
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.emplace_back(
        "DWARF CIE augmentation has no self-describing length");
    return true;
  }

  Cursor = ProgramStart;
  // An .eh_frame CIE that never declared an FDE encoding leaves FDE initial
  // locations as absolute pointers, which is the psABI default.
  const uint8_t FDEEncoding =
      CIE.FDEPointerEncoding == Omit ? uint8_t(Absptr) : CIE.FDEPointerEncoding;
  decodeCFIProgram(Buf, Size, Cursor, End, CIE, FDEEncoding, BufVA, Bases,
                   PtrSize, Img, Limits, CIE.InitialInstructions, Status,
                   Diagnostics);
  return true;
}

} // namespace

ParseResult parseEHFrame(const BinaryImage &Img, const FrameSection &Sec,
                         const PointerBases &Bases, const ParseLimits &Limits) {
  ParseResult Result;
  if (!Sec.Data || Sec.Size < 4)
    return Result;

  const unsigned PtrSize = Img.getPointerSize();
  const uint8_t *Buf = Sec.Data;
  const size_t Size = Sec.Size;

  auto malformed = [&](const char *Message) {
    Result.ParseStatus = mergeExceptionParseStatus(
        Result.ParseStatus, ExceptionParseStatus::Malformed);
    Result.Diagnostics.emplace_back(Message);
  };

  size_t Offset = 0;
  size_t Entries = 0;
  while (Offset + 4 <= Size) {
    if (++Entries > Limits.MaxEntries) {
      Result.ParseStatus = mergeExceptionParseStatus(
          Result.ParseStatus, ExceptionParseStatus::Partial);
      Result.Diagnostics.emplace_back(
          "DWARF frame section exceeds entry decode budget");
      break;
    }

    const size_t EntryStart = Offset;
    uint64_t Length = readLE<uint32_t>(Buf + Offset);
    Offset += 4;
    bool Is64BitDwarf = false;
    if (Length == 0xffffffffu) {
      if (!rangeInBounds(Offset, 8, Size)) {
        malformed("truncated 64-bit DWARF frame entry length");
        break;
      }
      Length = readLE<uint64_t>(Buf + Offset);
      Offset += 8;
      Is64BitDwarf = true;
    }
    // A zero length is the section terminator.  Padding after it is normal.
    if (Length == 0)
      break;
    if (!rangeInBounds(Offset, Length, Size)) {
      malformed("DWARF frame entry length leaves its section");
      break;
    }

    const size_t EntryEnd = Offset + static_cast<size_t>(Length);
    const size_t IdSize = Is64BitDwarf ? 8 : 4;
    if (!rangeInBounds(Offset, IdSize, EntryEnd)) {
      malformed("truncated DWARF frame entry identifier");
      break;
    }
    const size_t IdOffset = Offset;
    const uint64_t Id = Is64BitDwarf ? readLE<uint64_t>(Buf + Offset)
                                     : uint64_t(readLE<uint32_t>(Buf + Offset));
    Offset += IdSize;

    if (Id == 0) {
      DwarfCIE CIE;
      CIE.SectionOffset = EntryStart;
      if (parseCIE(Buf, Size, Offset, EntryEnd, Sec.VA, Bases, PtrSize, &Img,
                   Limits, CIE, Result.ParseStatus, Result.Diagnostics))
        Result.CIEs.push_back(std::move(CIE));
      Offset = EntryEnd;
      continue;
    }

    // In .eh_frame the identifier is the distance back from the identifier
    // field to the start of the owning CIE.
    if (Id > IdOffset) {
      malformed("DWARF FDE names a CIE before its section");
      Offset = EntryEnd;
      continue;
    }
    const uint64_t CIEOffset = IdOffset - Id;
    const DwarfCIE *CIE = nullptr;
    for (const DwarfCIE &Candidate : Result.CIEs)
      if (Candidate.SectionOffset == CIEOffset)
        CIE = &Candidate;
    if (!CIE) {
      Result.ParseStatus = mergeExceptionParseStatus(
          Result.ParseStatus, ExceptionParseStatus::Partial);
      Result.Diagnostics.emplace_back("DWARF FDE names an undecoded CIE");
      Offset = EntryEnd;
      continue;
    }

    DwarfFDE FDE;
    FDE.SectionOffset = EntryStart;
    FDE.CIESectionOffset = CIEOffset;

    const uint8_t Encoding = CIE->FDEPointerEncoding == Omit
                                 ? uint8_t(Absptr)
                                 : CIE->FDEPointerEncoding;
    va_t Initial = 0;
    FDE.InitialLocationOffset = Offset;
    if (!readEncodedPointer(Buf, EntryEnd, Offset, Sec.VA, Encoding, Bases,
                            PtrSize, &Img, Initial)) {
      malformed("unreadable DWARF FDE initial location");
      Offset = EntryEnd;
      continue;
    }
    FDE.InitialLocation = Initial;

    // The address range uses the same value format but is never relative:
    // it is a length, so the application nibble must not be applied to it.
    va_t Range = 0;
    if (!readEncodedPointer(Buf, EntryEnd, Offset, Sec.VA, getFormat(Encoding),
                            Bases, PtrSize, &Img, Range)) {
      malformed("unreadable DWARF FDE address range");
      Offset = EntryEnd;
      continue;
    }
    FDE.AddressRange = Range;

    size_t ProgramStart = Offset;
    if (CIE->HasAugmentationData) {
      uint64_t AugLength = 0;
      if (!readULEB128(Buf, EntryEnd, Offset, AugLength)) {
        malformed("truncated DWARF FDE augmentation length");
        Offset = EntryEnd;
        continue;
      }
      if (!rangeInBounds(Offset, AugLength, EntryEnd)) {
        malformed("DWARF FDE augmentation data leaves its entry");
        Offset = EntryEnd;
        continue;
      }
      const size_t AugEnd = Offset + static_cast<size_t>(AugLength);
      ProgramStart = AugEnd;
      if (CIE->LSDAPointerEncoding != Omit) {
        PointerBases FDEBases = Bases;
        FDEBases.Func = FDE.InitialLocation;
        va_t LSDA = 0;
        if (readEncodedPointer(Buf, AugEnd, Offset, Sec.VA,
                               CIE->LSDAPointerEncoding, FDEBases, PtrSize,
                               &Img, LSDA)) {
          FDE.LSDAVA = LSDA;
        } else {
          Result.ParseStatus = mergeExceptionParseStatus(
              Result.ParseStatus, ExceptionParseStatus::Partial);
          Result.Diagnostics.emplace_back("unresolved DWARF FDE LSDA pointer");
        }
      }
      Offset = AugEnd;
    }

    PointerBases FDEBases = Bases;
    FDEBases.Func = FDE.InitialLocation;
    decodeCFIProgram(Buf, Size, ProgramStart, EntryEnd, *CIE, Encoding, Sec.VA,
                     FDEBases, PtrSize, &Img, Limits, FDE.Instructions,
                     Result.ParseStatus, Result.Diagnostics);

    Result.FDEs.push_back(std::move(FDE));
    Offset = EntryEnd;
  }

  LLVM_DEBUG(llvm::dbgs() << "dwarf-eh: decoded " << Result.CIEs.size()
                          << " CIEs and " << Result.FDEs.size() << " FDEs\n");
  return Result;
}

} // namespace neverd::dwarf_eh
