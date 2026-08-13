//===- ARMEHABITestsDetail.h - ARM EHABI test harness -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The synthetic ARM image and the index, entry and LSDA builders shared
// by the ARMEHABI* translation units.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_LANGUAGE_ARMEHABITESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_LANGUAGE_ARMEHABITESTSDETAIL_H

/// fixture would leave the rest of the encoding -- including everything
/// malformed -- unreachable.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/ELF/ARMEHABI.h"

#include "llvm/BinaryFormat/ELF.h"

#include <cstring>

namespace neverd::arm_ehabi_test {

//===----------------------------------------------------------------------===//
// Image and table construction
//===----------------------------------------------------------------------===//

inline constexpr va_t kTextVA = 0x1000;
inline constexpr uint64_t kTextSize = 0x1000;
inline constexpr va_t kTableVA = 0x3000;
inline constexpr uint64_t kTableSize = 0x1000;
inline constexpr va_t kDataVA = 0x5000;
inline constexpr uint64_t kDataSize = 0x1000;

/// Where each table lands inside the readable segment at \ref kTableVA.
inline constexpr va_t kExTabVA = kTableVA;
inline constexpr va_t kExIdxVA = kTableVA + 0x800;

/// A `std::type_info` and the mangled name behind it.
inline constexpr va_t kTypeInfoVA = kDataVA + 0x100;
inline constexpr va_t kTypeNameVA = kDataVA + 0x200;
inline constexpr va_t kTypeCellVA = kDataVA + 0x300;
inline constexpr const char *kTypeName = "15CxxProbeError";

/// The personality routine a generic entry names.  Placed in code because that
/// is where one lives -- a PLT veneer on a dynamically linked image, the
/// routine itself on a static one.
inline constexpr va_t kPersonalityVA = kTextVA + 0x800;

class ByteBuilder {
public:
  void u8(uint8_t V) { Bytes.push_back(V); }
  void u32(uint32_t V) {
    for (unsigned I = 0; I < 4; ++I)
      Bytes.push_back(static_cast<uint8_t>((V >> (I * 8)) & 0xFF));
  }
  void uleb(uint64_t V) {
    do {
      uint8_t Byte = V & 0x7F;
      V >>= 7;
      if (V)
        Byte |= 0x80;
      Bytes.push_back(Byte);
    } while (V);
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

private:
  std::vector<uint8_t> Bytes;
};

/// Encode a `prel31`: the 31-bit signed distance from the word that holds it
/// to \p Target.
inline uint32_t prel31(va_t Target, va_t FieldVA) {
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(Target) - static_cast<uint64_t>(FieldVA)) &
      0x7FFFFFFFull);
}

inline BinaryImage makeARMImage() {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Format = BinaryFormat::ELF;
  Img.Bits = Bitness::Bits32;
  Img.Base = kTextVA;
  Img.Entry = kTextVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = kTextVA;
  Text.Size = kTextSize;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(kTextSize, 0x00);
  Img.Segments.push_back(std::move(Text));

  Segment Tables;
  Tables.Name = ".rodata";
  Tables.VA = kTableVA;
  Tables.Size = kTableSize;
  Tables.Flags = SegmentFlags::Readable;
  Tables.Data.assign(kTableSize, 0);
  Img.Segments.push_back(std::move(Tables));

  Segment Data;
  Data.Name = ".data.rel.ro";
  Data.VA = kDataVA;
  Data.Size = kDataSize;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.assign(kDataSize, 0);
  Img.Segments.push_back(std::move(Data));

  // The last index entry's function runs to the end of the section it starts
  // in, so a text section has to exist for that extent to be recoverable.
  Section Text2;
  Text2.Name = ".text";
  Text2.VA = kTextVA;
  Text2.Size = kTextSize;
  Text2.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Text2));
  return Img;
}

inline void write(BinaryImage &Img, va_t VA, const std::vector<uint8_t> &Bytes) {
  ASSERT_TRUE(Img.writeVA(VA, Bytes.data(), Bytes.size()));
}

/// Write the `std::type_info` the type tables below dispatch on, plus the cell
/// an image would reach it through if it were linked with `R_ARM_TARGET2` as
/// `R_ARM_GOT_PREL`.
inline void writeTypeInfo(BinaryImage &Img) {
  ByteBuilder Info;
  Info.u32(0x12345678); // vtable pointer, unread but never zero in practice
  Info.u32(static_cast<uint32_t>(kTypeNameVA));
  write(Img, kTypeInfoVA, Info.data());

  ByteBuilder Name;
  Name.str(kTypeName);
  write(Img, kTypeNameVA, Name.data());

  ByteBuilder Cell;
  Cell.u32(static_cast<uint32_t>(kTypeInfoVA));
  write(Img, kTypeCellVA, Cell.data());
}

/// A `.gcc_except_table` with one region that catches a type and one that only
/// cleans up.
///
/// Twenty bytes, laid out exactly as a producer emits them: a header whose
/// type-table offset is measured from the byte after itself, a call-site table
/// whose entries are displacements from the landing-pad base, the action chain
/// the call sites name, and one type-table slot below the base the header
/// pointed at.
inline std::vector<uint8_t> buildLSDA(uint8_t TypeTableEncoding, uint32_t SlotValue) {
  ByteBuilder B;
  B.u8(0xFF);               // landing-pad base omitted: the function start
  B.u8(TypeTableEncoding);  //
  B.uleb(17);               // type-table base, measured from the next byte
  B.u8(0x01);               // call sites are ULEB128 displacements
  B.u8(8);                  // call-site table length
  B.uleb(0x10);             // region one: [+0x10, +0x18)
  B.uleb(0x08);             //
  B.uleb(0x40);             // landing pad at +0x40
  B.uleb(1);                // first action record, 1-based
  B.uleb(0x18);             // region two: [+0x18, +0x38), no landing pad
  B.uleb(0x20);             //
  B.uleb(0x00);             //
  B.uleb(0x00);             //
  B.u8(0x01);               // action: catch on type-table slot 1
  B.u8(0x00);               // end of the chain
  B.pad(1);                 // to the word the type table grows down from
  B.u32(SlotValue);
  return B.data();
}

/// Assembles an index and installs it as `.ARM.exidx`.
class IndexBuilder {
public:
  void cantUnwind(va_t Function) { add(Function, 1); }
  void inlineCompact(va_t Function, uint32_t Descriptor) {
    add(Function, Descriptor);
  }
  void tableRef(va_t Function, va_t Table) {
    const va_t EntryVA = kExIdxVA + B.size();
    B.u32(prel31(Function, EntryVA));
    B.u32(prel31(Table, EntryVA + 4));
  }
  void install(BinaryImage &Img) const {
    Section Sec;
    Sec.Name = ".ARM.exidx";
    Sec.VA = kExIdxVA;
    Sec.Size = B.size();
    Sec.Data = B.data();
    Img.Sections.push_back(std::move(Sec));
  }

private:
  void add(va_t Function, uint32_t Word) {
    const va_t EntryVA = kExIdxVA + B.size();
    B.u32(prel31(Function, EntryVA));
    B.u32(Word);
  }
  ByteBuilder B;
};

/// A `.ARM.extab` entry naming its personality routine by address, which is
/// the only form that can carry language data.
///
/// \p Word1 holds the count of further opcode words in its top byte and the
/// first three opcode bytes below it, exactly as the ABI packs them.
inline std::vector<uint8_t> buildGenericEntry(va_t TableVA, va_t PersonalityVA,
                                       uint32_t Word1,
                                       const std::vector<uint32_t> &ExtraWords,
                                       const std::vector<uint8_t> &HandlerData) {
  ByteBuilder B;
  B.u32(prel31(PersonalityVA, TableVA));
  B.u32(Word1);
  for (uint32_t Word : ExtraWords)
    B.u32(Word);
  for (uint8_t Byte : HandlerData)
    B.u8(Byte);
  return B.data();
}

inline const ExceptionFunction *frameAt(const BinaryImage &Img, va_t Address) {
  for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions)
    if (F.CodeRange.Begin == Address)
      return &F;
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Index shapes
//===----------------------------------------------------------------------===//

} // namespace neverd::arm_ehabi_test

#endif // NEVERD_UNITTESTS_LIFT_LANGUAGE_ARMEHABITESTSDETAIL_H
