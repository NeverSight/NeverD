//===- COFFARMStackAnalysisTests.cpp - Windows ARM stack frame analysis tests -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFARMPipelineTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::coff_arm_test;

TEST_F(COFFARMPipeline, X86_64StackFrameUsesDefinedEntrySP) {
  const fs::path Path = fixture("test_roundtrip.o");
  ASSERT_TRUE(fs::exists(Path));

  RunResult Decompile = decompileToHighC(Path);
  ASSERT_EQ(Decompile.exitCode, 0) << Decompile.err;
  std::string C = readTextFile(tmpFile("decompiled_high.c"));
  auto LoopC = cFunctionBody(C, "rt_for_loop");
  auto StartC = cFunctionBody(C, "start");
  ASSERT_TRUE(LoopC.has_value()) << C;
  ASSERT_TRUE(StartC.has_value()) << C;
  for (llvm::StringRef Body :
       {llvm::StringRef(*LoopC), llvm::StringRef(*StartC)}) {
    EXPECT_TRUE(Body.contains("stack_storage[")) << Body.str();
    expectNoLocalReadBeforeDefinition(Body);
    expectFrameBaseInitializedOnce(Body);
  }
}

TEST_F(COFFARMPipeline, HighCStackStorageIsAlignedBoundedAndAliasSafe) {
  HighFunc Func;
  Func.Name = "stack_bounds";
  Func.FrameSize = 5;
  Func.FrameHeadroom = 8;
  Func.ReturnType = NdType::makeInt(4);

  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = Arch::AArch64;
  SP.Id = 1;
  SP.Size = 8;
  SP.RegOff = getTargetRegInfo(Arch::AArch64).StackPointer;
  auto Addr = HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeVar(SP), HighExpr::makeConst(4, 8));
  auto Store = std::make_shared<HighExpr>();
  Store->Kind = ExprKind::Store;
  Store->Type = NdType::makeInt(4);
  Store->Operands.push_back(HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP), HighExpr::makeConst(4, 8)));
  Store->Operands.push_back(HighExpr::makeConst(7, 4));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeBinop(
      NdOp::INT_ADD, Store,
      HighExpr::makeLoad(Addr, NdType::makeInt(4)));
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  constexpr llvm::StringLiteral Storage =
      "_Alignas(16) uint8_t stack_storage[32];";
  size_t StorageAt = C.find(Storage);
  ASSERT_NE(StorageAt, std::string::npos) << C;
  EXPECT_EQ(C.find(Storage, StorageAt + Storage.size()), std::string::npos)
      << C;
  EXPECT_NE(C.find("const uintptr_t frame_base = "
                   "(uintptr_t)(stack_storage + 16);"),
            std::string::npos)
      << C;
  size_t FrameBaseAt = C.find("const uintptr_t frame_base =");
  ASSERT_NE(FrameBaseAt, std::string::npos) << C;
  EXPECT_EQ(C.find("const uintptr_t frame_base =", FrameBaseAt + 1),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("neverd_mem_load_"), std::string::npos) << C;
  EXPECT_NE(C.find("neverd_mem_store_"), std::string::npos) << C;
  EXPECT_NE(C.find("memcpy(&value, (const void *)address, sizeof(value));"),
            std::string::npos)
      << C;
  EXPECT_EQ(C.find("*(int32_t*)"), std::string::npos) << C;
  EXPECT_NE(C.find("frame_base + 4"), std::string::npos) << C;
  EXPECT_NE(C.find("frame_base - 4"), std::string::npos) << C;

  const fs::path CPath = tmpFile("stack_bounds.c");
  std::ofstream Out(CPath);
  Out << C;
  Out.close();
  ASSERT_TRUE(Out.good());
  RunResult Syntax = exec("clang", {"-std=c11", "-fsyntax-only",
                                    CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

TEST_F(COFFARMPipeline, StackBoundsSplitSlotStraddlingEntrySP) {
  constexpr Arch TheArch = Arch::X64;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "straddling_stack_slot";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Low.Entry;
  Block.EndAddr = Low.Entry + 1;

  NdVar Addr = NdVar::tmp(TmpBase, 8);
  LowOp FormAddr;
  FormAddr.Opcode = NdOp::INT_ADD;
  FormAddr.Output = Addr;
  FormAddr.addInput(NdVar::reg(TRI.StackPointer, 8));
  FormAddr.addInput(NdVar::cst(static_cast<uint64_t>(-4), 8));
  Block.Ops.push_back(FormAddr);

  NdVar Value = NdVar::tmp(TmpBase + TmpStride, 8);
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = Value;
  Load.addInput(Addr);
  Block.Ops.push_back(Load);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(Value);
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 4);
  EXPECT_EQ(Med.FrameHeadroom, 4);
}

TEST_F(COFFARMPipeline, StackAnalysisKillsAddressOnArbitraryRedefinition) {
  constexpr Arch TheArch = Arch::ARM;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "reused_frame_scratch";
  LowBlock Block;
  Block.Id = 0;

  NdVar Reused = NdVar::tmp(TmpBase, TRI.PointerSize);
  LowOp FormAddr;
  FormAddr.Opcode = NdOp::INT_SUB;
  FormAddr.Output = Reused;
  FormAddr.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  FormAddr.addInput(NdVar::cst(24, TRI.PointerSize));
  Block.Ops.push_back(FormAddr);

  LowOp RedefineAsData;
  RedefineAsData.Opcode = NdOp::INT_LEFT;
  RedefineAsData.Output = Reused;
  RedefineAsData.addInput(
      NdVar::reg(TRI.IntParamRegs.front(), TRI.PointerSize));
  RedefineAsData.addInput(NdVar::cst(24, TRI.PointerSize));
  Block.Ops.push_back(RedefineAsData);

  NdVar DataReg = NdVar::reg(TRI.IntParamRegs[2], TRI.PointerSize);
  LowOp CopyData;
  CopyData.Opcode = NdOp::COPY;
  CopyData.Output = DataReg;
  CopyData.addInput(Reused);
  Block.Ops.push_back(CopyData);

  LowOp ModerateDataAdd;
  ModerateDataAdd.Opcode = NdOp::INT_ADD;
  ModerateDataAdd.Output = DataReg;
  ModerateDataAdd.addInput(DataReg);
  ModerateDataAdd.addInput(NdVar::cst(100000, TRI.PointerSize));
  Block.Ops.push_back(ModerateDataAdd);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(DataReg);
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 24);
  EXPECT_EQ(Med.FrameHeadroom, 0);
}

TEST_F(COFFARMPipeline, StackAnalysisAccumulatesInPlaceSPUpdates) {
  constexpr Arch TheArch = Arch::X86;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "in_place_sp_updates";
  LowBlock Block;
  Block.Id = 0;
  NdVar SP = NdVar::reg(TRI.StackPointer, TRI.PointerSize);
  for (uint64_t Amount : {4u, 4u, 24u}) {
    LowOp Sub;
    Sub.Opcode = NdOp::INT_SUB;
    Sub.Output = SP;
    Sub.addInput(SP);
    Sub.addInput(NdVar::cst(Amount, TRI.PointerSize));
    Block.Ops.push_back(Sub);
  }
  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 32);
  EXPECT_EQ(Med.FrameHeadroom, 0);
}

TEST_F(COFFARMPipeline, StackAnalysisPropagatesAddressThroughWidthViews) {
  constexpr Arch TheArch = Arch::X86;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "frame_address_width_views";
  LowBlock Block;
  Block.Id = 0;

  NdVar Wide = NdVar::tmp(TmpBase, 8);
  LowOp Extend;
  Extend.Opcode = NdOp::INT_ZEXT;
  Extend.Output = Wide;
  Extend.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  Block.Ops.push_back(Extend);

  NdVar WideAddr = NdVar::tmp(TmpBase + TmpStride, 8);
  LowOp FormAddr;
  FormAddr.Opcode = NdOp::INT_SUB;
  FormAddr.Output = WideAddr;
  FormAddr.addInput(Wide);
  FormAddr.addInput(NdVar::cst(8, 8));
  Block.Ops.push_back(FormAddr);

  NdVar NarrowAddr = NdVar::tmp(TmpBase + 2 * TmpStride, TRI.PointerSize);
  LowOp Narrow;
  Narrow.Opcode = NdOp::SUBBYTES;
  Narrow.Output = NarrowAddr;
  Narrow.addInput(WideAddr);
  Narrow.addInput(NdVar::cst(0, 8));
  Block.Ops.push_back(Narrow);

  NdVar Value = NdVar::tmp(TmpBase + 3 * TmpStride, TRI.PointerSize);
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = Value;
  Load.addInput(NarrowAddr);
  Block.Ops.push_back(Load);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.addInput(Value);
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 8);
  EXPECT_EQ(Med.FrameHeadroom, 0);
}

TEST_F(COFFARMPipeline, StackAnalysisRetainsFrameBaseThroughDynamicIndex) {
  constexpr Arch TheArch = Arch::X64;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "red_zone_dynamic_index";
  LowBlock Block;
  Block.Id = 0;

  NdVar Addr = NdVar::tmp(TmpBase, TRI.PointerSize);
  LowOp CopySP;
  CopySP.Opcode = NdOp::COPY;
  CopySP.Output = Addr;
  CopySP.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  Block.Ops.push_back(CopySP);

  LowOp AddIndex;
  AddIndex.Opcode = NdOp::INT_ADD;
  AddIndex.Output = Addr;
  AddIndex.addInput(Addr);
  AddIndex.addInput(NdVar::reg(TRI.IntParamRegs.front(), TRI.PointerSize));
  Block.Ops.push_back(AddIndex);

  LowOp AddBound;
  AddBound.Opcode = NdOp::INT_SUB;
  AddBound.Output = Addr;
  AddBound.addInput(Addr);
  AddBound.addInput(NdVar::cst(105, TRI.PointerSize));
  Block.Ops.push_back(AddBound);

  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Addr);
  Store.addInput(NdVar::cst(1, 1));
  Block.Ops.push_back(Store);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 105);
  EXPECT_EQ(Med.FrameHeadroom, 0);
}

TEST_F(COFFARMPipeline, StackAnalysisScopesFlagPairsToInstruction) {
  constexpr Arch TheArch = Arch::X64;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "frame_address_reuses_flag_temporaries";
  LowBlock Block;
  Block.Id = 0;

  NdVar Addr = NdVar::tmp(TmpBase, TRI.PointerSize);
  NdVar Index = NdVar::tmp(TmpBase + TmpStride, TRI.PointerSize);

  LowOp UnrelatedFlag;
  UnrelatedFlag.Opcode = NdOp::INT_CARRY;
  UnrelatedFlag.Addr = 0x1000;
  UnrelatedFlag.Output = NdVar::tmp(TmpBase + 2 * TmpStride, 1);
  UnrelatedFlag.addInput(Addr);
  UnrelatedFlag.addInput(Index);
  Block.Ops.push_back(UnrelatedFlag);

  LowOp CopySP;
  CopySP.Opcode = NdOp::COPY;
  CopySP.Addr = 0x1004;
  CopySP.Output = Addr;
  CopySP.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  Block.Ops.push_back(CopySP);

  LowOp CopyIndex;
  CopyIndex.Opcode = NdOp::COPY;
  CopyIndex.Addr = 0x1004;
  CopyIndex.Output = Index;
  CopyIndex.addInput(
      NdVar::reg(TRI.IntParamRegs.front(), TRI.PointerSize));
  Block.Ops.push_back(CopyIndex);

  LowOp AddIndex;
  AddIndex.Opcode = NdOp::INT_ADD;
  AddIndex.Addr = 0x1004;
  AddIndex.Output = Addr;
  AddIndex.addInput(Addr);
  AddIndex.addInput(Index);
  Block.Ops.push_back(AddIndex);

  LowOp AddBound;
  AddBound.Opcode = NdOp::INT_SUB;
  AddBound.Addr = 0x1004;
  AddBound.Output = Addr;
  AddBound.addInput(Addr);
  AddBound.addInput(NdVar::cst(128, TRI.PointerSize));
  Block.Ops.push_back(AddBound);

  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.Addr = 0x1004;
  Store.addInput(Addr);
  Store.addInput(NdVar::cst(1, 1));
  Block.Ops.push_back(Store);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 128);
  EXPECT_EQ(Med.FrameHeadroom, 0);
}

TEST_F(COFFARMPipeline, StackAnalysisRefinesWidthBeforeAddressReuse) {
  constexpr Arch TheArch = Arch::X64;
  const auto &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "frame_width_before_address_reuse";
  LowBlock Block;
  Block.Id = 0;

  NdVar Addr = NdVar::tmp(TmpBase, TRI.PointerSize);
  LowOp FormAddr;
  FormAddr.Opcode = NdOp::INT_SUB;
  FormAddr.Output = Addr;
  FormAddr.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  FormAddr.addInput(NdVar::cst(32, TRI.PointerSize));
  Block.Ops.push_back(FormAddr);

  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Addr);
  Store.addInput(NdVar::cst(1, 16));
  Block.Ops.push_back(Store);

  LowOp ReuseAsData;
  ReuseAsData.Opcode = NdOp::INT_XOR;
  ReuseAsData.Output = Addr;
  ReuseAsData.addInput(NdVar::reg(TRI.IntParamRegs.front(), TRI.PointerSize));
  ReuseAsData.addInput(NdVar::cst(0x55, TRI.PointerSize));
  Block.Ops.push_back(ReuseAsData);

  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Ret);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  EXPECT_EQ(Med.FrameSize, 32);
  EXPECT_TRUE(std::any_of(Med.Locals.begin(), Med.Locals.end(),
                          [](const MedVar &Local) {
                            return Local.StackOff == -32 && Local.Size == 16;
                          }));
}

TEST_F(COFFARMPipeline, LLVMStackStorageMergesHeadroomAndPreservesX64Residue) {
  auto MakeVoidFunc = [](llvm::StringRef Name) {
    MedFunc Func;
    Func.Name = Name.str();
    Func.ReturnType = NdType::makeVoid();
    MedBlock Block;
    Block.Id = 0;
    MedOp Ret;
    Ret.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Ret);
    Func.Blocks.push_back(std::move(Block));
    return Func;
  };
  auto EmitIR = [](const MedFunc &Func, Arch TheArch,
                   BinaryFormat Format = BinaryFormat::ELF) {
    llvm::LLVMContext Ctx;
    auto Module = MedLLVMEmitter().emit({Func}, Ctx, "stack_test", TheArch, {},
                                        nullptr, Format);
    EXPECT_NE(Module, nullptr);
    std::string IR;
    llvm::raw_string_ostream OS(IR);
    if (Module)
      Module->print(OS, nullptr);
    OS.flush();
    return IR;
  };

  MedFunc Variadic = MakeVoidFunc("variadic_headroom");
  Variadic.FrameHeadroom = 64;
  Variadic.IsVariadic = true;
  Variadic.VariadicOverflowBase = 0;
  Variadic.VariadicOverflowCount = 1;
  std::string VariadicIR = EmitIR(Variadic, Arch::AArch64);
  EXPECT_NE(VariadicIR.find("alloca [128 x i8]"), std::string::npos)
      << VariadicIR;

  MedFunc NativeVariadic = MakeVoidFunc("native_variadic_overflow");
  NativeVariadic.FrameSize = 16;
  NativeVariadic.FrameHeadroom = 64;
  NativeVariadic.IsVariadic = true;
  std::string NativeVariadicIR = EmitIR(NativeVariadic, Arch::AArch64);
  EXPECT_NE(NativeVariadicIR.find("alloca [16 x i8]"), std::string::npos)
      << NativeVariadicIR;
  EXPECT_EQ(NativeVariadicIR.find("alloca [96 x i8]"), std::string::npos)
      << NativeVariadicIR;

  MedFunc X64 = MakeVoidFunc("x64_entry_residue");
  X64.FrameSize = 4;
  std::string X64IR = EmitIR(X64, Arch::X64);
  EXPECT_NE(X64IR.find("alloca [24 x i8]"), std::string::npos) << X64IR;
  EXPECT_NE(X64IR.find("getelementptr inbounds i8, ptr %frame, i64 24"),
            std::string::npos)
      << X64IR;

  MedFunc I386 = MakeVoidFunc("i386_entry_residue");
  I386.FrameSize = 4;
  std::string MachOIR = EmitIR(I386, Arch::X86, BinaryFormat::MachO);
  EXPECT_NE(MachOIR.find("alloca [28 x i8]"), std::string::npos) << MachOIR;
  EXPECT_NE(MachOIR.find("getelementptr inbounds i8, ptr %frame, i64 28"),
            std::string::npos)
      << MachOIR;

  std::string COFFIR = EmitIR(I386, Arch::X86, BinaryFormat::COFF);
  EXPECT_NE(COFFIR.find("alloca [16 x i8]"), std::string::npos) << COFFIR;
  EXPECT_NE(COFFIR.find("getelementptr inbounds i8, ptr %frame, i64 16"),
            std::string::npos)
      << COFFIR;
}

} // namespace
