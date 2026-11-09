//===- MIPS16E2PipelineTests.cpp - e2 source and memory boundaries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>
using namespace neverd;
namespace {
TEST(MIPS16E2Pipeline, PartialImageStoresBindOnlyWritableSelectedBytes) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool Left : {false, true})
      for (bool Writable : {false, true}) {
        SCOPED_TRACE(int(Target));
        SCOPED_TRACE(Left);
        SCOPED_TRACE(Writable);
        mips_test::ELFBytes F(Target);
        F.abiFlags();
        F.Bytes.resize(0x308);
        F.put(36, 0x74001000, 4);
        F.put(24, 0x10101, 4);
        F.put(0xec, 0x4400, 4);
        F.put(44, 3, 2);
        F.put(52 + 24, 5, 4);
        F.put(116, 1, 4);
        F.put(120, 0x300, 4);
        F.put(124, 0x10300, 4);
        F.put(132, 8, 4);
        F.put(136, 8, 4);
        F.put(140, Writable ? 6 : 4, 4);
        F.put(144, 0x1000, 4);
        F.put(0x300, 0x12345678, 4);
        // lui a0,1; ori a0,0x301; swl/swr a1,0(a0); li v0,7; jrc ra.
        const uint16_t Code[] = {
            0xf000, 0x6c21, 0xf300, 0x6c41, uint16_t(Left ? 0xf004 : 0xf014),
            0xd5e0, 0x6a07, 0xe8a0};
        for (unsigned N = 0; N < std::size(Code); ++N)
          F.put(0x100 + 2 * N, Code[N], 2);
        ELFLoader Loader;
        auto I = Loader.loadBuffer(F.buffer());
        ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
        auto Symbol = Symbol::makeFunc(I->Entry, sizeof(Code));
        Symbol.Name = "e2_image_store";
        I->Symbols.push_back(std::move(Symbol));
        for (bool LLVM : {false, true}) {
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.OnlyFunctionEntries = {I->Entry};
          Options.LiftMode = LLVM;
          Options.EmitDumpOutput = false;
          auto Result = Pipeline().run(*I, Context, Options);
          if (!Result.Success) {
            EXPECT_FALSE(Writable) << Result.Error;
            EXPECT_NE(Result.Error.find("bounded original-image memory view"),
                      std::string::npos)
                << Result.Error;
            continue;
          }
          EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
          if (LLVM) {
            ASSERT_TRUE(Writable);
            ASSERT_TRUE(Result.LlvmModule);
            EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            unsigned Writes = 0;
            for (const auto &Function : *Result.LlvmModule)
              for (const auto &Block : Function)
                for (const auto &Instruction : Block)
                  if (const auto *Store =
                          llvm::dyn_cast<llvm::StoreInst>(&Instruction);
                      Store && Store->isVolatile()) {
                    EXPECT_TRUE(
                        Store->getValueOperand()->getType()->isIntegerTy(8));
                    EXPECT_TRUE(llvm::isa<llvm::GlobalVariable>(
                        llvm::getUnderlyingObject(Store->getPointerOperand())));
                    ++Writes;
                  }
            EXPECT_GT(Writes, 0u);
          } else {
            unsigned ScalarAddresses = 0;
            for (auto &Function : Result.HighFuncs)
              walkStmts(Function.Body, [&](HighStmt &Statement) {
                forEachExpr(Statement, [&](const ExprPtr &Root) {
                  std::vector<ExprPtr> Pending{Root};
                  while (!Pending.empty()) {
                    const auto E = Pending.back();
                    Pending.pop_back();
                    if (!E)
                      continue;
                    if ((E->IntrinsicId == Intrinsic::MIPSSwl ||
                         E->IntrinsicId == Intrinsic::MIPSSwr) &&
                        E->Operands[0]->Kind == ExprKind::Const) {
                      E->Operands[0]->ConstProvenance =
                          ConstantAddressProvenance::Scalar;
                      ++ScalarAddresses;
                    }
                    Pending.insert(Pending.end(), E->Operands.begin(),
                                   E->Operands.end());
                  }
                });
              });
            ASSERT_EQ(ScalarAddresses, 1u);
            CEmitterOptions Emit;
            Emit.TheArch = Target;
            Emit.Format = BinaryFormat::ELF;
            Emit.Image = &*I;
            std::string Text;
            llvm::raw_string_ostream Out(Text);
            if (Writable) {
              ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emit));
              EXPECT_NE(Text.find("volatile uint8_t *"), std::string::npos)
                  << Text;
              EXPECT_EQ(Text.find("const volatile uint8_t *"),
                        std::string::npos)
                  << Text;
              EXPECT_EQ(Text.find(")(66817)"), std::string::npos) << Text;
              EXPECT_EQ(Text.find(")(0x10301)"), std::string::npos) << Text;
              EXPECT_NE(Text.find("nomicromips,nomips16"), std::string::npos)
                  << Text;
            } else
              EXPECT_THROW(HighCEmitter().emit(Result.HighFuncs, Out, Emit),
                           std::invalid_argument);
          }
        }
      }
}
TEST(MIPS16E2Pipeline, FourByteE2CannotOccupyTwoByteReturnDelaySlot) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    mips_test::ELFBytes F(Target);
    F.abiFlags();
    F.put(36, 0x74001000, 4);
    F.put(24, 0x10101, 4);
    F.put(0xec, 0x4400, 4);
    F.put(0x100, 0xe820, 2); // jr ra, requires a two-byte delay instruction
    F.put(0x102, 0xf010, 2);
    F.put(0x104, 0x6a61, 2); // e2 ANDI, four bytes
    ELFLoader Loader;
    auto I = Loader.loadBuffer(F.buffer());
    ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
    Decoder D;
    ASSERT_TRUE(D.init(*I));
    DecodedExecutionUnit Unit;
    // The physical decoder rejects the unit before any LowIR effects exist.
    EXPECT_EQ(D.decodeUnitForLift(F.Bytes.data() + 0x100, 6, I->Entry, Unit),
              0);
  }
}
} // namespace
