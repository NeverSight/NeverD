//===- RegistrationObjectTests.cpp - PE32 trivial object contracts ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstdlib>
#include <set>

namespace {
using namespace neverd;

TEST(RegistrationObject, RequiresOneTrivialIdentityCopy) {
  for (unsigned Mutation = 0; Mutation != 17; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Segment Table;
    Table.VA = 0x2000;
    Table.Flags = SegmentFlags::Readable;
    Table.Data.resize(0x100);
    Table.Size = Table.Data.size();
    auto Word = [&](size_t Offset, uint32_t Value) {
      writeLE<uint32_t>(Table.Data.data() + Offset, Value);
    };
    Word(12, 0x2020);
    Word(0x20, 1);
    Word(0x24, 0x2030);
    Word(0x34, 0x2080);
    Word(0x3c, UINT32_MAX);
    Word(0x44, 12);
    const std::string Name = ".?AUTrivialObject@@";
    std::copy(Name.begin(), Name.end(), Table.Data.begin() + 0x88);
    switch (Mutation) {
    case 1:
      Word(4, 0x1000);
      break;
    case 2:
      Word(8, 0x1000);
      break;
    case 3:
      Word(0x48, 0x1000);
      break;
    case 4:
      Word(0x20, 2);
      break;
    case 5:
      Word(0x30, 2);
      break;
    case 6:
      Word(0x30, 4);
      break;
    case 7:
      Word(0x30, 8);
      break;
    case 8:
      Word(0x30, 16);
      break;
    case 9:
      Word(0x38, 4);
      break;
    case 10:
      Word(0x3c, 0);
      break;
    case 11:
      Word(0x40, 4);
      break;
    case 12:
      Word(0x44, 0);
      break;
    case 13:
      Word(0x44, 0x100001);
      break;
    case 14:
      Table.Flags = Table.Flags | SegmentFlags::Writable;
      break;
    case 15:
      Word(0x30, 1);
      break;
    case 16:
      Word(0x34, 0x2048);
      break;
    }
    Image.Segments.push_back(std::move(Table));
    const auto Info =
        coff_loader::getCheckedX86SimpleCxxThrowInfo(Image, 0x2000);
    ASSERT_EQ(bool(Info), Mutation == 0 || Mutation == 15);
    if (Info) {
      EXPECT_EQ(Info->ObjectSize, 12u);
      EXPECT_EQ(Info->TypeDescriptorVA, 0x2080u);
      EXPECT_EQ(Info->ReadOnlyRanges.size(), 3u);
    }
  }
}

TEST(RegistrationObject, InputPE32BindsEveryObjectByte) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32 to the "
                    "object fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto Entry = llvm::find_if(Image->Exports, [](const auto &Export) {
    return Export.Name == "callback_parent";
  });
  ASSERT_NE(Entry, Image->Exports.end());
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  const auto Low =
      CFGBuilder().build(*Image, Decode, Entry->Addr, "object-parent");
  ASSERT_TRUE(Low.RegistrationStates);
  const auto &States = *Low.RegistrationStates;
  ASSERT_TRUE(States.Complete);
  ASSERT_TRUE(States.CxxCatchObjectsComplete);
  ASSERT_TRUE(States.RuntimeObjectAccessesComplete);
  ASSERT_TRUE(States.CallFrameEffectsComplete);
  ASSERT_EQ(States.CxxCatchObjects.size(), 2u);
  std::set<uint32_t> Sizes;
  for (const auto &Object : States.CxxCatchObjects) {
    EXPECT_TRUE(Object.ObjectSize == 8 || Object.ObjectSize == 12);
    EXPECT_EQ(Object.Reference, Object.TryIndex == 0);
    Sizes.insert(Object.ObjectSize);
  }
  EXPECT_TRUE(Sizes.count(12));
  std::set<int32_t> Reads, Writes;
  for (const auto &Access : States.RuntimeObjectAccesses)
    (Access.Write ? Writes : Reads).insert(Access.Offset);
  EXPECT_TRUE(Reads.count(0) && Reads.count(4) && Reads.count(8));
  EXPECT_TRUE(Writes.count(0) && Writes.count(4));
}
} // namespace
