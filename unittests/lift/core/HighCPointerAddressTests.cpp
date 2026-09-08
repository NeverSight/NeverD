//===- HighCPointerAddressTests.cpp - Raw byte address projection --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

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
                          Arch TheArch = Arch::X64) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = TheArch;
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
      EXPECT_NE(Source.find("(uintptr_t)arg0 + arg1 * 4"), std::string::npos)
          << Source;
      EXPECT_EQ(Source.find("(uintptr_t)(arg0 +"), std::string::npos) << Source;
    }
  }
}

TEST(HighCPointerAddresses, CastsPointerReturnsAfterMachineArithmetic) {
  auto Func = pointerFunction("advance", NdType::makePtr(NdType::makeInt(4)));
  returnValue(Func, byteOffset());
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(
      Source.find("return (int32_t*)(uintptr_t)((uintptr_t)arg0 + arg1 * 4);"),
      std::string::npos)
      << Source;

  auto Identity = pointerFunction("identity", Func.ReturnType);
  returnValue(Identity, parameter(0));
  const std::string IdentitySource = emitFunctions({Identity});
  EXPECT_NE(
      IdentitySource.find("return (int32_t*)(uintptr_t)((uintptr_t)arg0);"),
      std::string::npos)
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
  EXPECT_NE(
      Source.find("arg0 = (int32_t*)(uintptr_t)((uintptr_t)arg0 + arg1 * 4);"),
      std::string::npos)
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
  EXPECT_NE(AddressSource.find("(uintptr_t)(&arg0)"), std::string::npos)
      << AddressSource;
  EXPECT_EQ(AddressSource.find("&(uintptr_t)"), std::string::npos)
      << AddressSource;
}

TEST(HighCPointerAddresses, DoesNotRetypeIntegerParametersOrRenamedLocals) {
  auto Func = pointerFunction("integer_address", NdType::makeInt(8));
  Func.Params[0].Type = NdType::makeInt(8);
  returnValue(Func, byteOffset());
  const std::string Source = emitFunctions({Func});
  EXPECT_NE(Source.find("return arg0 + arg1 * 4;"), std::string::npos)
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

} // namespace
