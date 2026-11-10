//===- MIPS16E2PipelineTests.cpp - e2 source and memory boundaries --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/MIPS/MIPSBarrierSource.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
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
TEST(MIPS16E2Pipeline,
     StatefulInstructionsSurviveDeadResultsAndRefuseNativeSource) {
  const struct {
    uint32_t Word;
    Intrinsic Id;
  } Cases[] = {{0xf00492c0, Intrinsic::MIPSLoadLinkedWord},
               {0xf004d2c0, Intrinsic::MIPSStoreConditionalWord},
               {0xf000304c, Intrinsic::MIPSReadHardwareRegister},
               {0xf002674c, Intrinsic::MIPSDisableInterrupts},
               {0xf003674c, Intrinsic::MIPSEnableInterrupts},
               {0xf006670c, Intrinsic::MIPSDisableInterrupts},
               {0xf007670c, Intrinsic::MIPSEnableInterrupts}};
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (auto C : Cases)
      for (bool Dead : {false, true}) {
        SCOPED_TRACE(C.Word);
        SCOPED_TRACE(Dead);
        mips_test::ELFBytes F(A);
        F.abiFlags();
        F.put(36, 0x74001000, 4);
        F.put(24, 0x10101, 4);
        F.put(0xec, 0x4400, 4);
        F.put(0x100, C.Word >> 16, 2);
        F.put(0x102, C.Word & 65535, 2);
        F.put(0x104, Dead ? 0x6a07 : 0x6500, 2); // overwrite result or NOP
        F.put(0x106, 0xe8a0, 2);
        ELFLoader Loader;
        auto I = Loader.loadBuffer(F.buffer());
        ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
        for (bool LLVM : {false, true}) {
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.OnlyFunctionEntries = {I->Entry};
          Options.LiftMode = LLVM;
          Options.EmitDumpOutput = false;
          auto Result = Pipeline().run(*I, Context, Options);
          if (LLVM) {
            EXPECT_FALSE(Result.Success);
            EXPECT_NE(Result.Error.find(mips::kStatefulSourceUnsupported),
                      std::string::npos)
                << Result.Error;
            continue;
          }
          ASSERT_TRUE(Result.Success) << Result.Error;
          EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
          unsigned Count = 0;
          for (const auto &Function : Result.MedFuncs)
            for (const auto &Block : Function.Blocks)
              for (const auto &Op : Block.Ops)
                if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                    Op.Inputs[0].isConst() &&
                    Op.Inputs[0].ConstVal == uint64_t(C.Id))
                  ++Count;
          EXPECT_EQ(Count, 1u);
          CEmitterOptions Emit;
          Emit.TheArch = A;
          Emit.Format = BinaryFormat::ELF;
          Emit.Image = &*I;
          std::string Text;
          llvm::raw_string_ostream Out(Text);
          try {
            (void)HighCEmitter().emit(Result.HighFuncs, Out, Emit);
            FAIL() << "stateful intrinsic unexpectedly lowered to C";
          } catch (const std::runtime_error &Error) {
            EXPECT_NE(std::string(Error.what())
                          .find(mips::kStatefulSourceUnsupported),
                      std::string::npos);
          }
        }
      }
}

TEST(MIPS16E2Pipeline, BarrierNamesDoNotCaptureDeferredNativeCallees) {
  const auto Name =
      std::string(mips::BarrierHelperPrefix) + mips::ExecutionHazardAsm;
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool BarrierFirst : {false, true})
      for (bool NamedHint : {false, true})
        for (bool SeparateFunction : {false, true}) {
          SCOPED_TRACE(int(Target));
          SCOPED_TRACE(BarrierFirst);
          SCOPED_TRACE(NamedHint);
          SCOPED_TRACE(SeparateFunction);
          MedFunc Function;
          Function.Name = "barrier_caller";
          Function.Entry = 0x1000;
          Function.ReturnType = NdType::makeVoid();
          auto &Block = Function.Blocks.emplace_back();
          Block.Id = 0;
          Block.StartAddr = 0x1000;
          MedOp Barrier;
          Barrier.Opcode = NdOp::INTRINSIC;
          Barrier.addInput(
              MedVar::makeConst(uint64_t(Intrinsic::MIPSExecutionHazardBarrier),
                                sizeof(Intrinsic)));
          MedOp Call;
          Call.Opcode = NdOp::CALL;
          Call.addInput(MedVar::makeConst(0x2000, 4));
          Block.Ops = BarrierFirst ? std::vector<MedOp>{Barrier, Call}
                                   : std::vector<MedOp>{Call, Barrier};
          MedOp Return;
          Return.Opcode = NdOp::RETURN;
          Block.Ops.push_back(Return);
          std::vector<std::pair<va_t, std::string>> Imports = {
              {0x3000, Name + ".1"}};
          if (NamedHint) {
            MedCallInfo Info;
            Info.BlockId = 0;
            Info.OpIdx = BarrierFirst ? 1 : 0;
            Info.TargetAddr = 0x2000;
            Info.TargetName = Name;
            Function.CallInfos.push_back(Info);
          } else {
            Imports.emplace_back(0x2000, Name);
          }
          std::vector<MedFunc> Functions = {Function};
          if (SeparateFunction) {
            auto Caller = Function;
            Caller.Name = "native_caller";
            Caller.Entry = 0x1100;
            Caller.Blocks[0].StartAddr = Caller.Entry;
            Caller.Blocks[0].Ops = {Call, Return};
            if (NamedHint)
              Caller.CallInfos[0].OpIdx = 0;
            Function.Blocks[0].Ops = {Barrier, Return};
            Function.CallInfos.clear();
            Functions = BarrierFirst ? std::vector<MedFunc>{Function, Caller}
                                     : std::vector<MedFunc>{Caller, Function};
          }
          BinaryImage Image;
          Image.Arch = Target;
          Image.Bits = Bitness::Bits32;
          Image.Format = BinaryFormat::ELF;
          Segment Storage;
          Storage.VA = 0x4000;
          Storage.Size = Storage.FileSz = 4;
          Storage.Data.resize(4);
          Storage.Flags = SegmentFlags::Readable;
          Image.Segments.push_back(std::move(Storage));
          // Canonical storage evidence need not have a matching Imports row.
          ASSERT_TRUE(Image.recordImportStorageSlot(
              0x4000, Name + ".2", 0, ImportStorageEvidence::LoaderBind));
          llvm::LLVMContext Context;
          const auto Module =
              MedLLVMEmitter().emit(Functions, Context, "barrier_names", Target,
                                    Imports, &Image, BinaryFormat::ELF);
          ASSERT_NE(Module, nullptr);
          ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
          const auto *Native = Module->getFunction(Name);
          ASSERT_NE(Native, nullptr);
          EXPECT_TRUE(Native->isDeclaration());
          EXPECT_FALSE(Native->hasFnAttribute(mips::BarrierSourceMarker));
          const auto *Body = Module->getFunction(Function.Name);
          ASSERT_NE(Body, nullptr);
          std::vector<const llvm::Function *> Calls;
          for (const auto &B : *Body)
            for (const auto &Instruction : B)
              if (const auto *C = llvm::dyn_cast<llvm::CallBase>(&Instruction))
                Calls.push_back(C->getCalledFunction());
          if (SeparateFunction)
            for (const auto &B : *Module->getFunction("native_caller"))
              for (const auto &Instruction : B)
                if (const auto *C =
                        llvm::dyn_cast<llvm::CallBase>(&Instruction))
                  Calls.push_back(C->getCalledFunction());
          ASSERT_EQ(Calls.size(), 2u);
          const unsigned HelperIndex = SeparateFunction || BarrierFirst ? 0 : 1;
          EXPECT_EQ(Calls[1 - HelperIndex], Native);
          const auto *Helper = Calls[HelperIndex];
          ASSERT_NE(Helper, nullptr);
          EXPECT_NE(Helper, Native);
          EXPECT_NE(Helper->getName(), Name + ".1");
          EXPECT_NE(Helper->getName(), Name + ".2");
          EXPECT_TRUE(Helper->hasFnAttribute(mips::BarrierSourceMarker));
          CEmitterOptions Options;
          Options.TheArch = Target;
          std::string Text;
          llvm::raw_string_ostream Out(Text);
          ASSERT_TRUE(LLVMCEmitter().emit(*Module, Out, Options, nullptr,
                                          nullptr, Body));
          EXPECT_NE(Text.find("__asm__ volatile(\"ehb\""), std::string::npos)
              << Text;
        }
}

TEST(MIPS16E2Pipeline,
     SavedReturnAcrossBarriersKeepsNativeOrderingInBothSources) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (unsigned Stype = 0; Stype != 32; ++Stype) {
      mips_test::ELFBytes F(Target);
      F.abiFlags();
      F.put(36, 0x74001000, 4);
      F.put(24, 0x10101, 4);
      F.put(0xec, 0x4400, 4);
      const uint16_t Code[] = {
          0x64c4, 0xf0c0, 0x3010, uint16_t(0xf000 | (Stype << 6)),
          0x3014, 0x6a07, 0x6444, 0xe8a0};
      for (unsigned N = 0; N != std::size(Code); ++N)
        F.put(0x100 + 2 * N, Code[N], 2);
      ELFLoader Loader;
      auto I = Loader.loadBuffer(F.buffer());
      ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
      const auto FunctionName = Stype == 31
                                    ? std::string(mips::BarrierHelperPrefix) +
                                          mips::ExecutionHazardAsm
                                    : std::string("barrier_leaf");
      auto Symbol = Symbol::makeFunc(I->Entry, sizeof(Code));
      Symbol.Name = FunctionName;
      I->Symbols.push_back(std::move(Symbol));
      for (bool LLVM : {false, true}) {
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.OnlyFunctionEntries = {I->Entry};
        Options.LiftMode = LLVM;
        Options.EmitDumpOutput = false;
        auto Result = Pipeline().run(*I, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
        CEmitterOptions Emit;
        Emit.TheArch = Target;
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &*I;
        std::string Text;
        llvm::raw_string_ostream Out(Text);
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          unsigned Barriers = 0;
          for (const auto &Function : *Result.LlvmModule)
            for (const auto &Block : Function)
              for (const auto &Instruction : Block)
                if (const auto *Call =
                        llvm::dyn_cast<llvm::CallInst>(&Instruction))
                  if (mips::nativeBarrierSource(*Call))
                    ++Barriers;
          EXPECT_EQ(Barriers, 2u);
          const auto *Selected = Result.LlvmModule->getFunction(FunctionName);
          ASSERT_NE(Selected, nullptr);
          // The CLI's selected-function projection must retain source-only
          // helper definitions, not just their external declarations.
          ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, Emit,
                                          nullptr, &*I, Selected));
        } else {
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emit));
        }
        EXPECT_NE(Text.find("__asm__ volatile(\"ehb\""), std::string::npos)
            << Text;
        EXPECT_NE(Text.find("__asm__ volatile(\"sync " + std::to_string(Stype) +
                            "\""),
                  std::string::npos)
            << Text;
        EXPECT_NE(Text.find("__attribute__((noinline))"), std::string::npos)
            << Text;
        EXPECT_NE(Text.find("nomicromips,nomips16"), std::string::npos) << Text;
      }
    }
}

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
