//===- LiftCommonTests.cpp - shared lift emission invariants ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/lift/LiftCommon.h"
#include "neverd/support/TextEncoding.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

using namespace neverd;

namespace {

LowOp emitExtension(NdOp Opcode, uint16_t OutputSize, uint16_t InputSize) {
  std::vector<LowOp> Ops;
  LiftStateBase State(0x1000, 1, Ops);
  State.emit(Opcode, NdVar::tmp(TmpBase, OutputSize),
             {NdVar::tmp(TmpBase + TmpStride, InputSize)});
  EXPECT_EQ(Ops.size(), 1U);
  return Ops.front();
}

} // namespace

TEST(LiftStateBase, EqualWidthZextCanonicalizesToCopy) {
  LowOp Op = emitExtension(NdOp::INT_ZEXT, 1, 1);
  EXPECT_EQ(Op.Opcode, NdOp::COPY);
  EXPECT_EQ(Op.Output.Size, 1U);
  ASSERT_EQ(Op.NumInputs, 1U);
  EXPECT_EQ(Op.Inputs[0].Size, 1U);
}

TEST(LiftStateBase, EqualWidthSextCanonicalizesToCopy) {
  LowOp Op = emitExtension(NdOp::INT_SEXT, 8, 8);
  EXPECT_EQ(Op.Opcode, NdOp::COPY);
  EXPECT_EQ(Op.Output.Size, 8U);
  ASSERT_EQ(Op.NumInputs, 1U);
  EXPECT_EQ(Op.Inputs[0].Size, 8U);
}

TEST(LiftStateBase, GenuineZextRemainsExtension) {
  LowOp Op = emitExtension(NdOp::INT_ZEXT, 8, 1);
  EXPECT_EQ(Op.Opcode, NdOp::INT_ZEXT);
  EXPECT_EQ(Op.Output.Size, 8U);
  ASSERT_EQ(Op.NumInputs, 1U);
  EXPECT_EQ(Op.Inputs[0].Size, 1U);
}

TEST(LiftStateBase, GenuineSextRemainsExtension) {
  LowOp Op = emitExtension(NdOp::INT_SEXT, 8, 1);
  EXPECT_EQ(Op.Opcode, NdOp::INT_SEXT);
  EXPECT_EQ(Op.Output.Size, 8U);
  ASSERT_EQ(Op.NumInputs, 1U);
  EXPECT_EQ(Op.Inputs[0].Size, 1U);
}

TEST(LiftStateBase, NarrowingExtensionIsFatal) {
  for (NdOp Opcode : {NdOp::INT_ZEXT, NdOp::INT_SEXT}) {
    EXPECT_EXIT(
        {
          // Keep the fatal boundary observable without asking a platform crash
          // reporter to retain an aborting death-test child.
          llvm::install_fatal_error_handler(
              [](void *, const char *Reason, bool) {
                std::fputs(Reason, stderr);
                std::fputc('\n', stderr);
                std::fflush(stderr);
                std::_Exit(1);
              });
          std::vector<LowOp> Ops;
          LiftStateBase State(0x1000, 1, Ops);
          State.emit(Opcode, NdVar::tmp(TmpBase, 1),
                     {NdVar::tmp(TmpBase + TmpStride, 4)});
        },
        ::testing::ExitedWithCode(1),
        "integer extension input must be narrower than output.*"
        "instruction address 4096, input bytes 4, output bytes 1");
  }
}

TEST(LiftStateBase, ExcessOperandsLeaveNoPartialOperationOrSequenceGap) {
  std::vector<LowOp> Ops;
  LiftStateBase State(0x1000, 4, Ops);
  const auto X = NdVar::cst(7, 4);
  State.emit(NdOp::COPY, State.makeTemp(4), {X});
  const auto Original = Ops.front();
  EXPECT_THROW(State.emit(NdOp::CALL, {}, {X, X, X, X, X, X, X, X, X}),
               std::length_error);
  EXPECT_THROW(
      State.emitIntrinsic(Intrinsic::MIPSSync, {}, {X, X, X, X, X, X, X, X}),
      std::length_error);
  ASSERT_EQ(Ops.size(), 1u);
  EXPECT_EQ(Ops.front(), Original);
  State.emit(NdOp::INT_ADD, State.makeTemp(4), {X, X});
  ASSERT_EQ(Ops.size(), 2u);
  EXPECT_EQ(Ops.back().Seq, Original.Seq + 1);
  EXPECT_EQ(Ops.back().Inputs[0].Provenance, ConstantAddressProvenance::Scalar);
  EXPECT_EQ(Original.Inputs[0].Provenance, ConstantAddressProvenance::Unknown);
}

TEST(TextEncoding, EscapesOnlyMalformedUTF8Bytes) {
  std::string Input = "ascii:";
  Input.append("\xe4\xb8\xad", 3); // U+4E2D, a legal three-byte sequence.
  Input.push_back(static_cast<char>(0xff));
  Input.push_back(static_cast<char>(0xc3));
  Input.push_back('(');
  Input.push_back(static_cast<char>(0xe2));
  Input.push_back(static_cast<char>(0x82));

  std::string Expected = "ascii:";
  Expected.append("\xe4\xb8\xad", 3);
  Expected += "\\xFF\\xC3(\\xE2\\x82";

  EXPECT_EQ(escapeInvalidUTF8(Input), Expected);
  EXPECT_EQ(escapeInvalidUTF8("ordinary metadata"), "ordinary metadata");
}
