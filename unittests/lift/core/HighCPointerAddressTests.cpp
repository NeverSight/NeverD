//===- HighCPointerAddressTests.cpp - Raw byte address projection --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionInfo.h"

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
#include <map>
#include <optional>
#include <string>
#include <vector>

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

TEST(HighCPointerAddresses,
     UnknownCalleeReturnIsNotDiscardedByBareSiblingReturn) {
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
  EXPECT_EQ(Source.find("void cookie"), std::string::npos) << Source;
  EXPECT_NE(Source.find("sub_14000173C("), std::string::npos) << Source;
  EXPECT_NE(Source.find("return t3"), std::string::npos) << Source;
  EXPECT_NE(Source.find("t3 ="), std::string::npos) << Source;
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

} // namespace
