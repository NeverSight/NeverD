//===- LanguageEHTestsDetail.h - Non-Windows EH test harness ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The byte-buffer builder and the synthetic image and .eh_frame layouts
// shared by the LanguageEH* translation units.  Every definition is
// `inline` so the several TUs that include this can be linked together.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_LANGUAGE_LANGUAGEEHTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_LANGUAGE_LANGUAGEEHTESTSDETAIL_H

#include "gtest/gtest.h"

#include "neverd/loader/DWARF/EHFrame.h"
#include "neverd/loader/DWARF/ItaniumEH.h"
#include "neverd/loader/DWARF/LSDA.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/Rust/RustEH.h"

#include <cstring>

#include "neverd/loader/BinaryImage.h"

#include <cstddef>
#include <vector>

namespace neverd::language_eh_test {

//===----------------------------------------------------------------------===//
// Byte-buffer builder
//===----------------------------------------------------------------------===//

/// Assembles the exact byte layouts the decoders read.  Building the tables
/// here rather than compiling a fixture keeps every encoding branch reachable,
/// including the malformed ones no real toolchain emits.
class ByteBuilder {
public:
  void u8(uint8_t V) { Bytes.push_back(V); }
  void u16(uint16_t V) { append(&V, sizeof(V)); }
  void u32(uint32_t V) { append(&V, sizeof(V)); }
  void u64(uint64_t V) { append(&V, sizeof(V)); }
  void i32(int32_t V) { append(&V, sizeof(V)); }

  void uleb(uint64_t V) {
    do {
      uint8_t Byte = V & 0x7f;
      V >>= 7;
      if (V)
        Byte |= 0x80;
      Bytes.push_back(Byte);
    } while (V);
  }

  void sleb(int64_t V) {
    bool More = true;
    while (More) {
      uint8_t Byte = V & 0x7f;
      V >>= 7;
      if ((V == 0 && !(Byte & 0x40)) || (V == -1 && (Byte & 0x40)))
        More = false;
      else
        Byte |= 0x80;
      Bytes.push_back(Byte);
    }
  }

  void str(const char *S) {
    while (*S)
      Bytes.push_back(static_cast<uint8_t>(*S++));
    Bytes.push_back(0);
  }

  void pad(size_t Count, uint8_t Fill = 0) {
    Bytes.insert(Bytes.end(), Count, Fill);
  }

  size_t size() const { return Bytes.size(); }
  const std::vector<uint8_t> &data() const { return Bytes; }
  std::vector<uint8_t> &data() { return Bytes; }

  /// Overwrite a previously reserved 32-bit slot.
  void patch32(size_t Offset, uint32_t Value) {
    std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
  }

private:
  void append(const void *P, size_t N) {
    const auto *B = static_cast<const uint8_t *>(P);
    Bytes.insert(Bytes.end(), B, B + N);
  }
  std::vector<uint8_t> Bytes;
};

inline constexpr va_t kTextVA = 0x400000;

inline constexpr va_t kDataVA = 0x500000;

inline /// A minimal 64-bit image with one executable segment and one data segment.
BinaryImage makeImage(size_t TextSize = 0x1000, size_t DataSize = 0x1000) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::ELF;
  Img.Bits = Bitness::Bits64;
  Img.Base = kTextVA;
  Img.Entry = kTextVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = kTextVA;
  Text.Size = TextSize;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(TextSize, 0x90);
  Img.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = ".data";
  Data.VA = kDataVA;
  Data.Size = DataSize;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.assign(DataSize, 0);
  Img.Segments.push_back(std::move(Data));
  return Img;
}

inline void writeData(BinaryImage &Img, va_t VA, const std::vector<uint8_t> &Bytes) {
  ASSERT_TRUE(Img.writeVA(VA, Bytes.data(), Bytes.size()));
}

/// Build a `.eh_frame` holding one CIE and one FDE.
///
/// \param Augmentation  CIE augmentation string.
/// \param LSDAOffset    When the CIE declares 'L', the FDE's LSDA pointer is
///                      emitted as this pcrel/sdata4 displacement.
struct FrameBytes {
  std::vector<uint8_t> Bytes;
  size_t CIEOffset = 0;
  size_t FDEOffset = 0;
};

inline FrameBytes buildSimpleFrame(va_t SectionVA, va_t FuncVA, uint64_t FuncSize,
                            const char *Augmentation, va_t PersonalityVA,
                            va_t LSDAVA) {
  FrameBytes Result;
  ByteBuilder B;

  const bool HasPersonality = std::strchr(Augmentation, 'P') != nullptr;
  const bool HasLSDA = std::strchr(Augmentation, 'L') != nullptr;

  // --- CIE ---
  Result.CIEOffset = 0;
  const size_t CIELengthSlot = B.size();
  B.u32(0);
  const size_t CIEBodyStart = B.size();
  B.u32(0); // CIE id
  B.u8(1);  // version
  B.str(Augmentation);
  B.uleb(1);  // code alignment factor
  B.sleb(-8); // data alignment factor
  B.u8(16);   // return address register

  if (Augmentation[0] == 'z') {
    ByteBuilder Aug;
    for (const char *P = Augmentation + 1; *P; ++P) {
      switch (*P) {
      case 'L':
        Aug.u8(0x1b); // DW_EH_PE_pcrel | DW_EH_PE_sdata4
        break;
      case 'P': {
        Aug.u8(0x00); // DW_EH_PE_absptr
        Aug.u64(PersonalityVA);
        break;
      }
      case 'R':
        Aug.u8(0x1b);
        break;
      default:
        break;
      }
    }
    B.uleb(Aug.size());
    for (uint8_t Byte : Aug.data())
      B.u8(Byte);
  }
  B.u8(0x0c); // DW_CFA_def_cfa
  B.uleb(7);  // rsp
  B.uleb(8);
  B.u8(0x90); // DW_CFA_offset | 16
  B.uleb(1);
  while ((B.size() - CIEBodyStart) % 8 != 0)
    B.u8(0); // DW_CFA_nop padding
  B.patch32(CIELengthSlot, static_cast<uint32_t>(B.size() - CIEBodyStart));

  // --- FDE ---
  Result.FDEOffset = B.size();
  const size_t FDELengthSlot = B.size();
  B.u32(0);
  const size_t FDEBodyStart = B.size();
  B.u32(static_cast<uint32_t>(FDEBodyStart - Result.CIEOffset)); // CIE pointer

  // initial_location, pcrel|sdata4 relative to its own address.
  const size_t InitialLocSlot = B.size();
  const va_t InitialLocVA = SectionVA + InitialLocSlot;
  B.i32(static_cast<int32_t>(static_cast<int64_t>(FuncVA) -
                             static_cast<int64_t>(InitialLocVA)));
  B.u32(static_cast<uint32_t>(FuncSize));

  if (Augmentation[0] == 'z') {
    ByteBuilder Aug;
    if (HasLSDA) {
      const va_t LSDASlotVA = SectionVA + B.size() + 1;
      Aug.i32(LSDAVA == 0
                  ? 0
                  : static_cast<int32_t>(static_cast<int64_t>(LSDAVA) -
                                         static_cast<int64_t>(LSDASlotVA)));
    }
    B.uleb(Aug.size());
    for (uint8_t Byte : Aug.data())
      B.u8(Byte);
  }
  (void)HasPersonality;

  B.u8(0x41); // DW_CFA_advance_loc 1
  B.u8(0x0e); // DW_CFA_def_cfa_offset
  B.uleb(16);
  while ((B.size() - FDEBodyStart) % 8 != 0)
    B.u8(0);
  B.patch32(FDELengthSlot, static_cast<uint32_t>(B.size() - FDEBodyStart));

  B.u32(0); // section terminator
  Result.Bytes = B.data();
  return Result;
}

} // namespace neverd::language_eh_test

#endif // NEVERD_UNITTESTS_LIFT_LANGUAGE_LANGUAGEEHTESTSDETAIL_H
