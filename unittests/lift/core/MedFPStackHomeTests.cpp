//===- MedFPStackHomeTests.cpp - FP stack-home closure
//---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Writable homes propagate through overlapping scalar stack-slot pairs.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedCallingConvDetail.h"

#include <algorithm>
#include <optional>
#include <set>

namespace {

using namespace neverd;
using med_calling_conv_detail::preserveFPStackHomes;

MedOp fpRead(int64_t Offset, unsigned Bytes, bool Scalar, Arch Architecture) {
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output.Kind = MedVar::Temp;
  Op.Output.Size = Bytes + 4;
  Op.Output.TheArch = Architecture;
  Op.addInput(MedVar::makeConst(unsigned(Intrinsic::X86FPArithMemoryState), 2));
  Op.addInput(MedVar::makeConst(Offset, 8));
  const unsigned Control = (Scalar ? 16 : 0) | (Bytes == 8 ? 8 : 0);
  Op.addInput(MedVar::makeConst(Control, 1));
  Op.addInput(MedVar::makeConst(0, Bytes));
  Op.addInput(MedVar::makeConst(0x1f80, 4));
  return Op;
}

TEST(MedFPStackHomes, OverlappingPairsPropagateWithoutRepeatedRootQueries) {
  for (bool Reverse : {false, true})
    for (int Seed : {0, 12}) {
      SCOPED_TRACE(testing::Message() << Reverse << ':' << Seed);
      MedFunc F;
      F.Blocks.resize(2);
      for (int I = 0; I < 12; ++I)
        F.Blocks[I % 2].Ops.push_back(fpRead(4 + I * 4, 8, true, Arch::X86));
      if (Reverse) {
        std::reverse(F.Blocks.begin(), F.Blocks.end());
        for (auto &Block : F.Blocks)
          std::reverse(Block.Ops.begin(), Block.Ops.end());
      }
      unsigned Queries = 0;
      auto trace = [&](const MedVar &Address) -> std::optional<int64_t> {
        ++Queries;
        return int64_t(Address.ConstVal);
      };
      std::set<int64_t> Homes = {4 + Seed * 4};

      preserveFPStackHomes(F, 4, 4, trace, Homes);

      std::set<int64_t> Expected;
      for (int I = 0; I <= 12; ++I)
        Expected.insert(4 + I * 4);
      EXPECT_EQ(Homes, Expected);
      EXPECT_LE(Queries, 12u);
    }
}

TEST(MedFPStackHomes, AdjacentComponentsAndUnalignedSeedsStayIndependent) {
  MedFunc F;
  F.Blocks.resize(1);
  auto &Ops = F.Blocks[0].Ops;
  Ops.push_back(fpRead(4, 8, true, Arch::X86));
  Ops.push_back(fpRead(12, 8, true, Arch::X86));
  Ops.push_back(fpRead(8, 4, true, Arch::X86));
  Ops.push_back(fpRead(12, 4, true, Arch::X86));
  auto trace = [](const MedVar &Address) -> std::optional<int64_t> {
    return int64_t(Address.ConstVal);
  };
  for (auto Seed : {std::set<int64_t>{}, std::set<int64_t>{5},
                    std::set<int64_t>{5, 8}, std::set<int64_t>{12}}) {
    auto Expected = Seed;
    if (Seed.count(8))
      Expected.insert(4);
    if (Seed.count(12))
      Expected.insert(16);

    preserveFPStackHomes(F, 4, 4, trace, Seed);

    EXPECT_EQ(Seed, Expected);
  }
}

TEST(MedFPStackHomes, ForcedHomesSeedPairsRegardlessOfReadOrder) {
  for (bool Packed : {false, true})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(testing::Message() << Packed << ':' << Reverse);
      MedFunc F;
      F.Blocks.resize(1);
      auto &Ops = F.Blocks[0].Ops;
      for (int Offset : {4, 8, 12})
        Ops.push_back(fpRead(Offset, 8, true, Arch::X86));
      // Packed reads retain their memory contract. An unaligned scalar read
      // spanning slots also requires its physical homes.
      Ops.push_back(
          fpRead(Packed ? 16 : 18, Packed ? 16 : 8, !Packed, Arch::X86));
      Ops.push_back(fpRead(36, 8, true, Arch::X86));
      if (Reverse)
        std::reverse(Ops.begin(), Ops.end());
      auto trace = [](const MedVar &Address) -> std::optional<int64_t> {
        return int64_t(Address.ConstVal);
      };
      std::set<int64_t> Homes;

      preserveFPStackHomes(F, 4, 4, trace, Homes);

      std::set<int64_t> Expected;
      for (int Offset = 4; Offset <= (Packed ? 28 : 24); Offset += 4)
        Expected.insert(Offset);
      EXPECT_EQ(Homes, Expected);
    }
}

TEST(MedFPStackHomes, OnlyAuthenticatedAndResolvedMemoryReadsSeedHomes) {
  for (auto Architecture : {Arch::X86, Arch::X64})
    for (int64_t Base : {4, 8, 40}) {
      SCOPED_TRACE(testing::Message() << int(Architecture) << ':' << Base);
      const unsigned Slot = Architecture == Arch::X86 ? 4 : 8;
      MedFunc F;
      F.Blocks.resize(1);
      auto &Ops = F.Blocks[0].Ops;
      Ops.push_back(fpRead(Base, 4, true, Architecture));
      Ops.push_back(fpRead(Base + 32, 16, false, Architecture));
      auto OtherSpace = fpRead(Base + 64, 16, false, Architecture);
      OtherSpace.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      Ops.push_back(OtherSpace);
      auto Malformed = fpRead(Base + 96, 16, false, Architecture);
      Malformed.Output.Size = 0;
      Ops.push_back(Malformed);
      Ops.push_back(fpRead(Base + 128, 16, false, Architecture));
      unsigned Queries = 0;
      auto trace = [&](const MedVar &Address) -> std::optional<int64_t> {
        ++Queries;
        if (int64_t(Address.ConstVal) == Base + 128)
          return std::nullopt;
        return int64_t(Address.ConstVal);
      };
      std::set<int64_t> Homes;

      preserveFPStackHomes(F, Base, Slot, trace, Homes);

      std::set<int64_t> Expected;
      for (unsigned Offset = 32; Offset < 48; Offset += Slot)
        Expected.insert(Base + Offset);
      EXPECT_EQ(Homes, Expected);
      EXPECT_LE(Queries, 3u);
    }
}

} // namespace
