//===- EHFrame.h - DWARF call frame information decoding ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the bounded decoder for DWARF call frame information as it appears
/// in an `.eh_frame` / `__eh_frame` section.  The decoder is format neutral:
/// callers supply the section bytes, the address they are mapped at, and the
/// bases the DWARF EH pointer encodings resolve against.
///
/// Every length, count, offset, and pointer read here comes from untrusted
/// file data.  A record that cannot be fully validated lowers the reported
/// parse status and is reported with a deterministic diagnostic rather than
/// being dropped, so analysis keeps whatever the bytes did prove.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_DWARF_EHFRAME_H
#define NEVERD_LOADER_DWARF_EHFRAME_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionInfo.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::dwarf_eh {

/// A frame section to decode, together with where it is mapped.
struct FrameSection {
  const uint8_t *Data = nullptr;
  size_t Size = 0;
  /// Virtual address of `Data[0]`.  Required: `DW_EH_PE_pcrel` is resolved
  /// against the address of the encoded field itself.
  va_t VA = 0;
};

/// Bases that DWARF EH pointer encodings resolve against.  A base left at
/// zero makes its encoding unresolvable, which is reported rather than
/// silently producing an address relative to nothing.
struct PointerBases {
  /// `DW_EH_PE_textrel`: start of the text segment.
  va_t Text = 0;
  /// `DW_EH_PE_datarel`: the ELF psABI defines this as the address of the
  /// `.eh_frame_hdr` section for data inside it, and as the GOT base
  /// elsewhere.  Callers set whichever applies to the section being read.
  va_t Data = 0;
  /// `DW_EH_PE_funcrel`: start of the function owning the record.
  va_t Func = 0;
};

struct ParseLimits {
  /// Upper bound on entries decoded from one section.  A crafted section can
  /// otherwise describe more entries than the file has bytes for.
  size_t MaxEntries = 1u << 20;
  /// Upper bound on call-frame instructions decoded from one entry.
  size_t MaxInstructionsPerEntry = 1u << 16;
};

struct ParseResult {
  std::vector<DwarfCIE> CIEs;
  std::vector<DwarfFDE> FDEs;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;
  std::vector<std::string> Diagnostics;
};

/// Decode every CIE and FDE in \p Sec.
///
/// \p Img is consulted only to resolve `DW_EH_PE_indirect` pointers and to
/// classify addresses; the section bytes themselves come from \p Sec so a
/// caller can decode a section that is not mapped into a segment.
ParseResult parseEHFrame(const BinaryImage &Img, const FrameSection &Sec,
                         const PointerBases &Bases,
                         const ParseLimits &Limits = {});

/// Resolve one DWARF EH encoded pointer.
///
/// \param Buf       Buffer holding the encoded value.
/// \param Size      Size of \p Buf.
/// \param Cursor    In/out offset into \p Buf; advanced past the value.
/// \param BufVA     Virtual address of `Buf[0]`, for `DW_EH_PE_pcrel`.
/// \param Encoding  `DW_EH_PE_*` byte.
/// \param Bases     Bases for the relative applications.
/// \param PtrSize   Target pointer size, for `DW_EH_PE_absptr`.
/// \param Img       Image used to follow `DW_EH_PE_indirect`; may be null,
///                  in which case an indirect encoding fails.
/// \param Out       Receives the resolved address.
/// \param SlotOut   Optional.  For `DW_EH_PE_indirect`, receives the address
///                  of the slot the value was loaded through.  A dynamically
///                  bound slot holds nothing useful in the file image, so the
///                  slot address is what lets a caller name the target from a
///                  relocation or import binding instead.
/// \returns false if the value could not be read or resolved.  \p Cursor is
///          still advanced when the failure was only in resolution, so a
///          caller decoding a record can continue past a field it could not
///          interpret.
bool readEncodedPointer(const uint8_t *Buf, size_t Size, size_t &Cursor,
                        va_t BufVA, uint8_t Encoding, const PointerBases &Bases,
                        unsigned PtrSize, const BinaryImage *Img, va_t &Out,
                        va_t *SlotOut = nullptr);

/// Read an unsigned LEB128.  Returns false on truncation or on an encoding
/// longer than 10 bytes, which cannot represent a 64-bit value.
bool readULEB128(const uint8_t *Buf, size_t Size, size_t &Cursor,
                 uint64_t &Out);

/// Read a signed LEB128 under the same limits.
bool readSLEB128(const uint8_t *Buf, size_t Size, size_t &Cursor, int64_t &Out);

} // namespace neverd::dwarf_eh

#endif // NEVERD_LOADER_DWARF_EHFRAME_H
