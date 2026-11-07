//===- StringTransferTests.cpp - Ordered repeated memory semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Independent byte oracles for bounded transfer expansion.

#include "../../lib/analysis/arch/x86_64/StringTransfer.h"
#include "gtest/gtest.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/symbolic/SymExec.h"

#include <array>
#include <limits>

using namespace neverd;
using namespace neverd::analysis::detail;
using namespace neverd::symbolic;

namespace {

LowOp transfer(Intrinsic Id, unsigned Width, bool Fill) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(1024, 8);
  Op.addInput(NdVar::scalar(static_cast<uint16_t>(Id), 2));
  Op.addInput(NdVar::reg(0, 8));
  Op.addInput(NdVar::reg(8, 8));
  Op.addInput(NdVar::reg(16, Fill ? Width : 8));
  Op.addInput(NdVar::reg(24, 1));
  return Op;
}

TEST(StringTransfer, ElementOrderMatchesIndependentOverlappingCopyAndFill) {
  const Intrinsic Moves[] = {Intrinsic::Movsb, Intrinsic::Movsw,
                             Intrinsic::Movsd, Intrinsic::Movsq};
  const Intrinsic Fills[] = {Intrinsic::Stosb, Intrinsic::Stosw,
                             Intrinsic::Stosd, Intrinsic::Stosq};
  constexpr uint64_t Base = 0x10000;
  constexpr uint64_t FillValue = 0x9371ab05d2e648fc;
  for (unsigned SizeIndex = 0; SizeIndex < 4; ++SizeIndex) {
    const unsigned Width = 1u << SizeIndex;
    for (bool RuntimeDirection : {false, true})
      for (bool Complete : {false, true})
        for (bool Fill : {false, true})
          for (bool Backward : {false, true})
            for (unsigned Count : {0, 1, 3, 7})
              for (int Gap : {-32, -8, -1, 0, 1, 8, 32}) {
                SCOPED_TRACE(::testing::Message()
                             << Width << ":" << Fill << ":" << Backward << ":"
                             << Count << ":" << Gap << ":" << Complete << ":"
                             << RuntimeDirection);
                std::array<uint8_t, 256> Expected;
                SymContext Ctx;
                SymState State(Ctx);
                for (unsigned I = 0; I < Expected.size(); ++I) {
                  Expected[I] = static_cast<uint8_t>(I * 29 + I / 7);
                  State.store(Ctx.mkConst(64, Base + I),
                              Ctx.mkConst(8, Expected[I]));
                }
                const int Source = Backward ? 160 : 64;
                const int Destination = Source + Gap;
                for (unsigned I = 0; I < Count; ++I) {
                  const int Delta = (Backward ? -1 : 1) * int(I * Width);
                  std::array<uint8_t, 8> Element;
                  for (unsigned B = 0; B < Width; ++B)
                    Element[B] =
                        Fill ? static_cast<uint8_t>(FillValue >> (8 * B))
                             : Expected[Source + Delta + B];
                  for (unsigned B = 0; B < Width; ++B)
                    Expected[Destination + Delta + B] = Element[B];
                }
                LowOp Op = transfer(Fill ? Fills[SizeIndex] : Moves[SizeIndex],
                                    Width, Fill);
                std::vector<LowOp> Instruction;
                if (Complete) {
                  const auto Id = Fill ? Intrinsic::X86RepStosState
                                       : Intrinsic::X86RepMovsState;
                  Op.Inputs[0] = NdVar::scalar(static_cast<uint16_t>(Id), 2);
                  Op.Inputs[3].Size = 8;
                  Op.Inputs[4].Size = X86RepStatusBytes;
                  Op.addInput(NdVar::scalar(
                      *x86RepDescriptor(Id, Arch::X64, Width, 8, 0, false), 4));
                }
                Instruction.push_back(Op);
                if (Complete)
                  for (unsigned J = 1; J < (Fill ? 2u : 3u); ++J) {
                    LowOp Bind;
                    Bind.Opcode = NdOp::COPY;
                    Bind.Output = NdVar::tmp(2048 + J * 16, 8);
                    Bind.addInput(NdVar::tmp(2056 + J * 16, 8));
                    Instruction.push_back(Bind);
                  }
                State.write(
                    SymSpace::Register, 0,
                    Ctx.mkConst(64, Base + (Fill ? Destination : Source)));
                State.write(SymSpace::Register, 8,
                            Ctx.mkConst(64, Fill ? Count : Base + Destination));
                State.write(SymSpace::Register, 16,
                            Ctx.mkConst(Fill && !Complete ? Width * 8 : 64,
                                        Fill ? FillValue : Count));
                State.write(SymSpace::Register, 24,
                            Ctx.mkConst(Complete ? 32 : 8,
                                        Complete ? ((uint32_t(Backward)
                                                     << X86RepDirectionShift) |
                                                    0x8d5)
                                                 : (Backward ? 0x80 : 0)));
                auto Lowered = lowerStringTransfer(
                    Op, Count,
                    RuntimeDirection ? std::nullopt
                                     : std::optional<bool>(Backward),
                    Instruction, 4096, 1024);
                ASSERT_TRUE(static_cast<bool>(Lowered))
                    << llvm::toString(Lowered.takeError());
                SymExec Exec(Ctx, State);
                for (const auto &Scalar : *Lowered)
                  ASSERT_EQ(Exec.step(Scalar), StepResult::Continue);
                for (unsigned I = 0; I < Expected.size(); ++I) {
                  const auto Actual =
                      Ctx.asConst(State.load(Ctx.mkConst(64, Base + I), 1));
                  ASSERT_TRUE(Actual.has_value());
                  EXPECT_EQ(Actual->getZExtValue(), Expected[I]);
                }
                const auto Output = Ctx.asConst(
                    State.read(SymSpace::Temporary, Op.Output.Offset, 8));
                ASSERT_TRUE(Output.has_value());
                if (Complete) {
                  const uint64_t Distance = Count * Width;
                  const uint64_t Delta =
                      Backward ? uint64_t(0) - Distance : Distance;
                  EXPECT_EQ(Output->getZExtValue(),
                            Base + (Fill ? Destination : Source) + Delta);
                  for (unsigned J = 1; J < Instruction.size(); ++J) {
                    const auto Value = Ctx.asConst(
                        State.read(SymSpace::Temporary,
                                   Instruction[J].Inputs[0].Offset, 8));
                    ASSERT_TRUE(Value);
                    EXPECT_EQ(Value->getZExtValue(),
                              J + 1 == Instruction.size()
                                  ? 0
                                  : Base + Destination + Delta);
                  }
                } else
                  EXPECT_TRUE(Output->isZero());
                EXPECT_EQ(Exec.opaqueOperationCount(), 0u);
                if (!Count)
                  for (const auto &Scalar : *Lowered) {
                    EXPECT_NE(Scalar.Opcode, NdOp::LOAD);
                    EXPECT_NE(Scalar.Opcode, NdOp::STORE);
                  }
              }
  }
}

TEST(StringTransfer, ExactBudgetScratchAndMalformedShapes) {
  const auto Op = transfer(Intrinsic::Movsq, 8, false);
  auto Good = lowerStringTransfer(Op, 7, true, {Op}, 4096, 29);
  ASSERT_TRUE(static_cast<bool>(Good)) << llvm::toString(Good.takeError());
  const auto Refused = [&](const LowOp &Candidate, uint64_t Count,
                           uint64_t Limit, uint64_t Budget) {
    auto Result = lowerStringTransfer(Candidate, Count, false, {Candidate},
                                      Limit, Budget);
    EXPECT_FALSE(static_cast<bool>(Result));
    if (!Result)
      llvm::consumeError(Result.takeError());
  };
  Refused(Op, 7, 4096, 28);
  Refused(Op, std::numeric_limits<uint64_t>::max(), 4096,
          std::numeric_limits<uint64_t>::max());
  Refused(Op, std::numeric_limits<uint64_t>::max() / 4, 4096,
          std::numeric_limits<uint64_t>::max());
  Refused(Op, 1, 1055, 5);
  auto Zero = lowerStringTransfer(Op, 0, true, {Op}, 1032, 1);
  ASSERT_TRUE(static_cast<bool>(Zero)) << llvm::toString(Zero.takeError());
  for (unsigned Kind = 0; Kind < 6; ++Kind) {
    LowOp Bad = Op;
    switch (Kind) {
    case 0:
      Bad.NumInputs = 4;
      break;
    case 1:
      Bad.Inputs[3].Size = 4;
      break;
    case 2:
      Bad.Inputs[4].Size = 8;
      break;
    case 3:
      Bad.Output = NdVar::reg(32, 8);
      break;
    case 4:
      Bad.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 5:
      Bad.Inputs[0] = NdVar::scalar(static_cast<uint16_t>(Intrinsic::Lodsb), 2);
      break;
    }
    Refused(Bad, 0, 4096, 1);
  }
}

TEST(StringTransfer, RuntimeDirectionHasExactBudgetAndDisjointScratch) {
  const auto Op = transfer(Intrinsic::Movsq, 8, false);
  auto Good = lowerStringTransfer(Op, 7, std::nullopt, {Op}, 4096, 37);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->size(), 37u);
  for (const auto &[Count, Limit, Budget] :
       {std::array<uint64_t, 3>{7, 4096, 36},
        std::array<uint64_t, 3>{1, 1071, 7},
        std::array<uint64_t, 3>{std::numeric_limits<uint64_t>::max(), 4096,
                                std::numeric_limits<uint64_t>::max()}}) {
    auto Bad =
        lowerStringTransfer(Op, Count, std::nullopt, {Op}, Limit, Budget);
    EXPECT_FALSE(bool(Bad));
    if (!Bad)
      llvm::consumeError(Bad.takeError());
  }
  auto Zero = lowerStringTransfer(Op, 0, std::nullopt, {Op}, 1032, 1);
  ASSERT_TRUE(bool(Zero)) << llvm::toString(Zero.takeError());
  ASSERT_EQ(Zero->size(), 1u);
  EXPECT_EQ(Zero->front().Opcode, NdOp::COPY);
}

TEST(StringTransfer, CompleteStateRequiresOwnedDisjointOutputBindings) {
  auto Op = transfer(Intrinsic::X86RepMovsState, 1, false);
  Op.Inputs[4].Size = X86RepStatusBytes;
  Op.addInput(NdVar::scalar(
      *x86RepDescriptor(Intrinsic::X86RepMovsState, Arch::X64, 1, 8, 0, false),
      4));
  std::vector<LowOp> Ops{Op};
  for (unsigned I = 0; I < 2; ++I) {
    LowOp Bind;
    Bind.Opcode = NdOp::COPY;
    Bind.Output = NdVar::tmp(2048 + 16 * I, 8);
    Bind.addInput(NdVar::tmp(2056 + 16 * I, 8));
    Ops.push_back(Bind);
  }
  auto Good = lowerStringTransfer(Op, 0, false, Ops, 4096, 3);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->size(), 3u);
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
    auto Bad = Ops;
    switch (Mutation) {
    case 0:
      Bad.pop_back();
      break;
    case 1:
      Bad[1].Inputs[0] = Op.Output;
      break;
    case 2:
      Bad[2].Inputs[0] = Bad[1].Inputs[0];
      break;
    case 3:
      Bad[1].Inputs[0].Size = 4;
      break;
    case 4:
      Bad[1].Output = Bad[1].Inputs[0];
      break;
    case 5:
      Bad[1].Opcode = NdOp::INT_ZEXT;
      break;
    }
    auto Result = lowerStringTransfer(Op, 0, false, Bad, 4096, 3);
    EXPECT_FALSE(bool(Result));
    if (!Result)
      llvm::consumeError(Result.takeError());
  }
  for (unsigned Address : {4, 8})
    for (unsigned Segment : {0, 0x64, 0x65}) {
      auto Candidate = Op;
      Candidate.Inputs[5] =
          NdVar::scalar(*x86RepDescriptor(Intrinsic::X86RepMovsState, Arch::X64,
                                          1, Address, Segment, false),
                        4);
      EXPECT_EQ(stringTransferShape(Candidate).has_value(),
                Address == 8 && Segment == 0);
    }
}

TEST(StringTransfer, ScratchCannotClobberAnotherInstructionOperand) {
  auto Op = transfer(Intrinsic::Movsb, 1, false);
  Op.Inputs[1] = NdVar::tmp(2048, 8);
  LowOp Later;
  Later.Opcode = NdOp::COPY;
  Later.Output = NdVar::tmp(2080, 8);
  Later.addInput(Op.Output);
  auto Lowered = lowerStringTransfer(Op, 1, false, {Op, Later}, 4096, 5);
  ASSERT_TRUE(static_cast<bool>(Lowered))
      << llvm::toString(Lowered.takeError());
  for (const auto &Scalar : *Lowered)
    if (Scalar.Output.Size && Scalar.Output != Op.Output)
      EXPECT_GE(Scalar.Output.Offset, 2088u);
}

} // namespace
