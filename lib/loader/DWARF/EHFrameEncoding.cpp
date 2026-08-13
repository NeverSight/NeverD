//===- EHFrameEncoding.cpp - DWARF EH variable-length value decoding ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decodes the variable-length primitives DWARF call frame information is
/// built out of: unsigned and signed LEB128 integers, and the `DW_EH_PE_*`
/// encoded pointers whose format nibble states how a value is stored and
/// whose application nibble states what it is measured from.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"

#include <limits>

namespace neverd::dwarf_eh {

using namespace dweh;

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

} // namespace neverd::dwarf_eh
