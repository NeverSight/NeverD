//===- CMemoryCopyTests.cpp - Inline C memory semantics -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"

namespace {
using namespace neverd;

void compileAndRun(const std::string &Source,
                   llvm::StringRef Optimization = "-O2",
                   const std::string &ReferenceIR = {},
                   bool CheckUndefinedBehavior = false) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  llvm::SmallString<128> ReferencePath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-c-memory-copy", "ll",
                                                  ReferencePath));
  llvm::FileRemover RemoveReference(ReferencePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-c-memory-copy", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-c-memory-copy", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-c-memory-copy", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  if (!ReferenceIR.empty()) {
    llvm::raw_fd_ostream OS(ReferencePath, EC);
    ASSERT_FALSE(EC);
    OS << ReferenceIR;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  llvm::SmallVector<llvm::StringRef, 12> Arguments{Compiler,
                                                   "-std=c11",
                                                   Optimization,
                                                   "-Werror=uninitialized",
                                                   "-Werror=return-type",
                                                   SourcePath,
                                                   "-o",
                                                   BinaryPath};
  if (!ReferenceIR.empty())
    Arguments.push_back(ReferencePath);
  if (CheckUndefinedBehavior) {
    Arguments.push_back("-fsanitize=undefined");
    Arguments.push_back("-fsanitize-trap=all");
    // Plain pointer casts read scalars at any address, as the targets do.
    Arguments.push_back("-fno-sanitize=alignment");
  }
  // Decompiled C reads and writes scalars through plain pointer casts and is
  // built as it documents: without strict aliasing.
  Arguments.push_back("-fno-strict-aliasing");
  std::string Error;
  int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                         Redirects, 30, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Result, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "")
                       << '\n'
                       << Source;
  Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                     Redirects, 30, 0, &Error);
  ASSERT_EQ(Result, 0) << Error << '\n' << Source;
}

ExprPtr param(unsigned Id, TypeRef Type) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Id;
  V.Size = Type->Size;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, Type);
}

HighStmt ret(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

ExprPtr cast(ExprPtr Value, TypeRef Type) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Cast;
  E->Type = E->CastTo = Type;
  E->Operands = {std::move(Value)};
  return E;
}

TEST(CMemoryCopy, HighCExecutesCopiesAtTheirOriginalEvaluationPoint) {
  const auto U64 = NdType::makeInt(8, false);
  const auto I64 = NdType::makeInt(8, true);
  const auto U8 = NdType::makeInt(1, false);
  const auto I8 = NdType::makeInt(1, true);
  std::vector<HighFunc> Functions;
  auto Function = [&](const char *Name, TypeRef Result,
                      std::vector<HighParam> Params) -> HighFunc & {
    Functions.emplace_back();
    auto &F = Functions.back();
    F.Name = Name;
    F.ReturnType = std::move(Result);
    F.Params = std::move(Params);
    return F;
  };
  Function("read_signed", I64, {{"arg0", U64}}).Body = {
      ret(HighExpr::makeLoad(param(0, U64), I8))};
  auto Pair =
      HighExpr::makeBinop(NdOp::INT_ADD, HighExpr::makeLoad(param(0, U64), U64),
                          HighExpr::makeLoad(param(1, U64), U64));
  Pair->Type = U64;
  Function("read_pair", U64, {{"arg0", U64}, {"arg1", U64}}).Body = {ret(Pair)};
  auto &Guarded = Function("guarded", U64, {{"arg0", U64}, {"arg1", U64}});
  auto Select = std::make_shared<HighExpr>();
  Select->Kind = ExprKind::BinOp;
  Select->Op = NdOp::SELECT;
  Select->Type = U64;
  Select->Operands = {param(0, U64), HighExpr::makeLoad(param(1, U64), U64),
                      HighExpr::makeConst(17, 8)};
  Guarded.Body = {ret(Select)};
  HighStmt If;
  If.Kind = StmtKind::If;
  If.Cond = HighExpr::makeBinop(NdOp::BOOL_AND, param(0, U64),
                                HighExpr::makeLoad(param(1, U64), U8));
  If.Body = {ret(HighExpr::makeConst(1, 8))};
  Function("short_circuit", U64, {{"arg0", U64}, {"arg1", U64}}).Body = {
      If, ret(HighExpr::makeConst(0, 8))};
  auto StoreExpr = std::make_shared<HighExpr>();
  StoreExpr->Kind = ExprKind::Store;
  StoreExpr->Type = U8;
  StoreExpr->Operands = {param(0, U64), cast(param(1, U64), U8)};
  Function("store_result", U64, {{"arg0", U64}, {"arg1", U64}}).Body = {
      ret(StoreExpr)};
  HighStmt Loop;
  Loop.Kind = StmtKind::While;
  Loop.Cond = HighExpr::makeLoad(param(0, U64), U8);
  HighStmt Decrement;
  Decrement.Kind = StmtKind::Store;
  Decrement.StoreAddr = param(0, U64);
  Decrement.StoreVal = cast(
      HighExpr::makeBinop(NdOp::INT_SUB, HighExpr::makeLoad(param(0, U64), U8),
                          HighExpr::makeConst(1, 1)),
      U8);
  Loop.Body = {Decrement};
  Function("drain", U64, {{"arg0", U64}}).Body = {
      Loop, ret(HighExpr::makeLoad(param(0, U64), U8))};
  // Assignment from a narrow load must still sign extend, rather than copying
  // one byte directly into an eight-byte destination.
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = 18;
  V.Size = 8;
  V.TheArch = Arch::X64;
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = HighExpr::makeVar(V, I64);
  Assign.Val = HighExpr::makeLoad(param(0, U64), I8);
  Function("widen_local", I64, {{"arg0", U64}}).Body = {Assign,
                                                        ret(Assign.Dst)};

  V.Id = 19;
  Assign.Dst = HighExpr::makeVar(V, U64);
  Assign.Val = HighExpr::makeLoad(param(0, U64), U64);
  Function("load_local", U64, {{"arg0", U64}}).Body = {Assign, ret(Assign.Dst)};
  HighStmt Init = Assign;
  Init.Val = HighExpr::makeConst(0x12345678, 8);
  auto AddressOf = std::make_shared<HighExpr>();
  AddressOf->Kind = ExprKind::Addr;
  AddressOf->Type = U64;
  AddressOf->Operands = {Assign.Dst};
  Assign.Val = HighExpr::makeLoad(AddressOf, U64);
  Function("self_load", U64, {}).Body = {Init, Assign, ret(Assign.Dst)};

  auto Call = [&](const char *Name) {
    auto E = HighExpr::makeCall(Name, 0, {});
    E->Type = U64;
    SourceCallTypeHint Hint;
    Hint.TargetName = Name;
    Hint.Signature.ReturnType = U64;
    E->SourceCallHint = std::make_shared<const SourceCallTypeHint>(Hint);
    return E;
  };
  auto AddressCall = Call("next_address");
  auto ValueCall = Call("next_value");
  auto EffectStore = std::make_shared<HighExpr>();
  EffectStore->Kind = ExprKind::Store;
  EffectStore->Type = U8;
  EffectStore->Operands = {AddressCall, cast(ValueCall, U8)};
  Function("effect_store", U64, {}).Body = {ret(EffectStore)};
  Function("collision", U64, {{"memory_value", U64}}).Body = {
      ret(HighExpr::makeLoad(param(0, U64), U64))};

  std::string Checks;
  for (uint16_t Bytes : {1, 2, 4, 8, 16}) {
    const auto T = NdType::makeInt(Bytes, false);
    const auto Name = "copy" + std::to_string(Bytes);
    auto &F = Function(Name.c_str(), NdType::makeVoid(),
                       {{"arg0", U64}, {"arg1", U64}});
    HighStmt Copy;
    Copy.Kind = StmtKind::Store;
    Copy.StoreAddr = param(0, U64);
    Copy.StoreVal = HighExpr::makeLoad(param(1, U64), T);
    F.Body = {Copy};
    Checks += "for (unsigned off = 0; off != 8; ++off) {\n"
              "  unsigned char src[32], dst[32];\n"
              "  for (unsigned i = 0; i != 32; ++i) src[i] = i * 17 + 3;\n"
              "  memset(dst, 0xa5, sizeof(dst));\n" +
              Name +
              "((uintptr_t)(dst + off), (uintptr_t)(src + off));\n"
              "  if (memcmp(src + off, dst + off, " +
              std::to_string(Bytes) +
              ")) return 10;\n"
              "  for (unsigned i = 0; i != 32; ++i)\n"
              "    if ((i < off || i >= off + " +
              std::to_string(Bytes) + ") && dst[i] != 0xa5) return 11;\n}\n";
  }
  const std::string Harness = R"(
#include <string.h>
static unsigned address_calls, value_calls;
static unsigned char effect_byte;
uint64_t next_address(void) { ++address_calls; return (uintptr_t)&effect_byte; }
uint64_t next_value(void) { ++value_calls; return 0x123; }
int main(void) {
  if (effect_store() != 0x23 || effect_byte != 0x23 || address_calls != 1 || value_calls != 1) return 8;
  unsigned char bytes[32] = {0};
  bytes[1] = 0x81;
  if (read_signed((uintptr_t)(bytes + 1)) != -127) return 1;
  if (widen_local((uintptr_t)(bytes + 1)) != -127) return 2;
  if (guarded(0, 0) != 17 || short_circuit(0, 0) != 0) return 3;
  uint64_t a = 0xf123456789abcdefULL, b = 0x135792468abcdef0ULL;
  memcpy(bytes + 1, &a, 8); memcpy(bytes + 11, &b, 8);
  if (read_pair((uintptr_t)(bytes + 1), (uintptr_t)(bytes + 11)) != a + b) return 4;
  if (load_local((uintptr_t)(bytes + 1)) != a || self_load() != 0x12345678) return 9;
  if (guarded(1, (uintptr_t)(bytes + 1)) != a || collision((uintptr_t)(bytes + 1)) != a) return 5;
  if (store_result((uintptr_t)(bytes + 2), 0x123) != 0x23 || bytes[2] != 0x23) return 6;
  bytes[1] = 17;
  if (drain((uintptr_t)(bytes + 1)) != 0 || bytes[1] != 0) return 7;
)" + Checks + "return 0;\n}\n";
  for (bool Unaligned : {false, true}) {
    CEmitterOptions Opts;
    Opts.TheArch = Arch::X64;
    Opts.UseUnalignedPointers = Unaligned;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Opts));
    EXPECT_EQ(Source.find("neverd_mem_load_"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("neverd_mem_store_"), std::string::npos) << Source;
    if (!Unaligned) {
      EXPECT_NE(Source.find("__builtin_memcpy("), std::string::npos) << Source;
      EXPECT_NE(Source.find("__builtin_memcpy(&t19,"), std::string::npos)
          << Source;
      const auto Self = Source.substr(Source.find("uint64_t self_load("));
      EXPECT_NE(Self.find("__builtin_memcpy(&memory_value,"), std::string::npos)
          << Self;
      EXPECT_EQ(Self.substr(0, Self.find("\n}")).find("__builtin_memcpy(&t19,"),
                std::string::npos)
          << Self;
    }
    for (llvm::StringRef Optimization : {"-O0", "-O2"})
      compileAndRun(Source + Harness, Optimization, {}, true);
  }
}

TEST(CMemoryCopy, LLVMCAlignedRawPointersKeepByteAliasing) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
target datalayout = "e-p:64:64-i64:64-i128:128"
define i64 @mixed(i64 %address, i64 %bits, float %real) {
  %p = inttoptr i64 %address to ptr
  store i64 %bits, ptr %p, align 8
  store float %real, ptr %p, align 4
  %result = load i64, ptr %p, align 8
  ret i64 %result
}
define i64 @unaligned(i64 %address) {
  %p = inttoptr i64 %address to ptr
  %result = load i64, ptr %p, align 1
  ret i64 %result
}
define i64 @guarded(i64 %enabled, i64 %address) {
  %yes = icmp ne i64 %enabled, 0
  br i1 %yes, label %read, label %skip
read:
  %p = inttoptr i64 %address to ptr
  %result = load i64, ptr %p, align 1
  ret i64 %result
skip:
  ret i64 17
}
define i64 @pointer_copy(i64 %address, ptr %value) {
  %p = inttoptr i64 %address to ptr
  store ptr %value, ptr %p, align 1
  %result = load ptr, ptr %p, align 1
  %bits = ptrtoint ptr %result to i64
  ret i64 %bits
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module);
  const std::string Harness = R"(
#include <string.h>
int main(void) {
  _Alignas(16) unsigned char bytes[32] = {0};
  uint64_t bits = 0x9876543212345678ULL, expected = bits;
  float real = 3.25f;
  memcpy(&expected, &real, sizeof(real));
  if (mixed((uintptr_t)bytes, bits, real) != expected) return 1;
  if (memcmp(bytes, &expected, 8)) return 2;
  memcpy(bytes + 1, &bits, 8);
  if (unaligned((uintptr_t)(bytes + 1)) != bits) return 3;
  if (guarded(0, 0) != 17 || guarded(1, (uintptr_t)(bytes + 1)) != bits) return 4;
  if (pointer_copy((uintptr_t)(bytes + 1), bytes + 19) != (uintptr_t)(bytes + 19)) return 5;
  return 0;
}
)";
  for (bool Unaligned : {false, true}) {
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Opts;
    Opts.TheArch = Arch::X64;
    Opts.UseUnalignedPointers = Unaligned;
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Opts));
    EXPECT_EQ(Source.find("neverd_mem_load_"), std::string::npos);
    EXPECT_EQ(Source.find("neverd_mem_store_"), std::string::npos);
    if (!Unaligned)
      EXPECT_NE(Source.find("__builtin_memcpy("), std::string::npos) << Source;
    for (llvm::StringRef Optimization : {"-O0", "-O2"})
      compileAndRun(Source + Harness, Optimization, {}, true);
  }
}

TEST(CMemoryCopy, LLVMCGlobalAccessesUseObjectStorage) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
target datalayout = "e-p:64:64-i64:64-i128:128"
@bits = global i64 0, align 8
@real = global float 0.0, align 4
@stored_pointer = global ptr null, align 8
@bytes = global [16 x i8] zeroinitializer, align 8
define void @write_bits(i64 %value) {
  store i64 %value, ptr @bits, align 8
  ret void
}
define i64 @read_bits() {
  %value = load i64, ptr @bits, align 8
  ret i64 %value
}
define void @write_real(float %value) {
  store float %value, ptr @real, align 4
  ret void
}
define float @read_real() {
  %value = load float, ptr @real, align 4
  ret float %value
}
define void @write_punned(float %value) {
  store float %value, ptr @bits, align 4
  ret void
}
define float @read_punned() {
  %value = load float, ptr @bits, align 4
  ret float %value
}
define void @write_pointer(ptr %value) {
  store ptr %value, ptr @stored_pointer, align 8
  ret void
}
define ptr @read_pointer() {
  %value = load ptr, ptr @stored_pointer, align 8
  ret ptr %value
}
define void @write_offset(i32 %value) {
  %address = getelementptr [16 x i8], ptr @bytes, i64 0, i64 3
  store i32 %value, ptr %address, align 1
  ret void
}
define i32 @read_offset() {
  %address = getelementptr [16 x i8], ptr @bytes, i64 0, i64 3
  %value = load i32, ptr %address, align 1
  ret i32 %value
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module);
  const std::string Harness = R"(
#include <string.h>
int main(void) {
  uint64_t expected = UINT64_C(0x9876543212345678);
  float expected_real = 3.25f;
  write_bits(expected);
  if (read_bits() != expected) return 1;
  write_real(expected_real);
  if (read_real() != expected_real) return 2;
  write_punned(expected_real);
  memcpy(&expected, &expected_real, sizeof(expected_real));
  if (read_bits() != expected || read_punned() != expected_real) return 3;
  write_pointer(&expected);
  if (read_pointer() != &expected) return 4;
  write_offset(0x12345678);
  if (read_offset() != 0x12345678) return 5;
  return 0;
}
)";
  for (bool Unaligned : {false, true}) {
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    Options.UseUnalignedPointers = Unaligned;
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
    for (llvm::StringRef Optimization : {"-O0", "-O2"})
      compileAndRun(Source + Harness, Optimization, {}, true);
  }
}

TEST(CMemoryCopy, LLVMCReadonlyImageLoadKeepsItsConstantProjection) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
target datalayout = "e-p:64:64"
define i64 @read_image() {
  %result = load i64, ptr inttoptr (i64 5368721408 to ptr), align 8
  ret i64 %result
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Segment Data;
  Data.Name = ".rdata";
  Data.VA = 0x140003000;
  Data.Size = 8;
  Data.Flags = SegmentFlags::Readable;
  Data.Data = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
  Image.Segments.push_back(std::move(Data));
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options, nullptr, &Image));
  EXPECT_EQ(Source.find("__builtin_memcpy("), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + "\nint main(void) { return read_image() != "
                           "UINT64_C(0x1122334455667788); }\n",
                  Optimization, {}, true);
}

} // namespace
