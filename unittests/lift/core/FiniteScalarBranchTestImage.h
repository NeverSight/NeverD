//===- FiniteScalarBranchTestImage.h - Arithmetic dispatch ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently authored instruction streams with no address-table loads.

#ifndef NEVERD_UNITTESTS_LIFT_CORE_FINITESCALARBRANCHTESTIMAGE_H
#define NEVERD_UNITTESTS_LIFT_CORE_FINITESCALARBRANCHTESTIMAGE_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace finite_scalar_branch_test {

inline neverd::BinaryImage image(neverd::Arch Architecture,
                                 neverd::BinaryFormat Format,
                                 bool Reverse = false) {
  using namespace neverd;
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Format = Format;
  Image.Base = Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (Architecture == Arch::AArch64) {
    Text.Data = {0x09, 0x04, 0x00, 0x12,  // and w9,w0,3
                 0xea, 0x03, 0x00, 0x10,  // adr x10,0x1080
                 0x4a, 0x0d, 0x09, 0x8b,  // add x10,x10,x9,lsl 3
                 0x40, 0x01, 0x1f, 0xd6}; // br x10
  } else {
    Text.Data = {0x89, uint8_t(Format == BinaryFormat::COFF ? 0xc8 : 0xf8),
                 0x83, 0xe0, 3}; // mov eax,arg; and eax,3
    if (Reverse)
      Text.Data.insert(Text.Data.end(), {0xf7, 0xd8, 0x83, 0xc0, 3});
    if (Architecture == Arch::X64) {
      const uint8_t Disp = 0x80 - (Text.Data.size() + 7);
      Text.Data.insert(Text.Data.end(), {0x4c, 0x8d, 0x1d, Disp, 0, 0, 0, 0x4d,
                                         0x8d, 0x1c, 0xc3, 0x41, 0xff, 0xe3});
    } else
      Text.Data.insert(Text.Data.end(), {0xc1, 0xe0, 3, // shl eax,3
                                         0x05, 0x80, 0x10, 0, 0, 0xff,
                                         0xe0}); // add eax,0x1080; jmp
  }
  Text.Data.resize(0x80, Architecture == Arch::AArch64 ? 0 : 0xcc);
  for (uint8_t Value : {17, 29, 43, 61}) {
    if (Architecture == Arch::AArch64) {
      const size_t At = Text.Data.size();
      Text.Data.resize(At + 8);
      writeLE<uint32_t>(Text.Data.data() + At, 0x52800000 | (Value << 5));
      writeLE<uint32_t>(Text.Data.data() + At + 4, 0xd65f03c0);
    } else
      Text.Data.insert(Text.Data.end(),
                       {0xb8, Value, 0, 0, 0, 0xc3, 0x90, 0x90});
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  auto Function = Symbol::makeFunc(Image.Entry);
  Function.Name = "finite_dispatch_probe";
  Function.Size = Image.Segments.front().Size;
  Image.Symbols.push_back(std::move(Function));
  return Image;
}

inline neverd::BinaryImage joinedImage(neverd::Arch Architecture,
                                       neverd::BinaryFormat Format) {
  using namespace neverd;
  auto Image = image(Architecture, Format);
  auto &Bytes = Image.Segments.front().Data;
  std::fill(Bytes.begin(), Bytes.begin() + 0x80,
            Architecture == Arch::AArch64 ? 0 : 0xcc);
  const auto Put = [&](size_t At, std::initializer_list<uint8_t> Values) {
    std::copy(Values.begin(), Values.end(), Bytes.begin() + At);
  };
  if (Architecture == Arch::AArch64) {
    Put(0, {0x80, 0, 0, 0xb4,                     // cbz x0,0x1010
            0xf0, 3, 0, 0x10,                     // adr x16,0x1080
            6, 0, 0, 0x14});                      // b 0x1020
    Put(0x10, {0xd0, 3, 0, 0x10, 3, 0, 0, 0x14}); // adr x16,0x1088; b
    Put(0x20, {0, 2, 0x1f, 0xd6});                // br x16
  } else {
    Put(0, {0x85, uint8_t(Format == BinaryFormat::COFF ? 0xc9 : 0xff), 0x74,
            0x0c, 0x4c, 0x8d, 0x1d, 0x75, 0, 0, 0, 0xeb, 0x13});
    Put(0x10, {0x4c, 0x8d, 0x1d, 0x71, 0, 0, 0, 0xeb, 7});
    Put(0x20, {0x41, 0xff, 0xe3});
  }
  return Image;
}

} // namespace finite_scalar_branch_test

#endif // NEVERD_UNITTESTS_LIFT_CORE_FINITESCALARBRANCHTESTIMAGE_H
