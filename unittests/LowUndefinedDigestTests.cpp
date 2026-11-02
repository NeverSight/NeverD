//===- LowUndefinedDigestTests.cpp - Operation identity bytes -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/LowUndefinedEffects.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

using namespace neverd;

namespace {

NdVar variable(uint64_t Index) {
  return {static_cast<VnodeSpace>((3 * Index + 1) % 5),
          UINT64_C(0x8123456789abcdef) ^ (Index * UINT64_C(0x102030405)),
          static_cast<uint16_t>((11 * Index + 3) % 65536),
          static_cast<ConstantAddressProvenance>((Index + 2) % 6),
          UINT64_C(0xfedcba9876543210) ^ (Index * UINT64_C(0x20304050607))};
}

LowOp operation(unsigned Index) {
  LowOp Op;
  // Identity covers stored bits, including unused slots. This fixture does
  // not claim that arbitrary opcode or ordering values are valid semantics.
  Op.Opcode = static_cast<NdOp>((0x81 + 13 * Index) % 256);
  Op.MemoryOrdering = static_cast<NdMemoryOrdering>((1 + 3 * Index) % 256);
  Op.MemoryAddressSpace =
      static_cast<NdMemoryAddressSpace>((2 + 7 * Index) % 256);
  Op.Output = variable(7 * Index);
  Op.NumInputs = Index % 7;
  for (unsigned Slot = 0; Slot != kLowUndefinedV1InputSlots; ++Slot)
    Op.Inputs[Slot] = variable(7 * Index + Slot + 1);
  Op.Addr = UINT64_C(0xefcdab8967452301) ^ (Index * UINT64_C(0x100010001));
  Op.Seq = -1 - static_cast<int>(Index * 17);
  return Op;
}

std::vector<LowOp> operations(unsigned Count) {
  std::vector<LowOp> Ops;
  for (unsigned I = 0; I != Count; ++I)
    Ops.push_back(operation(I));
  return Ops;
}

std::string digest(const LowOp &Op) {
  return lowUndefinedOperationDigest(llvm::ArrayRef<LowOp>(&Op, 1));
}

TEST(LowUndefinedDigest, IndependentEncodingVectors) {
  // Generated with Python hashlib over the literal v1 domain followed by
  // struct.pack('<Q', word) for each specified word. They cover inline-buffer
  // growth (3/4 operations) and the bounded/streaming boundary (199/200).
  const struct {
    unsigned Count;
    const char *Expected;
  } Cases[] = {
      {0, "0277bf13b2ddefb2a9939587f498a5c3657ccd7067496513d5ca9170ba16ab66"},
      {1, "36b8d47ac0ec83ffe3a28c23f88c4c935a5826c16ab5ec95a82d1fe05832b03e"},
      {2, "f07f952fc0f7e2691414c08116341cf2027cd609e3c8bd2502d5409275035a85"},
      {3, "485806ac3b3160175cd5b2917ce59916ec6be008f13fdf8d7201dd575358be1a"},
      {4, "74902ad42958abfb18655d05231630da97e70a7f0a1b3a45b0f2f7fcca666508"},
      {198, "dc9ba207663199bafc3e22d76c0fb59f0cb19d8d721bcf52c68b1c3e516fb128"},
      {199, "5b62a60585490150bd25378ae89e182601f863d07c3c3f845a88c779b8cccbf5"},
      {200, "d9a4ba51e5db985ce6b2cc72b5e9e0c15488c440591a4c5f6195350f1dd5530c"},
      {201, "90d1236916c623b5c01c6b6e4add2355911bc402ad6b6ca73bc7a237e73d7b43"},
      {512, "97b357839656ec32a4e6af69be5698312884713b50c3e675f21b0daad06ad8e5"},
      {4096,
       "0bf3673baca04a506f1dc42b1f84e77e64a1f9084c2ca9b2fdf5c00637624a78"},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Count);
    const auto Ops = operations(Case.Count);
    EXPECT_EQ(lowUndefinedOperationDigest(Ops), Case.Expected);
  }
}

TEST(LowUndefinedDigest, SignExtendsTheSequenceWord) {
  LowOp Op = operation(0);
  Op.Seq = std::numeric_limits<int>::min();
  EXPECT_EQ(digest(Op),
            "7981bcfeb4774f8b96cfd788ade16991def1b15904f4cf251f0249598d919548");
  Op.Seq = std::numeric_limits<int>::max();
  EXPECT_EQ(digest(Op),
            "df06e52e8a103e261fe883a5b365ccc231806dfef745fa510c03aa8c738473a3");
}

TEST(LowUndefinedDigest, BindsEveryStoredFieldIncludingUnusedInputs) {
  const LowOp Original = operation(0);
  ASSERT_EQ(Original.NumInputs, 0);
  const auto Expected = digest(Original);
  unsigned Changes = 0;
  const auto ChangedDigest = [&](auto Change) {
    SCOPED_TRACE(Changes++);
    LowOp Changed = Original;
    Change(Changed);
    EXPECT_NE(digest(Changed), Expected);
  };
  ChangedDigest([](LowOp &Op) { Op.Opcode = NdOp::NOP; });
  ChangedDigest([](LowOp &Op) { Op.MemoryOrdering = NdMemoryOrdering::None; });
  ChangedDigest(
      [](LowOp &Op) { Op.MemoryAddressSpace = NdMemoryAddressSpace::Default; });
  ChangedDigest([](LowOp &Op) { Op.NumInputs = 6; });
  ChangedDigest([](LowOp &Op) { Op.Addr ^= UINT64_C(0x8000000000000000); });
  ChangedDigest([](LowOp &Op) { Op.Seq = 0; });
  for (unsigned Slot = 0; Slot != 7; ++Slot) {
    const auto Variable = [&](LowOp &Op) -> NdVar & {
      return Slot ? Op.Inputs[Slot - 1] : Op.Output;
    };
    ChangedDigest([&](LowOp &Op) {
      auto &V = Variable(Op);
      V.Space =
          static_cast<VnodeSpace>((static_cast<unsigned>(V.Space) + 1) % 5);
    });
    ChangedDigest([&](LowOp &Op) { Variable(Op).Offset ^= UINT64_C(1) << 63; });
    ChangedDigest([&](LowOp &Op) { Variable(Op).Size ^= 0x8000; });
    ChangedDigest([&](LowOp &Op) {
      auto &V = Variable(Op);
      V.Provenance = static_cast<ConstantAddressProvenance>(
          (static_cast<unsigned>(V.Provenance) + 1) % 6);
    });
    ChangedDigest(
        [&](LowOp &Op) { Variable(Op).AddressOwnerVA ^= UINT64_C(1) << 63; });
  }
  EXPECT_EQ(Changes, 41U);
}

TEST(LowUndefinedDigest, RetainsOrderCountAndTheLastWord) {
  for (unsigned Count : {2U, 3U, 4U, 198U, 199U, 200U, 201U, 512U}) {
    SCOPED_TRACE(Count);
    auto Ops = operations(Count);
    const auto Expected = lowUndefinedOperationDigest(Ops);
    std::swap(Ops.front(), Ops.back());
    EXPECT_NE(lowUndefinedOperationDigest(Ops), Expected);
    std::swap(Ops.front(), Ops.back());
    EXPECT_EQ(lowUndefinedOperationDigest(Ops), Expected);
    Ops.back().Seq ^= 1;
    EXPECT_NE(lowUndefinedOperationDigest(Ops), Expected);
    Ops.back().Seq ^= 1;
    Ops.back().Inputs[5].AddressOwnerVA ^= 1;
    EXPECT_NE(lowUndefinedOperationDigest(Ops), Expected);
    Ops.pop_back();
    EXPECT_NE(lowUndefinedOperationDigest(Ops), Expected);
  }
}

TEST(LowUndefinedDigest, LeavesInputStorageUnchanged) {
  for (unsigned Count : {1U, 3U, 4U, 199U, 200U, 4096U}) {
    SCOPED_TRACE(Count);
    const auto Ops = operations(Count);
    std::vector<unsigned char> Before(Ops.size() * sizeof(LowOp));
    std::memcpy(Before.data(), Ops.data(), Before.size());
    EXPECT_EQ(lowUndefinedOperationDigest(Ops).size(), 64U);
    EXPECT_EQ(std::memcmp(Before.data(), Ops.data(), Before.size()), 0);
  }
}

TEST(LowUndefinedDigest, IgnoresObjectPadding) {
  LowOp Left = operation(0), Right = Left;
  bool FieldBytes[sizeof(LowOp)] = {};
  const auto Mark = [&](const auto &Field) {
    const auto Offset = reinterpret_cast<const unsigned char *>(&Field) -
                        reinterpret_cast<const unsigned char *>(&Left);
    std::fill_n(FieldBytes + Offset, sizeof(Field), true);
  };
  Mark(Left.Opcode);
  Mark(Left.MemoryOrdering);
  Mark(Left.MemoryAddressSpace);
  Mark(Left.NumInputs);
  Mark(Left.Addr);
  Mark(Left.Seq);
  const auto MarkVariable = [&](const NdVar &V) {
    Mark(V.Space);
    Mark(V.Offset);
    Mark(V.Size);
    Mark(V.Provenance);
    Mark(V.AddressOwnerVA);
  };
  MarkVariable(Left.Output);
  for (const auto &V : Left.Inputs)
    MarkVariable(V);
  auto *LeftBytes = reinterpret_cast<unsigned char *>(&Left);
  auto *RightBytes = reinterpret_cast<unsigned char *>(&Right);
  for (size_t I = 0; I != sizeof(LowOp); ++I) {
    if (FieldBytes[I])
      continue;
    LeftBytes[I] = 0xa5;
    RightBytes[I] = 0x5a;
  }
  EXPECT_EQ(digest(Left), digest(Right));
}

TEST(LowUndefinedDigest, ExtendedSlotsUseIndependentV2Identities) {
  const struct {
    unsigned Count;
    const char *Expected;
  } Cases[] = {
#define LOW_UNDEFINED_V2_CASE(Count, Expected) {Count, Expected},
#include "LowUndefinedDigestFixtures.def"
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Count);
    auto Ops = operations(Case.Count);
    for (unsigned I = 0; I != Ops.size(); ++I)
      for (unsigned N = kLowUndefinedV1InputSlots;
           N != std::size(Ops[I].Inputs); ++N)
        Ops[I].Inputs[N] = variable(7 * I + N + 1);
    EXPECT_EQ(lowUndefinedOperationDigest(Ops), Case.Expected);
    const auto Expected = lowUndefinedOperationDigest(Ops);
    Ops.back().Inputs[std::size(Ops.back().Inputs) - 1].AddressOwnerVA ^= 1;
    EXPECT_NE(lowUndefinedOperationDigest(Ops), Expected);
  }
}

TEST(LowUndefinedDigest, DefaultExtensionsRetainV1AndStoredBitsSelectV2) {
  auto Op = operation(0);
  const auto Legacy = digest(Op);
  for (unsigned N = kLowUndefinedV1InputSlots; N != std::size(Op.Inputs); ++N) {
    SCOPED_TRACE(N);
    Op.Inputs[N].Offset = 1;
    EXPECT_NE(digest(Op), Legacy);
    Op.Inputs[N] = NdVar{};
    EXPECT_EQ(digest(Op), Legacy);
  }
  Op.NumInputs = kLowUndefinedV1InputSlots + 1;
  EXPECT_NE(digest(Op), Legacy);
  Op.NumInputs = 0;
  EXPECT_EQ(digest(Op), Legacy);
}

} // namespace
