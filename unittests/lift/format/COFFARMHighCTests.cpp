//===- COFFARMHighCTests.cpp - Windows ARM High C emission tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "COFFARMPipelineTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::coff_arm_test;

TEST_F(COFFARMPipeline, HighCX64StackBaseHasNativeEntryResidue) {
  HighFunc Func;
  Func.Name = "x64_stack_residue";
  Func.FrameSize = 4;
  Func.ReturnType = NdType::makeInt(4);

  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = Arch::X64;
  SP.Id = 1;
  SP.Size = 8;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  auto Addr = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP), HighExpr::makeConst(4, 8));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeLoad(Addr, NdType::makeInt(4));
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  EXPECT_NE(C.find("_Alignas(16) uint8_t stack_storage[24];"),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("(uintptr_t)(stack_storage + 24);"), std::string::npos)
      << C;
}

TEST_F(COFFARMPipeline, HighCI386StackBaseUsesBinaryFormatResidue) {
  auto EmitC = [&](BinaryFormat Format, llvm::StringRef Name) {
    HighFunc Func;
    Func.Name = Name.str();
    Func.FrameSize = 4;
    Func.ReturnType = NdType::makeInt(4);

    MedVar SP;
    SP.Kind = MedVar::Reg;
    SP.TheArch = Arch::X86;
    SP.Id = 1;
    SP.Size = 4;
    SP.RegOff = getTargetRegInfo(Arch::X86).StackPointer;
    auto Addr = HighExpr::makeBinop(
        NdOp::INT_SUB, HighExpr::makeVar(SP), HighExpr::makeConst(4, 4));
    HighStmt Ret;
    Ret.Kind = StmtKind::Return;
    Ret.RetVal = HighExpr::makeLoad(Addr, Func.ReturnType);
    Func.Body.push_back(std::move(Ret));

    std::string C;
    llvm::raw_string_ostream OS(C);
    CEmitterOptions Opts;
    Opts.TheArch = Arch::X86;
    Opts.Format = Format;
    EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
    OS.flush();
    return C;
  };

  std::string MachO = EmitC(BinaryFormat::MachO, "macho_i386_stack");
  EXPECT_NE(MachO.find("stack_storage[28]"), std::string::npos) << MachO;
  EXPECT_NE(MachO.find("(uintptr_t)(stack_storage + 28)"), std::string::npos)
      << MachO;

  std::string COFF = EmitC(BinaryFormat::COFF, "coff_i386_stack");
  EXPECT_NE(COFF.find("stack_storage[16]"), std::string::npos) << COFF;
  EXPECT_NE(COFF.find("(uintptr_t)(stack_storage + 16)"), std::string::npos)
      << COFF;
}

TEST_F(COFFARMPipeline, HighCLoadLvaluesAndAddressesRemainValidC) {
  HighFunc Func;
  Func.Name = "load_lvalue";
  Func.ReturnType = NdType::makePtr(NdType::makeInt(4));
  Func.Params.push_back({"arg0", NdType::makePtr(NdType::makeInt(4))});

  MedVar Arg;
  Arg.Kind = MedVar::Param;
  Arg.Id = 0;
  Arg.Size = 8;
  Arg.TheArch = Arch::AArch64;
  auto ArgExpr = HighExpr::makeVar(Arg, Func.Params[0].Type);
  auto Loaded = HighExpr::makeLoad(ArgExpr, NdType::makeInt(4));

  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = Loaded;
  Assign.Val = HighExpr::makeConst(7, 4);
  Func.Body.push_back(std::move(Assign));

  auto Address = std::make_shared<HighExpr>();
  Address->Kind = ExprKind::Addr;
  Address->Type = Func.ReturnType;
  Address->Operands.push_back(Loaded);
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = Address;
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  EXPECT_NE(C.find("neverd_mem_store_"), std::string::npos) << C;
  EXPECT_NE(C.find("(int32_t *)(uintptr_t)(arg0)"), std::string::npos) << C;
  EXPECT_EQ(C.find("&neverd_mem_load_"), std::string::npos) << C;

  const fs::path CPath = tmpFile("load_lvalue.c");
  std::ofstream Out(CPath);
  Out << C;
  Out.close();
  ASSERT_TRUE(Out.good());
  RunResult Syntax = exec("clang", {"-std=c11", "-fsyntax-only",
                                    CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

TEST_F(COFFARMPipeline, HighCForwardedParameterReturnDoesNotBecomeVoid) {
  HighFunc Func;
  Func.Name = "forwarded_parameter";
  Func.ReturnType = NdType::makeInt(4);
  Func.Params.push_back({"arg0", NdType::makeInt(4)});
  Func.Params.push_back({"arg1", NdType::makePtr(NdType::makeInt(4))});

  MedVar Value;
  Value.Kind = MedVar::Param;
  Value.Id = 0;
  Value.Size = 4;
  Value.TheArch = Arch::AArch64;
  MedVar Address = Value;
  Address.Id = 1;
  Address.Size = 8;

  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeVar(Address, Func.Params[1].Type);
  Store.StoreVal = HighExpr::makeVar(Value, Func.Params[0].Type);
  Func.Body.push_back(std::move(Store));

  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeLoad(
      HighExpr::makeVar(Address, Func.Params[1].Type), Func.ReturnType);
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  auto Body = cFunctionBody(C, "forwarded_parameter");
  ASSERT_TRUE(Body.has_value()) << C;
  EXPECT_NE(Body->find("return arg0;"), std::string::npos) << *Body;
  EXPECT_EQ(C.find("void forwarded_parameter("), std::string::npos) << C;

  const fs::path CPath = tmpFile("forwarded_parameter.c");
  std::ofstream Out(CPath);
  Out << C;
  Out.close();
  ASSERT_TRUE(Out.good());
  RunResult Syntax =
      exec("clang", {"-std=c11", "-fsyntax-only", CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

TEST_F(COFFARMPipeline, HighCForwardingPreservesFrameLvaluesAndAddresses) {
  HighFunc Func;
  Func.Name = "frame_lvalue_address";
  Func.FrameSize = 4;
  Func.ReturnType = NdType::makePtr(NdType::makeInt(4));

  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.TheArch = Arch::AArch64;
  SP.Id = 1;
  SP.Size = 8;
  SP.RegOff = getTargetRegInfo(Arch::AArch64).StackPointer;
  auto FrameAddr = [&]() {
    return HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeVar(SP),
                               HighExpr::makeConst(4, 8));
  };

  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = FrameAddr();
  Store.StoreVal = HighExpr::makeConst(7, 4);
  Func.Body.push_back(std::move(Store));

  HighStmt Read;
  Read.Kind = StmtKind::ExprStmt;
  Read.Val = HighExpr::makeLoad(FrameAddr(), NdType::makeInt(4));
  Func.Body.push_back(std::move(Read));

  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeLoad(FrameAddr(), NdType::makeInt(4));
  Assign.Val = HighExpr::makeConst(9, 4);
  Func.Body.push_back(std::move(Assign));

  auto Address = std::make_shared<HighExpr>();
  Address->Kind = ExprKind::Addr;
  Address->Type = Func.ReturnType;
  Address->Operands.push_back(
      HighExpr::makeLoad(FrameAddr(), NdType::makeInt(4)));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = Address;
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  auto Body = cFunctionBody(C, "frame_lvalue_address");
  ASSERT_TRUE(Body.has_value()) << C;
  EXPECT_NE(Body->find("stack_storage[16]"), std::string::npos) << *Body;
  EXPECT_NE(Body->find("frame_base - 4"), std::string::npos) << *Body;

  const fs::path CPath = tmpFile("frame_lvalue_address.c");
  std::ofstream Out(CPath);
  Out << C;
  Out.close();
  ASSERT_TRUE(Out.good());
  RunResult Syntax =
      exec("clang", {"-std=c11", "-fsyntax-only", CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

TEST_F(COFFARMPipeline, HighCAddressOfLoadDoesNotDeleteStore) {
  HighFunc Func;
  Func.Name = "address_preserves_store";
  Func.ReturnType = NdType::makePtr(NdType::makeInt(4));
  Func.Params.push_back({"arg0", NdType::makePtr(NdType::makeInt(4))});

  MedVar Address;
  Address.Kind = MedVar::Param;
  Address.Id = 0;
  Address.Size = 8;
  Address.TheArch = Arch::AArch64;
  auto ParamAddress = [&]() {
    return HighExpr::makeVar(Address, Func.Params[0].Type);
  };

  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = ParamAddress();
  Store.StoreVal = HighExpr::makeConst(7, 4);
  Func.Body.push_back(std::move(Store));

  auto AddressOf = std::make_shared<HighExpr>();
  AddressOf->Kind = ExprKind::Addr;
  AddressOf->Type = Func.ReturnType;
  AddressOf->Operands.push_back(
      HighExpr::makeLoad(ParamAddress(), NdType::makeInt(4)));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = AddressOf;
  Func.Body.push_back(std::move(Ret));

  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Opts;
  Opts.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Opts));
  OS.flush();

  auto Body = cFunctionBody(C, "address_preserves_store");
  ASSERT_TRUE(Body.has_value()) << C;
  EXPECT_NE(Body->find("neverd_mem_store_"), std::string::npos) << *Body;
  EXPECT_NE(Body->find(", 7);"), std::string::npos) << *Body;
  EXPECT_NE(Body->find("return (int32_t *)(uintptr_t)(arg0);"),
            std::string::npos)
      << *Body;

  const fs::path CPath = tmpFile("address_preserves_store.c");
  std::ofstream Out(CPath);
  Out << C;
  Out.close();
  ASSERT_TRUE(Out.good());
  RunResult Syntax =
      exec("clang", {"-std=c11", "-fsyntax-only", CPath.string()});
  EXPECT_EQ(Syntax.exitCode, 0) << Syntax.err << "\n" << C;
}

} // namespace
