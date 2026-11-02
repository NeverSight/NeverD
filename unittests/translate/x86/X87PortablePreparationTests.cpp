//===- X87PortablePreparationTests.cpp - Checked portable function ABI ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real source bytes, malformed scalar/control boundaries and default refusal.
//===----------------------------------------------------------------------===//

#include "X87PortableTestImage.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/X86/PortableX87Function.h"

#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <stdexcept>
#include <string>

using namespace neverd;
using namespace neverd::test;

namespace {

void expectRefusal(const MedFunc &Function) {
  auto Prepared =
      preparePortableX87Function(Function, Arch::X64, BinaryFormat::ELF);
  ASSERT_FALSE(static_cast<bool>(Prepared));
  EXPECT_FALSE(llvm::toString(Prepared.takeError()).empty());
}

TEST(X87PortablePreparation, RealLeafBranchesAndLoopsKeepTheExplicitInterface) {
  for (bool NoOpt : {false, true})
    for (const auto &Bytes : {portableX87LeafBytes(), portableX87BranchBytes(),
                              portableX87LoopBytes()}) {
      SCOPED_TRACE(NoOpt);
      SCOPED_TRACE(Bytes.size());
      auto Source = liftPortableX87Fixture(Bytes, NoOpt);
      ASSERT_TRUE(static_cast<bool>(Source))
          << llvm::toString(Source.takeError());
      auto Prepared =
          preparePortableX87Function(*Source, Arch::X64, BinaryFormat::ELF);
      if (!Prepared)
        Pipeline::dumpMedIR({*Source}, llvm::errs());
      ASSERT_TRUE(static_cast<bool>(Prepared))
          << llvm::toString(Prepared.takeError());
      if (auto Error = Prepared->validate())
        FAIL() << llvm::toString(std::move(Error));
      EXPECT_EQ(Prepared->sourceEntry(), 0x1000u);
      EXPECT_EQ(Prepared->sourceParameterBytes().size(),
                Bytes.size() == portableX87LeafBytes().size() ? 3u : 4u);
      EXPECT_EQ(Prepared->executionParameterIndex(), Source->Params.size());
      EXPECT_EQ(Prepared->sourceReturnParameterIndex(),
                Source->Params.size() + 1);
      for (auto Bytes : Prepared->sourceParameterBytes())
        EXPECT_EQ(Bytes, 8u);
      EXPECT_FALSE(Prepared->name().empty());
    }
}

TEST(X87PortablePreparation,
     RefusesUnknownStateHandoffsAndUnsupportedSourceForms) {
  auto Bytes = portableX87LeafBytes();
  // FSQRT is a native context form without a portable implementation.
  Bytes.insert(Bytes.begin() + 13, {0xd9, 0xfa});
  auto Unsupported = liftPortableX87Fixture(Bytes);
  ASSERT_TRUE(static_cast<bool>(Unsupported))
      << llvm::toString(Unsupported.takeError());
  expectRefusal(*Unsupported);

  auto Source = liftPortableX87Fixture(portableX87LeafBytes());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  auto Changed = *Source;
  Changed.CallInfos.emplace_back();
  expectRefusal(Changed);
  Changed = *Source;
  Changed.ConsumesX87Argument = true;
  expectRefusal(Changed);
  auto WrongArch =
      preparePortableX87Function(*Source, Arch::X86, BinaryFormat::ELF);
  ASSERT_FALSE(static_cast<bool>(WrongArch));
  llvm::consumeError(WrongArch.takeError());

  bool Mutated = false;
  for (auto &Block : Source->Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
          Op.Inputs[0].ConstVal ==
              static_cast<uint64_t>(Intrinsic::X87ContextStep)) {
        Op.Inputs[1].Id += 10000;
        Mutated = true;
        break;
      }
  ASSERT_TRUE(Mutated);
  expectRefusal(*Source);
}

TEST(X87PortablePreparation, RejectsMalformedScalarsAndCounterfeitEntryCopies) {
  auto Source = liftPortableX87Fixture(portableX87LeafBytes());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  auto Changed = *Source;
  Changed.Blocks.front().Ops.front().NumInputs = 0;
  expectRefusal(Changed);
  Changed = *Source;
  bool Mutated = false;
  for (auto &Op : Changed.Blocks.front().Ops)
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
        Op.Output.Kind == MedVar::Reg && Op.Output == Op.Inputs[0]) {
      ++Op.Inputs[0].RegOff;
      Mutated = true;
      break;
    }
  ASSERT_TRUE(Mutated);
  expectRefusal(Changed);
  Changed = *Source;
  Changed.Blocks.front().Ops.back().NumInputs = 0;
  expectRefusal(Changed);
  Changed = *Source;
  Changed.Blocks.front().Succs = {999};
  expectRefusal(Changed);
}

TEST(X87PortablePreparation, RefusesNarrowOrUnknownUpperReturnBits) {
  for (bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    auto Source = liftPortableX87Fixture(portableX87LeafBytes(), NoOpt);
    ASSERT_TRUE(static_cast<bool>(Source))
        << llvm::toString(Source.takeError());
    MedVar Narrow;
    for (const auto &Block : Source->Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].ConstVal ==
                static_cast<uint64_t>(Intrinsic::X87ContextStep) &&
            Op.Output.Size == 2)
          Narrow = Op.Output;
    ASSERT_EQ(Narrow.Size, 2);
    for (auto &Block : Source->Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::RETURN)
          Op.Inputs[0] = Narrow;
    expectRefusal(*Source);

    auto Bytes = portableX87LeafBytes();
    Bytes.erase(Bytes.begin(),
                Bytes.begin() + 7); // No reaching full RAX value.
    Source = liftPortableX87Fixture(Bytes, NoOpt);
    ASSERT_TRUE(static_cast<bool>(Source))
        << llvm::toString(Source.takeError());
    expectRefusal(*Source);
  }
}

TEST(X87PortablePreparation, OrdinaryEmittersCannotTreatHelpersAsNativeCalls) {
  MedFunc Function;
  Function.Name = "unprepared";
  Function.Entry = 0x1000;
  MedBlock Block;
  Block.Id = 0;
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(
      MedVar::makeConst(static_cast<uint64_t>(Intrinsic::PortableX87Step), 4));
  Block.Ops.push_back(Op);
  Function.Blocks.push_back(Block);
  llvm::LLVMContext Context;
  MedLLVMEmitter LLVM;
  EXPECT_THROW(LLVM.emit({Function}, Context), std::invalid_argument);

  HighFunc High;
  High.Name = "unprepared";
  auto Call = std::make_shared<HighExpr>();
  Call->Kind = ExprKind::Call;
  Call->IntrinsicId = Intrinsic::PortableX87Result;
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = Call;
  High.Body.push_back(Statement);
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  HighCEmitter C;
  EXPECT_THROW(C.emit({High}, Out), std::invalid_argument);
  EXPECT_TRUE(Text.empty());
  EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::PortableX87Step));
  EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::PortableX87Result));
}

} // namespace
