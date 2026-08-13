//===- ObjCEHTestsDetail.h - Objective-C EH test harness --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The synthetic Objective-C image, its frame and LSDA builders, and the
// descriptor writers shared by the ObjCEH* translation units.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_LANGUAGE_OBJCEHTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_LANGUAGE_OBJCEHTESTSDETAIL_H

///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/loader/DWARF/ItaniumEH.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/ObjC/ObjCEH.h"

#include <cstring>

namespace neverd::objc_eh_test {

//===----------------------------------------------------------------------===//
// Byte-buffer builder
//===----------------------------------------------------------------------===//

class ByteBuilder {
public:
  void u8(uint8_t V) { Bytes.push_back(V); }
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

  size_t size() const { return Bytes.size(); }
  const std::vector<uint8_t> &data() const { return Bytes; }

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

//===----------------------------------------------------------------------===//
// Image layout
//===----------------------------------------------------------------------===//

inline constexpr va_t kTextVA = 0x400000;
inline constexpr va_t kDataVA = 0x500000;
inline constexpr va_t kFuncVA = kTextVA + 0x100;
inline constexpr uint64_t kFuncSize = 0x80;
inline constexpr va_t kFrameVA = kDataVA;
inline constexpr va_t kLSDAVA = kDataVA + 0x400;
/// Where the fixtures place `objc_typeinfo` records and class-name strings.
inline constexpr va_t kDescriptorVA = kDataVA + 0x600;
inline constexpr va_t kStringVA = kDataVA + 0x700;
inline constexpr va_t kClassVA = kDataVA + 0x780;
/// Where the fixtures place the runtime entry points a body calls.
inline constexpr va_t kRuntimeVA = kTextVA + 0x800;

inline BinaryImage makeImage() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::ELF;
  Img.Bits = Bitness::Bits64;
  Img.Base = kTextVA;
  Img.Entry = kTextVA;

  Segment Text;
  Text.Name = ".text";
  Text.VA = kTextVA;
  Text.Size = 0x1000;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(0x1000, 0x90);
  Img.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = ".data";
  Data.VA = kDataVA;
  Data.Size = 0x1000;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.assign(0x1000, 0);
  Img.Segments.push_back(std::move(Data));
  return Img;
}

inline void writeData(BinaryImage &Img, va_t VA, const std::vector<uint8_t> &Bytes) {
  ASSERT_TRUE(Img.writeVA(VA, Bytes.data(), Bytes.size()));
}

inline void addSymbol(BinaryImage &Img, const char *Name, va_t Addr,
               bool IsFunc = true) {
  Symbol S;
  S.Name = Name;
  S.Addr = Addr;
  S.IsFunc = IsFunc;
  Img.Symbols.push_back(std::move(S));
}

//===----------------------------------------------------------------------===//
// `.eh_frame`
//===----------------------------------------------------------------------===//

/// One CIE naming \p PersonalityVA plus one FDE covering the fixture function
/// and pointing at \ref kLSDAVA.
inline std::vector<uint8_t> buildFrame(va_t PersonalityVA) {
  ByteBuilder B;

  const size_t CIELengthSlot = B.size();
  B.u32(0);
  const size_t CIEBodyStart = B.size();
  B.u32(0); // CIE id
  B.u8(1);  // version
  B.str("zPLR");
  B.uleb(1);  // code alignment factor
  B.sleb(-8); // data alignment factor
  B.u8(16);   // return address register
  {
    ByteBuilder Aug;
    Aug.u8(0x00); // 'P': DW_EH_PE_absptr
    Aug.u64(PersonalityVA);
    Aug.u8(0x1b); // 'L': DW_EH_PE_pcrel | DW_EH_PE_sdata4
    Aug.u8(0x1b); // 'R'
    B.uleb(Aug.size());
    for (uint8_t Byte : Aug.data())
      B.u8(Byte);
  }
  B.u8(0x0c); // DW_CFA_def_cfa
  B.uleb(7);
  B.uleb(8);
  while ((B.size() - CIEBodyStart) % 8 != 0)
    B.u8(0);
  B.patch32(CIELengthSlot, static_cast<uint32_t>(B.size() - CIEBodyStart));

  const size_t FDELengthSlot = B.size();
  B.u32(0);
  const size_t FDEBodyStart = B.size();
  B.u32(static_cast<uint32_t>(FDEBodyStart));

  const va_t InitialLocVA = kFrameVA + B.size();
  B.i32(static_cast<int32_t>(static_cast<int64_t>(kFuncVA) -
                             static_cast<int64_t>(InitialLocVA)));
  B.u32(static_cast<uint32_t>(kFuncSize));
  {
    ByteBuilder Aug;
    const va_t LSDASlotVA = kFrameVA + B.size() + 1;
    Aug.i32(static_cast<int32_t>(static_cast<int64_t>(kLSDAVA) -
                                 static_cast<int64_t>(LSDASlotVA)));
    B.uleb(Aug.size());
    for (uint8_t Byte : Aug.data())
      B.u8(Byte);
  }
  B.u8(0x41); // DW_CFA_advance_loc 1
  while ((B.size() - FDEBodyStart) % 8 != 0)
    B.u8(0);
  B.patch32(FDELengthSlot, static_cast<uint32_t>(B.size() - FDEBodyStart));

  B.u32(0); // section terminator
  return B.data();
}

//===----------------------------------------------------------------------===//
// `.gcc_except_table`
//===----------------------------------------------------------------------===//

/// One protected region and the pad it reaches.
struct SiteSpec {
  uint32_t Start = 0;
  uint32_t Length = 0x10;
  uint32_t Pad = 0;
  /// 1-based action-table index, as a call site spells it.  Zero is a pad with
  /// no action at all, which is pure cleanup.
  uint64_t Action = 0;
};

/// An LSDA whose type table holds \p Slots, read as `DW_EH_PE_absptr`, and
/// whose action table holds one single-link catch record per slot: record
/// `2 * (I - 1)` selects slot `I`.  That is the shape clang emits for a `@try`
/// with one `@catch` clause per type.
inline std::vector<uint8_t> buildLSDA(const std::vector<va_t> &Slots,
                               const std::vector<SiteSpec> &Sites) {
  ByteBuilder B;
  B.u8(0xff); // landing-pad base defaults to the function start
  B.u8(0x00); // DW_EH_PE_absptr type-table entries

  ByteBuilder Body;
  {
    ByteBuilder CallSites;
    for (const SiteSpec &Site : Sites) {
      CallSites.u32(Site.Start);
      CallSites.u32(Site.Length);
      CallSites.u32(Site.Pad);
      CallSites.uleb(Site.Action);
    }
    Body.u8(0x03); // DW_EH_PE_udata4 call sites
    Body.uleb(CallSites.size());
    for (uint8_t Byte : CallSites.data())
      Body.u8(Byte);

    for (size_t I = 1; I <= Slots.size(); ++I) {
      Body.sleb(static_cast<int64_t>(I)); // catch on slot I
      Body.sleb(0);                       // end of chain
    }
  }

  // Entries grow downward from the base, so slot N is emitted first and the
  // base lands just past slot 1.
  const size_t TypeTableBaseOffset = Body.size() + 8 * Slots.size();
  B.uleb(TypeTableBaseOffset);
  const size_t AfterOffsetField = B.size();
  for (uint8_t Byte : Body.data())
    B.u8(Byte);
  for (size_t I = Slots.size(); I >= 1; --I)
    B.u64(Slots[I - 1]);
  EXPECT_EQ(AfterOffsetField + TypeTableBaseOffset, B.size());
  return B.data();
}

//===----------------------------------------------------------------------===//
// Objective-C fixtures
//===----------------------------------------------------------------------===//

/// Write an Apple `objc_typeinfo` at \ref kDescriptorVA.
///
/// The record is `{ const void **vtable; const char *name; Class cls; }`, and
/// its first two fields are laid out as `std::type_info`'s are precisely so
/// that one type table can hold both an Objective-C class and a C++ type.
inline void writeAppleDescriptor(BinaryImage &Img, const char *ClassName) {
  if (ClassName) {
    ByteBuilder Name;
    Name.str(ClassName);
    writeData(Img, kStringVA, Name.data());
  }
  ByteBuilder Desc;
  Desc.u64(kDataVA + 0x7f0);        // vtable: objc_ehtype_vtable + 2
  Desc.u64(ClassName ? kStringVA : 0);
  Desc.u64(kClassVA);
  writeData(Img, kDescriptorVA, Desc.data());
}

/// Write a bare class-name string at \ref kStringVA, which is the whole of
/// what a GNU-runtime type-table slot holds.
inline void writeNameString(BinaryImage &Img, const char *Text) {
  ByteBuilder Name;
  Name.str(Text);
  writeData(Img, kStringVA, Name.data());
}

/// Place a direct `call rel32` to \p TargetVA at \p SiteVA.
inline void writeCall(BinaryImage &Img, va_t SiteVA, va_t TargetVA) {
  ByteBuilder Call;
  Call.u8(0xe8);
  Call.i32(static_cast<int32_t>(static_cast<int64_t>(TargetVA) -
                               static_cast<int64_t>(SiteVA + 5)));
  writeData(Img, SiteVA, Call.data());
}

/// An image with one function whose frame installs \p Personality and whose
/// LSDA holds \p Slots and \p Sites.
inline BinaryImage makeObjCImage(const char *Personality,
                          const std::vector<va_t> &Slots,
                          const std::vector<SiteSpec> &Sites) {
  BinaryImage Img = makeImage();
  const va_t PersonalityVA = kTextVA + 0x900;
  addSymbol(Img, Personality, PersonalityVA);
  // Every Objective-C image sends messages; without this the runtime gate
  // would be answering the personality's question twice over.
  addSymbol(Img, "objc_msgSend", kTextVA + 0x910);

  const std::vector<uint8_t> Frame = buildFrame(PersonalityVA);
  writeData(Img, kFrameVA, Frame);

  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = kFrameVA;
  EhFrame.Size = Frame.size();
  EhFrame.Data = Frame;
  Img.Sections.push_back(std::move(EhFrame));

  writeData(Img, kLSDAVA, buildLSDA(Slots, Sites));
  return Img;
}

/// Decode \p Img and return its single exception record.
inline const ExceptionFunction &decode(BinaryImage &Img) {
  Img.ExceptionMetadata.Runtime = detectLanguageRuntime(Img);
  dwarf_eh::parseItaniumExceptions(Img);
  objc_eh::parseObjCExceptions(Img);
  EXPECT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  return Img.ExceptionMetadata.Functions.front();
}

//===----------------------------------------------------------------------===//
// Personality identification
//===----------------------------------------------------------------------===//

} // namespace neverd::objc_eh_test

#endif // NEVERD_UNITTESTS_LIFT_LANGUAGE_OBJCEHTESTSDETAIL_H
