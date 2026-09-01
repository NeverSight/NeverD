//===- SymConcreteTests.cpp - Exact concrete shadow tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/symbolic/SymConcrete.h"

#include <limits>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (const NdVar &Input : Inputs)
    Result.addInput(Input);
  return Result;
}

std::optional<uint64_t> binaryValue(NdOp Opcode, uint16_t Bytes, uint64_t Left,
                                    uint64_t Right, uint16_t OutputBytes = 0) {
  constexpr uint64_t LeftRegister = 0;
  constexpr uint64_t RightRegister = 8;
  constexpr uint64_t OutputRegister = 16;

  SymExecConcreteShadow Shadow;
  const uint16_t ResultBytes = OutputBytes == 0 ? Bytes : OutputBytes;
  if (!Shadow.reset(llvm::endianness::little) ||
      !Shadow.setRegister(LeftRegister, Bytes, Left) ||
      !Shadow.setRegister(RightRegister, Bytes, Right) ||
      !Shadow.step(op(
          Opcode, NdVar::reg(OutputRegister, ResultBytes),
          {NdVar::reg(LeftRegister, Bytes), NdVar::reg(RightRegister, Bytes)})))
    return std::nullopt;
  return Shadow.value(NdVar::reg(OutputRegister, ResultBytes));
}

TEST(SymExecConcreteShadow, NarrowSignedCompareUsesTheDeclaredWidth) {
  constexpr uint64_t Input = 0;
  constexpr uint64_t Predicate = 16;

  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(Input, 1, 0xFF));
  ASSERT_TRUE(Shadow.step(op(NdOp::INT_SLESS, NdVar::reg(Predicate, 1),
                             {NdVar::reg(Input, 1), NdVar::cst(0, 1)})));

  ASSERT_TRUE(Shadow.value(NdVar::reg(Predicate, 1)).has_value());
  EXPECT_EQ(*Shadow.value(NdVar::reg(Predicate, 1)), 1u);
}

TEST(SymExecConcreteShadow, CoreIntegerSemanticsUseEverySupportedSeedWidth) {
  for (uint16_t Bytes : {uint16_t(1), uint16_t(2), uint16_t(4), uint16_t(8)}) {
    SCOPED_TRACE(Bytes);
    const unsigned Bits = unsigned(Bytes) * 8;
    const uint64_t SignBit = uint64_t(1) << (Bits - 1);
    const uint64_t Mask = Bytes == sizeof(uint64_t)
                              ? std::numeric_limits<uint64_t>::max()
                              : (uint64_t(1) << Bits) - 1;

    EXPECT_EQ(binaryValue(NdOp::INT_EQUAL, Bytes, Mask, Mask, 1), 1u);
    EXPECT_EQ(binaryValue(NdOp::INT_SLESS, Bytes, SignBit, 1, 1), 1u);
    EXPECT_EQ(binaryValue(NdOp::INT_CARRY, Bytes, Mask, 1, 1), 1u);
    EXPECT_EQ(binaryValue(NdOp::INT_SOVF, Bytes, SignBit - 1, 1, 1), 1u);

    EXPECT_EQ(binaryValue(NdOp::INT_DIV, Bytes, Mask, 3), Mask / 3);
    EXPECT_EQ(binaryValue(NdOp::INT_SDIV, Bytes, Mask - 7, 2), Mask - 3)
        << "-8 / 2 == -4";
    EXPECT_EQ(binaryValue(NdOp::INT_REM, Bytes, Mask, 7), Mask % 7);
    EXPECT_EQ(binaryValue(NdOp::INT_SREM, Bytes, Mask - 8, 4), Mask)
        << "-9 % 4 == -1";

    EXPECT_EQ(binaryValue(NdOp::INT_LEFT, Bytes, Mask, 1), Mask - 1);
    EXPECT_EQ(binaryValue(NdOp::INT_RIGHT, Bytes, SignBit, 1), SignBit >> 1);
    EXPECT_EQ(binaryValue(NdOp::INT_ASHR, Bytes, SignBit, 1),
              SignBit | (SignBit >> 1));
  }
}

TEST(SymExecConcreteShadow, SeedsAreCompleteNonOverlappingByteRanges) {
  SymExecConcreteShadow Shadow;

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.setRegister(0, 0, 0));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.setRegister(0, 9, 0));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.setRegister(0, 1, 0x100));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.setRegister(std::numeric_limits<uint64_t>::max(), 2, 0));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(0, 4, 0x55667788));
  EXPECT_FALSE(Shadow.setRegister(2, 4, 0x11223344));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(0, 4, 0x55667788));
  ASSERT_TRUE(Shadow.setRegister(4, 4, 0x11223344));
  EXPECT_EQ(Shadow.value(NdVar::reg(0, 8)), 0x1122334455667788ULL);

  ASSERT_TRUE(Shadow.reset(llvm::endianness::big));
  ASSERT_TRUE(Shadow.setRegister(0, 4, 0x11223344));
  ASSERT_TRUE(Shadow.setRegister(4, 4, 0x55667788));
  EXPECT_EQ(Shadow.value(NdVar::reg(0, 8)), 0x1122334455667788ULL);
}

TEST(SymExecConcreteShadow, MissingRegisterAndTemporaryBytesFailClosed) {
  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(0, 4, 0x12345678));
  EXPECT_FALSE(
      Shadow.step(op(NdOp::COPY, NdVar::reg(16, 8), {NdVar::reg(0, 8)})));
  EXPECT_FALSE(Shadow.value(NdVar::reg(16, 8)).has_value());

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(
      Shadow.step(op(NdOp::COPY, NdVar::reg(16, 8), {NdVar::tmp(0, 8)})));
}

TEST(SymExecConcreteShadow, InitialMemoryAndApproximateEffectsFailClosed) {
  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.step(
      op(NdOp::LOAD, NdVar::reg(16, 4), {NdVar::cst(0x401000, 8)})));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(Shadow.setRegister(0, 8, 4));
  EXPECT_FALSE(
      Shadow.step(op(NdOp::FLOAT_SQRT, NdVar::reg(16, 8), {NdVar::reg(0, 8)})));

  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  EXPECT_FALSE(Shadow.step(op(NdOp::CALL, NdVar{}, {NdVar::cst(0x402000, 8)})));
}

TEST(SymExecConcreteShadow, ExactStoreThenLoadKeepsTheShadowComplete) {
  SymExecConcreteShadow Shadow;
  ASSERT_TRUE(Shadow.reset(llvm::endianness::little));
  ASSERT_TRUE(
      Shadow.step(op(NdOp::STORE, NdVar{},
                     {NdVar::cst(0x401000, 8), NdVar::cst(0x12345678, 4)})));
  ASSERT_TRUE(Shadow.step(
      op(NdOp::LOAD, NdVar::reg(16, 4), {NdVar::cst(0x401000, 8)})));
  EXPECT_EQ(Shadow.value(NdVar::reg(16, 4)), 0x12345678u);
}

} // namespace
