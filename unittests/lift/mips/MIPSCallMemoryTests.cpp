//===- MIPSCallMemoryTests.cpp - Partial stores and call recovery --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
using namespace neverd;
namespace {
MedVar value(int Id, uint64_t Register, Arch A, bool Temporary = false) {
  MedVar V;
  V.Kind = Temporary ? MedVar::Temp : MedVar::Reg;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = 4;
  V.RegOff = Register;
  V.TheArch = A;
  return V;
}
MedOp operation(NdOp Opcode, MedVar Output,
                std::initializer_list<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (const auto &V : Inputs)
    Op.addInput(V);
  return Op;
}
TEST(MIPSCallMemory,
     PartialStoreInvalidatesStackSetupButPreservesRegisterSetup) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (Intrinsic Id : {Intrinsic::MIPSSwl, Intrinsic::MIPSSwr,
                         Intrinsic::MIPSStoreConditionalWord,
                         Intrinsic::MIPSCache, Intrinsic::MIPSPrefetch})
      for (bool Overwrite : {false, true})
        for (unsigned Graph = 0; Graph < 4; ++Graph) {
          const auto &TRI = getTargetRegInfo(A);
          const auto Layout = TRI.integerArgumentLayout(BinaryFormat::ELF);
          ASSERT_EQ(Layout.Registers.size(), 4u);
          MedFunc F;
          F.Entry = 0x1000;
          F.Blocks.resize(1);
          auto &B = F.Blocks[0];
          B.Id = 0;
          for (unsigned I = 0; I < 4; ++I)
            B.Ops.push_back(operation(NdOp::COPY,
                                      value(100 + I, Layout.Registers[I], A),
                                      {MedVar::makeConst(47 + I, 4)}));
          const auto SP = value(200, TRI.StackPointer, A);
          B.Ops.push_back(operation(NdOp::COPY, SP, {SP}));
          const auto Address = value(201, 0, A, true),
                     Partial = value(202, 0, A, true);
          B.Ops.push_back(operation(NdOp::INT_ADD, Address,
                                    {SP, MedVar::makeConst(16, 4)}));
          B.Ops.push_back(
              operation(NdOp::STORE, {}, {Address, MedVar::makeConst(99, 4)}));
          B.Ops.push_back(operation(
              NdOp::INT_ADD, Partial,
              {Address,
               MedVar::makeConst(
                   Id == Intrinsic::MIPSStoreConditionalWord ? 0 : 1, 4)}));
          if (Overwrite) {
            auto Store =
                operation(NdOp::INTRINSIC,
                          Id == Intrinsic::MIPSStoreConditionalWord
                              ? value(203, 0, A, true)
                              : MedVar{},
                          {MedVar::makeConst(uint64_t(Id), sizeof(Intrinsic)),
                           Partial, MedVar::makeConst(0x12345678, 4)});
            if (mips::isCacheIntrinsic(Id)) {
              Store.Inputs[1] = MedVar::makeConst(30, 1);
              Store.Inputs[2] = Partial;
            }
            EXPECT_TRUE(endsCallArgumentSetup(Store));
            EXPECT_FALSE(endsCallArgumentSetup(Store, true));
            auto Malformed = Store;
            Malformed.Inputs[2].Size = 8;
            EXPECT_TRUE(endsCallArgumentSetup(Malformed, true));
            B.Ops.push_back(Store);
          }
          B.Ops.push_back(
              operation(NdOp::CALL, {}, {MedVar::makeConst(0x2000, 4)}));
          B.StartAddr = F.Entry;
          if (Graph) {
            auto Ops = std::move(B.Ops);
            const auto Call = Ops.back();
            Ops.pop_back();
            MedOp PartialStore;
            if (Overwrite) {
              PartialStore = Ops.back();
              Ops.pop_back();
            }
            F.Blocks.resize(Graph == 1 ? 2 : 4);
            F.Blocks[0].Ops = std::move(Ops);
            const unsigned Last = F.Blocks.size() - 1;
            for (unsigned N = 1; N <= Last; ++N) {
              F.Blocks[N].Id = N;
              F.Blocks[N].StartAddr = F.Entry + 16 * N;
            }
            if (Graph == 1) {
              if (Overwrite)
                F.Blocks[0].Ops.push_back(PartialStore);
              F.Blocks[0].Succs = {1};
              F.Blocks[1].Preds = {0};
            } else {
              F.Blocks[0].Succs = {1, 2};
              for (unsigned N : {1u, 2u}) {
                F.Blocks[N].Preds = {0};
                F.Blocks[N].Succs = {3};
              }
              if (Overwrite)
                F.Blocks[1].Ops.push_back(PartialStore);
              F.Blocks[3].Preds =
                  Graph == 2 ? std::vector<int>{1, 2} : std::vector<int>{2, 1};
            }
            F.Blocks[Last].Ops.push_back(Call);
          }
          BinaryImage Image;
          Image.Arch = A;
          Image.Bits = Bitness::Bits32;
          Image.Format = BinaryFormat::ELF;
          std::map<va_t, int> Registers{{0x2000, 4}}, Total{{0x2000, 5}};
          recoverCallAbi(F, A, {{0x2000, "five_arguments"}}, &Image, &Registers,
                         &Total);
          ASSERT_EQ(F.CallInfos.size(), 1u);
          const auto &Args = F.CallInfos[0].Args;
          ASSERT_EQ(Args.size(), 5u);
          for (unsigned I = 0; I < 4; ++I) {
            ASSERT_TRUE(Args[I].isConst());
            EXPECT_EQ(Args[I].ConstVal, 47 + I);
          }
          if (Overwrite) {
            EXPECT_EQ(Args[4].Kind, MedVar::Unspecified);
          } else {
            ASSERT_EQ(Args.size(), 5u);
            ASSERT_TRUE(Args[4].isConst());
            EXPECT_EQ(Args[4].ConstVal, 99u);
          }
        }
}
TEST(MIPSCallMemory, IncompleteStackProofCannotOmitARequiredArgument) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (Intrinsic Id : {Intrinsic::MIPSSwl, Intrinsic::MIPSSwr,
                         Intrinsic::MIPSStoreConditionalWord,
                         Intrinsic::MIPSCache, Intrinsic::MIPSPrefetch})
      for (unsigned Boundary = 0; Boundary != 6; ++Boundary) {
        SCOPED_TRACE(Boundary);
        const bool WorkLimit = Boundary == 1;
        const auto &TRI = getTargetRegInfo(A);
        const auto Layout = TRI.integerArgumentLayout(BinaryFormat::ELF);
        MedFunc F;
        F.Entry = 0x1000;
        F.Blocks.resize(Boundary == 0 ? limits::kCallArgStoreAddressDepth + 2
                        : WorkLimit || Boundary == 5 ? 1
                                                     : 2);
        for (unsigned I = 0; I < F.Blocks.size(); ++I) {
          auto &B = F.Blocks[I];
          B.Id = I;
          B.StartAddr = F.Entry + 16 * I;
          if (I)
            B.Preds = {int(I - 1)};
          if (I + 1 < F.Blocks.size())
            B.Succs = {int(I + 1)};
        }
        auto &Entry = F.Blocks.front();
        const auto SP = value(200, TRI.StackPointer, A);
        const auto Address = value(201, 0, A, true);
        Entry.Ops.push_back(operation(NdOp::COPY, SP, {SP}));
        Entry.Ops.push_back(
            operation(NdOp::INT_ADD, Address, {SP, MedVar::makeConst(16, 4)}));
        Entry.Ops.push_back(
            operation(NdOp::STORE, {}, {Address, MedVar::makeConst(99, 4)}));
        Entry.Ops.push_back(operation(
            NdOp::INTRINSIC,
            Id == Intrinsic::MIPSStoreConditionalWord ? value(203, 0, A, true)
                                                      : MedVar{},
            {MedVar::makeConst(uint64_t(Id), sizeof(Intrinsic)), Address,
             MedVar::makeConst(0x12345678, 4)}));
        auto &Last = F.Blocks.back();
        if (Boundary == 2)
          Last.ExceptionalPreds = {
              {0, F.Entry, ExceptionalEdgeKind::CxxCleanup}};
        if (Boundary == 3) {
          Entry.Succs.clear();
          Last.Preds.clear();
        }
        if (Boundary == 4)
          F.ModuleAnalysisRoots.insert(Last.StartAddr);
        if (Boundary == 5) {
          Entry.Ops.clear();
          Entry.Preds = Entry.Succs = {0};
        }
        if (WorkLimit)
          for (unsigned I = 0; I < limits::kMaxCallSetupStackProofWork; ++I)
            Last.Ops.push_back(operation(NdOp::COPY, value(300 + I, 0, A, true),
                                         {MedVar::makeConst(I, 4)}));
        for (unsigned I = 0; I < 4; ++I)
          Last.Ops.push_back(operation(NdOp::COPY,
                                       value(100 + I, Layout.Registers[I], A),
                                       {MedVar::makeConst(47 + I, 4)}));
        Last.Ops.push_back(
            operation(NdOp::CALL, {}, {MedVar::makeConst(0x2000, 4)}));
        BinaryImage Image;
        Image.Arch = A;
        Image.Bits = Bitness::Bits32;
        Image.Format = BinaryFormat::ELF;
        std::map<va_t, int> Registers{{0x2000, 4}}, Total{{0x2000, 5}};
        recoverCallAbi(F, A, {{0x2000, "five_arguments"}}, &Image, &Registers,
                       &Total);
        ASSERT_EQ(F.CallInfos.size(), 1u);
        const auto &Args = F.CallInfos[0].Args;
        ASSERT_EQ(Args.size(), 5u);
        EXPECT_EQ(Args[4].Kind, MedVar::Unspecified);
        for (unsigned I = 0; I < 4; ++I) {
          ASSERT_TRUE(Args[I].isConst());
          EXPECT_EQ(Args[I].ConstVal, 47 + I);
        }
      }
}
void checkFifthArgument(MedFunc &F, Arch A, std::optional<uint64_t> Expected) {
  BinaryImage Image;
  Image.Arch = A;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  std::map<va_t, int> Registers{{0x2000, 4}, {0x3000, 0}},
      Total{{0x2000, 5}, {0x3000, 0}};
  recoverCallAbi(F, A, {{0x2000, "five_arguments"}, {0x3000, "opaque_call"}},
                 &Image, &Registers, &Total);
  const auto Call =
      std::find_if(F.CallInfos.begin(), F.CallInfos.end(),
                   [](const auto &C) { return C.TargetAddr == 0x2000; });
  ASSERT_NE(Call, F.CallInfos.end());
  ASSERT_EQ(Call->Args.size(), 5u);
  for (unsigned I = 0; I < 4; ++I) {
    ASSERT_TRUE(Call->Args[I].isConst());
    EXPECT_EQ(Call->Args[I].ConstVal, 47 + I);
  }
  if (Expected) {
    ASSERT_TRUE(Call->Args[4].isConst());
    EXPECT_EQ(Call->Args[4].ConstVal, *Expected);
  } else
    EXPECT_EQ(Call->Args[4].Kind, MedVar::Unspecified);
}
TEST(MIPSCallMemory, LostMemoryEvidenceCannotBeRevivedByLocalOrTailScans) {
  enum Effect {
    PriorCall,
    UnknownAddress,
    Atomic,
    InteriorByte,
    Narrow,
    Crossing,
    BytesAfterMerge,
    FullWordAfterMerge
  };
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool Tail : {false, true})
      for (Effect E : {PriorCall, UnknownAddress, Atomic, InteriorByte, Narrow,
                       Crossing, BytesAfterMerge, FullWordAfterMerge}) {
        SCOPED_TRACE(int(A));
        SCOPED_TRACE(Tail);
        SCOPED_TRACE(int(E));
        const auto &TRI = getTargetRegInfo(A);
        const auto Layout = TRI.integerArgumentLayout(BinaryFormat::ELF);
        MedFunc F;
        F.Entry = 0x1000;
        F.Blocks.resize(1);
        auto &B = F.Blocks[0];
        B.Id = 0;
        B.StartAddr = F.Entry;
        const auto SP = value(200, TRI.StackPointer, A);
        const auto Address = value(201, 0, A, true);
        const auto Interior = value(202, 0, A, true);
        B.Ops.push_back(operation(NdOp::COPY, SP, {SP}));
        B.Ops.push_back(
            operation(NdOp::INT_ADD, Address, {SP, MedVar::makeConst(16, 4)}));
        B.Ops.push_back(
            operation(NdOp::STORE, {}, {Address, MedVar::makeConst(99, 4)}));
        B.Ops.push_back(
            operation(NdOp::INT_ADD, Interior,
                      {Address, MedVar::makeConst(E == Crossing ? 3 : 1, 4)}));
        switch (E) {
        case PriorCall:
          B.Ops.push_back(
              operation(NdOp::CALL, {}, {MedVar::makeConst(0x3000, 4)}));
          break;
        case UnknownAddress:
          B.Ops.push_back(operation(
              NdOp::STORE, {},
              {value(400, Layout.Registers[0], A), MedVar::makeConst(7, 4)}));
          break;
        case Atomic:
          B.Ops.push_back(operation(NdOp::ATOMIC_XCHG, value(203, 0, A, true),
                                    {Address, MedVar::makeConst(7, 4)}));
          break;
        case InteriorByte:
        case Crossing:
        case Narrow:
          B.Ops.push_back(
              operation(NdOp::STORE, {},
                        {E == Narrow ? Address : Interior,
                         MedVar::makeConst(0xaabb, E == Crossing ? 2 : 1)}));
          break;
        case BytesAfterMerge:
        case FullWordAfterMerge:
          B.Ops.push_back(
              operation(NdOp::INTRINSIC, {},
                        {MedVar::makeConst(uint64_t(Intrinsic::MIPSSwl),
                                           sizeof(Intrinsic)),
                         Interior, MedVar::makeConst(0x12345678, 4)}));
          if (E == FullWordAfterMerge)
            B.Ops.push_back(operation(NdOp::STORE, {},
                                      {Address, MedVar::makeConst(123, 4)}));
          else {
            B.Ops.push_back(operation(NdOp::STORE, {},
                                      {Address, MedVar::makeConst(0x77, 1)}));
            B.Ops.push_back(operation(NdOp::STORE, {},
                                      {Interior, MedVar::makeConst(0x88, 1)}));
          }
          break;
        }
        for (unsigned I = 0; I < 4; ++I)
          B.Ops.push_back(operation(NdOp::COPY,
                                    value(100 + I, Layout.Registers[I], A),
                                    {MedVar::makeConst(47 + I, 4)}));
        B.Ops.push_back(
            operation(NdOp::CALL, {}, {MedVar::makeConst(0x2000, 4)}));
        B.Ops.back().IsTailCall = Tail;
        if (Tail)
          B.Ops.push_back(operation(NdOp::RETURN, {}, {}));
        checkFifthArgument(F, A,
                           E == FullWordAfterMerge
                               ? std::optional<uint64_t>(123)
                               : std::nullopt);
      }
}
TEST(MIPSCallMemory, DisagreeingPredecessorSlotsStayUnknownInEitherOrder) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (unsigned Other : {0u, 1u, 2u, 3u})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(Other);
        SCOPED_TRACE(Reverse);
        const auto &TRI = getTargetRegInfo(A);
        const auto Layout = TRI.integerArgumentLayout(BinaryFormat::ELF);
        MedFunc F;
        F.Entry = 0x1000;
        F.Blocks.resize(4);
        for (unsigned I = 0; I < 4; ++I) {
          F.Blocks[I].Id = I;
          F.Blocks[I].StartAddr = F.Entry + 16 * I;
        }
        const auto SP = value(200, TRI.StackPointer, A),
                   Address = value(201, 0, A, true);
        F.Blocks[0].Ops.push_back(operation(NdOp::COPY, SP, {SP}));
        F.Blocks[0].Ops.push_back(
            operation(NdOp::INT_ADD, Address, {SP, MedVar::makeConst(16, 4)}));
        F.Blocks[0].Succs = {1, 2};
        for (unsigned I : {1u, 2u}) {
          F.Blocks[I].Preds = {0};
          F.Blocks[I].Succs = {3};
        }
        F.Blocks[1].Ops.push_back(
            operation(NdOp::STORE, {}, {Address, MedVar::makeConst(99, 4)}));
        if (Other)
          F.Blocks[2].Ops.push_back(
              operation(NdOp::STORE, {},
                        {Address, MedVar::makeConst(Other == 1 ? 100 : 99,
                                                    Other == 3 ? 1 : 4)}));
        auto &Last = F.Blocks[3];
        Last.Preds = Reverse ? std::vector<int>{2, 1} : std::vector<int>{1, 2};
        for (unsigned I = 0; I < 4; ++I)
          Last.Ops.push_back(operation(NdOp::COPY,
                                       value(100 + I, Layout.Registers[I], A),
                                       {MedVar::makeConst(47 + I, 4)}));
        Last.Ops.push_back(
            operation(NdOp::CALL, {}, {MedVar::makeConst(0x2000, 4)}));
        checkFifthArgument(
            F, A, Other == 2 ? std::optional<uint64_t>(99) : std::nullopt);
      }
}
TEST(MIPSCallMemory, PartialOverwriteCannotAuthenticateAnOldSpilledCallee) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool CaptureBefore : {false, true})
      for (bool Left : {false, true}) {
        mips_test::ELFBytes F(A);
        F.Bytes.resize(0x188);
        F.put(52 + 16, F.Bytes.size(), 4);
        F.put(52 + 20, F.Bytes.size(), 4);
        // Save link and a known callee pointer, then partially replace the
        // pointer's slot. Only a register capture before the write survives.
        const uint32_t Store =
            ((Left ? 42u : 46u) << 26) | (29u << 21) | (9u << 16) | 17;
        const uint32_t Code[] = {0x27bdffe0,
                                 0xafbf001c,
                                 0x3c080001,
                                 0x25080180,
                                 0xafa80010,
                                 0x24091234,
                                 CaptureBefore ? 0x8fb90010u : Store,
                                 CaptureBefore ? Store : 0x8fb90010u,
                                 0x0320f809,
                                 0,
                                 0x8fbf001c,
                                 0x03e00008,
                                 0x27bd0020};
        for (unsigned N = 0; N < std::size(Code); ++N)
          F.put(0x100 + 4 * N, Code[N], 4);
        F.put(0x180, 0x03e00008, 4);
        F.put(0x184, 0x24020007, 4);
        ELFLoader Loader;
        auto I = Loader.loadBuffer(F.buffer());
        ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
        auto Caller = Symbol::makeFunc(I->Entry, sizeof(Code));
        Caller.Name = "partial_callee";
        auto Callee = Symbol::makeFunc(0x10180, 8);
        Callee.Name = "old_callee";
        I->Symbols.push_back(std::move(Caller));
        I->Symbols.push_back(std::move(Callee));
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.OnlyFunctionEntries = {I->Entry};
        Options.EmitDumpOutput = false;
        auto Result = Pipeline().run(*I, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_EQ(Result.MedFuncs.size(), 1u);
        ASSERT_EQ(Result.MedFuncs[0].CallInfos.size(), 1u);
        EXPECT_EQ(Result.MedFuncs[0].CallInfos[0].TargetAddr == 0x10180,
                  CaptureBefore);
      }
}

TEST(MIPSCallMemory, PartlyWrittenFifthArgumentCannotBeOmittedOrInvented) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool Overwrite : {false, true})
      for (bool Left : {false, true})
        for (bool Padded : {false, true}) {
          if (Padded && Overwrite)
            continue;
          mips_test::ELFBytes F(A);
          F.Bytes.resize(0x1a8);
          F.put(52 + 16, F.Bytes.size(), 4);
          F.put(52 + 20, F.Bytes.size(), 4);
          const uint32_t Partial =
              ((Left ? 42u : 46u) << 26) | (29u << 21) | (9u << 16) | 17;
          std::vector<uint32_t> Code = {0x27bdffe0, 0xafbf001c,
                                        0x2404002f, 0x24050030,
                                        0x24060031, 0x24070032,
                                        0x24080063, 0xafa80010,
                                        0x24091234, Overwrite ? Partial : 0u,
                                        0x0c004060, 0,
                                        0x8fbf001c, 0x03e00008,
                                        0x27bd0020};
          // Read all four register arguments and the actual fifth stack slot.
          std::vector<uint32_t> CalleeCode = {0x00851021, 0x00461021,
                                              0x00471021, 0x8fa80010,
                                              0x03e00008, 0x00481021};
          if (Padded) {
            // The outgoing wide pair at 24/28(sp) must not overwrite the
            // caller's saved link. Keep the larger frame eight-byte aligned.
            Code[0] = 0x27bdffd8;
            Code[1] = 0xafbf0024;
            Code[12] = 0x8fbf0024;
            Code[14] = 0x27bd0028;
            Code.insert(Code.begin() + 10, {0xafa90018, 0xafa9001c});
            CalleeCode.insert(CalleeCode.begin() + 4,
                              {0x8fa90018, 0x00491021, 0x8fa9001c, 0x00491021});
          }
          for (unsigned N = 0; N < std::size(Code); ++N)
            F.put(0x100 + 4 * N, Code[N], 4);
          for (unsigned N = 0; N < std::size(CalleeCode); ++N)
            F.put(0x180 + 4 * N, CalleeCode[N], 4);
          ELFLoader Loader;
          auto Image = Loader.loadBuffer(F.buffer());
          ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
          auto Caller = Symbol::makeFunc(Image->Entry, Code.size() * 4);
          Caller.Name = "partial_stack_argument";
          auto Callee = Symbol::makeFunc(0x10180, CalleeCode.size() * 4);
          Callee.Name = "five_arguments";
          Image->Symbols.push_back(std::move(Caller));
          Image->Symbols.push_back(std::move(Callee));
          for (bool LLVM : {false, true}) {
            SCOPED_TRACE(int(A));
            SCOPED_TRACE(Overwrite);
            SCOPED_TRACE(Left);
            SCOPED_TRACE(LLVM);
            SCOPED_TRACE(Padded);
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.OnlyFunctionEntries = {Image->Entry};
            Options.EmitDumpOutput = false;
            Options.LiftMode = LLVM;
            Options.SourceProjection = true;
            auto Result = Pipeline().run(*Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            ASSERT_EQ(Result.MedFuncs.size(), 1u);
            const auto &Calls = Result.MedFuncs[0].CallInfos;
            ASSERT_EQ(Calls.size(), 1u);
            ASSERT_EQ(Calls[0].Args.size(), Padded ? 8u : 5u);
            if (Overwrite)
              EXPECT_EQ(Calls[0].Args[4].Kind, MedVar::Unspecified);
            else {
              // Source projection retains the machine COPY values in MedIR.
              // Check the bound value, without requiring constant folding.
              const uint32_t Expected[] = {47, 48, 49,     50,
                                           99, 0,  0x1234, 0x1234};
              for (unsigned N = 0; N < Calls[0].Args.size(); ++N) {
                auto V = Calls[0].Args[N];
                if (!V.isConst())
                  for (const auto &B : Result.MedFuncs[0].Blocks)
                    for (const auto &Op : B.Ops)
                      if (Op.Output == V && Op.Opcode == NdOp::COPY &&
                          Op.NumInputs == 1 && Op.Inputs[0].isConst())
                        V = Op.Inputs[0];
                ASSERT_TRUE(V.isConst()) << "argument " << N;
                EXPECT_EQ(V.ConstVal, Expected[N]);
              }
            }
            CEmitterOptions Emit;
            Emit.TheArch = A;
            Emit.Format = BinaryFormat::ELF;
            Emit.Image = &*Image;
            std::string Text;
            llvm::raw_string_ostream Out(Text);
            ASSERT_TRUE(LLVM
                            ? LLVMCEmitter().emit(*Result.LlvmModule, Out, Emit,
                                                  nullptr, &*Image)
                            : HighCEmitter().emit(Result.HighFuncs, Out, Emit));
            // This slice cannot yet reconstruct a stack word after a partial
            // write. Both source routes must fail explicitly at that use.
            EXPECT_EQ(Text.find("__builtin_trap()") != std::string::npos,
                      Overwrite)
                << Text;
          }
        }
}
} // namespace
