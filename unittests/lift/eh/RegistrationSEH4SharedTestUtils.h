//===- RegistrationSEH4SharedTestUtils.h -----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Build an independent PE32 shared SEH4 instruction and scope-table fixture.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONSEH4SHAREDTESTUTILS_H
#define NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONSEH4SHAREDTESTUTILS_H

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace neverd::registration_test {
enum class SharedSEH4Body { Plain, OrdinaryCall, FilteredCall, ThreadRead };

inline BinaryImage sharedSEH4Image(uint32_t Base, bool Wide,
                                   uint32_t Allocation = 32,
                                   SharedSEH4Body Kind = SharedSEH4Body::Plain,
                                   uint16_t ParentPop = 0) {
  const bool CallInBody = Kind == SharedSEH4Body::OrdinaryCall ||
                          Kind == SharedSEH4Body::FilteredCall;
  const bool Filtered = Kind == SharedSEH4Body::FilteredCall;
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Base;
  Image.DynInfo.SecurityCookieRVA = 0x2080;
  for (bool Code : {true, false}) {
    Segment Segment;
    Segment.VA = Base + (Code ? 0x1000 : 0x2000);
    Segment.Size = Segment.FileSz = 0x400;
    Segment.Flags = SegmentFlags::Readable |
                    (Code ? SegmentFlags::Executable : SegmentFlags::None);
    Segment.Data.resize(Segment.Size, Code ? 0xcc : 0);
    Section Section;
    Section.VA = Segment.VA;
    Section.Size = Section.FileSz = Segment.Size;
    Section.Flags = Segment.Flags;
    Image.Segments.push_back(std::move(Segment));
    Image.Sections.push_back(std::move(Section));
  }
  auto &Text = Image.Segments.front().Data;
  auto Put = [&](size_t At, std::initializer_list<uint8_t> Bytes) {
    std::copy(Bytes.begin(), Bytes.end(), Text.begin() + At);
  };
  auto Word = [&](size_t At, uint32_t Value) {
    writeLE<uint32_t>(Text.data() + At, Value);
  };
  auto Call = [&](size_t At, size_t Target) {
    Text[At] = 0xe8;
    Word(At + 1, uint32_t(Target - At - 5));
  };
  Text[0] = Wide ? 0x68 : 0x6a;
  if (Wide)
    Word(1, Allocation);
  else
    Text[1] = uint8_t(Allocation);
  const size_t Push = Wide ? 5 : 2;
  Text[Push] = 0x68;
  Word(Push + 1, Base + 0x2100);
  Call(Push + 5, 0x100);
  const size_t Body = Push + 10;
  Put(Body, {0xc7, 0x45, 0xfc, 0, 0, 0, 0});
  if (CallInBody)
    Call(Body + 7, 0x180);
  else
    Text[Body + 7] = 0x90;
  size_t Finish = Body + (CallInBody ? 12 : 8);
  if (Kind == SharedSEH4Body::ThreadRead) {
    Put(Body + 7, {0x64, 0xa1, 0x18, 0, 0, 0, 0x8b, 0x40, 4});
    Finish = Body + 16;
  }
  if (Filtered) {
    Put(Body + 12,
        {0xc7, 0x45, 0xfc, 0xfe, 0xff, 0xff, 0xff, 0xb8, 9, 0, 0, 0, 0xeb, 15});
    Put(Body + 26, {0x8b, 0x45, 0xfc, 0x83, 0xc0, 1, 0xc3});
    Put(Body + 33, {0x8b, 0x65, 0xe8, 0xb8, 7, 0, 0, 0});
    Finish = Body + 41;
  }
  Put(Finish, {0xc7, 0x45, 0xfc, 0xfe, 0xff, 0xff, 0xff, 0x31, 0xc0});
  if (Kind == SharedSEH4Body::ThreadRead)
    Put(Finish + 7, {0x89, 0xc0}); // Return the actual StackBase word in EAX.
  Call(Finish + 9, 0x150);
  Text[Finish + 14] = ParentPop ? 0xc2 : 0xc3;
  if (ParentPop)
    writeLE<uint16_t>(Text.data() + Finish + 15, ParentPop);
  Put(0x80, {0xb8, 1, 0, 0, 0, 0xc3});
  Put(0x180, {0x31, 0xc0, 0xc3});

  // Save the caller EBP in its allocation word, move the helper return PC,
  // encode the table and publish the registration only after initialization.
  Put(0x100,
      {0x68, 0,    0,    0,    0,    0x64, 0xff, 0x35, 0,    0,    0,    0,
       0x8b, 0x44, 0x24, 0x10, 0x89, 0x6c, 0x24, 0x10, 0x8d, 0x6c, 0x24, 0x10,
       0x2b, 0xe0, 0x53, 0x56, 0x57, 0xa1, 0,    0,    0,    0,    0x31, 0x45,
       0xfc, 0x33, 0xc5, 0x50, 0x89, 0x65, 0xe8, 0xff, 0x75, 0xf8, 0x8b, 0x45,
       0xfc, 0xc7, 0x45, 0xfc, 0xfe, 0xff, 0xff, 0xff, 0x89, 0x45, 0xf8, 0x8d,
       0x45, 0xf0, 0x64, 0xa3, 0,    0,    0,    0,    0xc3});
  Word(0x101, Base + 0x1200);
  Word(0x11e, Base + 0x2080);
  Put(0x150, {0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0,    0,    0,    0,
              0x59, 0x5f, 0x5f, 0x5e, 0x5b, 0x8b, 0xe5, 0x5d, 0x51, 0xc3});

  Put(0x200, {0x55, 0x8b, 0xec, 0xff, 0x75, 0x14, 0xff, 0x75, 0x10,
              0xff, 0x75, 0x0c, 0xff, 0x75, 0x08, 0x68, 0,    0,
              0,    0,    0x68, 0,    0,    0,    0});
  Word(0x210, Base + 0x1280);
  Word(0x215, Base + 0x2080);
  Call(0x219, 0x290);
  Put(0x21e, {0x83, 0xc4, 0x18, 0x5d, 0xc3});
  Text[0x280] = 0xc3;
  Put(0x290, {0xff, 0x25, 0, 0, 0, 0});
  Word(0x292, Base + 0x2008);
  Image.Imports.push_back(
      {"msvcr100.dll", "_except_handler4_common", 0, Base + 0x2008});
  auto &Data = Image.Segments.back().Data;
  const uint32_t Words[] = {uint32_t(-2),
                            0,
                            uint32_t(-32 - Allocation),
                            0,
                            uint32_t(-2),
                            Filtered ? Base + uint32_t(0x1000 + Body + 26) : 0,
                            Filtered ? Base + uint32_t(0x1000 + Body + 33)
                                     : Base + 0x1080};
  for (size_t I = 0; I != std::size(Words); ++I)
    writeLE<uint32_t>(Data.data() + 0x100 + I * 4, Words[I]);
  for (uint32_t RVA : {0x1000, 0x1080, 0x1100, 0x1150, 0x1200, 0x1280, 0x1290})
    Image.Symbols.push_back(Symbol::makeFunc(Base + RVA));
  if (CallInBody)
    Image.Symbols.push_back(Symbol::makeFunc(Base + 0x1180));
  coff_loader::parseX86RegistrationExceptions(Image);
  return Image;
}

inline LowFunc liftSharedSEH4(BinaryImage &Image) {
  Decoder Decoder;
  EXPECT_TRUE(Decoder.init(Image));
  return CFGBuilder().build(Image, Decoder, Image.Base + 0x1000, "shared-seh4");
}
} // namespace neverd::registration_test

#endif // NEVERD_UNITTESTS_LIFT_EH_REGISTRATIONSEH4SHAREDTESTUTILS_H
