//===- DwarfEH.h - DWARF Exception Handling pointer encoding --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Constants for the DWARF Exception Handling pointer encoding scheme used
/// in ELF .eh_frame_hdr sections.  Matches the DW_EH_PE_* encoding from
/// llvm/BinaryFormat/Dwarf.h, but provides a typed C++ interface.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_DWARFEH_H
#define NEVERD_SUPPORT_DWARFEH_H

#include <cstdint>
#include <cstring>

namespace neverd {
namespace dweh {

// ===--------------------------------------------------------------------===//
// Value format (lower nibble of DW_EH_PE encoding)
// ===--------------------------------------------------------------------===//

enum ValueFormat : uint8_t {
  Absptr = 0x00,
  Uleb128 = 0x01,
  Udata2 = 0x02,
  Udata4 = 0x03,
  Udata8 = 0x04,
  Sleb128 = 0x09,
  Sdata2 = 0x0A,
  Sdata4 = 0x0B,
  Sdata8 = 0x0C,
};

// ===--------------------------------------------------------------------===//
// Application / relocation base (upper nibble of DW_EH_PE encoding)
// ===--------------------------------------------------------------------===//

enum Application : uint8_t {
  AbsoluteApp = 0x00,
  PCRel = 0x10,
  TextRel = 0x20,
  DataRel = 0x30,
  FuncRel = 0x40,
  Aligned = 0x50,
  Indirect = 0x80,
  Omit = 0xFF,
};

constexpr uint8_t FormatMask = 0x0F;
constexpr uint8_t ApplicationMask = 0x70;

inline uint8_t getFormat(uint8_t Enc) { return Enc & FormatMask; }
inline uint8_t getApplication(uint8_t Enc) { return Enc & ApplicationMask; }

/// Size in bytes of a DWARF EH pointer value for the given format encoding.
/// Returns 0 for variable-length encodings (ULEB128/SLEB128) or unknown.
inline size_t getEncodedSize(uint8_t Enc) {
  switch (getFormat(Enc)) {
  case Udata2:
  case Sdata2:
    return 2;
  case Udata4:
  case Sdata4:
    return 4;
  case Udata8:
  case Sdata8:
    return 8;
  default:
    return 0;
  }
}

/// Read a DWARF-encoded pointer value from \p Buf at \p Cursor, advancing
/// \p Cursor past the read bytes.  Returns the raw (unresolved) value.
/// Only handles fixed-size formats (udata2/4/8, sdata2/4/8).
inline int64_t readEncoded(const uint8_t *Buf, size_t BufSize, size_t &Cursor,
                           uint8_t Enc) {
  size_t Sz = getEncodedSize(Enc);
  if (Sz == 0 || Cursor + Sz > BufSize)
    return 0;
  int64_t Val = 0;
  switch (getFormat(Enc)) {
  case Udata2: {
    uint16_t V;
    std::memcpy(&V, Buf + Cursor, sizeof(V));
    Val = V;
    break;
  }
  case Sdata2: {
    int16_t V;
    std::memcpy(&V, Buf + Cursor, sizeof(V));
    Val = V;
    break;
  }
  case Udata4: {
    uint32_t V;
    std::memcpy(&V, Buf + Cursor, sizeof(V));
    Val = V;
    break;
  }
  case Sdata4: {
    int32_t V;
    std::memcpy(&V, Buf + Cursor, sizeof(V));
    Val = V;
    break;
  }
  case Udata8: {
    uint64_t V;
    std::memcpy(&V, Buf + Cursor, sizeof(V));
    Val = static_cast<int64_t>(V);
    break;
  }
  case Sdata8: {
    int64_t V;
    std::memcpy(&V, Buf + Cursor, sizeof(V));
    Val = V;
    break;
  }
  default:
    break;
  }
  Cursor += Sz;
  return Val;
}

// ===--------------------------------------------------------------------===//
// .eh_frame_hdr layout
// ===--------------------------------------------------------------------===//

struct EhFrameHdrHeader {
  uint8_t Version;
  uint8_t EhFramePtrEnc;
  uint8_t FdeCountEnc;
  uint8_t TableEnc;
};

static_assert(sizeof(EhFrameHdrHeader) == 4,
              "EhFrameHdrHeader must be 4 bytes");

constexpr uint8_t kEhFrameHdrVersion = 1;
constexpr size_t kEhFrameHdrMinSize = sizeof(EhFrameHdrHeader) + 8;

/// Each FDE table entry: (initial_location, fde_pointer), both sdata4.
constexpr size_t kFdeEntrySize = 8;

} // namespace dweh
} // namespace neverd

#endif // NEVERD_SUPPORT_DWARFEH_H
