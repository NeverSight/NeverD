//===- RegistrationRethrowTests.cpp - PE32 rethrow ABI --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise private and direct PE32 runtime throw-entry proofs.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/support/BinaryEncoding.h"

using namespace neverd;

namespace {
struct RethrowImage {
  static constexpr va_t Entry = 0x401000;
  static constexpr va_t Stub = Entry + 0x80;
  static constexpr va_t Data = 0x403000;
  BinaryImage Image;
  va_t Call = 0;

  explicit RethrowImage(std::vector<uint8_t> Arguments = {0x6a, 0, 0x6a, 0}) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = Entry;
    Segment Text;
    Text.Name = ".text";
    Text.VA = Entry;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = {0x55, 0x8b, 0xec, 0x8b, 0x45, 4, 0xa3, 0, 0x30, 0x40, 0};
    Text.Data.insert(Text.Data.end(), Arguments.begin(), Arguments.end());
    Call = Entry + Text.Data.size();
    Text.Data.push_back(0xe8);
    const auto Displacement = Text.Data.size();
    Text.Data.resize(Displacement + 4);
    writeLE<uint32_t>(Text.Data.data() + Displacement, Stub - (Call + 5));
    Text.Data.resize(0x86, 0xcc);
    Text.Data[0x80] = 0xff;
    Text.Data[0x81] = 0x25;
    writeLE<uint32_t>(Text.Data.data() + 0x82, Data + 4);
    Text.Size = Text.Data.size();
    Image.Segments.push_back(std::move(Text));
    Segment Storage;
    Storage.Name = ".data";
    Storage.VA = Data;
    Storage.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Storage.Data.resize(8);
    Storage.Size = 8;
    Image.Segments.push_back(std::move(Storage));
    Import Runtime;
    Runtime.Module = "vcruntime140.dll";
    Runtime.Name = "_CxxThrowException";
    Runtime.IATAddr = Data + 4;
    Image.Imports.push_back(std::move(Runtime));
  }
};

TEST(RegistrationRethrowABI, RequiresBothNullArgumentsAndTheExactRuntime) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    RethrowImage F;
    if (Mutation == 1)
      F = RethrowImage({0x6a, 1, 0x6a, 0});
    if (Mutation == 2)
      F = RethrowImage({0x6a, 0, 0x6a, 1});
    if (Mutation == 3)
      F = RethrowImage({0x6a, 0, 0x55});
    if (Mutation == 4)
      F = RethrowImage({0x6a, 0, 0x50});
    if (Mutation == 5)
      F.Image.Imports.front().Module = "other.dll";
    if (Mutation == 6)
      F.Image.Imports.front().Name = "CxxThrowException";
    if (Mutation == 7)
      F.Image.Segments.front().Data[0x81] = 0x15;
    if (Mutation == 8)
      F.Image.Segments.back().Data.resize(4);
    auto ABI = getCheckedX86RegistrationThrowCalleeABI(F.Image, F.Entry);
    ASSERT_EQ(bool(ABI), Mutation == 0);
    if (!ABI)
      continue;
    EXPECT_TRUE(ABI->IsRethrow);
    EXPECT_EQ(ABI->ThrowInfo.Address, InvalidVA);
    EXPECT_EQ(ABI->ThrowCallVA, F.Call);
    EXPECT_EQ(ABI->ImportVA, F.Stub);
    ASSERT_EQ(ABI->CallerPCWrites.size(), 1u);
    EXPECT_EQ(ABI->CallerPCWrites.front().Begin, F.Data);
    EXPECT_EQ(ABI->CallerPCWrites.front().End, F.Data + 4);
    ASSERT_EQ(ABI->CodeRanges.size(), 2u);
    EXPECT_EQ(ABI->CodeRanges.front().End, F.Call + 5);
    EXPECT_EQ(ABI->CodeRanges.back().Begin, F.Stub);
  }
}

TEST(RegistrationRethrowABI, DirectRuntimeEntryRequiresExactImportStorage) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    RethrowImage F;
    if (Mutation == 1)
      F.Image.Imports.front().Module = "other.dll";
    if (Mutation == 2)
      F.Image.Imports.front().Name = "CxxThrowException";
    if (Mutation == 3)
      F.Image.Segments.front().Data[0x81] = 0x15;
    if (Mutation == 4)
      F.Image.Segments.back().Data.resize(4);
    if (Mutation == 5)
      F.Image.Bits = Bitness::Bits64;
    if (Mutation == 6)
      F.Image.Segments.front().Data.resize(0x85);
    const auto ABI = getCheckedX86RegistrationThrowImportABI(F.Image, F.Stub);
    EXPECT_EQ(bool(ABI), Mutation == 0);
    if (!ABI)
      continue;
    EXPECT_EQ(ABI->Target, F.Stub);
    EXPECT_EQ(ABI->IATVA, F.Data + 4);
    LowFunc Caller;
    Caller.Blocks.resize(1);
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(F.Stub, 4));
    Caller.Blocks.front().Ops.push_back(Call);
    RegistrationCallCalleeIndex Index(F.Image);
    const auto Contracts = Index.contracts(Caller);
    ASSERT_TRUE(Contracts);
    ASSERT_EQ(Contracts->size(), 1u);
    EXPECT_TRUE(Contracts->front().isRuntimeRethrow());
    EXPECT_TRUE(Contracts->front().isRethrow());
    EXPECT_EQ(Contracts->front().ThrownTypeVA, 0u);
  }
}

TEST(RegistrationRethrowABI, DoesNotInventANewThrownObject) {
  RethrowImage F;
  LowFunc Caller;
  Caller.Blocks.resize(1);
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(F.Entry, 4));
  Caller.Blocks.front().Ops.push_back(Call);
  RegistrationCallCalleeIndex Index(F.Image);
  auto Contracts = Index.contracts(Caller);
  ASSERT_TRUE(Contracts);
  ASSERT_EQ(Contracts->size(), 1u);
  const auto &Contract = Contracts->front();
  EXPECT_TRUE(Contract.isRethrow());
  EXPECT_TRUE(Contract.isThrow());
  EXPECT_TRUE(Contract.DoesNotReturn);
  EXPECT_EQ(Contract.ThrownTypeVA, 0u);
  EXPECT_EQ(Contract.ThrownObjectSize, 0u);
  EXPECT_TRUE(Contract.ECXReads.empty());
  EXPECT_TRUE(Contract.ECXWrites.empty());
}
} // namespace
