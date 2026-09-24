//===- HighCPointerAddressTests.cpp - Raw byte address projection --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#ifndef NEVERD_BINARY_CORPUS_ROOT
#define NEVERD_BINARY_CORPUS_ROOT ""
#endif

namespace {
using namespace neverd;

ExprPtr parameter(unsigned Id, TypeRef ExprType = NdType::makeInt(8)) {
  MedVar Var;
  Var.Kind = MedVar::Param;
  Var.Id = static_cast<int>(Id);
  Var.Size = 8;
  Var.TheArch = Arch::X64;
  return HighExpr::makeVar(Var, ExprType);
}

void returnValue(HighFunc &Func, ExprPtr Value) {
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Value);
  Func.Body.push_back(std::move(Return));
}

HighFunc pointerFunction(const char *Name, TypeRef ReturnType) {
  HighFunc Func;
  Func.Name = Name;
  Func.ReturnType = ReturnType;
  Func.Params = {{"arg0", NdType::makePtr(NdType::makeInt(4))},
                 {"arg1", NdType::makeInt(8, false)}};
  return Func;
}

ExprPtr byteOffset(ExprPtr Base = parameter(0)) {
  return HighExpr::makeBinop(NdOp::INT_ADD, Base,
                             HighExpr::makeBinop(NdOp::INT_MULT, parameter(1),
                                                 HighExpr::makeConst(4, 8)));
}

std::string emitFunctions(const std::vector<HighFunc> &Functions,
                          Arch TheArch = Arch::X64,
                          const BinaryImage *Image = nullptr) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = TheArch;
  Options.Image = Image;
  EXPECT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  OS.flush();
  return Source;
}

TEST(HighCPointerAddresses, TypedAndMachineWidthParametersUseByteOffsets) {
  for (Arch TheArch : {Arch::X64, Arch::AArch64}) {
    for (bool TypedExpr : {false, true}) {
      SCOPED_TRACE(static_cast<int>(TheArch));
      SCOPED_TRACE(TypedExpr);
      auto Func = pointerFunction("indexed_load", NdType::makeInt(4));
      auto Base =
          parameter(0, TypedExpr ? Func.Params[0].Type : NdType::makeInt(8));
      returnValue(Func, HighExpr::makeLoad(byteOffset(Base), Func.ReturnType));
      const std::string Source = emitFunctions({Func}, TheArch);
      EXPECT_NE(Source.find("int32_t* arg0"), std::string::npos) << Source;
      EXPECT_NE(Source.find("(uintptr_t)arg0"), std::string::npos) << Source;
      EXPECT_NE(Source.find(" * "), std::string::npos) << Source;
      EXPECT_EQ(Source.find("(uintptr_t)(arg0 +"), std::string::npos) << Source;
    }
  }
}

TEST(HighCPointerAddresses, CastsPointerReturnsAfterMachineArithmetic) {
  auto Func = pointerFunction("advance", NdType::makePtr(NdType::makeInt(4)));
  returnValue(Func, byteOffset());
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("return (int32_t*)(uintptr_t)("), std::string::npos)
      << Source;

  auto Identity = pointerFunction("identity", Func.ReturnType);
  returnValue(Identity, parameter(0));
  const std::string IdentitySource = emitFunctions({Identity});
  EXPECT_NE(IdentitySource.find("return arg0;"), std::string::npos)
      << IdentitySource;
  EXPECT_EQ(IdentitySource.find("(uintptr_t)arg0"), std::string::npos)
      << IdentitySource;
}

TEST(HighCPointerAddresses, PreservesParameterLvaluesAndAddressOf) {
  auto Func = pointerFunction("assign_then_load", NdType::makeInt(4));
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = parameter(0);
  Assign.Val = byteOffset();
  Func.Body.push_back(std::move(Assign));
  returnValue(Func, HighExpr::makeLoad(parameter(0), Func.ReturnType));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("arg0 = (int32_t*)(uintptr_t)("), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("(uintptr_t)arg0 ="), std::string::npos) << Source;

  auto Address =
      pointerFunction("address_of", NdType::makePtr(Func.Params[0].Type));
  auto Addr = std::make_shared<HighExpr>();
  Addr->Kind = ExprKind::Addr;
  Addr->Type = Address.ReturnType;
  Addr->Operands.push_back(parameter(0));
  returnValue(Address, Addr);
  const std::string AddressSource = emitFunctions({Address});
  EXPECT_NE(AddressSource.find("return &arg0;"), std::string::npos)
      << AddressSource;
  EXPECT_EQ(AddressSource.find("&(uintptr_t)"), std::string::npos)
      << AddressSource;
}

TEST(HighCPointerAddresses, DoesNotRetypeIntegerParametersOrRenamedLocals) {
  auto Func = pointerFunction("integer_address", NdType::makeInt(8));
  Func.Params[0].Type = NdType::makeInt(8);
  returnValue(Func, byteOffset());
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("int64_t integer_address(int64_t arg0, uint64_t arg1)"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("(uintptr_t)arg0"), std::string::npos) << Source;

  auto Local = pointerFunction("renamed_local", NdType::makeInt(8));
  auto Renamed = parameter(0);
  Renamed->Var.RenameTag = 3;
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = Renamed;
  Assign.Val = HighExpr::makeConst(12, 8);
  Local.Body.push_back(std::move(Assign));
  returnValue(Local, Renamed);
  const std::string LocalSource = emitFunctions({Local});
  EXPECT_NE(LocalSource.find("return v3;"), std::string::npos) << LocalSource;
  EXPECT_EQ(LocalSource.find("(uintptr_t)v3"), std::string::npos)
      << LocalSource;
}

TEST(HighCPointerAddresses, EmittedCExecutesByteLoadsStoresAndPointerResults) {
  const auto I32 = NdType::makeInt(4);
  const auto I32Ptr = NdType::makePtr(I32);
  std::vector<HighFunc> Functions;

  auto Load = pointerFunction("indexed_load", I32);
  returnValue(Load, HighExpr::makeLoad(byteOffset(), I32));
  Functions.push_back(std::move(Load));

  auto Advance = pointerFunction("advance", I32Ptr);
  returnValue(Advance, byteOffset());
  Functions.push_back(std::move(Advance));

  auto Identity = pointerFunction("identity", I32Ptr);
  returnValue(Identity, parameter(0));
  Functions.push_back(std::move(Identity));

  auto Difference = pointerFunction("difference", NdType::makeInt(8));
  Difference.Params[1].Type = I32Ptr;
  returnValue(Difference,
              HighExpr::makeBinop(NdOp::INT_SUB, parameter(0), parameter(1)));
  Functions.push_back(std::move(Difference));

  auto Mask = pointerFunction("address_mask", NdType::makeInt(8, false));
  returnValue(Mask, HighExpr::makeBinop(NdOp::INT_AND, parameter(0),
                                        HighExpr::makeConst(255, 8)));
  Functions.push_back(std::move(Mask));

  auto Store = pointerFunction("indexed_store", I32);
  HighStmt Write;
  Write.Kind = StmtKind::Store;
  Write.StoreAddr = byteOffset();
  Write.StoreVal = HighExpr::makeConst(91, 4);
  Write.MemoryOrdering = NdMemoryOrdering::Relaxed;
  Store.Body.push_back(std::move(Write));
  returnValue(Store, HighExpr::makeConst(0, 4));
  Functions.push_back(std::move(Store));

  auto Assign = pointerFunction("assign_then_load", I32);
  HighStmt Change;
  Change.Kind = StmtKind::Assign;
  Change.Dst = parameter(0);
  Change.Val = byteOffset();
  Assign.Body.push_back(std::move(Change));
  returnValue(Assign, HighExpr::makeLoad(parameter(0), I32));
  Functions.push_back(std::move(Assign));

  for (unsigned Form = 0; Form < 3; ++Form) {
    auto PointerStore = pointerFunction("pointer_store", I32Ptr);
    PointerStore.Name += std::to_string(Form);
    PointerStore.Params = {{"arg0", NdType::makePtr(I32Ptr)}, {"arg1", I32Ptr}};
    HighStmt WritePointer;
    auto Address = parameter(0, PointerStore.Params[0].Type);
    auto Value = parameter(1, I32Ptr);
    if (Form == 0) {
      WritePointer.Kind = StmtKind::Store;
      WritePointer.StoreAddr = Address;
      WritePointer.StoreVal = Value;
      WritePointer.MemoryOrdering = NdMemoryOrdering::Release;
    } else if (Form == 1) {
      WritePointer.Kind = StmtKind::Assign;
      WritePointer.Dst = HighExpr::makeLoad(Address, I32Ptr);
      WritePointer.Val = Value;
    } else {
      WritePointer.Kind = StmtKind::ExprStmt;
      WritePointer.Val = std::make_shared<HighExpr>();
      WritePointer.Val->Kind = ExprKind::Store;
      WritePointer.Val->Type = I32Ptr;
      WritePointer.Val->Operands = {Address, Value};
    }
    PointerStore.Body.push_back(std::move(WritePointer));
    returnValue(PointerStore, HighExpr::makeLoad(Address, I32Ptr));
    Functions.push_back(std::move(PointerStore));
  }

  std::string Source = emitFunctions(Functions);
  Source += R"(
int main(void) {
    int32_t values[16] = {3, 17, 29, 41, 53, 67, 79, 83};
    if (indexed_load(values, 1) != 17) return 1;
    if (advance(values, 2) != &values[2]) return 2;
    if (identity(values, 0) != values) return 3;
    if (difference(&values[3], &values[1]) != 8) return 4;
    if (address_mask(values, 0) != ((uintptr_t)values & 255)) return 5;
    indexed_store(values, 1);
    if (values[1] != 91 || values[4] != 53) return 6;
    if (assign_then_load(values, 2) != 29) return 7;
    int32_t *slot = 0;
    if (pointer_store0(&slot, &values[3]) != &values[3] || slot != &values[3]) return 8;
    if (pointer_store1(&slot, &values[7]) != &values[7] || slot != &values[7]) return 9;
    if (pointer_store2(&slot, &values[2]) != &values[2] || slot != &values[2]) return 10;
    if (pointer_store0(&slot, 0) || slot) return 11;
    if (pointer_store1(&slot, 0) || slot) return 12;
    if (pointer_store2(&slot, 0) || slot) return 13;
    if (advance(&values[2], UINT64_MAX) != &values[1]) return 14;
    if (advance(values, (UINT64_C(1) << 62) + 1) != &values[1]) return 15;
    if (difference(&values[1], &values[3]) != -8) return 16;
    return 0;
}
)";

#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto FoundCompiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(FoundCompiler)) << "clang is required";
  const std::string Compiler = *FoundCompiler;
#endif
  llvm::SmallString<128> SourcePath, ExecutablePath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-pointer-values", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-pointer-values",
                                                  "exe", ExecutablePath));
  llvm::FileRemover RemoveExecutable(ExecutablePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-pointer-values",
                                                  "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC) << EC.message();
    OS << Source;
  }
  llvm::SmallVector<llvm::StringRef, 12> Arguments{
      Compiler,
      "-std=c11",
      "-O1",
      "-Werror=int-conversion",
      "-Werror=incompatible-pointer-types",
      "-fsanitize=signed-integer-overflow",
      "-fsanitize-trap=signed-integer-overflow",
      SourcePath,
      "-o",
      ExecutablePath};
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  std::string Error;
  const int CompileStatus = llvm::sys::ExecuteAndWait(
      Compiler, Arguments, std::nullopt, Redirects, 30, 0, &Error);
  auto ErrorBuffer = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(CompileStatus, 0)
      << Error << (ErrorBuffer ? (*ErrorBuffer)->getBuffer().str() : "") << "\n"
      << Source;
  llvm::SmallVector<llvm::StringRef, 1> RunArguments{ExecutablePath};
  EXPECT_EQ(llvm::sys::ExecuteAndWait(ExecutablePath, RunArguments,
                                      std::nullopt, {}, 30, 0, &Error),
            0)
      << Error << "\n"
      << Source;
}

TEST(HighCPointerAddresses, BytePointersKeepCPointerArithmetic) {
  auto Func = pointerFunction("byte_load", NdType::makeInt(1, false));
  Func.Params[0].Type = NdType::makePtr(NdType::makeInt(1, false));
  returnValue(Func, HighExpr::makeLoad(
                        HighExpr::makeBinop(NdOp::INT_ADD,
                                            parameter(0, Func.Params[0].Type),
                                            parameter(1)),
                        Func.ReturnType));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("uint8_t* arg0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("(uintptr_t)arg0"), std::string::npos) << Source;
  EXPECT_NE(Source.find("*(uint8_t *)(arg0 +"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, FrameSlotsRenderAsNamedLocalsAndAddressOf) {
  const auto I32 = NdType::makeInt(4);
  HighFunc Func;
  Func.Name = "frame_slot";
  Func.FrameSize = 16;
  Func.ReturnType = NdType::makePtr(I32);
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Size = 8;
  SP.TheArch = Arch::X64;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  auto SlotAddr = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP, NdType::makeInt(8, false)),
      HighExpr::makeConst(8, 8));
  HighStmt Init;
  Init.Kind = StmtKind::Store;
  Init.StoreAddr = SlotAddr;
  Init.StoreVal = HighExpr::makeConst(7, 4);
  Func.Body.push_back(std::move(Init));
  auto Addr = std::make_shared<HighExpr>();
  Addr->Kind = ExprKind::Addr;
  Addr->Type = Func.ReturnType;
  Addr->Operands.push_back(HighExpr::makeLoad(SlotAddr, I32));
  returnValue(Func, Addr);
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("int32_t var_m8;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("var_m8 = 7;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("return &var_m8;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("stack_storage"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("neverd_mem_"), std::string::npos) << Source;
}

BinaryImage makeImageObjectFixture(va_t Addr, std::vector<uint8_t> Bytes,
                                   bool Writable) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Segment Seg;
  Seg.Name = Writable ? ".data" : ".rdata";
  Seg.VA = Addr;
  Seg.Size = Bytes.size();
  Seg.Flags = SegmentFlags::Readable;
  if (Writable)
    Seg.Flags = Seg.Flags | SegmentFlags::Writable;
  Seg.Data = std::move(Bytes);
  Img.Segments.push_back(std::move(Seg));
  return Img;
}

TEST(HighCPointerAddresses, FoldsReadonlyImageIntegerLoad) {
  BinaryImage Img =
      makeImageObjectFixture(0x140003260, {0x01, 0x10, 0x42, 0xE0}, false);
  HighFunc Func;
  Func.Name = "load_code";
  Func.ReturnType = NdType::makeInt(4, false);
  returnValue(Func, HighExpr::makeLoad(HighExpr::makeConst(0x140003260, 8),
                                       Func.ReturnType));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("0xE0421001"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("0x140003260"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, NamesImageDataFromDebugObject) {
  BinaryImage Img =
      makeImageObjectFixture(0x1400050E0, {0, 0, 0, 0, 0, 0, 0, 0}, true);
  class NamedData : public NullDebugContext {
  public:
    std::vector<DataObjectSym> allDataObjects() const override {
      return {{"__security_cookie", 0x1400050E0, 0, false}};
    }
    bool hasInfo() const override { return true; }
  } Dbg;
  HighFunc Func;
  Func.Name = "load_cookie";
  Func.ReturnType = NdType::makeInt(8);
  returnValue(Func, HighExpr::makeLoad(HighExpr::makeConst(0x1400050E0, 8),
                                       Func.ReturnType));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options, &Dbg));
  OS.flush();
  EXPECT_NE(Source.find("__security_cookie"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("g_1400050E0"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, KeepsLeadingUnderscoreRuntimeNames) {
  HighFunc Func;
  Func.Name = "__security_check_cookie";
  Func.ReturnType = NdType::makeVoid();
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("_security_check_cookie("), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("nd__security_check_cookie"), std::string::npos)
      << Source;
}

TEST(HighCPointerAddresses, RotateOrPrintsBuiltin) {
  HighFunc Func;
  Func.Name = "rol16";
  Func.ReturnType = NdType::makeInt(8, false);
  Func.Params = {{"arg0", NdType::makeInt(8, false)}};
  auto Arg = parameter(0, NdType::makeInt(8, false));
  auto Shl =
      HighExpr::makeBinop(NdOp::INT_LEFT, Arg, HighExpr::makeConst(16, 8));
  auto Shr =
      HighExpr::makeBinop(NdOp::INT_RIGHT, Arg, HighExpr::makeConst(48, 8));
  Shl->Type = NdType::makeInt(8, false);
  Shr->Type = NdType::makeInt(8, false);
  auto Rot = HighExpr::makeBinop(NdOp::INT_OR, Shl, Shr);
  Rot->Type = NdType::makeInt(8, false);
  returnValue(Func, Rot);
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("__builtin_rotateleft64(arg0, 16)"), std::string::npos)
      << Source;
}

TEST(HighCPointerAddresses, OmitsCopyForwardedTempDeclarations) {
  HighFunc Func;
  Func.Name = "fwd_temp";
  Func.ReturnType = NdType::makeInt(8);
  Func.Params = {{"arg0", NdType::makeInt(8)}};
  MedVar Temp;
  Temp.Kind = MedVar::Temp;
  Temp.Id = 1;
  Temp.Size = 8;
  Temp.TheArch = Arch::X64;
  HighStmt Copy;
  Copy.Kind = StmtKind::Assign;
  Copy.Dst = HighExpr::makeVar(Temp, NdType::makeInt(8));
  Copy.Val = parameter(0, NdType::makeInt(8));
  Func.Body.push_back(std::move(Copy));
  returnValue(Func, HighExpr::makeVar(Temp, NdType::makeInt(8)));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("return arg0;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("t1;"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, SbbCfIdiomDoesNotPrintUnknown) {
  HighFunc Func;
  Func.Name = "sbb_cf";
  Func.ReturnType = NdType::makeInt(4);
  Func.Params = {{"arg0", NdType::makeInt(4)}};
  auto Cond =
      HighExpr::makeBinop(NdOp::INT_NOTEQUAL, parameter(0, NdType::makeInt(4)),
                          HighExpr::makeConst(0, 4));
  Cond->Type = NdType::makeInt(4, false);
  auto Inner = HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeUndef(4), Cond);
  Inner->Type = NdType::makeInt(4, false);
  auto Outer =
      HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeConst(0, 4), Inner);
  Outer->Type = NdType::makeInt(4, false);
  returnValue(Func, Outer);
  const std::string Source = emitFunctions({Func});
  EXPECT_EQ(Source.find("0 /* unknown */"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("clobbered"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, EmptyIfIsNotPrinted) {
  HighFunc Func;
  Func.Name = "empty_if";
  Func.ReturnType = NdType::makeVoid();
  Func.Params = {{"arg0", NdType::makeInt(4)}};
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, parameter(0, NdType::makeInt(4)),
                          HighExpr::makeConst(0, 4));
  Func.Body.push_back(std::move(Branch));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_EQ(Source.find("if ("), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, MsvcDecorationIsNotTheOnlyCalleeSpelling) {
  HighFunc Func;
  Func.Name = "?Get@MemManager@Contoso@@SAAEAV12@XZ";
  Func.ReturnType = NdType::makeInt(8);
  HighStmt Call;
  Call.Kind = StmtKind::Return;
  Call.RetVal = HighExpr::makeCall(
      "?CreateGlobalMemoryAllocator@Contoso@@YAPEAVIAllocator@1@XZ",
      0x140001000, {});
  Call.RetVal->Type = NdType::makeInt(8);
  Func.Body.push_back(std::move(Call));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("Contoso_MemManager_Get"), std::string::npos) << Source;
  EXPECT_NE(Source.find("Contoso_CreateGlobalMemoryAllocator"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("_x3F_"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, NamesWritableImageDataStore) {
  BinaryImage Img = makeImageObjectFixture(0x1400050E0, {0, 0, 0, 0}, true);
  HighFunc Func;
  Func.Name = "store_sink";
  Func.ReturnType = NdType::makeVoid();
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeConst(0x1400050E0, 8);
  Store.StoreVal = HighExpr::makeConst(41, 4);
  Store.StoreVal->Type = NdType::makeInt(4, true);
  Func.Body.push_back(std::move(Store));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("int32_t g_1400050E0;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("g_1400050E0 = 41;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("*(int32_t *)(0x1400050E0)"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("dword_"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("data_1400050E0"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, NamesWritableNdDataGlobal) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-data", Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  auto *GV = new llvm::GlobalVariable(Module, I32, /*isConstant=*/false,
                                      llvm::GlobalValue::ExternalLinkage,
                                      nullptr, makeNdDataSymbol(0x1400050E0));
  llvm::FunctionType *FnTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "store_sink", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateStore(llvm::ConstantInt::get(I32, 41), GV);
  Builder.CreateRetVoid();

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("g_1400050E0"), std::string::npos) << Source;
  EXPECT_NE(Source.find("extern uint32_t g_1400050E0;"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("data_1400050E0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("dword_"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, DeclaresAssignedTempsAndUnusedCallResults) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-locals", Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  llvm::FunctionType *FnTy = llvm::FunctionType::get(I32, {I32}, false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "probe_like", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Code = Builder.CreateAdd(
      Function->getArg(0), llvm::ConstantInt::get(I32, 0xE0421001), "t22");
  llvm::FunctionType *RaiseTy =
      llvm::FunctionType::get(I64, {I32, I32, I32, I64}, false);
  llvm::Function *Raise = llvm::Function::Create(
      RaiseTy, llvm::GlobalValue::ExternalLinkage, "RaiseException", Module);
  Builder.CreateCall(Raise,
                     {Code, llvm::ConstantInt::get(I32, 0),
                      llvm::ConstantInt::get(I32, 0),
                      llvm::ConstantInt::get(I64, 0)},
                     "v36");
  Builder.CreateMul(Function->getArg(0), llvm::ConstantInt::get(I32, 3),
                    "dead_flag");
  Builder.CreateRet(Code);

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("uint32_t t22"), std::string::npos) << Source;
  EXPECT_NE(Source.find("uint64_t v36"), std::string::npos) << Source;
  EXPECT_NE(Source.find("uint32_t dead_flag"), std::string::npos) << Source;
  EXPECT_NE(Source.find("RaiseException"), std::string::npos) << Source;
  const size_t DeadAssign = Source.find("dead_flag");
  ASSERT_NE(DeadAssign, std::string::npos) << Source;
  EXPECT_NE(Source.find(" = ", DeadAssign), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, UsesDebugDataObjectName) {
  BinaryImage Img = makeImageObjectFixture(0x1400050E0, {0, 0, 0, 0}, true);
  class NamedDataDbg : public NullDebugContext {
  public:
    std::vector<DataObjectSym> allDataObjects() const override {
      return {{"ProbeSink", 0x1400050E0, 4, false}};
    }
    bool hasInfo() const override { return true; }
  } Dbg;
  HighFunc Func;
  Func.Name = "store_sink";
  Func.ReturnType = NdType::makeVoid();
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = HighExpr::makeConst(0x1400050E0, 8);
  Store.StoreVal = HighExpr::makeConst(41, 4);
  Store.StoreVal->Type = NdType::makeInt(4, true);
  Func.Body.push_back(std::move(Store));
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({Func}, OS, Options, &Dbg));
  OS.flush();
  EXPECT_NE(Source.find("int32_t ProbeSink;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("ProbeSink = 41;"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, DeclaresAssignedTempsAndUnusedCallResults) {
  HighFunc Func;
  Func.Name = "probe_like";
  Func.ReturnType = NdType::makeInt(4);
  Func.Params = {{"arg0", NdType::makeInt(4)}};

  MedVar Temp;
  Temp.Kind = MedVar::Temp;
  Temp.Id = 22;
  Temp.SSAVer = 1;
  Temp.Size = 4;
  Temp.TheArch = Arch::X64;

  MedVar CallDest;
  CallDest.Kind = MedVar::Reg;
  CallDest.Id = 36;
  CallDest.SSAVer = 0;
  CallDest.Size = 8;
  CallDest.TheArch = Arch::X64;

  MedVar PhiDest;
  PhiDest.Kind = MedVar::Reg;
  PhiDest.Id = 3;
  PhiDest.SSAVer = 0;
  PhiDest.Size = 8;
  PhiDest.TheArch = Arch::X64;

  HighStmt LoadCode;
  LoadCode.Kind = StmtKind::Assign;
  LoadCode.Dst = HighExpr::makeVar(Temp, NdType::makeInt(4, false));
  LoadCode.Val = HighExpr::makeConst(0xE0421001, 4);

  HighStmt Call;
  Call.Kind = StmtKind::Assign;
  Call.Dst = HighExpr::makeVar(CallDest, NdType::makeInt(8));
  Call.Val =
      HighExpr::makeCall("RaiseException", 0,
                         {HighExpr::makeVar(Temp, NdType::makeInt(4, false)),
                          HighExpr::makeConst(0, 4), HighExpr::makeConst(0, 4),
                          HighExpr::makeConst(0, 8)});
  Call.Val->Type = NdType::makeInt(8);

  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, parameter(0, NdType::makeInt(4)),
                          HighExpr::makeConst(7, 4));
  Branch.Body.push_back(std::move(LoadCode));
  Branch.Body.push_back(std::move(Call));
  Func.Body.push_back(std::move(Branch));

  HighStmt PhiAssign;
  PhiAssign.Kind = StmtKind::Assign;
  PhiAssign.Dst = HighExpr::makeVar(PhiDest, NdType::makeInt(8));
  PhiAssign.Dst->Kind = ExprKind::Phi;
  PhiAssign.Val = HighExpr::makeConst(41, 8);
  Func.Body.push_back(std::move(PhiAssign));

  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeVar(PhiDest, NdType::makeInt(4));
  Ret.RetVal->Kind = ExprKind::Phi;
  Func.Body.push_back(std::move(Ret));

  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("t22_1;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("v36_0;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("v3_0;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("t22_1 = "), std::string::npos) << Source;
  EXPECT_NE(Source.find("v36_0 = RaiseException"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, DeclaresFrameSlotAfterParamHomeOverwrite) {
  HighFunc Func;
  Func.Name = "home_then_write";
  Func.ReturnType = NdType::makeInt(4);
  Func.Params = {{"arg0", NdType::makeInt(4)}};
  Func.FrameSize = 16;
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Size = 8;
  SP.TheArch = Arch::X64;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  auto SlotAddr = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP, NdType::makeInt(8, false)),
      HighExpr::makeConst(8, 8));
  HighStmt Spill;
  Spill.Kind = StmtKind::Store;
  Spill.StoreAddr = SlotAddr;
  Spill.StoreVal = parameter(0, NdType::makeInt(4));
  Func.Body.push_back(std::move(Spill));
  HighStmt Overwrite;
  Overwrite.Kind = StmtKind::Store;
  Overwrite.StoreAddr = SlotAddr;
  Overwrite.StoreVal = HighExpr::makeConst(41, 4);
  Overwrite.StoreVal->Type = NdType::makeInt(4, true);
  Func.Body.push_back(std::move(Overwrite));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeLoad(SlotAddr, NdType::makeInt(4));
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("var_m8;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("var_m8 = 41;"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, AssignsPhiAtPredecessorEdges) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-phi", Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *FnTy =
      llvm::FunctionType::get(I32, {llvm::Type::getInt1Ty(Context)}, false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "join", Module);
  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Function);
  llvm::BasicBlock *Left = llvm::BasicBlock::Create(Context, "left", Function);
  llvm::BasicBlock *Right =
      llvm::BasicBlock::Create(Context, "right", Function);
  llvm::BasicBlock *Join = llvm::BasicBlock::Create(Context, "join", Function);
  llvm::IRBuilder<> EntryBuilder(Entry);
  EntryBuilder.CreateCondBr(Function->getArg(0), Left, Right);
  llvm::IRBuilder<> LeftBuilder(Left);
  LeftBuilder.CreateBr(Join);
  llvm::IRBuilder<> RightBuilder(Right);
  RightBuilder.CreateBr(Join);
  llvm::IRBuilder<> JoinBuilder(Join);
  llvm::PHINode *Phi = JoinBuilder.CreatePHI(I32, 2, "joined");
  Phi->addIncoming(llvm::ConstantInt::get(I32, 1), Left);
  Phi->addIncoming(llvm::ConstantInt::get(I32, 2), Right);
  JoinBuilder.CreateRet(Phi);

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("joined"), std::string::npos) << Source;
  EXPECT_NE(Source.find(" = 1;"), std::string::npos) << Source;
  EXPECT_NE(Source.find(" = 2;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("/* phi:"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, NamesIATIndirectCall) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-iat", Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  auto *Slot = new llvm::GlobalVariable(
      Module, I64, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
      nullptr, (kNdCodePtrPrefix + llvm::utohexstr(0x140003000)).str());
  llvm::FunctionType *FnTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "call_import", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Loaded = Builder.CreateLoad(I64, Slot, "icall.import.target");
  llvm::Value *Callee =
      Builder.CreateIntToPtr(Loaded, llvm::PointerType::getUnqual(Context));
  Builder.CreateCall(FnTy, Callee);
  Builder.CreateRetVoid();

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Import Imp;
  Imp.Name = "RaiseException";
  Imp.IATAddr = 0x140003000;
  Img.Imports.push_back(std::move(Imp));

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options, nullptr, &Img));
  OS.flush();
  EXPECT_NE(Source.find("RaiseException("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("((void*)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("icall_import_target"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, UndefOperandIsNotClobberComment) {
  HighFunc Func;
  Func.Name = "undef_add";
  Func.ReturnType = NdType::makeInt(4);
  MedVar Dst;
  Dst.Kind = MedVar::Temp;
  Dst.Id = 1;
  Dst.Size = 4;
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeVar(Dst);
  Assign.Val = HighExpr::makeBinop(NdOp::INT_ADD, HighExpr::makeUndef(4),
                                   HighExpr::makeConst(1, 4));
  Func.Body.push_back(std::move(Assign));
  returnValue(Func, HighExpr::makeVar(Dst));
  const std::string Source = emitFunctions({Func});
  EXPECT_EQ(Source.find("caller-saved register clobbered"), std::string::npos)
      << Source;
}

TEST(HighCPointerAddresses, CxxRethrowNullObjectPrintsBareThrow) {
  HighFunc Func;
  Func.Name = "rethrows";
  Func.Entry = 0x140001000;
  Func.ReturnType = NdType::makeVoid();
  HighStmt Throw;
  Throw.Kind = StmtKind::Call;
  Throw.CallExpr = HighExpr::makeCall(
      "_CxxThrowException", 0x140002000,
      {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8)});
  Func.Body.push_back(std::move(Throw));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("throw;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("throw 0"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, CxxThrowCallPrintsThrowWithoutDebugBreak) {
  HighFunc Func;
  Func.Name = "throws";
  Func.Entry = 0x140001000;
  Func.ReturnType = NdType::makeVoid();
  HighStmt Throw;
  Throw.Kind = StmtKind::Call;
  Throw.CallExpr = HighExpr::makeCall(
      "_CxxThrowException", 0x140002000,
      {HighExpr::makeConst(0x140001100, 8), HighExpr::makeConst(0, 8)});
  Func.Body.push_back(std::move(Throw));
  HighStmt Trap;
  Trap.Kind = StmtKind::Call;
  auto Int3 = std::make_shared<HighExpr>();
  Int3->Kind = ExprKind::Call;
  Int3->IntrinsicId = Intrinsic::Int3;
  Trap.CallExpr = Int3;
  Func.Body.push_back(std::move(Trap));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("throw 0x140001100"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__debugbreak"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, UnassignedRegisterReturnIsBareReturn) {
  HighFunc Func;
  Func.Name = "voidish";
  Func.ReturnType = NdType::makeInt(8);
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  MedVar Rax;
  Rax.Kind = MedVar::Reg;
  Rax.Id = 0;
  Rax.SSAVer = 3;
  Rax.Size = 8;
  Ret.RetVal = HighExpr::makeVar(Rax);
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("return;"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, AttachesCxxFuncletBodyIntoCatch) {
  HighFunc Parent;
  Parent.Name = "parent";
  Parent.Entry = 0x140001000;
  Parent.ReturnType = NdType::makeInt(4);
  HighStmt Try;
  Try.Kind = StmtKind::CxxTry;
  Try.EHIsReducible = true;
  HighStmt TryReturn;
  TryReturn.Kind = StmtKind::Return;
  TryReturn.RetVal = HighExpr::makeConst(static_cast<uint64_t>(-100), 4);
  Try.Body.push_back(std::move(TryReturn));
  HighEHClause Catch;
  Catch.Kind = HighEHClauseKind::CxxCatch;
  Catch.HandlerVA = 0x140002000;
  Catch.TypeName = "ProbeError";
  Catch.Adjectives = 0x9;
  Try.EHClauses.push_back(std::move(Catch));
  Try.EHClauseBodies.emplace_back();
  Parent.Body.push_back(std::move(Try));

  HighFunc Handler;
  Handler.Name = "catch_funclet";
  Handler.Entry = 0x140002000;
  Handler.ReturnType = NdType::makeVoid();
  HighStmt Caught;
  Caught.Kind = StmtKind::Call;
  Caught.CallExpr = HighExpr::makeCall("caught", 0x140003000, {});
  Handler.Body.push_back(std::move(Caught));

  const std::string Source = emitFunctions({Parent, Handler});
  EXPECT_NE(Source.find("catch (const ProbeError &)"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("caught()"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("handler @"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, AttachesCxxUnwindFuncletAsDestructorCall) {
  HighFunc Parent;
  Parent.Name = "parent";
  Parent.Entry = 0x140001000;
  Parent.ReturnType = NdType::makeInt(4);
  Parent.FrameSize = 0x40;
  HighStmt Try;
  Try.Kind = StmtKind::CxxTry;
  Try.EHIsReducible = true;
  HighStmt TryReturn;
  TryReturn.Kind = StmtKind::Return;
  TryReturn.RetVal = HighExpr::makeConst(static_cast<uint64_t>(-100), 4);
  Try.Body.push_back(std::move(TryReturn));
  HighEHClause Cleanup;
  Cleanup.Kind = HighEHClauseKind::CxxCleanup;
  Cleanup.FilterOrActionVA = 0x140002000;
  Cleanup.State = 0;
  Cleanup.UnwindActionKind = CxxUnwindAction::ActionKind::Direct;
  Cleanup.UnwindObjectOffset = 40;
  Try.EHClauses.push_back(std::move(Cleanup));
  Try.EHClauseBodies.emplace_back();
  Parent.Body.push_back(std::move(Try));

  HighFunc Dtor;
  Dtor.Name = "unwind_funclet";
  Dtor.Entry = 0x140002000;
  Dtor.ReturnType = NdType::makeInt(8);
  MedVar T0;
  T0.Kind = MedVar::Temp;
  T0.Id = 22;
  T0.Size = 8;
  HighStmt CallDtor;
  CallDtor.Kind = StmtKind::Assign;
  CallDtor.Dst = HighExpr::makeVar(T0);
  CallDtor.Val = HighExpr::makeCall(
      "dtor", 0x140003000,
      {HighExpr::makeBinop(NdOp::INT_ADD, parameter(1),
                           HighExpr::makeConst(40, 8))});
  Dtor.Body.push_back(std::move(CallDtor));
  HighStmt UnwindRet;
  UnwindRet.Kind = StmtKind::Return;
  UnwindRet.RetVal = HighExpr::makeVar(T0);
  Dtor.Body.push_back(std::move(UnwindRet));

  const std::string Source = emitFunctions({Parent, Dtor});
  const auto FuncletAt = Source.find("unwind_funclet");
  const std::string ParentSrc =
      FuncletAt == std::string::npos ? Source : Source.substr(0, FuncletAt);
  EXPECT_NE(ParentSrc.find("unwind cleanup"), std::string::npos) << Source;
  EXPECT_NE(ParentSrc.find("dtor("), std::string::npos) << Source;
  EXPECT_EQ(ParentSrc.find("arg1"), std::string::npos) << Source;
  EXPECT_EQ(ParentSrc.find("return t22"), std::string::npos) << ParentSrc;
}

TEST(HighCPointerAddresses, CatchFuncletParentFrameStoreBecomesReturn) {
  // x64 catch funclets write the result to [rdx+k] then ret. rdx is the parent
  // frame, not a parent parameter, so HighC must not print arg1 / return v0.
  HighFunc Parent;
  Parent.Name = "parent";
  Parent.Entry = 0x140001000;
  Parent.ReturnType = NdType::makeInt(4);
  Parent.FrameSize = 0x40;
  HighStmt Try;
  Try.Kind = StmtKind::CxxTry;
  Try.EHIsReducible = true;
  HighStmt TryReturn;
  TryReturn.Kind = StmtKind::Return;
  TryReturn.RetVal = HighExpr::makeConst(static_cast<uint64_t>(-100), 4);
  Try.Body.push_back(std::move(TryReturn));

  MedVar T11;
  T11.Kind = MedVar::Temp;
  T11.Id = 11;
  T11.Size = 8;
  MedVar T25;
  T25.Kind = MedVar::Temp;
  T25.Id = 25;
  T25.Size = 4;
  HighStmt LoadPtr;
  LoadPtr.Kind = StmtKind::Assign;
  LoadPtr.Dst = HighExpr::makeVar(T11);
  LoadPtr.Val = HighExpr::makeLoad(
      HighExpr::makeBinop(NdOp::INT_ADD, parameter(1),
                          HighExpr::makeConst(40, 8)),
      NdType::makeInt(8));
  HighStmt LoadVal;
  LoadVal.Kind = StmtKind::Assign;
  LoadVal.Dst = HighExpr::makeVar(T25);
  LoadVal.Val = HighExpr::makeLoad(HighExpr::makeVar(T11), NdType::makeInt(4));
  HighStmt Home;
  Home.Kind = StmtKind::Store;
  Home.StoreAddr = HighExpr::makeBinop(NdOp::INT_ADD, parameter(1),
                                       HighExpr::makeConst(36, 8));
  Home.StoreVal = HighExpr::makeVar(T25, NdType::makeInt(4));
  HighStmt CatchRet;
  CatchRet.Kind = StmtKind::Return;
  MedVar Rax;
  Rax.Kind = MedVar::Reg;
  Rax.Id = 0;
  Rax.SSAVer = 3;
  Rax.RenameTag = 0;
  Rax.Size = 8;
  CatchRet.RetVal = HighExpr::makeVar(Rax);

  HighEHClause Catch;
  Catch.Kind = HighEHClauseKind::CxxCatch;
  Catch.TypeName = "ProbeError";
  Catch.Adjectives = 0x9;
  Catch.CatchObjectOffset = 40;
  Try.EHClauses.push_back(std::move(Catch));
  std::vector<HighStmt> CatchBody;
  CatchBody.push_back(std::move(LoadPtr));
  CatchBody.push_back(std::move(LoadVal));
  CatchBody.push_back(std::move(Home));
  CatchBody.push_back(std::move(CatchRet));
  Try.EHClauseBodies.push_back(std::move(CatchBody));
  Parent.Body.push_back(std::move(Try));

  const std::string Source = emitFunctions({Parent});
  EXPECT_EQ(Source.find("arg1"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return v0"), std::string::npos) << Source;
  EXPECT_NE(Source.find("return t25"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, AttachCxxFuncletBodiesDoesNotRecurseOnCyclicCatch) {
  HighFunc Parent;
  Parent.Name = "parent";
  Parent.Entry = 0x140001000;
  Parent.ReturnType = NdType::makeInt(4);
  HighStmt Try;
  Try.Kind = StmtKind::CxxTry;
  Try.EHIsReducible = true;
  HighEHClause Catch;
  Catch.Kind = HighEHClauseKind::CxxCatch;
  Catch.HandlerVA = 0x140002000;
  Catch.TypeName = "ProbeError";
  Try.EHClauses.push_back(std::move(Catch));
  Try.EHClauseBodies.emplace_back();
  Parent.Body.push_back(std::move(Try));

  HighFunc Handler;
  Handler.Name = "catch_funclet";
  Handler.Entry = 0x140002000;
  Handler.ReturnType = NdType::makeVoid();
  HighStmt Nested;
  Nested.Kind = StmtKind::CxxTry;
  Nested.EHIsReducible = true;
  HighEHClause NestedCatch;
  NestedCatch.Kind = HighEHClauseKind::CxxCatch;
  NestedCatch.HandlerVA = 0x140001000;
  Nested.EHClauses.push_back(std::move(NestedCatch));
  Nested.EHClauseBodies.emplace_back();
  HighStmt Caught;
  Caught.Kind = StmtKind::Call;
  Caught.CallExpr = HighExpr::makeCall("caught", 0x140003000, {});
  Nested.Body.push_back(std::move(Caught));
  Handler.Body.push_back(std::move(Nested));

  const std::string Source = emitFunctions({Parent, Handler});
  EXPECT_NE(Source.find("caught()"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, OmitsReturnAfterThrowDespiteJunkAssigns) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-throw-junk", Context);
  llvm::Type *Ptr = llvm::PointerType::getUnqual(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *ThrowTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(Context), {Ptr, Ptr}, false);
  llvm::Function *ThrowFn =
      llvm::Function::Create(ThrowTy, llvm::GlobalValue::ExternalLinkage,
                             "_CxxThrowException", Module);
  llvm::FunctionType *FnTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "throws", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateCall(ThrowFn, {llvm::ConstantPointerNull::get(Ptr),
                               llvm::ConstantPointerNull::get(Ptr)});
  Builder.CreateAdd(llvm::ConstantInt::get(I32, 1),
                    llvm::ConstantInt::get(I32, 2), "junk");
  Builder.CreateRetVoid();

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("throw "), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, CxxThrowCallPrintsThrowWithoutDebugTrap) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-throw", Context);
  llvm::Type *Ptr = llvm::PointerType::getUnqual(Context);
  llvm::FunctionType *ThrowTy = llvm::FunctionType::get(
      llvm::Type::getVoidTy(Context), {Ptr, Ptr}, false);
  llvm::Function *ThrowFn =
      llvm::Function::Create(ThrowTy, llvm::GlobalValue::ExternalLinkage,
                             "_CxxThrowException", Module);
  llvm::FunctionType *FnTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "throws", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateCall(ThrowFn, {llvm::ConstantPointerNull::get(Ptr),
                               llvm::ConstantPointerNull::get(Ptr)});
  llvm::Function *Trap = llvm::Intrinsic::getOrInsertDeclaration(
      &Module, llvm::Intrinsic::debugtrap);
  Builder.CreateCall(Trap);
  Builder.CreateUnreachable();

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("throw "), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__debugbreak"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, EmptyIfCallReturnDoesNotLeaveUninitOrUnknownArgs) {
  HighFunc Func;
  Func.Name = "gs_like";
  Func.Entry = 0x140001000;
  Func.ReturnType = NdType::makeInt(8);
  Func.ExceptionMetadata = ExceptionFunction{};
  for (int I = 0; I < 8; ++I)
    Func.Params.push_back(
        {"arg" + std::to_string(I), NdType::makePtr(NdType::makeVoid())});

  MedVar One;
  One.Kind = MedVar::Temp;
  One.Id = 1;
  One.Size = 8;
  HighStmt SetOne;
  SetOne.Kind = StmtKind::Assign;
  SetOne.Dst = HighExpr::makeVar(One);
  SetOne.Val = HighExpr::makeConst(1, 8);
  Func.Body.push_back(std::move(SetOne));

  MedVar Result;
  Result.Kind = MedVar::Temp;
  Result.Id = 4;
  Result.Size = 8;
  MedVar Join;
  Join.Kind = MedVar::Temp;
  Join.Id = 3;
  Join.Size = 8;

  HighStmt IfElse;
  IfElse.Kind = StmtKind::IfElse;
  IfElse.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, parameter(0),
                                    HighExpr::makeConst(0, 8));

  MedVar Fwd;
  Fwd.Kind = MedVar::Temp;
  Fwd.Id = 51;
  Fwd.Size = 8;
  HighStmt Copy;
  Copy.Kind = StmtKind::Assign;
  Copy.Dst = HighExpr::makeVar(Fwd);
  Copy.Val = parameter(0);
  IfElse.Body.push_back(std::move(Copy));

  MedVar GsTmp;
  GsTmp.Kind = MedVar::Temp;
  GsTmp.Id = 52;
  GsTmp.Size = 8;
  HighStmt GsLoad;
  GsLoad.Kind = StmtKind::Assign;
  GsLoad.Dst = HighExpr::makeVar(GsTmp);
  GsLoad.Val =
      HighExpr::makeLoad(parameter(0), NdType::makeInt(8),
                         NdMemoryOrdering::None, NdMemoryAddressSpace::X86GS);
  IfElse.Body.push_back(std::move(GsLoad));

  HighStmt ThenJoin;
  ThenJoin.Kind = StmtKind::Assign;
  ThenJoin.Dst = HighExpr::makeVar(Join);
  ThenJoin.Val = HighExpr::makeVar(One);
  IfElse.Body.push_back(std::move(ThenJoin));

  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeVar(Result);
  Assign.Val = HighExpr::makeCall(
      "CxxFrameHandler3", 0x140002000,
      {parameter(0), parameter(1), parameter(2), parameter(3)});
  IfElse.ElseBody.push_back(std::move(Assign));
  HighStmt ElseJoin;
  ElseJoin.Kind = StmtKind::Assign;
  ElseJoin.Dst = HighExpr::makeVar(Join);
  ElseJoin.Val = HighExpr::makeVar(Result);
  IfElse.ElseBody.push_back(std::move(ElseJoin));
  Func.Body.push_back(std::move(IfElse));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeVar(Join);
  Func.Body.push_back(std::move(Ret));

  const std::string Source = emitFunctions({Func});
  EXPECT_EQ(Source.find("0 /* unknown */"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("caller-saved register clobbered"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("CxxFrameHandler3("), std::string::npos) << Source;
  EXPECT_NE(Source.find("arg0"), std::string::npos) << Source;
  EXPECT_NE(Source.find("arg1"), std::string::npos) << Source;
  EXPECT_NE(Source.find("arg2"), std::string::npos) << Source;
  EXPECT_NE(Source.find("arg3"), std::string::npos) << Source;
  EXPECT_NE(Source.find("return t1"), std::string::npos) << Source;
  EXPECT_EQ(Source.find(") {\n    }"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return t3"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return t4"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, Win64CallReloadsParamsFromCalleeSaves) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;

  auto Reg = [](int Id, int SSA, uint64_t Off) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = Arch::X64;
    V.Id = Id;
    V.SSAVer = SSA;
    V.Size = 8;
    V.RegOff = Off;
    return V;
  };
  auto Copy = [](MedVar Dst, MedVar Src, va_t Addr) {
    MedOp Op;
    Op.Opcode = NdOp::COPY;
    Op.Output = Dst;
    Op.addInput(Src);
    Op.Addr = Addr;
    return Op;
  };

  const MedVar RCX0 = Reg(10, 0, x86reg::RCX);
  const MedVar RDX0 = Reg(11, 0, x86reg::RDX);
  const MedVar R80 = Reg(12, 0, x86reg::R8);
  const MedVar R90 = Reg(13, 0, x86reg::R9);
  const MedVar RBP1 = Reg(20, 1, x86reg::RBP);
  const MedVar RSI1 = Reg(21, 1, x86reg::RSI);
  const MedVar R141 = Reg(22, 1, x86reg::R14);
  const MedVar RDI1 = Reg(23, 1, x86reg::RDI);
  const MedVar R81 = Reg(12, 1, x86reg::R8);
  const MedVar RCX2 = Reg(10, 2, x86reg::RCX);
  const MedVar RDX2 = Reg(11, 2, x86reg::RDX);
  const MedVar R82 = Reg(12, 2, x86reg::R8);
  const MedVar R92 = Reg(13, 2, x86reg::R9);

  MedFunc Med;
  Med.Entry = 0x140001000;
  Med.Name = "gs_handler";
  Med.CC = CallingConv::Win64;
  const uint64_t ParamOffs[] = {x86reg::RCX, x86reg::RDX, x86reg::R8,
                                x86reg::R9};
  for (int I = 0; I < 4; ++I) {
    MedVar P;
    P.Kind = MedVar::Param;
    P.TheArch = Arch::X64;
    P.Id = 6 + I;
    P.Size = 8;
    P.RegOff = ParamOffs[I];
    Med.Params.push_back(P);
  }

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x140001000;
  Block.EndAddr = 0x140001080;
  Block.Ops.push_back(Copy(RBP1, RCX0, 0x140001000));
  Block.Ops.push_back(Copy(RSI1, RDX0, 0x140001004));
  Block.Ops.push_back(Copy(R141, R80, 0x140001008));
  Block.Ops.push_back(Copy(RDI1, R90, 0x14000100c));

  MedOp Flags;
  Flags.Opcode = NdOp::COPY;
  Flags.Output = R81;
  Flags.addInput(MedVar::makeConst(0x66, 4));
  Flags.Addr = 0x140001010;
  Block.Ops.push_back(std::move(Flags));

  Block.Ops.push_back(Copy(R92, RDI1, 0x140001020));
  Block.Ops.push_back(Copy(R82, R141, 0x140001024));
  Block.Ops.push_back(Copy(RDX2, RSI1, 0x140001028));
  Block.Ops.push_back(Copy(RCX2, RBP1, 0x14000102c));

  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->CallKind = SourceCallTypeHint::Kind::Native;
  Hint->TargetName = "CxxFrameHandler3";
  Hint->TargetAddress = 0x140002000;
  Hint->Signature.ReturnType = NdType::makeInt(4);
  Hint->Signature.Architecture = Arch::X64;
  for (int I = 0; I < 4; ++I) {
    SourceParameterTypeHint P;
    P.Name = "arg" + std::to_string(I);
    P.Type = NdType::makePtr(NdType::makeVoid());
    Hint->Signature.Parameters.push_back(P);
  }

  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x140001030;
  Call.SourceCallHint = Hint;
  Call.Output = Reg(0, 1, x86reg::RAX);
  Call.Output.Size = 4;
  Call.addInput(MedVar::makeConst(0x140002000, 8));
  Call.addInput(Reg(10, 1, x86reg::RCX));
  Call.addInput(Reg(11, 1, x86reg::RDX));
  Call.addInput(R81);
  Call.addInput(Reg(13, 1, x86reg::R9));
  Block.Ops.push_back(std::move(Call));

  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = 0x140001040;
  Ret.addInput(Reg(0, 1, x86reg::RAX));
  Block.Ops.push_back(std::move(Ret));
  Med.Blocks.push_back(std::move(Block));

  std::map<va_t, std::string> Names;
  Names[0x140002000] = "CxxFrameHandler3";
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Img);
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);

  const std::string Source = emitFunctions({High}, Arch::X64, &Img);
  EXPECT_NE(Source.find("(void*)(uintptr_t)(arg0), (void*)(uintptr_t)(arg1), "
                        "(void*)(uintptr_t)(arg2), (void*)(uintptr_t)(arg3)"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("0 /* unknown */"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("0x66"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, Win64ParamSsaIdMapsToAbiSlot) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;

  MedFunc Med;
  Med.Entry = 0x140001000;
  Med.Name = "ssa_param";
  Med.CC = CallingConv::Win64;
  const uint64_t ParamOffs[] = {x86reg::RCX, x86reg::RDX, x86reg::R8,
                                x86reg::R9};
  for (int I = 0; I < 4; ++I) {
    MedVar P;
    P.Kind = MedVar::Param;
    P.TheArch = Arch::X64;
    P.Id = 6 + I;
    P.Size = 8;
    P.RegOff = ParamOffs[I];
    Med.Params.push_back(P);
  }
  for (int I = 4; I < 8; ++I) {
    MedVar P;
    P.Kind = MedVar::Param;
    P.TheArch = Arch::X64;
    P.Id = I;
    P.Size = 8;
    P.RegOff = kNoParamReg;
    Med.Params.push_back(P);
  }

  MedVar Addr;
  Addr.Kind = MedVar::Param;
  Addr.TheArch = Arch::X64;
  Addr.Id = 6;
  Addr.Size = 8;
  Addr.RegOff = x86reg::RCX;

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x140001000;
  Block.EndAddr = 0x140001010;
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->CallKind = SourceCallTypeHint::Kind::Native;
  Hint->TargetName = "use_param";
  Hint->TargetAddress = 0x140002000;
  SourceParameterTypeHint HP;
  HP.Name = "p";
  HP.Type = NdType::makePtr(NdType::makeVoid());
  Hint->Signature.Parameters.push_back(HP);
  Hint->Signature.ReturnType = NdType::makeVoid();
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x140001000;
  Call.SourceCallHint = Hint;
  Call.addInput(MedVar::makeConst(0x140002000, 8));
  Call.addInput(Addr);
  Block.Ops.push_back(std::move(Call));
  MedOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = 0x140001008;
  Block.Ops.push_back(std::move(Ret));
  Med.Blocks.push_back(std::move(Block));

  std::map<va_t, std::string> Names;
  Names[0x140002000] = "use_param";
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Img);
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);
  std::string IR;
  for (const HighStmt &S : High.Body)
    IR += S.str() + "\n";
  EXPECT_NE(IR.find("use_param(arg0)"), std::string::npos) << IR;
  EXPECT_EQ(IR.find("arg6"), std::string::npos) << IR;
}

TEST(HighCPointerAddresses, ThrowOfFrameOffsetUsesNamedSlot) {
  HighFunc Func;
  Func.Name = "throws_local";
  Func.Entry = 0x140001000;
  Func.FrameSize = 0x60;
  Func.ReturnType = NdType::makeVoid();
  MedVar Sp;
  Sp.Kind = MedVar::Temp;
  Sp.Id = 0;
  Sp.Size = 8;
  HighStmt Throw;
  Throw.Kind = StmtKind::Call;
  Throw.CallExpr = HighExpr::makeCall(
      "_CxxThrowException", 0x140002000,
      {HighExpr::makeBinop(NdOp::INT_ADD, HighExpr::makeVar(Sp),
                           HighExpr::makeConst(64, 8)),
       HighExpr::makeConst(0, 8)});
  Func.Body.push_back(std::move(Throw));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("throw "), std::string::npos) << Source;
  EXPECT_NE(Source.find("&var_40"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, OmitsReturnAndEmptyBlockAfterCxxThrow) {
  HighFunc Func;
  Func.Name = "throws";
  Func.Entry = 0x140001000;
  Func.ReturnType = NdType::makeInt(8);
  Func.Params = {{"arg0", NdType::makePtr(NdType::makeVoid())}};

  HighStmt Throw;
  Throw.Kind = StmtKind::Call;
  Throw.CallExpr =
      HighExpr::makeCall("_CxxThrowException", 0x140002000,
                         {parameter(0), HighExpr::makeConst(0, 8)});
  Func.Body.push_back(std::move(Throw));
  HighStmt Empty;
  Empty.Kind = StmtKind::Block;
  Func.Body.push_back(std::move(Empty));
  MedVar Dead;
  Dead.Kind = MedVar::Temp;
  Dead.Id = 5;
  Dead.Size = 8;
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeVar(Dead);
  Func.Body.push_back(std::move(Ret));

  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("throw "), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return t5"), std::string::npos) << Source;
  EXPECT_EQ(Source.find(") {\n    }"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, Win64ReportGsFailureKeepsCookieArgument) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;

  MedFunc Med;
  Med.Entry = 0x140001000;
  Med.Name = "security_check_cookie";
  Med.CC = CallingConv::Win64;
  MedVar RCX;
  RCX.Kind = MedVar::Reg;
  RCX.TheArch = Arch::X64;
  RCX.Id = 1;
  RCX.SSAVer = 0;
  RCX.Size = 8;
  RCX.RegOff = x86reg::RCX;
  MedVar Param;
  Param.Kind = MedVar::Param;
  Param.TheArch = Arch::X64;
  Param.Id = 1;
  Param.Size = 8;
  Param.RegOff = x86reg::RCX;
  Med.Params.push_back(Param);

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x140001000;
  Block.EndAddr = 0x140001010;
  MedOp LiveIn;
  LiveIn.Opcode = NdOp::COPY;
  LiveIn.Output = RCX;
  LiveIn.addInput(RCX);
  LiveIn.Addr = 0x140001000;
  Block.Ops.push_back(std::move(LiveIn));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x140001008;
  Call.addInput(MedVar::makeConst(0x140002000, 8));
  Block.Ops.push_back(std::move(Call));
  Med.Blocks.push_back(std::move(Block));

  std::map<va_t, std::string> Names;
  Names[0x140002000] = "report_gsfailure";
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Img);
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);
  const std::string Source = emitFunctions({High}, Arch::X64, &Img);
  EXPECT_NE(Source.find("report_gsfailure(arg0)"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, Win64CookiePhiPrefersIncomingParam) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;

  auto Reg = [](int Id, int SSA, uint64_t Off) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = Arch::X64;
    V.Id = Id;
    V.SSAVer = SSA;
    V.Size = 8;
    V.RegOff = Off;
    return V;
  };

  MedFunc Med;
  Med.Entry = 0x140001000;
  Med.Name = "security_check_cookie";
  Med.CC = CallingConv::Win64;
  MedVar Param;
  Param.Kind = MedVar::Param;
  Param.TheArch = Arch::X64;
  Param.Id = 0;
  Param.Size = 8;
  Param.RegOff = x86reg::RCX;
  Med.Params.push_back(Param);

  const MedVar RCX0 = Reg(1, 0, x86reg::RCX);
  const MedVar RCX1 = Reg(1, 1, x86reg::RCX);
  const MedVar RCX2 = Reg(1, 2, x86reg::RCX);
  const MedVar RCX3 = Reg(1, 3, x86reg::RCX);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.Succs = {1, 2};
  MedOp LiveIn;
  LiveIn.Opcode = NdOp::COPY;
  LiveIn.Output = RCX0;
  LiveIn.addInput(RCX0);
  LiveIn.Addr = 0x140001000;
  Entry.Ops.push_back(std::move(LiveIn));
  MedOp Br;
  Br.Opcode = NdOp::COND_BR;
  Br.Addr = 0x140001008;
  Br.addInput(MedVar::makeConst(0x140001020, 8));
  Br.addInput(MedVar::makeConst(1, 1));
  Entry.Ops.push_back(std::move(Br));
  Med.Blocks.push_back(std::move(Entry));

  MedBlock Restore;
  Restore.Id = 1;
  Restore.Preds = {0};
  Restore.Succs = {2};
  MedVar Imm16;
  Imm16.Kind = MedVar::Temp;
  Imm16.TheArch = Arch::X64;
  Imm16.Id = 40;
  Imm16.Size = 8;
  MedOp LoadImm;
  LoadImm.Opcode = NdOp::COPY;
  LoadImm.Output = Imm16;
  LoadImm.addInput(MedVar::makeConst(16, 8));
  LoadImm.Addr = 0x14000100c;
  Restore.Ops.push_back(std::move(LoadImm));
  MedOp Rol;
  Rol.Opcode = NdOp::INT_LEFT;
  Rol.Output = RCX1;
  Rol.addInput(RCX0);
  Rol.addInput(Imm16);
  Rol.Addr = 0x140001010;
  Restore.Ops.push_back(std::move(Rol));
  MedOp Ror;
  Ror.Opcode = NdOp::INT_RIGHT;
  Ror.Output = RCX2;
  Ror.addInput(RCX1);
  Ror.addInput(Imm16);
  Ror.Addr = 0x140001014;
  Restore.Ops.push_back(std::move(Ror));
  Med.Blocks.push_back(std::move(Restore));

  MedBlock Fail;
  Fail.Id = 2;
  Fail.Preds = {0, 1};
  PhiNode Phi;
  Phi.Output = RCX3;
  Phi.Args.push_back({0, RCX0});
  Phi.Args.push_back({1, RCX2});
  Fail.Phis.push_back(std::move(Phi));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x140001020;
  Call.addInput(MedVar::makeConst(0x140002000, 8));
  Fail.Ops.push_back(std::move(Call));
  Med.Blocks.push_back(std::move(Fail));

  std::map<va_t, std::string> Names;
  Names[0x140002000] = "report_gsfailure";
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Img);
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);
  const std::string Source = emitFunctions({High}, Arch::X64, &Img);
  EXPECT_NE(Source.find("report_gsfailure(arg0)"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, CallNameUsesImageFunctionSymbol) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Symbol GsFail;
  GsFail.Name = "__report_gsfailure";
  GsFail.Addr = 0x140002000;
  GsFail.IsFunc = true;
  Img.Symbols.push_back(GsFail);

  MedFunc Med;
  Med.Entry = 0x140001000;
  Med.Name = "security_check_cookie";
  Med.CC = CallingConv::Win64;
  MedVar RCX;
  RCX.Kind = MedVar::Reg;
  RCX.TheArch = Arch::X64;
  RCX.Id = 1;
  RCX.SSAVer = 0;
  RCX.Size = 8;
  RCX.RegOff = x86reg::RCX;
  MedVar Param;
  Param.Kind = MedVar::Param;
  Param.TheArch = Arch::X64;
  Param.Id = 0;
  Param.Size = 8;
  Param.RegOff = x86reg::RCX;
  Med.Params.push_back(Param);

  MedBlock Block;
  Block.Id = 0;
  MedOp LiveIn;
  LiveIn.Opcode = NdOp::COPY;
  LiveIn.Output = RCX;
  LiveIn.addInput(RCX);
  LiveIn.Addr = 0x140001000;
  Block.Ops.push_back(std::move(LiveIn));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x140001008;
  Call.addInput(MedVar::makeConst(0x140002000, 8));
  Block.Ops.push_back(std::move(Call));
  Med.Blocks.push_back(std::move(Block));

  MedToHighConverter Converter;
  Converter.setBinaryImage(&Img);
  HighFunc High = Converter.convert(Med, Arch::X64);
  const std::string Source = emitFunctions({High}, Arch::X64, &Img);
  EXPECT_NE(Source.find("report_gsfailure(arg0)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("sub_140002000"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, Win64ThreeArgCallIgnoresLiveInR9) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;

  auto Reg = [](int Id, int SSA, uint64_t Off) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = Arch::X64;
    V.Id = Id;
    V.SSAVer = SSA;
    V.Size = 8;
    V.RegOff = Off;
    return V;
  };
  auto Copy = [](MedVar Dst, MedVar Src, va_t Addr) {
    MedOp Op;
    Op.Opcode = NdOp::COPY;
    Op.Output = Dst;
    Op.addInput(Src);
    Op.Addr = Addr;
    return Op;
  };

  MedFunc Med;
  Med.Entry = 0x140001000;
  Med.Name = "gs_common_site";
  Med.CC = CallingConv::Win64;
  const uint64_t ParamOffs[] = {x86reg::RCX, x86reg::RDX, x86reg::R8,
                                x86reg::R9};
  for (int I = 0; I < 4; ++I) {
    MedVar P;
    P.Kind = MedVar::Param;
    P.TheArch = Arch::X64;
    P.Id = 6 + I;
    P.Size = 8;
    P.RegOff = ParamOffs[I];
    Med.Params.push_back(P);
  }

  const MedVar RCX0 = Reg(10, 0, x86reg::RCX);
  const MedVar RDX0 = Reg(11, 0, x86reg::RDX);
  const MedVar R80 = Reg(12, 0, x86reg::R8);
  const MedVar R90 = Reg(13, 0, x86reg::R9);
  const MedVar RSI1 = Reg(21, 1, x86reg::RSI);
  const MedVar RCX1 = Reg(10, 1, x86reg::RCX);
  const MedVar RDX1 = Reg(11, 1, x86reg::RDX);
  const MedVar R81 = Reg(12, 1, x86reg::R8);

  MedBlock Block;
  Block.Id = 0;
  Block.Ops.push_back(Copy(RCX0, RCX0, 0x140001000));
  Block.Ops.push_back(Copy(RDX0, RDX0, 0x140001001));
  Block.Ops.push_back(Copy(R80, R80, 0x140001002));
  Block.Ops.push_back(Copy(R90, R90, 0x140001003));
  Block.Ops.push_back(Copy(RSI1, RDX0, 0x140001004));
  Block.Ops.push_back(Copy(RCX1, RSI1, 0x140001008));
  Block.Ops.push_back(Copy(RDX1, R90, 0x14000100c));
  MedOp Lea;
  Lea.Opcode = NdOp::INT_ADD;
  Lea.Output = R81;
  Lea.addInput(R90);
  Lea.addInput(MedVar::makeConst(4, 8));
  Lea.Addr = 0x140001010;
  Block.Ops.push_back(std::move(Lea));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x140001018;
  Call.addInput(MedVar::makeConst(0x140002100, 8));
  Block.Ops.push_back(std::move(Call));
  Med.Blocks.push_back(std::move(Block));

  std::map<va_t, std::string> Names;
  Names[0x140002100] = "GSHandlerCheckCommon";
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Img);
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);
  const std::string Source = emitFunctions({High}, Arch::X64, &Img);
  const auto CallAt = Source.rfind("GSHandlerCheckCommon(");
  ASSERT_NE(CallAt, std::string::npos) << Source;
  const auto Open = Source.find('(', CallAt);
  const auto Close = Source.find(')', Open);
  ASSERT_NE(Open, std::string::npos) << Source;
  ASSERT_NE(Close, std::string::npos) << Source;
  const std::string Args = Source.substr(Open + 1, Close - Open - 1);
  EXPECT_EQ(std::count(Args.begin(), Args.end(), ','), 2) << Source;
  EXPECT_NE(Args.find("arg1"), std::string::npos) << Source;
  EXPECT_NE(Args.find("arg3"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, Win64GsHandlerRestoresParamsAcrossCall) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;

  auto Reg = [](int Id, int SSA, uint64_t Off) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = Arch::X64;
    V.Id = Id;
    V.SSAVer = SSA;
    V.Size = 8;
    V.RegOff = Off;
    return V;
  };
  auto Copy = [](MedVar Dst, MedVar Src, va_t Addr) {
    MedOp Op;
    Op.Opcode = NdOp::COPY;
    Op.Output = Dst;
    Op.addInput(Src);
    Op.Addr = Addr;
    return Op;
  };

  MedFunc Med;
  Med.Entry = 0x140001000;
  Med.Name = "GSHandlerCheck_EH";
  Med.CC = CallingConv::Win64;
  const uint64_t ParamOffs[] = {x86reg::RCX, x86reg::RDX, x86reg::R8,
                                x86reg::R9};
  for (int I = 0; I < 4; ++I) {
    MedVar P;
    P.Kind = MedVar::Param;
    P.TheArch = Arch::X64;
    P.Id = 6 + I;
    P.Size = 8;
    P.RegOff = ParamOffs[I];
    Med.Params.push_back(P);
  }

  const MedVar RCX0 = Reg(10, 0, x86reg::RCX);
  const MedVar RDX0 = Reg(11, 0, x86reg::RDX);
  const MedVar R80 = Reg(12, 0, x86reg::R8);
  const MedVar R90 = Reg(13, 0, x86reg::R9);
  const MedVar RBP1 = Reg(20, 1, x86reg::RBP);
  const MedVar RSI1 = Reg(21, 1, x86reg::RSI);
  const MedVar R141 = Reg(22, 1, x86reg::R14);
  const MedVar RDI1 = Reg(23, 1, x86reg::RDI);
  const MedVar RDI2 = Reg(23, 2, x86reg::RDI);
  const MedVar RCX1 = Reg(10, 1, x86reg::RCX);
  const MedVar RDX1 = Reg(11, 1, x86reg::RDX);
  const MedVar R81 = Reg(12, 1, x86reg::R8);
  const MedVar RCX3 = Reg(10, 3, x86reg::RCX);
  const MedVar RDX3 = Reg(11, 3, x86reg::RDX);
  const MedVar R89 = Reg(12, 9, x86reg::R8);
  const MedVar R92 = Reg(13, 2, x86reg::R9);

  MedBlock Setup;
  Setup.Id = 0;
  Setup.StartAddr = 0x140001000;
  Setup.EndAddr = 0x140001030;
  Setup.Succs = {1};
  Setup.Ops.push_back(Copy(RCX0, RCX0, 0x140001000));
  Setup.Ops.push_back(Copy(RDX0, RDX0, 0x140001001));
  Setup.Ops.push_back(Copy(R80, R80, 0x140001002));
  Setup.Ops.push_back(Copy(R90, R90, 0x140001003));
  Setup.Ops.push_back(Copy(RSI1, RDX0, 0x140001004));
  Setup.Ops.push_back(Copy(R141, R80, 0x140001008));
  Setup.Ops.push_back(Copy(RBP1, RCX0, 0x14000100c));
  Setup.Ops.push_back(Copy(RDI1, R90, 0x140001010));
  Setup.Ops.push_back(Copy(RDX1, R90, 0x140001014));
  Setup.Ops.push_back(Copy(RCX1, RSI1, 0x140001018));
  MedOp Lea;
  Lea.Opcode = NdOp::INT_ADD;
  Lea.Output = R81;
  Lea.addInput(R90);
  Lea.addInput(MedVar::makeConst(4, 8));
  Lea.Addr = 0x14000101c;
  Setup.Ops.push_back(std::move(Lea));
  MedOp GsCall;
  GsCall.Opcode = NdOp::CALL;
  GsCall.Addr = 0x140001020;
  GsCall.addInput(MedVar::makeConst(0x140002100, 8));
  Setup.Ops.push_back(std::move(GsCall));
  Med.Blocks.push_back(std::move(Setup));

  MedBlock Cxx;
  Cxx.Id = 1;
  Cxx.StartAddr = 0x140001030;
  Cxx.EndAddr = 0x140001050;
  Cxx.Preds = {0};
  // Live MSVC MedIR sometimes names this `rdi.2` without a new def.
  Cxx.Ops.push_back(Copy(R92, RDI2, 0x140001030));
  Cxx.Ops.push_back(Copy(R89, R80, 0x140001034));
  Cxx.Ops.push_back(Copy(RDX3, RSI1, 0x140001038));
  Cxx.Ops.push_back(Copy(RCX3, RCX0, 0x14000103c));
  MedOp CxxCall;
  CxxCall.Opcode = NdOp::CALL;
  CxxCall.Addr = 0x140001040;
  CxxCall.addInput(MedVar::makeConst(0x140002200, 8));
  Cxx.Ops.push_back(std::move(CxxCall));
  Med.Blocks.push_back(std::move(Cxx));

  std::map<va_t, std::string> Names;
  Names[0x140002100] = "GSHandlerCheckCommon";
  Names[0x140002200] = "CxxFrameHandler3";
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Img);
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);
  const std::string Source = emitFunctions({High}, Arch::X64, &Img);

  const auto GsAt = Source.rfind("GSHandlerCheckCommon(");
  ASSERT_NE(GsAt, std::string::npos) << Source;
  const auto GsOpen = Source.find('(', GsAt);
  const auto GsClose = Source.find(')', GsOpen);
  ASSERT_NE(GsClose, std::string::npos) << Source;
  const std::string GsArgs = Source.substr(GsOpen + 1, GsClose - GsOpen - 1);
  EXPECT_EQ(std::count(GsArgs.begin(), GsArgs.end(), ','), 2) << Source;

  const auto CxxAt = Source.rfind("CxxFrameHandler3(");
  ASSERT_NE(CxxAt, std::string::npos) << Source;
  EXPECT_NE(Source.find("arg0"), std::string::npos) << Source;
  const auto CxxOpen = Source.find('(', CxxAt);
  const auto CxxClose = Source.find(')', CxxOpen);
  ASSERT_NE(CxxClose, std::string::npos) << Source;
  const std::string CxxArgs =
      Source.substr(CxxOpen + 1, CxxClose - CxxOpen - 1);
  EXPECT_EQ(std::count(CxxArgs.begin(), CxxArgs.end(), ','), 3) << Source;
  const auto A0 = CxxArgs.find("arg0");
  const auto A1 = CxxArgs.find("arg1");
  const auto A2 = CxxArgs.find("arg2");
  const auto A3 = CxxArgs.find("arg3");
  EXPECT_NE(A0, std::string::npos) << Source;
  EXPECT_NE(A1, std::string::npos) << Source;
  EXPECT_NE(A2, std::string::npos) << Source;
  EXPECT_NE(A3, std::string::npos) << Source;
  EXPECT_LT(A0, A1);
  EXPECT_LT(A1, A2);
  EXPECT_LT(A2, A3);
  EXPECT_EQ(Source.find("0 /* unknown */"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, SecurityCheckCookieKeepsSingleArgument) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;

  auto Reg = [](int Id, int SSA, uint64_t Off) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = Arch::X64;
    V.Id = Id;
    V.SSAVer = SSA;
    V.Size = 8;
    V.RegOff = Off;
    return V;
  };
  auto Copy = [](MedVar Dst, MedVar Src, va_t Addr) {
    MedOp Op;
    Op.Opcode = NdOp::COPY;
    Op.Output = Dst;
    Op.addInput(Src);
    Op.Addr = Addr;
    return Op;
  };

  MedFunc Med;
  Med.Entry = 0x140001000;
  Med.Name = "gs_common";
  Med.CC = CallingConv::Win64;
  const uint64_t ParamOffs[] = {x86reg::RCX, x86reg::RDX, x86reg::R8,
                                x86reg::R9};
  for (int I = 0; I < 4; ++I) {
    MedVar P;
    P.Kind = MedVar::Param;
    P.TheArch = Arch::X64;
    P.Id = I;
    P.Size = 8;
    P.RegOff = ParamOffs[I];
    Med.Params.push_back(P);
  }

  const MedVar RCX0 = Reg(10, 0, x86reg::RCX);
  const MedVar RDX0 = Reg(11, 0, x86reg::RDX);
  const MedVar R80 = Reg(12, 0, x86reg::R8);
  const MedVar R90 = Reg(13, 0, x86reg::R9);
  const MedVar RCX1 = Reg(10, 1, x86reg::RCX);
  const MedVar RDX1 = Reg(11, 1, x86reg::RDX);
  const MedVar R81 = Reg(12, 1, x86reg::R8);
  const MedVar R91 = Reg(13, 1, x86reg::R9);

  MedBlock Block;
  Block.Id = 0;
  Block.Ops.push_back(Copy(RCX0, RCX0, 0x140001000));
  Block.Ops.push_back(Copy(RDX0, RDX0, 0x140001001));
  Block.Ops.push_back(Copy(R80, R80, 0x140001002));
  Block.Ops.push_back(Copy(R90, R90, 0x140001003));
  Block.Ops.push_back(Copy(RCX1, RCX0, 0x140001008));
  Block.Ops.push_back(Copy(RDX1, RDX0, 0x14000100c));
  Block.Ops.push_back(Copy(R81, R80, 0x140001010));
  Block.Ops.push_back(Copy(R91, R90, 0x140001014));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x140001018;
  Call.addInput(MedVar::makeConst(0x140002000, 8));
  Block.Ops.push_back(std::move(Call));
  Med.Blocks.push_back(std::move(Block));

  std::map<va_t, std::string> Names;
  Names[0x140002000] = "__security_check_cookie";
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Img);
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);
  const std::string Source = emitFunctions({High}, Arch::X64, &Img);
  const auto At = Source.rfind("_security_check_cookie(");
  ASSERT_NE(At, std::string::npos) << Source;
  const auto Open = Source.find('(', At);
  const auto Close = Source.find(')', Open);
  ASSERT_NE(Close, std::string::npos) << Source;
  const std::string Args = Source.substr(Open + 1, Close - Open - 1);
  EXPECT_EQ(std::count(Args.begin(), Args.end(), ','), 0) << Source;
  EXPECT_FALSE(Args.empty()) << Source;
}

TEST(HighCPointerAddresses, FrameAddressValueDeclaresSlot) {
  HighFunc Func;
  Func.Name = "addr_value";
  Func.FrameSize = 16;
  Func.ReturnType = NdType::makeInt(8);
  Func.Params = {{"arg0", NdType::makeInt(8)}};
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Size = 8;
  SP.TheArch = Arch::X64;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  auto SlotAddr = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP, NdType::makeInt(8, false)),
      HighExpr::makeConst(8, 8));
  HighStmt Home;
  Home.Kind = StmtKind::Store;
  Home.StoreAddr = SlotAddr;
  Home.StoreVal = parameter(0, NdType::makeInt(8));
  Func.Body.push_back(std::move(Home));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = SlotAddr;
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("var_m8"), std::string::npos) << Source;
  EXPECT_NE(Source.find("&var_m8"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, OverwrittenCopyForwardDestIsDeclared) {
  HighFunc Func;
  Func.Name = "join_overwrite";
  Func.ReturnType = NdType::makeInt(4);
  Func.Params = {{"arg0", NdType::makeInt(4)}};
  MedVar Join;
  Join.Kind = MedVar::Temp;
  Join.Id = 0;
  Join.Size = 4;
  HighStmt Fwd;
  Fwd.Kind = StmtKind::Assign;
  Fwd.Dst = HighExpr::makeVar(Join, NdType::makeInt(4));
  Fwd.Val = parameter(0, NdType::makeInt(4));
  Func.Body.push_back(std::move(Fwd));
  HighStmt IfElse;
  IfElse.Kind = StmtKind::IfElse;
  IfElse.Cond =
      HighExpr::makeBinop(NdOp::INT_EQUAL, parameter(0, NdType::makeInt(4)),
                          HighExpr::makeConst(0, 4));
  HighStmt Then;
  Then.Kind = StmtKind::Assign;
  Then.Dst = HighExpr::makeVar(Join, NdType::makeInt(4));
  Then.Val = HighExpr::makeConst(1, 4);
  IfElse.Body.push_back(std::move(Then));
  HighStmt Else;
  Else.Kind = StmtKind::Assign;
  Else.Dst = HighExpr::makeVar(Join, NdType::makeInt(4));
  Else.Val = parameter(0, NdType::makeInt(4));
  IfElse.ElseBody.push_back(std::move(Else));
  Func.Body.push_back(std::move(IfElse));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeVar(Join, NdType::makeInt(4));
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("t0;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("t0 = 1"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, UnusedFrameAliasIsNotDeclared) {
  HighFunc Func;
  Func.Name = "alias_slot";
  Func.FrameSize = 64;
  Func.ReturnType = NdType::makeInt(8);
  Func.Params = {{"arg0", NdType::makeInt(8)}};
  MedVar SP;
  SP.Kind = MedVar::Reg;
  SP.Size = 8;
  SP.TheArch = Arch::X64;
  SP.RegOff = getTargetRegInfo(Arch::X64).StackPointer;
  auto AliasAddr = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP, NdType::makeInt(8, false)),
      HighExpr::makeConst(0x38, 8));
  MedVar Alias;
  Alias.Kind = MedVar::Temp;
  Alias.Id = 7;
  Alias.Size = 8;
  Alias.TheArch = Arch::X64;
  HighStmt Home;
  Home.Kind = StmtKind::Assign;
  Home.Dst = HighExpr::makeVar(Alias, NdType::makeInt(8));
  Home.Val = AliasAddr;
  Func.Body.push_back(std::move(Home));
  auto LiveAddr = HighExpr::makeBinop(
      NdOp::INT_SUB, HighExpr::makeVar(SP, NdType::makeInt(8, false)),
      HighExpr::makeConst(8, 8));
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = LiveAddr;
  Store.StoreVal = HighExpr::makeVar(Alias, NdType::makeInt(8));
  Func.Body.push_back(std::move(Store));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = LiveAddr;
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("&var_m8"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("var_m38"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, UnusedLoadAssignIsNotDeclared) {
  HighFunc Func;
  Func.Name = "unused_load";
  Func.ReturnType = NdType::makeInt(4);
  Func.Params = {{"arg0", NdType::makePtr(NdType::makeInt(4))}};
  MedVar Dead;
  Dead.Kind = MedVar::Temp;
  Dead.Id = 13;
  Dead.SSAVer = 1;
  Dead.Size = 1;
  Dead.TheArch = Arch::X64;
  HighStmt Load;
  Load.Kind = StmtKind::Assign;
  Load.Dst = HighExpr::makeVar(Dead, NdType::makeInt(1));
  Load.Val = HighExpr::makeLoad(parameter(0), NdType::makeInt(1));
  Func.Body.push_back(std::move(Load));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeConst(0, 4);
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_EQ(Source.find("t13"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, FastFailPrintsIntrinsicWithoutAssign) {
  HighFunc Func;
  Func.Name = "gs_fail";
  Func.ReturnType = NdType::makeVoid();
  HighStmt Fail;
  Fail.Kind = StmtKind::Assign;
  MedVar Dst;
  Dst.Kind = MedVar::Temp;
  Dst.Id = 1;
  Dst.Size = 8;
  Fail.Dst = HighExpr::makeVar(Dst);
  Fail.Val = HighExpr::makeCall(
      "int", 0, {HighExpr::makeConst(0x29, 1), HighExpr::makeConst(2, 4)});
  Fail.Val->IntrinsicId = Intrinsic::IntN;
  Func.Body.push_back(std::move(Fail));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("__fastfail(2)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("int 41"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("t1 ="), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, ConstantReturnIsNotInferredVoid) {
  HighFunc Func;
  Func.Name = "GSHandlerCheck";
  Func.ReturnType = NdType::makeInt(8);
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeConst(1, 4);
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("return 1"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("void GSHandlerCheck"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, UnnamedGsFailureCallOmitsSuccessReturn) {
  HighFunc Func;
  Func.Name = "cookie";
  Func.Entry = 0x140001350;
  Func.ReturnType = NdType::makeInt(8);
  Func.Params = {{"arg0", NdType::makeInt(8)}};

  HighStmt IfElse;
  IfElse.Kind = StmtKind::IfElse;
  IfElse.Cond = HighExpr::makeBinop(NdOp::INT_EQUAL, parameter(0),
                                    HighExpr::makeConst(1, 8));
  HighStmt Ok;
  Ok.Kind = StmtKind::Return;
  MedVar Rax;
  Rax.Kind = MedVar::Reg;
  Rax.Id = 0;
  Rax.SSAVer = 0;
  Rax.Size = 8;
  Ok.RetVal = HighExpr::makeVar(Rax);
  IfElse.Body.push_back(std::move(Ok));
  Func.Body.push_back(std::move(IfElse));

  MedVar Dst;
  Dst.Kind = MedVar::Temp;
  Dst.Id = 3;
  Dst.Size = 8;
  HighStmt Call;
  Call.Kind = StmtKind::Assign;
  Call.Dst = HighExpr::makeVar(Dst);
  Call.Val = HighExpr::makeCall("sub_14000173C", 0x14000173C, {parameter(0)});
  Func.Body.push_back(std::move(Call));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeVar(Dst);
  Func.Body.push_back(std::move(Ret));

  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("void cookie"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_14000173C("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return t3"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("t3 ="), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, GetCurrentProcessTakesNoArguments) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;

  auto Reg = [](int Id, int SSA, uint64_t Off) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = Arch::X64;
    V.Id = Id;
    V.SSAVer = SSA;
    V.Size = 8;
    V.RegOff = Off;
    return V;
  };
  auto Copy = [](MedVar Dst, MedVar Src, va_t Addr) {
    MedOp Op;
    Op.Opcode = NdOp::COPY;
    Op.Output = Dst;
    Op.addInput(Src);
    Op.Addr = Addr;
    return Op;
  };

  MedFunc Med;
  Med.Entry = 0x140001000;
  Med.Name = "raise_securityfailure";
  Med.CC = CallingConv::Win64;
  const uint64_t ParamOffs[] = {x86reg::RCX, x86reg::RDX, x86reg::R8,
                                x86reg::R9};
  for (int I = 0; I < 4; ++I) {
    MedVar P;
    P.Kind = MedVar::Param;
    P.TheArch = Arch::X64;
    P.Id = I;
    P.Size = 8;
    P.RegOff = ParamOffs[I];
    Med.Params.push_back(P);
  }

  const MedVar RCX0 = Reg(10, 0, x86reg::RCX);
  const MedVar RDX0 = Reg(11, 0, x86reg::RDX);
  const MedVar R80 = Reg(12, 0, x86reg::R8);
  const MedVar R90 = Reg(13, 0, x86reg::R9);

  MedBlock Block;
  Block.Id = 0;
  Block.Ops.push_back(Copy(RCX0, RCX0, 0x140001000));
  Block.Ops.push_back(Copy(RDX0, RDX0, 0x140001001));
  Block.Ops.push_back(Copy(R80, R80, 0x140001002));
  Block.Ops.push_back(Copy(R90, R90, 0x140001003));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x140001018;
  Call.addInput(MedVar::makeConst(0x140002000, 8));
  Block.Ops.push_back(std::move(Call));
  Med.Blocks.push_back(std::move(Block));

  std::map<va_t, std::string> Names;
  Names[0x140002000] = "GetCurrentProcess";
  MedToHighConverter Converter;
  Converter.setBinaryImage(&Img);
  Converter.setFuncNames(&Names);
  HighFunc High = Converter.convert(Med, Arch::X64);
  const std::string Source = emitFunctions({High}, Arch::X64, &Img);
  const auto At = Source.rfind("GetCurrentProcess(");
  ASSERT_NE(At, std::string::npos) << Source;
  const auto Open = Source.find('(', At);
  const auto Close = Source.find(')', Open);
  ASSERT_NE(Close, std::string::npos) << Source;
  const std::string Args = Source.substr(Open + 1, Close - Open - 1);
  EXPECT_TRUE(Args.empty()) << Source;
}

TEST(HighCPointerAddresses, TerminateProcessOmitsSuccessReturn) {
  HighFunc Func;
  Func.Name = "raise";
  Func.ReturnType = NdType::makeInt(8);
  MedVar Dst;
  Dst.Kind = MedVar::Temp;
  Dst.Id = 3;
  Dst.Size = 8;
  HighStmt Call;
  Call.Kind = StmtKind::Assign;
  Call.Dst = HighExpr::makeVar(Dst);
  Call.Val = HighExpr::makeCall(
      "TerminateProcess", 0x140002000,
      {HighExpr::makeConst(0, 8), HighExpr::makeConst(0xC0000409, 4)});
  Func.Body.push_back(std::move(Call));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeVar(Dst);
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("TerminateProcess("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return t3"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("t3 ="), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, RaiseSecurityFailureOmitsSuccessReturn) {
  HighFunc Func;
  Func.Name = "report";
  Func.ReturnType = NdType::makeInt(8);
  MedVar Dst;
  Dst.Kind = MedVar::Temp;
  Dst.Id = 3;
  Dst.Size = 8;
  HighStmt Call;
  Call.Kind = StmtKind::Assign;
  Call.Dst = HighExpr::makeVar(Dst);
  Call.Val = HighExpr::makeCall("_raise_securityfailure", 0x140002000,
                                {HighExpr::makeConst(0x140003000, 8)});
  Func.Body.push_back(std::move(Call));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeVar(Dst);
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("raise_securityfailure("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return t3"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("t3"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, GsTebLoadPrintsReadGsQword) {
  HighFunc Func;
  Func.Name = "tls_guard";
  Func.ReturnType = NdType::makeInt(8);
  Func.ExceptionMetadata = ExceptionFunction{};
  MedVar Teb;
  Teb.Kind = MedVar::Temp;
  Teb.Id = 11;
  Teb.SSAVer = 4;
  Teb.Size = 8;
  HighStmt Load;
  Load.Kind = StmtKind::Assign;
  Load.Dst = HighExpr::makeVar(Teb, NdType::makeInt(8));
  Load.Val =
      HighExpr::makeLoad(HighExpr::makeConst(0x58, 8), NdType::makeInt(8),
                         NdMemoryOrdering::None, NdMemoryAddressSpace::X86GS);
  Func.Body.push_back(std::move(Load));
  HighStmt Ret;
  Ret.Kind = StmtKind::Return;
  Ret.RetVal = HighExpr::makeLoad(HighExpr::makeVar(Teb), NdType::makeInt(8));
  Func.Body.push_back(std::move(Ret));
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("__readgsqword(88)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("t11_4"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, MsvcDecorationIsNotTheOnlyCalleeSpelling) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-msvc-name", Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  llvm::FunctionType *AllocTy = llvm::FunctionType::get(I64, false);
  llvm::Function *Alloc = llvm::Function::Create(
      AllocTy, llvm::GlobalValue::ExternalLinkage,
      "?CreateGlobalMemoryAllocator@Contoso@@YAPEAVIAllocator@1@XZ", Module);
  llvm::FunctionType *FnTy = llvm::FunctionType::get(I64, false);
  llvm::Function *Function =
      llvm::Function::Create(FnTy, llvm::GlobalValue::ExternalLinkage,
                             "?Get@MemManager@Contoso@@SAAEAV12@XZ", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRet(Builder.CreateCall(Alloc, {}, "got"));

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("Contoso_MemManager_Get"), std::string::npos) << Source;
  EXPECT_NE(Source.find("Contoso_CreateGlobalMemoryAllocator"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("_x3F_"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, FastFailPrintsIntrinsicWithoutAssign) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-fastfail", Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::FunctionType *FailTy = llvm::FunctionType::get(Void, {I32}, false);
  llvm::Function *Fail = llvm::Function::Create(
      FailTy, llvm::GlobalValue::ExternalLinkage, "__fastfail", Module);
  Fail->addFnAttr(llvm::Attribute::NoReturn);
  llvm::FunctionType *FnTy = llvm::FunctionType::get(Void, false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "gs_fail", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateCall(Fail, {llvm::ConstantInt::get(I32, 2)});
  Builder.CreateRetVoid();

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("__fastfail(2)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("void _fastfail"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("int 41"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__fastfail(41)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, RaiseSecurityFailureOmitsSuccessReturn) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-raise", Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  llvm::FunctionType *RaiseTy = llvm::FunctionType::get(I64, {I64}, false);
  llvm::Function *Raise =
      llvm::Function::Create(RaiseTy, llvm::GlobalValue::ExternalLinkage,
                             "_raise_securityfailure", Module);
  llvm::FunctionType *FnTy = llvm::FunctionType::get(I64, false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "report", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Result = Builder.CreateCall(
      Raise, {llvm::ConstantInt::get(I64, 0x140003000)}, "t3");
  Builder.CreateRet(Result);

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("raise_securityfailure("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return t3"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("t3 ="), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, TerminateProcessOmitsSuccessReturn) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-terminate", Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *TermTy = llvm::FunctionType::get(I64, {I64, I32}, false);
  llvm::Function *TermFn = llvm::Function::Create(
      TermTy, llvm::GlobalValue::ExternalLinkage, "TerminateProcess", Module);
  llvm::FunctionType *FnTy = llvm::FunctionType::get(I64, false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "raise", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Result = Builder.CreateCall(
      TermFn,
      {llvm::ConstantInt::get(I64, 0), llvm::ConstantInt::get(I32, 0xC0000409)},
      "t3");
  Builder.CreateRet(Result);

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("TerminateProcess("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return t3"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("t3 ="), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, GsTebLoadPrintsReadGsQword) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-gs", Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  llvm::PointerType *GSPtr = llvm::PointerType::get(Context, 256);
  llvm::FunctionType *FnTy = llvm::FunctionType::get(I64, false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "tls_guard", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Addr = Builder.CreateIntToPtr(llvm::ConstantInt::get(I64, 0x58),
                                             GSPtr, "gsaddr");
  llvm::Value *Loaded = Builder.CreateLoad(I64, Addr, "t11_4");
  Builder.CreateRet(Loaded);

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("__readgsqword(88)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("t11_4"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, SingleFunctionEmitOmitsSiblingDefinitions) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-one", Context);
  llvm::Type *Void = llvm::Type::getVoidTy(Context);
  llvm::FunctionType *FnTy = llvm::FunctionType::get(Void, false);
  llvm::Function *Keep = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "keep_me", Module);
  llvm::Function *Drop = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "drop_me", Module);
  llvm::IRBuilder<> KeepBuilder(
      llvm::BasicBlock::Create(Context, "entry", Keep));
  KeepBuilder.CreateRetVoid();
  llvm::IRBuilder<> DropBuilder(
      llvm::BasicBlock::Create(Context, "entry", Drop));
  DropBuilder.CreateRetVoid();

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options, nullptr, nullptr, Keep));
  OS.flush();
  EXPECT_NE(Source.find("keep_me"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("drop_me"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, GsHandlerDataBit2AlignBranchIsPrinted) {
  // MSVC __GSHandlerCheckCommon: TEST [r8],4 / MOV r10,rcx / JZ, then align
  // r10 from HandlerData+4/+8. HighC must keep the bit-2 branch; the MOV
  // between TEST and Jcc does not clobber flags.
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t Entry = 0x140001000;
  Img.Entry = Entry;
  static const uint8_t kBytes[] = {
      0x41, 0xf6, 0x00, 0x04, 0x4c, 0x8b, 0xd1, 0x74, 0x13, 0x41, 0x8b,
      0x40, 0x08, 0x4d, 0x63, 0x50, 0x04, 0xf7, 0xd8, 0x4c, 0x03, 0xd1,
      0x48, 0x63, 0xc8, 0x4c, 0x23, 0xd1, 0x4c, 0x89, 0xd0, 0xc3,
  };
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = sizeof(kBytes);
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(std::begin(kBytes), std::end(kBytes));
  Img.Segments.push_back(std::move(Text));
  Section TextSection;
  TextSection.Name = ".text";
  TextSection.VA = Entry;
  TextSection.Size = sizeof(kBytes);
  TextSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(TextSection));
  Img.KnownCodeRanges.emplace_back(Entry, Entry + sizeof(kBytes));
  Symbol FuncSym = Symbol::makeFunc(Entry, sizeof(kBytes));
  FuncSym.Name = "GSHandlerCheckCommon";
  Img.Symbols.push_back(std::move(FuncSym));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_FALSE(Result.HighFuncs.empty());
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  EXPECT_NE(Source.find("& 4"), std::string::npos) << Source;
  EXPECT_NE(Source.find("if ("), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, CallResultUsedByTestEaxIsAssigned) {
  // `call helper / test eax, eax / je` must keep the call result. Dropping
  // the dest turns the compare into an uninitialized temp (cxx_eh_probe
  // 0x140001570 and MapleStory2 throw).
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t Entry = 0x140001000;
  constexpr va_t Helper = 0x140001020;
  Img.Entry = Entry;
  static const uint8_t kBytes[] = {
      0xe8, 0x1b, 0x00, 0x00, 0x00, // call helper
      0x85, 0xc0,                   // test eax, eax
      0x74, 0x01,                   // je +1
      0xc3,                         // ret
      0xc3,                         // ret
  };
  static const uint8_t kHelper[] = {
      0xb8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
      0xc3,
  };
  std::vector<uint8_t> Text(0x30, 0xcc);
  std::copy(std::begin(kBytes), std::end(kBytes), Text.begin());
  std::copy(std::begin(kHelper), std::end(kHelper), Text.begin() + 0x20);
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Entry;
  Seg.Size = Text.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = Text;
  Img.Segments.push_back(std::move(Seg));
  Section Sec;
  Sec.Name = ".text";
  Sec.VA = Entry;
  Sec.Size = Text.size();
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Sec));
  Img.KnownCodeRanges.emplace_back(Entry, Entry + sizeof(kBytes));
  Img.KnownCodeRanges.emplace_back(Helper, Helper + sizeof(kHelper));
  Symbol Main = Symbol::makeFunc(Entry, sizeof(kBytes));
  Main.Name = "uses_call_result";
  Img.Symbols.push_back(std::move(Main));
  Symbol Help = Symbol::makeFunc(Helper, sizeof(kHelper));
  Help.Name = "ret_one";
  Img.Symbols.push_back(std::move(Help));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_FALSE(Result.HighFuncs.empty());
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  EXPECT_NE(Source.find("= ret_one("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("\n    ret_one();"), std::string::npos) << Source;
  EXPECT_NE(Source.find("if ("), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, CallFollowedByInt3OmitsDebugBreakAndReturn) {
  // MSVC plants `int3` after noreturn helpers.  An unnamed call immediately
  // followed by `int3` must not print `__debugbreak()` or a success return
  // (cxx_eh_probe throw / abort join).
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t Entry = 0x140001000;
  constexpr va_t Helper = 0x140001020;
  Img.Entry = Entry;
  static const uint8_t kBytes[] = {
      0x85, 0xc9,                   // test ecx, ecx
      0x74, 0x06,                   // je helper_path
      0xb8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
      0xc3,                         // ret
      0xe8, 0x11, 0x00, 0x00, 0x00, // call helper (rel to 0x20)
      0xcc,                         // int3
      0xc3,                         // ret
  };
  static const uint8_t kHelper[] = {
      0xb8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
      0xc3,
  };
  std::vector<uint8_t> Text(0x30, 0xcc);
  std::copy(std::begin(kBytes), std::end(kBytes), Text.begin());
  std::copy(std::begin(kHelper), std::end(kHelper), Text.begin() + 0x20);
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Entry;
  Seg.Size = Text.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = Text;
  Img.Segments.push_back(std::move(Seg));
  Section Sec;
  Sec.Name = ".text";
  Sec.VA = Entry;
  Sec.Size = Text.size();
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Sec));
  Img.KnownCodeRanges.emplace_back(Entry, Entry + sizeof(kBytes));
  Img.KnownCodeRanges.emplace_back(Helper, Helper + sizeof(kHelper));
  Symbol Main = Symbol::makeFunc(Entry, sizeof(kBytes));
  Main.Name = "maybe_abort";
  Img.Symbols.push_back(std::move(Main));
  Symbol Help = Symbol::makeFunc(Helper, sizeof(kHelper));
  Help.Name = "abort_like";
  Img.Symbols.push_back(std::move(Help));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_FALSE(Result.HighFuncs.empty());
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS));
  OS.flush();
  EXPECT_NE(Source.find("abort_like("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__debugbreak"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("= abort_like("), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, TestRcxDoesNotEmitPopcount) {
  // `test rcx, rcx / je` only consumes ZF. PF via POPCOUNT must not appear in
  // --no-opt LLVM-to-C (cookie / seh_probe SSA noise).
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t Entry = 0x140001000;
  Img.Entry = Entry;
  static const uint8_t kBytes[] = {
      0x48, 0x85, 0xc9, // test rcx, rcx
      0x74, 0x01,       // je +1
      0xc3,             // ret
      0xc3,             // ret
  };
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Entry;
  Seg.Size = sizeof(kBytes);
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.assign(std::begin(kBytes), std::end(kBytes));
  Img.Segments.push_back(std::move(Seg));
  Section Sec;
  Sec.Name = ".text";
  Sec.VA = Entry;
  Sec.Size = sizeof(kBytes);
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Sec));
  Img.KnownCodeRanges.emplace_back(Entry, Entry + sizeof(kBytes));
  Symbol FuncSym = Symbol::makeFunc(Entry, sizeof(kBytes));
  FuncSym.Name = "test_rcx";
  Img.Symbols.push_back(std::move(FuncSym));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.NoOpt = true;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, Options));
  OS.flush();
  EXPECT_EQ(Source.find("__builtin_popcount"), std::string::npos) << Source;
  EXPECT_NE(Source.find("if ("), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, CookieCmpRolTestDoesNotEmitPopcount) {
  // Cookie-style `cmp / jne; rol; test; jne` joins two flag-producing
  // compares.  Unused PF PHIs must not keep `__builtin_popcount`.
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t Entry = 0x140001000;
  Img.Entry = Entry;
  static const uint8_t kBytes[] = {
      0x48, 0x39, 0xd1,       // cmp rcx, rdx
      0x75, 0x0a,             // jne fail
      0x48, 0xc1, 0xc1, 0x10, // rol rcx, 16
      0x66, 0x85, 0xc9,       // test cx, cx
      0x75, 0x01,             // jne fail
      0xc3,                   // ret
      0xc3,                   // fail: ret
  };
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Entry;
  Seg.Size = sizeof(kBytes);
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.assign(std::begin(kBytes), std::end(kBytes));
  Img.Segments.push_back(std::move(Seg));
  Section Sec;
  Sec.Name = ".text";
  Sec.VA = Entry;
  Sec.Size = sizeof(kBytes);
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Sec));
  Img.KnownCodeRanges.emplace_back(Entry, Entry + sizeof(kBytes));
  Symbol FuncSym = Symbol::makeFunc(Entry, sizeof(kBytes));
  FuncSym.Name = "check_cookie";
  Img.Symbols.push_back(std::move(FuncSym));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.NoOpt = true;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, Options));
  OS.flush();
  EXPECT_EQ(Source.find("__builtin_popcount"), std::string::npos) << Source;
  EXPECT_NE(Source.find("if ("), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, FuncLoadCxxThrowThunkPrintsThrowWithoutDebugBreak) {
  // `--func` skips scanImportThunks.  `call jmp-[IAT]; int3` must still
  // print `throw` from the import name, not `sub_*` plus `__debugbreak`.
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  constexpr va_t Entry = 0x140001000;
  constexpr va_t Thunk = 0x140001020;
  constexpr va_t IAT = 0x140003000;
  Img.Entry = Entry;
  Img.LoadOnlyFunctionEntries.insert(Entry);
  const int32_t CallRel = static_cast<int32_t>(Thunk - (Entry + 5));
  const int32_t ThunkDisp = static_cast<int32_t>(IAT - (Thunk + 6));
  std::vector<uint8_t> Text(0x30, 0xcc);
  Text[0] = 0xe8;
  std::memcpy(Text.data() + 1, &CallRel, sizeof(CallRel));
  Text[5] = 0xcc;
  Text[6] = 0xc3;
  Text[0x20] = 0xff;
  Text[0x21] = 0x25;
  std::memcpy(Text.data() + 0x22, &ThunkDisp, sizeof(ThunkDisp));
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Entry;
  Seg.Size = Text.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = Text;
  Img.Segments.push_back(std::move(Seg));
  Section Sec;
  Sec.Name = ".text";
  Sec.VA = Entry;
  Sec.Size = Text.size();
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Sec));
  Img.KnownCodeRanges.emplace_back(Entry, Entry + 7);
  Symbol FuncSym = Symbol::makeFunc(Entry, 7);
  FuncSym.Name = "throws";
  Img.Symbols.push_back(std::move(FuncSym));
  Import Imp;
  Imp.Name = "_CxxThrowException";
  Imp.IATAddr = IAT;
  Img.Imports.push_back(std::move(Imp));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_FALSE(Result.HighFuncs.empty());
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("throw"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__debugbreak"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("sub_140001020"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, CorpusFuncLoadCxxEhProbePrintsThrow) {
  if (NEVERD_BINARY_CORPUS_ROOT[0] == '\0')
    GTEST_SKIP() << "windows-eh corpus root is not configured";
  const std::filesystem::path Path =
      std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
      "corpus/windows-eh/msvc/x86_64/fh4/no-gs/o0/abi-probe/"
      "cxx_eh_probe-msvc-x86_64-fh4-no-gs-o0.exe";
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << Path.string() << " is missing";

  BinaryLoadOptions Discover;
  Discover.OnlyFunctionEntries.insert(1);
  auto DiscoverImg = loadBinary(Path, Discover);
  ASSERT_TRUE(static_cast<bool>(DiscoverImg))
      << llvm::toString(DiscoverImg.takeError());

  va_t ThrowIAT = 0;
  for (const Import &Imp : DiscoverImg->Imports)
    if (stripLeadingUnderscores(Imp.Name) == "CxxThrowException")
      ThrowIAT = Imp.IATAddr;
  ASSERT_NE(ThrowIAT, 0u);

  va_t Thunk = 0;
  va_t CallSite = 0;
  for (const Segment &Seg : DiscoverImg->Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < x86::kJmpIndirectLen)
      continue;
    for (size_t I = 0; I + x86::kJmpIndirectLen <= Seg.Data.size(); ++I) {
      if (Seg.Data[I] != x86::kJmpIndirectOp ||
          Seg.Data[I + 1] != x86::kJmpIndirectModRM)
        continue;
      int32_t Disp = 0;
      std::memcpy(&Disp, Seg.Data.data() + I + x86::kJmpIndirectDispOffset,
                  sizeof(Disp));
      const va_t Insn = Seg.VA + I;
      if (Insn + x86::kJmpIndirectLen + static_cast<int64_t>(Disp) == ThrowIAT)
        Thunk = Insn;
    }
  }
  ASSERT_NE(Thunk, 0u);
  for (const Segment &Seg : DiscoverImg->Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < x86::kCallRel32Len)
      continue;
    for (size_t I = 0; I + x86::kCallRel32Len <= Seg.Data.size(); ++I) {
      if (Seg.Data[I] != x86::kCallRel32)
        continue;
      int32_t Rel = 0;
      std::memcpy(&Rel, Seg.Data.data() + I + x86::kRel32DispOffset,
                  sizeof(Rel));
      const va_t Insn = Seg.VA + I;
      if (Insn + x86::kCallRel32Len + static_cast<int64_t>(Rel) == Thunk) {
        CallSite = Insn;
        break;
      }
    }
    if (CallSite)
      break;
  }
  ASSERT_NE(CallSite, 0u);

  va_t Entry = 0;
  for (const auto &Rec : DiscoverImg->COFFPDataRecords) {
    const va_t Begin = DiscoverImg->Base + Rec.BeginRVA;
    const va_t End = DiscoverImg->Base + Rec.EndRVA;
    if (CallSite >= Begin && CallSite < End) {
      Entry = Begin;
      break;
    }
  }
  ASSERT_NE(Entry, 0u);

  BinaryLoadOptions FuncOpts;
  FuncOpts.OnlyFunctionEntries.insert(Entry);
  auto Img = loadBinary(Path, FuncOpts);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());
  EXPECT_TRUE(Img->ImportStubIndices.empty());

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(*Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_FALSE(Result.HighFuncs.empty());
  std::vector<HighFunc> Related = Result.HighFuncs;
  attachCxxFuncletBodies(Related);
  const HighFunc *Attached = nullptr;
  for (const HighFunc &Func : Related)
    if (Func.Entry == Entry)
      Attached = &Func;
  ASSERT_NE(Attached, nullptr);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Img->Arch;
  Options.Format = Img->Format;
  Options.Image = &*Img;
  ASSERT_TRUE(HighCEmitter().emit({*Attached}, OS, Options));
  OS.flush();
  EXPECT_NE(Source.find("throw"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("__debugbreak"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("throw 0"), std::string::npos) << Source;
}

std::filesystem::path gsSehProbePath() {
  return std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
         "corpus/windows-eh/msvc/x86_64/fh4/gs/o0/abi-probe/"
         "seh_probe-msvc-x86_64-fh4-gs-o0.exe";
}

va_t pdataEntryContaining(const BinaryImage &Img, va_t Addr) {
  for (const auto &Rec : Img.COFFPDataRecords) {
    const va_t Begin = Img.Base + Rec.BeginRVA;
    const va_t End = Img.Base + Rec.EndRVA;
    if (Addr >= Begin && Addr < End)
      return Begin;
  }
  return 0;
}

va_t findExecutableBytes(const BinaryImage &Img, llvm::ArrayRef<uint8_t> Needle) {
  if (Needle.empty())
    return 0;
  for (const Segment &Seg : Img.Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < Needle.size())
      continue;
    auto It = std::search(Seg.Data.begin(), Seg.Data.end(), Needle.begin(),
                          Needle.end());
    if (It != Seg.Data.end())
      return Seg.VA + static_cast<va_t>(It - Seg.Data.begin());
  }
  return 0;
}

std::string llvmcOnlyFunction(BinaryImage Img, va_t Entry) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.NoOpt = true;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  if (!Result.Success || !Result.LlvmModule)
    return Result.Error;
  llvm::Function *Keep = nullptr;
  const std::string Want = "sub_" + llvm::utohexstr(Entry);
  for (llvm::Function &Fn : *Result.LlvmModule) {
    if (Fn.isDeclaration())
      continue;
    if (Fn.getName() == Want) {
      Keep = &Fn;
      break;
    }
    if (!Keep)
      Keep = &Fn;
  }
  if (!Keep)
    return {};
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  if (!LLVMCEmitter().emit(*Result.LlvmModule, OS, Options, nullptr, &Img,
                           Keep))
    return {};
  OS.flush();
  return Source;
}

std::string highcOnlyFunction(BinaryImage Img, va_t Entry) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  if (!Result.Success || Result.HighFuncs.empty())
    return Result.Error;
  std::vector<HighFunc> Related = Result.HighFuncs;
  attachCxxFuncletBodies(Related);
  const HighFunc *Attached = nullptr;
  for (const HighFunc &Func : Related)
    if (Func.Entry == Entry)
      Attached = &Func;
  if (!Attached)
    return {};
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  if (!HighCEmitter().emit({*Attached}, OS, Options))
    return {};
  OS.flush();
  return Source;
}

TEST(HighCPointerAddresses, CorpusFuncLoadGsCookieIsVoidNoreturnFail) {
  // Hex-Rays `_security_check_cookie` is void; the unnamed fail `jmp` is
  // noreturn. Drive the public /GS seh_probe cookie, not stuffed HighIR.
  if (NEVERD_BINARY_CORPUS_ROOT[0] == '\0')
    GTEST_SKIP() << "windows-eh corpus root is not configured";
  const auto Path = gsSehProbePath();
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << Path.string() << " is missing";

  BinaryLoadOptions Discover;
  Discover.OnlyFunctionEntries.insert(1);
  auto DiscoverImg = loadBinary(Path, Discover);
  ASSERT_TRUE(static_cast<bool>(DiscoverImg))
      << llvm::toString(DiscoverImg.takeError());
  const uint8_t RolRcx16[] = {0x48, 0xc1, 0xc1, 0x10};
  const va_t Rol = findExecutableBytes(*DiscoverImg, RolRcx16);
  ASSERT_NE(Rol, 0u);
  const va_t Entry = pdataEntryContaining(*DiscoverImg, Rol);
  ASSERT_NE(Entry, 0u);

  BinaryLoadOptions FuncOpts;
  FuncOpts.OnlyFunctionEntries.insert(Entry);
  auto Img = loadBinary(Path, FuncOpts);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());
  const std::string Source = highcOnlyFunction(std::move(*Img), Entry);
  ASSERT_FALSE(Source.empty()) << Source;
  EXPECT_NE(Source.find("void "), std::string::npos) << Source;
  EXPECT_EQ(Source.find("= sub_"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return v"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return t"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, CorpusFuncLoadGsHandlerCheckReturnsOne) {
  // Hex-Rays `_GSHandlerCheck` returns 1 (ExceptionContinueSearch), not void.
  if (NEVERD_BINARY_CORPUS_ROOT[0] == '\0')
    GTEST_SKIP() << "windows-eh corpus root is not configured";
  const auto Path = gsSehProbePath();
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << Path.string() << " is missing";

  BinaryLoadOptions Discover;
  Discover.OnlyFunctionEntries.insert(1);
  auto DiscoverImg = loadBinary(Path, Discover);
  ASSERT_TRUE(static_cast<bool>(DiscoverImg))
      << llvm::toString(DiscoverImg.takeError());
  const uint8_t MovEax1[] = {0xb8, 0x01, 0x00, 0x00, 0x00};
  va_t Entry = 0;
  for (const Segment &Seg : DiscoverImg->Segments) {
    if (!Seg.isExecutable() || Seg.Data.size() < sizeof(MovEax1))
      continue;
    for (auto It = Seg.Data.begin();;) {
      It = std::search(It, Seg.Data.end(), std::begin(MovEax1),
                       std::end(MovEax1));
      if (It == Seg.Data.end())
        break;
      const va_t Addr = Seg.VA + static_cast<va_t>(It - Seg.Data.begin());
      const va_t Found = pdataEntryContaining(*DiscoverImg, Addr);
      if (Found) {
        uint32_t Span = 0;
        for (const auto &Rec : DiscoverImg->COFFPDataRecords) {
          const va_t Begin = DiscoverImg->Base + Rec.BeginRVA;
          if (Begin != Found)
            continue;
          Span = Rec.EndRVA - Rec.BeginRVA;
          break;
        }
        if (Span != 0 && Span <= 0x28) {
          Entry = Found;
          break;
        }
      }
      ++It;
    }
    if (Entry)
      break;
  }
  ASSERT_NE(Entry, 0u) << "no small mov-eax-1 pdata body";

  BinaryLoadOptions FuncOpts;
  FuncOpts.OnlyFunctionEntries.insert(Entry);
  auto Img = loadBinary(Path, FuncOpts);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());
  const std::string Source = highcOnlyFunction(std::move(*Img), Entry);
  ASSERT_FALSE(Source.empty()) << Source;
  EXPECT_NE(Source.find("return 1"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("void sub_"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, CorpusFuncLoadCxxEhProbeCatchReturnsValue) {
  if (NEVERD_BINARY_CORPUS_ROOT[0] == '\0')
    GTEST_SKIP() << "windows-eh corpus root is not configured";
  const auto Path = std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
                    "corpus/windows-eh/msvc/x86_64/fh4/no-gs/o0/abi-probe/"
                    "cxx_eh_probe-msvc-x86_64-fh4-no-gs-o0.exe";
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << Path.string() << " is missing";

  BinaryLoadOptions Discover;
  Discover.OnlyFunctionEntries.insert(1);
  auto DiscoverImg = loadBinary(Path, Discover);
  ASSERT_TRUE(static_cast<bool>(DiscoverImg))
      << llvm::toString(DiscoverImg.takeError());

  va_t Entry = 0;
  std::string Source;
  for (const auto &Rec : DiscoverImg->COFFPDataRecords) {
    const va_t Begin = DiscoverImg->Base + Rec.BeginRVA;
    BinaryLoadOptions FuncOpts;
    FuncOpts.OnlyFunctionEntries.insert(Begin);
    auto Img = loadBinary(Path, FuncOpts);
    if (!Img)
      continue;
    std::string Text = highcOnlyFunction(*Img, Begin);
    if (Text.find("throw;") == std::string::npos ||
        Text.find("catch") == std::string::npos)
      continue;
    Entry = Begin;
    Source = std::move(Text);
    break;
  }
  ASSERT_NE(Entry, 0u) << "no nested rethrow with catch";
  EXPECT_EQ(Source.find("arg1"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return v0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return v1"), std::string::npos) << Source;
  EXPECT_NE(Source.find("throw;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("return t"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, CorpusFuncLoadCxxEhProbeNestedCatchReturnsValues) {
  if (NEVERD_BINARY_CORPUS_ROOT[0] == '\0')
    GTEST_SKIP() << "windows-eh corpus root is not configured";
  const auto Path = std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
                    "corpus/windows-eh/msvc/x86_64/fh4/no-gs/o0/abi-probe/"
                    "cxx_eh_probe-msvc-x86_64-fh4-no-gs-o0.exe";
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << Path.string() << " is missing";

  BinaryLoadOptions Discover;
  Discover.OnlyFunctionEntries.insert(1);
  auto DiscoverImg = loadBinary(Path, Discover);
  ASSERT_TRUE(static_cast<bool>(DiscoverImg))
      << llvm::toString(DiscoverImg.takeError());

  va_t Entry = 0;
  std::string Source;
  for (const auto &Rec : DiscoverImg->COFFPDataRecords) {
    const va_t Begin = DiscoverImg->Base + Rec.BeginRVA;
    BinaryLoadOptions FuncOpts;
    FuncOpts.OnlyFunctionEntries.insert(Begin);
    auto Img = loadBinary(Path, FuncOpts);
    if (!Img)
      continue;
    std::string Text = highcOnlyFunction(*Img, Begin);
    if (Text.find("return -200") == std::string::npos ||
        Text.find("catch") == std::string::npos)
      continue;
    Entry = Begin;
    Source = std::move(Text);
    break;
  }
  ASSERT_NE(Entry, 0u) << "no nested catch returning -200";
  EXPECT_EQ(Source.find("arg1"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return v2"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return v0"), std::string::npos) << Source;
  EXPECT_NE(Source.find("return -200"), std::string::npos) << Source;
}

TEST(HighCPointerAddresses, CorpusFuncLoadCxxEhProbePrintsUnwindDestructor) {
  if (NEVERD_BINARY_CORPUS_ROOT[0] == '\0')
    GTEST_SKIP() << "windows-eh corpus root is not configured";
  const auto Path = std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
                    "corpus/windows-eh/msvc/x86_64/fh4/no-gs/o0/abi-probe/"
                    "cxx_eh_probe-msvc-x86_64-fh4-no-gs-o0.exe";
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << Path.string() << " is missing";

  BinaryLoadOptions Discover;
  Discover.OnlyFunctionEntries.insert(1);
  auto DiscoverImg = loadBinary(Path, Discover);
  ASSERT_TRUE(static_cast<bool>(DiscoverImg))
      << llvm::toString(DiscoverImg.takeError());

  va_t Entry = 0;
  std::string Source;
  for (const auto &Rec : DiscoverImg->COFFPDataRecords) {
    const va_t Begin = DiscoverImg->Base + Rec.BeginRVA;
    BinaryLoadOptions FuncOpts;
    FuncOpts.OnlyFunctionEntries.insert(Begin);
    auto Img = loadBinary(Path, FuncOpts);
    if (!Img)
      continue;
    std::string Text = highcOnlyFunction(*Img, Begin);
    if (Text.find("unwind cleanup") == std::string::npos ||
        Text.find("return -200") == std::string::npos)
      continue;
    Entry = Begin;
    Source = std::move(Text);
    break;
  }
  ASSERT_NE(Entry, 0u) << "no nested catch with unwind cleanup";
  const auto CleanupAt = Source.find("unwind cleanup");
  ASSERT_NE(CleanupAt, std::string::npos) << Source;
  const std::string After = Source.substr(CleanupAt);
  EXPECT_NE(After.find("sub_"), std::string::npos) << After;
  EXPECT_NE(After.find("&var_m"), std::string::npos) << After;
  EXPECT_EQ(After.find("arg1"), std::string::npos) << After;
}

TEST(LLVMCPointerAddresses, NdDataGepPrintsSyntheticGlobalNotNullLoad) {
  llvm::LLVMContext Context;
  llvm::Module Module("llvm-c-nd-data-gep", Context);
  Module.setDataLayout("e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-"
                       "n8:16:32:64-S128");
  llvm::Type *I8 = llvm::Type::getInt8Ty(Context);
  llvm::Type *I64 = llvm::Type::getInt64Ty(Context);
  auto *BlobTy = llvm::ArrayType::get(I8, 256);
  auto *GV = new llvm::GlobalVariable(
      Module, BlobTy, /*isConstant=*/false, llvm::GlobalValue::ExternalLinkage,
      llvm::Constant::getNullValue(BlobTy), "__nd_data_140005000.data");
  auto *Off = llvm::ConstantInt::get(I64, 64);
  auto *GEP = llvm::ConstantExpr::getGetElementPtr(I8, GV, Off);
  llvm::FunctionType *FnTy = llvm::FunctionType::get(I64, false);
  llvm::Function *Function = llvm::Function::Create(
      FnTy, llvm::GlobalValue::ExternalLinkage, "load_cookie", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Ld = Builder.CreateLoad(I64, GEP, "ld");
  Builder.CreateRet(Ld);

  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, OS, Options, nullptr, nullptr,
                                  Function));
  OS.flush();
  EXPECT_EQ(Source.find("*(uint64_t*)0"), std::string::npos) << Source;
  EXPECT_NE(Source.find("g_140005040"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, CorpusFuncLoadGsCookieDoesNotLoadNull) {
  if (NEVERD_BINARY_CORPUS_ROOT[0] == '\0')
    GTEST_SKIP() << "windows-eh corpus root is not configured";
  const auto Path = gsSehProbePath();
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << Path.string() << " is missing";

  BinaryLoadOptions Discover;
  Discover.OnlyFunctionEntries.insert(1);
  auto DiscoverImg = loadBinary(Path, Discover);
  ASSERT_TRUE(static_cast<bool>(DiscoverImg))
      << llvm::toString(DiscoverImg.takeError());
  const uint8_t RolRcx16[] = {0x48, 0xc1, 0xc1, 0x10};
  const va_t Rol = findExecutableBytes(*DiscoverImg, RolRcx16);
  ASSERT_NE(Rol, 0u);
  const va_t Entry = pdataEntryContaining(*DiscoverImg, Rol);
  ASSERT_NE(Entry, 0u);

  BinaryLoadOptions FuncOpts;
  FuncOpts.OnlyFunctionEntries.insert(Entry);
  auto Img = loadBinary(Path, FuncOpts);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());
  const std::string Source = llvmcOnlyFunction(std::move(*Img), Entry);
  ASSERT_FALSE(Source.empty()) << Source;
  EXPECT_EQ(Source.find("*(uint64_t*)0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("*(uint32_t*)0"), std::string::npos) << Source;
  EXPECT_NE(Source.find("g_"), std::string::npos) << Source;
}

TEST(LLVMCPointerAddresses, CorpusFuncLoadSehProbeHasSingleWin64Arg) {
  if (NEVERD_BINARY_CORPUS_ROOT[0] == '\0')
    GTEST_SKIP() << "windows-eh corpus root is not configured";
  const auto Path = std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
                    "corpus/windows-eh/msvc/x86_64/fh4/no-gs/o0/abi-probe/"
                    "seh_probe-msvc-x86_64-fh4-no-gs-o0.exe";
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << Path.string() << " is missing";

  BinaryLoadOptions FuncOpts;
  FuncOpts.OnlyFunctionEntries.insert(0x140001050);
  auto Img = loadBinary(Path, FuncOpts);
  ASSERT_TRUE(static_cast<bool>(Img)) << llvm::toString(Img.takeError());
  const std::string Source = llvmcOnlyFunction(std::move(*Img), 0x140001050);
  ASSERT_FALSE(Source.empty()) << Source;
  EXPECT_NE(Source.find("arg0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("arg1"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("arg7"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("*(uint32_t*)0"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("*(uint64_t*)0"), std::string::npos) << Source;
}

} // namespace

BinaryImage makeCodeFixture(va_t Entry, std::vector<uint8_t> Bytes) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Entry;
  Seg.Size = Bytes.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Bytes);
  Img.Segments.push_back(std::move(Seg));
  return Img;
}

TEST(HighCPointerAddresses, StateSnapshotPrintsSourceIntrinsics) {
  // RtlXSave/RtlXRestore in ntoskrnl: the source used _xsave64/_xrstor64.
  // Both routes print that operation instead of aborting the function.
  constexpr va_t Entry = 0x140001000;
  const std::vector<uint8_t> Code = {0x48, 0x0f, 0xae, 0x21, // xsave64  [rcx]
                                     0x48, 0x0f, 0xae, 0x29, // xrstor64 [rcx]
                                     0xc3};
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(HighC.find("_xsave64((void *)"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("_xrstor64((void *)"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("hardware x87/SSE/AVX state"), std::string::npos)
      << HighC;
  EXPECT_EQ(HighC.find("__asm__"), std::string::npos) << HighC;

  const std::string LLVMC =
      llvmcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(LLVMC.find("xsave64"), std::string::npos) << LLVMC;
  EXPECT_NE(LLVMC.find("xrstor64"), std::string::npos) << LLVMC;
}

TEST(HighCPointerAddresses, OddWideIntegerSlicePrintsBitInt) {
  // A YMM value shifted by one 32-bit lane leaves a 224-bit slice.
  EXPECT_EQ(typeToC(NdType::makeInt(28, false)), "unsigned _BitInt(224)");
  EXPECT_EQ(typeToC(NdType::makeInt(32, false)), "uint256_t");
  EXPECT_THROW(typeToC(NdType::makeInt(65, false)), std::invalid_argument);
}

// Caller keeps RDX live across a direct call, as MSVC /LTCG does when the
// callee provably leaves RDX alone (PsGetJobSilo -> PspGetJobSilo).
std::vector<uint8_t> callerKeepsRdxAcrossCall(std::vector<uint8_t> Callee) {
  std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x28, // sub  rsp, 28h
                               0xe8, 0x17, 0x00, 0x00,
                               0x00,             // call +0x17 -> 0x140001020
                               0x48, 0x89, 0x02, // mov  [rdx], rax
                               0x48, 0x83, 0xc4, 0x28, // add  rsp, 28h
                               0xc3};                  // ret
  Code.resize(0x20, 0xcc);
  Code.insert(Code.end(), Callee.begin(), Callee.end());
  return Code;
}

TEST(HighCPointerAddresses, CalleeThatLeavesRdxAloneKeepsCallerValue) {
  constexpr va_t Entry = 0x140001000;
  const auto Code = callerKeepsRdxAcrossCall({0x48, 0x8b, 0xc1, // mov rax, rcx
                                              0xc3});
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_EQ(HighC.find("unknown"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("arg1"), std::string::npos) << HighC;
  const std::string LLVMC =
      llvmcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_EQ(LLVMC.find("unknown"), std::string::npos) << LLVMC;
  EXPECT_NE(LLVMC.find("arg1"), std::string::npos) << LLVMC;
}

TEST(HighCPointerAddresses, CalleeThatWritesRdxOrCallsIndirectlyClobbersIt) {
  constexpr va_t Entry = 0x140001000;
  for (const auto &Callee : std::vector<std::vector<uint8_t>>{
           {0x33, 0xd2, 0x48, 0x8b, 0xc1, 0xc3}, // xor edx, edx; mov rax, rcx
           {0xff, 0xd1, 0xc3},                   // call rcx
           {0x31, 0xc0, 0x0f, 0xa2, 0xc3}}) {    // xor eax, eax; cpuid
    const std::string HighC = highcOnlyFunction(
        makeCodeFixture(Entry, callerKeepsRdxAcrossCall(Callee)), Entry);
    EXPECT_EQ(HighC.find("arg1"), std::string::npos) << HighC;
  }
}

TEST(HighCPointerAddresses, LoopCarriedCopyKeepsItsEntryAssignment) {
  // `for (p = arg0; !check(p); p = p->next) ; return p;` with p in RCX across
  // a callee that leaves RCX alone.  The variable has two definitions, so
  // HighC must not rename its uses to the in-loop load temp.
  constexpr va_t Entry = 0x140001000;
  std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x28,       // sub  rsp, 28h
                               0x48, 0x85, 0xc9,             // test rcx, rcx
                               0x75, 0x08,                   // jne  0x140001011
                               0x33, 0xc0,                   // xor  eax, eax
                               0x48, 0x83, 0xc4, 0x28,       // add  rsp, 28h
                               0xc3,                         // ret
                               0xcc,                         // int3
                               0xe8, 0x8a, 0x00, 0x00, 0x00, // call 0x1400010A0
                               0x84, 0xc0,                   // test al, al
                               0x75, 0x09,                   // jne  0x140001023
                               0x48, 0x8b, 0x89, 0x78, 0x04,
                               0x00, 0x00,       // mov  rcx, [rcx+478h]
                               0xeb, 0xee,       // jmp  0x140001011
                               0x48, 0x8b, 0xc1, // mov  rax, rcx
                               0xeb, 0xe3};      // jmp  0x14000100B
  Code.resize(0xA0, 0xcc);
  Code.insert(Code.end(), {0x48, 0x85, 0xc9, // test rcx, rcx
                           0x0f, 0x94, 0xc0, // sete al
                           0xc3});           // ret
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  std::smatch Call;
  ASSERT_TRUE(
      std::regex_search(HighC, Call, std::regex(R"(sub_1400010A0\((\w+)\))")))
      << HighC;
  const std::string Carried = Call[1].str();
  EXPECT_NE(HighC.find(Carried + " = arg0;"), std::string::npos) << HighC;
}

TEST(HighCPointerAddresses, Win64RdiSurvivesAnUnsummarizedCall) {
  // The callee makes an indirect call, so only the Win64 ABI speaks for it:
  // RDI is nonvolatile there, unlike SysV.
  constexpr va_t Entry = 0x140001000;
  std::vector<uint8_t> Code = {0x57,                         // push rdi
                               0x48, 0x83, 0xec, 0x20,       // sub  rsp, 20h
                               0x48, 0x8b, 0xf9,             // mov  rdi, rcx
                               0xe8, 0x13, 0x00, 0x00, 0x00, // call 0x140001020
                               0x48, 0x8b, 0x07,             // mov  rax, [rdi]
                               0x48, 0x83, 0xc4, 0x20,       // add  rsp, 20h
                               0x5f,                         // pop  rdi
                               0xc3};                        // ret
  Code.resize(0x20, 0xcc);
  Code.insert(Code.end(), {0x48, 0x83, 0xec, 0x28, // sub  rsp, 28h
                           0xff, 0xd2,             // call rdx
                           0x48, 0x83, 0xc4, 0x28, // add  rsp, 28h
                           0xc3});                 // ret
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_EQ(HighC.find("unknown"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("arg0"), std::string::npos) << HighC;
  const std::string LLVMC =
      llvmcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_EQ(LLVMC.find("RDI"), std::string::npos) << LLVMC;
}

TEST(HighCPointerAddresses, UnwindlessNonLeafCandidateIsNotAFunction) {
  // In an x64 PE with an exception directory every function that moves RSP
  // or calls has a RUNTIME_FUNCTION.  An unnamed candidate without one is
  // data or a chunk remnant; an unnamed leaf without one stays a function.
  constexpr va_t Primary = 0x140001000;
  constexpr va_t NonLeaf = 0x140001010;
  constexpr va_t Leaf = 0x140001020;
  std::vector<uint8_t> Code = {0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x54, 0x5c, 0xc3}); // push rsp; pop rsp; ret
  Code.resize(0x20, 0xcc);
  Code.insert(Code.end(), {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3}); // mov eax,1
  BinaryImage Img = makeCodeFixture(Primary, Code);
  Img.KnownCodeRanges.push_back({Primary, Primary + 1});

  auto DispositionOf = [&](va_t Entry) {
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.OnlyFunctionEntries.insert(Entry);
    auto Result = Pipeline().run(Img, Ctx, Opts);
    for (const auto &Audit : Result.FunctionAudits)
      if (Audit.Entry == Entry)
        return Audit.Disposition;
    return PipelineFunctionDisposition::Candidate;
  };
  EXPECT_EQ(DispositionOf(NonLeaf),
            PipelineFunctionDisposition::RejectedUnwindlessNonLeaf);
  EXPECT_EQ(DispositionOf(Leaf), PipelineFunctionDisposition::Accepted);
}

TEST(HighCPointerAddresses, CalleeBranchToAnotherFunctionCountsItsWrites) {
  // The callee leaves through `jz g` to a separate function that clears
  // EDX.  The CFG keeps no block for g, so the summary must follow it.
  constexpr va_t Entry = 0x140001000;
  constexpr va_t G = 0x140001040;
  std::vector<uint8_t> Code = callerKeepsRdxAcrossCall(
      {0x48, 0x85, 0xc9,                   // test rcx, rcx
       0x0f, 0x84, 0x17, 0x00, 0x00, 0x00, // jz   0x140001040
       0x48, 0x8b, 0xc1,                   // mov  rax, rcx
       0xc3});                             // ret
  Code.resize(G - Entry, 0xcc);
  Code.insert(Code.end(), {0x33, 0xd2, 0xc3}); // xor edx, edx; ret
  BinaryImage Img = makeCodeFixture(Entry, Code);
  Symbol GSym = Symbol::makeFunc(G);
  GSym.Name = "clear_rdx";
  Img.Symbols.push_back(GSym);
  const std::string HighC = highcOnlyFunction(std::move(Img), Entry);
  EXPECT_EQ(HighC.find("arg1"), std::string::npos) << HighC;
}

TEST(HighCPointerAddresses, CalleeByteArgumentPassesOnlyTheByte) {
  // IoReleaseVpbSpinLock: `mov dl, cl` forwards a KIRQL to a callee that
  // reads only DL.  The untouched upper RDX bytes are not an argument.
  constexpr va_t Entry = 0x140001000;
  constexpr va_t G = 0x140001040;
  std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x28,       // sub rsp, 28h
                               0x88, 0xca,                   // mov dl, cl
                               0xb9, 0x09, 0x00, 0x00, 0x00, // mov ecx, 9
                               0xe8, 0x30, 0x00, 0x00, 0x00, // call G
                               0x48, 0x83, 0xc4, 0x28,       // add rsp, 28h
                               0xc3};
  Code.resize(G - Entry, 0xcc);
  Code.insert(Code.end(), {0x0f, 0xb6, 0xc2, 0xc3}); // movzx eax, dl; ret
  BinaryImage Img = makeCodeFixture(Entry, Code);
  Symbol GSym = Symbol::makeFunc(G);
  GSym.Name = "take_irql";
  Img.Symbols.push_back(GSym);
  const std::string HighC = highcOnlyFunction(std::move(Img), Entry);
  EXPECT_EQ(HighC.find("unknown"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("take_irql("), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("arg0"), std::string::npos) << HighC;
}

TEST(HighCPointerAddresses, Win64TwelveArgumentCallKeepsEveryStackArgument) {
  // HalGetScatterGatherList passes twelve arguments; the outgoing-area
  // stores span far more ops than a short scan window.
  constexpr va_t Entry = 0x140001000;
  constexpr va_t G = 0x140001080;
  std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x68}; // sub rsp, 68h
  for (uint8_t K = 0; K < 8; ++K) // mov qword [rsp+20h+8K], K+1
    Code.insert(Code.end(),
                {0x48, 0xc7, 0x44, 0x24, static_cast<uint8_t>(0x20 + 8 * K),
                 static_cast<uint8_t>(K + 1), 0x00, 0x00, 0x00});
  Code.insert(Code.end(), {0x33, 0xc9,       // xor ecx, ecx
                           0x33, 0xd2,       // xor edx, edx
                           0x45, 0x33, 0xc0, // xor r8d, r8d
                           0x45, 0x33, 0xc9, // xor r9d, r9d
                           0xe8});           // call G
  const int32_t Rel = static_cast<int32_t>(G - (Entry + Code.size() + 4));
  for (int I = 0; I < 4; ++I)
    Code.push_back(static_cast<uint8_t>(Rel >> (8 * I)));
  Code.insert(Code.end(), {0x48, 0x83, 0xc4, 0x68, 0xc3}); // add rsp; ret
  Code.resize(G - Entry, 0xcc);
  Code.insert(Code.end(), {0x48, 0x8b, 0x44, 0x24, 0x60, // mov rax, [rsp+60h]
                           0xc3});
  BinaryImage Img = makeCodeFixture(Entry, Code);
  Symbol GSym = Symbol::makeFunc(G);
  GSym.Name = "twelve";
  Img.Symbols.push_back(GSym);
  const std::string HighC = highcOnlyFunction(std::move(Img), Entry);
  EXPECT_NE(HighC.find("twelve(0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8)"),
            std::string::npos)
      << HighC;
}

TEST(HighCPointerAddresses, SlotStoredOnTwoPathsKeepsItsDeclaration) {
  // Two calls on two paths store different arguments to the same outgoing
  // slot; neither store may be forwarded away from its declaration.
  constexpr va_t Entry = 0x140001000;
  constexpr va_t G = 0x140001080;
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x38,       // sub rsp, 38h
      0x85, 0xc9,                   // test ecx, ecx
      0x74, 0x0f,                   // jz +15
      0x4c, 0x89, 0x4c, 0x24, 0x20, // mov [rsp+20h], r9
      0xe8, 0x6e, 0x00, 0x00, 0x00, // call G (0x140001080)
      0x48, 0x83, 0xc4, 0x38, 0xc3, // add rsp, 38h; ret
      0x4c, 0x89, 0x44, 0x24, 0x20, // mov [rsp+20h], r8
      0xe8, 0x5f, 0x00, 0x00, 0x00, // call G
      0x48, 0x83, 0xc4, 0x38, 0xc3};
  Code.resize(G - Entry, 0xcc);
  Code.insert(Code.end(), {0x48, 0x8b, 0x44, 0x24, 0x28, // mov rax, [rsp+28h]
                           0x48, 0x03, 0xc1,             // add rax, rcx
                           0xc3});
  BinaryImage Img = makeCodeFixture(Entry, Code);
  Symbol GSym = Symbol::makeFunc(G);
  GSym.Name = "five";
  Img.Symbols.push_back(GSym);
  const std::string HighC = highcOnlyFunction(std::move(Img), Entry);
  for (size_t Pos = HighC.find("var_m"); Pos != std::string::npos;
       Pos = HighC.find("var_m", Pos + 1)) {
    const std::string Name = HighC.substr(
        Pos, HighC.find_first_not_of("var_m0123456789ABCDEF", Pos) - Pos);
    EXPECT_NE(HighC.find(" " + Name + ";"), std::string::npos)
        << Name << " is used but not declared\n"
        << HighC;
  }
  EXPECT_NE(HighC.find("five(arg0, "), std::string::npos) << HighC;
}

TEST(HighCPointerAddresses, EntryLoopHeaderKeepsLoopCarriedValue) {
  // PopDirectedDripsFlushDeviceQueue: the first instruction is the loop
  // header, so the entry block has a back-edge predecessor.  The loop must
  // still advance RCX to [rcx+8] and test it on every iteration, including
  // the first.
  constexpr va_t Entry = 0x140001000;
  const std::vector<uint8_t> Code = {0x48, 0x85, 0xc9, // loop: test rcx, rcx
                                     0x74, 0x15,       // je ret
                                     0x48, 0x8b, 0x01, // mov rax, [rcx]
                                     0x48, 0x3b, 0xc1, // cmp rax, rcx
                                     0x75, 0x06,       // jne fail
                                     0x48, 0x8b, 0x49, 0x08, // mov rcx, [rcx+8]
                                     0xeb, 0xed,             // jmp loop
                                     0xb9, 0x03, 0x00, 0x00,
                                     0x00,       // fail: mov ecx, 3
                                     0xcd, 0x29, // int 29h
                                     0xc3};      // ret
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(HighC.find("__fastfail(3)"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("(uint64_t)(8)"), std::string::npos) << HighC;
  // The null test comes before the first load.
  EXPECT_LT(HighC.find("== 0"), HighC.find("(*(int64_t *)")) << HighC;
  const std::string LLVMC =
      llvmcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(LLVMC.find("+ 8"), std::string::npos) << LLVMC;
}

TEST(HighCPointerAddresses, LabeledCodeAfterFastFailKeepsItsLabel) {
  // VmpFreeMemoryRanges: the loop exit lands on code placed right after
  // `int 29h`.  That code is reached by a goto, so its label must survive.
  constexpr va_t Entry = 0x140001000;
  const std::vector<uint8_t> Code = {0x53,             // push rbx
                                     0x48, 0x8b, 0xd9, // mov rbx, rcx
                                     0x48, 0x8b, 0x0b, // loop: mov rcx, [rbx]
                                     0x48, 0x3b, 0xcb, // cmp rcx, rbx
                                     0x74, 0x15,       // je done
                                     0x48, 0x39, 0x59, 0x08, // cmp [rcx+8], rbx
                                     0x75, 0x08,             // jne fail
                                     0x48, 0x8b, 0x09,       // mov rcx, [rcx]
                                     0x48, 0x89, 0x0b,       // mov [rbx], rcx
                                     0xeb, 0xea,             // jmp loop
                                     0xb9, 0x03, 0x00, 0x00,
                                     0x00,             // fail: mov ecx, 3
                                     0xcd, 0x29,       // int 29h
                                     0x48, 0x8b, 0xc3, // done: mov rax, rbx
                                     0x5b,             // pop rbx
                                     0xc3};            // ret
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(HighC.find("__fastfail(3)"), std::string::npos) << HighC;
  std::smatch Goto;
  for (auto It = HighC.cbegin(); std::regex_search(
           It, HighC.cend(), Goto, std::regex(R"(goto (L_\w+);)"));
       It = Goto.suffix().first)
    EXPECT_NE(HighC.find(Goto[1].str() + ":"), std::string::npos)
        << Goto[1] << " has no label\n"
        << HighC;
  EXPECT_NE(HighC.find("return"), std::string::npos) << HighC;
}

TEST(HighCPointerAddresses, ControlAndDebugRegisterMovesUseMsvcIntrinsics) {
  // Inlined KeRaiseIrql/KeLowerIrql move CR8; ntoskrnl rejected ~2000
  // functions while these MOVs had no lift.
  constexpr va_t Entry = 0x140001000;
  const std::vector<uint8_t> Code = {0x44, 0x0f, 0x20, 0xc0, // mov rax, cr8
                                     0x44, 0x0f, 0x22, 0xc1, // mov cr8, rcx
                                     0x0f, 0x21, 0xfa,       // mov rdx, dr7
                                     0xc3};
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(HighC.find("__readcr8()"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("__writecr8("), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("__readdr(7)"), std::string::npos) << HighC;
  const std::string LLVMC =
      llvmcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(LLVMC.find("cr8"), std::string::npos) << LLVMC;
}

TEST(HighCPointerAddresses, DisplacedInvpcidDescriptorPrintsSourceIntrinsic) {
  // ntoskrnl builds the INVPCID descriptor on the stack: [rsp+20h].  The
  // address is computed, never loaded, and the source spelling is printed.
  constexpr va_t Entry = 0x140001000;
  const std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x38, // sub rsp, 38h
                                     0x66, 0x0f, 0x38, 0x82,
                                     0x44, 0x24, 0x20, // invpcid rax, [rsp+20h]
                                     0x48, 0x83, 0xc4, 0x38, // add rsp, 38h
                                     0xc3};
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(HighC.find("_invpcid("), std::string::npos) << HighC;
  const std::string LLVMC =
      llvmcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(LLVMC.find("invpcid"), std::string::npos) << LLVMC;
}

TEST(HighCPointerAddresses, Win64StackArgumentsFollowTheHomeArea) {
  // ObReferenceObjectByHandle-style call: RCX passes the caller's own first
  // argument through, [rsp+20h]/[rsp+28h] carry arguments 4 and 5.
  constexpr va_t Entry = 0x140001000;
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x48,       // sub rsp, 48h
      0x48, 0x8d, 0x44, 0x24, 0x40, // lea rax, [rsp+40h]
      0x48, 0xc7, 0x44, 0x24, 0x28, 0x00, 0x00, 0x00, 0x00, // mov [rsp+28h], 0
      0x48, 0x89, 0x44, 0x24, 0x20,       // mov [rsp+20h], rax
      0x41, 0xb9, 0x01, 0x00, 0x00, 0x00, // mov r9d, 1
      0x41, 0xb8, 0x02, 0x00, 0x00, 0x00, // mov r8d, 2
      0xba, 0x03, 0x00, 0x00, 0x00,       // mov edx, 3
      0xe8, 0x13, 0x00, 0x00, 0x00,       // call 0x140001040
      0x48, 0x83, 0xc4, 0x48,             // add rsp, 48h
      0xc3};
  Code.resize(0x40, 0xcc);
  // The callee reads all six arguments.
  Code.insert(Code.end(), {0x48, 0x8b, 0xc1,             // mov rax, rcx
                           0x48, 0x03, 0xc2,             // add rax, rdx
                           0x49, 0x03, 0xc0,             // add rax, r8
                           0x49, 0x03, 0xc1,             // add rax, r9
                           0x48, 0x03, 0x44, 0x24, 0x28, // add rax, [rsp+28h]
                           0x48, 0x03, 0x44, 0x24, 0x30, // add rax, [rsp+30h]
                           0xc3});
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  std::smatch Call;
  ASSERT_TRUE(std::regex_search(HighC, Call,
                                std::regex(R"(= sub_140001040\(([^;]*)\);)")))
      << HighC;
  const std::string Args = Call[1].str();
  EXPECT_EQ(std::count(Args.begin(), Args.end(), ','), 5) << HighC;
  EXPECT_EQ(Args.rfind("arg0", 0), 0u) << HighC;
  EXPECT_EQ(Args.substr(Args.size() - 1), "0") << HighC;
}

TEST(HighCPointerAddresses, SummarizedCalleeTakesOnlyTheArgumentsItReads) {
  // PsGetJobSilo -> PspGetJobSilo: the caller holds two arguments, the
  // callee reads RCX only, so the call passes one.
  constexpr va_t Entry = 0x140001000;
  const auto Code = callerKeepsRdxAcrossCall({0x48, 0x8b, 0xc1, // mov rax, rcx
                                              0xc3});
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(HighC.find("sub_140001020(arg0);"), std::string::npos) << HighC;
}

TEST(HighCPointerAddresses, Win64StackArgumentsThroughEntryStackCopy) {
  // RtlQueryPackageIdentity: `mov r11, rsp` addresses the outgoing slots,
  // and slot 5 is never written.  All seven arguments are still passed; the
  // local at [r11-18h], stored before the call and read after, is not one.
  constexpr va_t Entry = 0x140001000;
  std::vector<uint8_t> Code = {
      0x4c, 0x8b, 0xdc,                               // mov r11, rsp
      0x48, 0x83, 0xec, 0x58,                         // sub rsp, 58h
      0x49, 0xc7, 0x43, 0xe8, 0x00, 0x00, 0x00, 0x00, // mov [r11-18h], 0
      0x49, 0x8d, 0x43, 0xe8,                         // lea rax, [r11-18h]
      0x49, 0x89, 0x43, 0xd8,                         // mov [r11-28h], rax
      0x49, 0xc7, 0x43, 0xc8, 0x05, 0x00, 0x00, 0x00, // mov [r11-38h], 5
      0xe8, 0x1c, 0x00, 0x00, 0x00,                   // call 0x140001040
      0x48, 0x8b, 0x44, 0x24, 0x40,                   // mov rax, [rsp+40h]
      0x48, 0x83, 0xc4, 0x58,                         // add rsp, 58h
      0xc3};
  Code.resize(0x40, 0xcc);
  Code.insert(Code.end(), {0x48, 0x8b, 0xc1,             // mov rax, rcx
                           0x48, 0x03, 0xc2,             // add rax, rdx
                           0x49, 0x03, 0xc0,             // add rax, r8
                           0x49, 0x03, 0xc1,             // add rax, r9
                           0x48, 0x03, 0x44, 0x24, 0x28, // add rax, [rsp+28h]
                           0x48, 0x03, 0x44, 0x24, 0x30, // add rax, [rsp+30h]
                           0x48, 0x03, 0x44, 0x24, 0x38, // add rax, [rsp+38h]
                           0xc3});
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  std::smatch Call;
  ASSERT_TRUE(std::regex_search(HighC, Call,
                                std::regex(R"(= sub_140001040\(([^;]*)\);)")))
      << HighC;
  const std::string Args = Call[1].str();
  EXPECT_EQ(std::count(Args.begin(), Args.end(), ','), 6) << HighC;
  EXPECT_NE(Args.find(", 5, "), std::string::npos) << HighC;
}

TEST(HighCPointerAddresses, GotoToSmallReturnTailBecomesItsCopy) {
  // MmIsWriteErrorFatal: many paths `goto` a shared `result = 1; return`.
  MedVar Result;
  Result.Kind = MedVar::Temp;
  Result.Id = 7;
  Result.Size = 8;
  auto Assign = [&](uint64_t Value, va_t Addr) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = Addr;
    S.Dst = HighExpr::makeVar(Result);
    S.Val = HighExpr::makeConst(Value, 8);
    return S;
  };
  auto Return = [&](va_t Addr) {
    HighStmt S;
    S.Kind = StmtKind::Return;
    S.Addr = Addr;
    S.RetVal = HighExpr::makeVar(Result);
    return S;
  };
  HighStmt Goto;
  Goto.Kind = StmtKind::Goto;
  Goto.Addr = 0x1010;
  Goto.GotoTarget = 0x1040;
  HighStmt If;
  If.Kind = StmtKind::If;
  If.Addr = 0x1000;
  If.Cond = HighExpr::makeConst(1, 1);
  If.Body.push_back(Goto);

  std::vector<HighStmt> Body;
  Body.push_back(If);
  Body.push_back(Assign(0, 0x1020));
  Body.push_back(Return(0x1030));
  Body.push_back(Assign(1, 0x1040));
  Body.push_back(Return(0x1044));

  ASSERT_TRUE(duplicateSmallReturnTails(Body));
  ASSERT_EQ(Body[0].Body.size(), 2u);
  EXPECT_EQ(Body[0].Body[0].Kind, StmtKind::Assign);
  EXPECT_EQ(Body[0].Body[0].Val->ConstVal, 1u);
  EXPECT_EQ(Body[0].Body[1].Kind, StmtKind::Return);
  // The labelled original is kept for any other path.
  ASSERT_EQ(Body.size(), 5u);
  EXPECT_EQ(Body[3].Addr, 0x1040u);

  // A tail with an effect (a call) is not duplicated.
  std::vector<HighStmt> Effectful = Body;
  Effectful[0].Body = {Goto};
  Effectful[3].Val = HighExpr::makeCall("f", 0x2000, {});
  EXPECT_FALSE(duplicateSmallReturnTails(Effectful));
}

TEST(HighCPointerAddresses, InterruptFlagChangesDoNotClobberRax) {
  // Zw* stubs save RSP in RAX and then execute CLI.
  constexpr va_t Entry = 0x140001000;
  const std::vector<uint8_t> Code = {0x48, 0x8b, 0xc1, // mov rax, rcx
                                     0xfa,             // cli
                                     0xfb,             // sti
                                     0xc3};
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(HighC.find("_disable();"), std::string::npos) << HighC;
  EXPECT_EQ(HighC.find("= _disable()"), std::string::npos) << HighC;
  EXPECT_NE(HighC.find("return arg0;"), std::string::npos) << HighC;
}

TEST(HighCPointerAddresses, DirectionFlagIsClearOnEntry) {
  // Both x86 ABIs enter with DF clear, so REP MOVSB copies forward only.
  constexpr va_t Entry = 0x140001000;
  const std::vector<uint8_t> Code = {0x48, 0x8b, 0xf9, // mov rdi, rcx
                                     0x48, 0x8b, 0xf2, // mov rsi, rdx
                                     0x49, 0x8b, 0xc8, // mov rcx, r8
                                     0xf3, 0xa4,       // rep movsb
                                     0xc3};
  const std::string HighC =
      highcOnlyFunction(makeCodeFixture(Entry, Code), Entry);
  EXPECT_NE(HighC.find("rep movsb"), std::string::npos) << HighC;
  EXPECT_EQ(HighC.find("std"), std::string::npos) << HighC;
}
