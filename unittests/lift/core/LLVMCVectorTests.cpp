//===- LLVMCVectorTests.cpp - Executable LLVM C vector semantics ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/LLVMC/LLVMCWriter.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicsAArch64.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <stdexcept>

namespace {

void compileAndRun(const std::string &Source,
                   const std::string &RequestedCompiler = {}) {
#ifdef NEVERD_TEST_CLANG
  std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  std::string Compiler = *Program;
#endif
  if (!RequestedCompiler.empty())
    Compiler = RequestedCompiler;
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-vector", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-vector", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-vector", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  const llvm::StringRef SanitizerTrap =
      RequestedCompiler.empty() ? "-fsanitize-trap=all"
                                : "-fsanitize-undefined-trap-on-error";
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler, "-std=c11", Optimization, "-Werror=uninitialized",
        "-Werror=return-type", "-Werror=unused-but-set-variable",
        "-fsanitize=undefined",
        // Decompiled C is built as its prelude says: for a target with
        // unaligned access, without strict aliasing.
        "-fno-sanitize=alignment", "-fno-strict-aliasing", SanitizerTrap,
        SourcePath, "-o", BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 30, 0, &Error);
    Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
  }
}

std::string floatingVectorConstantSource(bool Double) {
  const uint64_t FloatBits[] = {0,          0x80000000, 0x7FC12345, 0x7F812345,
                                0xFFC54321, 0xFF854321, 1,          0x7F7FFFFF};
  const uint64_t DoubleBits[] = {0,
                                 0x8000000000000000ULL,
                                 0x7FF8123456789ABCULL,
                                 0x7FF0123456789ABCULL,
                                 0xFFF8ABCDEF012345ULL,
                                 0xFFF0ABCDEF012345ULL,
                                 1,
                                 0x7FEFFFFFFFFFFFFFULL};
  llvm::LLVMContext Context;
  neverd::CEmitterOptions Options;
  std::string Unused;
  llvm::raw_string_ostream Out(Unused);
  neverd::LLVMCWriter Writer(Out, Options, nullptr);
  const uint64_t *Patterns = Double ? DoubleBits : FloatBits;
  const unsigned Width = Double ? 64 : 32;
  const unsigned Lanes = Double ? 2 : 4;
  const std::string Scalar = Double ? "uint64_t" : "uint32_t";
  std::string Source =
      "#include <stdint.h>\n#include <string.h>\nint main(void) {\n";
  for (unsigned First = 0; First < 8; First += Lanes) {
    llvm::SmallVector<llvm::Constant *> Elements;
    for (unsigned Lane = 0; Lane < Lanes; ++Lane)
      Elements.push_back(llvm::ConstantFP::get(
          Context, llvm::APFloat(Double ? llvm::APFloat::IEEEdouble()
                                        : llvm::APFloat::IEEEsingle(),
                                 llvm::APInt(Width, Patterns[First + Lane]))));
    auto *Vector = llvm::ConstantVector::get(Elements);
    Source += "  { " + neverd::typeToCLLVM(Vector->getType()) +
              " value = " + Writer.constStr(Vector) + ";\n    " + Scalar +
              " actual[" + std::to_string(Lanes) + "];\n    const " + Scalar +
              " expected[] = {";
    for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
      if (Lane)
        Source += ", ";
      Source += "0x" + llvm::utohexstr(Patterns[First + Lane]) + "ULL";
    }
    Source += "};\n    memcpy(actual, &value, sizeof(actual));\n"
              "    if (memcmp(actual, expected, sizeof(actual))) return " +
              std::to_string(First + 1) + ";\n  }\n";
  }
  return Source + "  return 0;\n}\n";
}

// A block address is a native control destination, not a null data pointer.
// Portable C needs a separately proved continuation model before publishing it.
TEST(LLVMCValues, NativeBlockAddressesCannotBecomeZeroConstants) {
  struct Profile {
    const char *Triple;
    const char *Layout;
  };
  const Profile Profiles[] = {{"i686-pc-windows-msvc", "e-p:32:32"},
                              {"x86_64-pc-windows-msvc", "e-p:64:64"},
                              {"armv7-unknown-linux-gnueabihf", "e-p:32:32"},
                              {"aarch64-unknown-linux-gnu", "e-p:64:64"},
                              {"aarch64-apple-darwin", "e-p:64:64"},
                              {"powerpc64-unknown-linux-gnu", "E-p:64:64"}};
  for (const auto &Profile : Profiles)
    for (unsigned Form = 0; Form < 6; ++Form)
      for (bool Selected : {false, true}) {
        SCOPED_TRACE(Profile.Triple);
        SCOPED_TRACE(Form);
        SCOPED_TRACE(Selected);
        llvm::LLVMContext Context;
        llvm::Module Module("native-label-projection", Context);
        Module.setTargetTriple(llvm::Triple(Profile.Triple));
        Module.setDataLayout(Profile.Layout);
        llvm::IRBuilder<> B(Context);
        auto *Word = B.getIntNTy(Module.getDataLayout().getPointerSizeInBits());
        auto *Provider = llvm::Function::Create(
            llvm::FunctionType::get(B.getInt32Ty(), {}, false),
            llvm::GlobalValue::ExternalLinkage, "label_provider", Module);
        auto *Entry = llvm::BasicBlock::Create(Context, "entry", Provider);
        auto *Target =
            llvm::BasicBlock::Create(Context, "continuation", Provider);
        B.SetInsertPoint(Entry);
        B.CreateBr(Target);
        B.SetInsertPoint(Target);
        B.CreateRet(B.getInt32(7));
        auto *Address = llvm::BlockAddress::get(Provider, Target);
        auto *Bits = llvm::ConstantExpr::getPtrToInt(Address, Word);
        auto *Result = Form == 2 || Form == 3 ? static_cast<llvm::Type *>(Word)
                                              : B.getPtrTy();
        auto *Function = llvm::Function::Create(
            llvm::FunctionType::get(Result, {}, false),
            llvm::GlobalValue::ExternalLinkage, "publish_label", Module);
        B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
        if (Form == 0) {
          // A function can expose its own non-entry destination too.
          auto *Own = llvm::BasicBlock::Create(Context, "own_target", Function);
          B.CreateBr(Own);
          B.SetInsertPoint(Own);
          B.CreateRet(llvm::BlockAddress::get(Function, Own));
        } else if (Form == 1) {
          B.CreateRet(Address);
        } else if (Form == 2) {
          B.CreateRet(Bits);
        } else if (Form == 3) {
          B.CreateRet(llvm::ConstantExpr::getAdd(
              Bits, llvm::ConstantInt::get(Word, 16)));
        } else if (Form == 4) {
          auto *Record = llvm::StructType::get(B.getPtrTy(), Word);
          auto *Array = llvm::ArrayType::get(Record, 1);
          auto *Initializer = llvm::ConstantArray::get(
              Array, {llvm::ConstantStruct::get(Record, {Address, Bits})});
          auto *Table = new llvm::GlobalVariable(
              Module, Array, false, llvm::GlobalValue::InternalLinkage,
              Initializer, "continuation_table");
          auto *Element = B.CreateInBoundsGEP(
              Array, Table, {B.getInt32(0), B.getInt32(0), B.getInt32(0)});
          auto *Load = B.CreateLoad(B.getPtrTy(), Element);
          Load->setVolatile(true);
          B.CreateRet(Load);
        } else {
          auto *Observer = llvm::Function::Create(
              llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false),
              llvm::GlobalValue::ExternalLinkage, "consume_label", Module);
          B.CreateCall(Observer, {Address});
          B.CreateRet(llvm::ConstantPointerNull::get(B.getPtrTy()));
        }
        ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
        std::string Before, After, Source;
        llvm::raw_string_ostream BeforeOut(Before), AfterOut(After),
            Out(Source);
        Module.print(BeforeOut, nullptr);
        try {
          (void)neverd::LLVMCEmitter().emit(Module, Out, {}, nullptr, nullptr,
                                            Selected ? Function : nullptr);
          FAIL() << "native control address was published as C: " << Source;
        } catch (const std::runtime_error &Error) {
          EXPECT_NE(std::string(Error.what()).find("blockaddress"),
                    std::string::npos)
              << Error.what();
        }
        Module.print(AfterOut, nullptr);
        EXPECT_EQ(Before, After);
      }
}

TEST(LLVMCValues, UnsupportedSymbolConstantsCannotBecomeNullAddresses) {
  llvm::LLVMContext Context;
  llvm::Module Module("unsupported-symbol-constant", Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {}, false),
      llvm::GlobalValue::ExternalLinkage, "external_provider", Module);
  neverd::CEmitterOptions Options;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::LLVMCWriter Writer(Out, Options, nullptr);
  EXPECT_THROW(Writer.constStr(llvm::DSOLocalEquivalent::get(Function)),
               std::runtime_error);
  EXPECT_THROW(Writer.constStr(llvm::NoCFIValue::get(Function)),
               std::runtime_error);
}

TEST(LLVMCValues, FloatingVectorConstantsPreserveExactBitsInC) {
  for (bool Double : {false, true})
    compileAndRun(floatingVectorConstantSource(Double));
}

TEST(LLVMCValues, FloatingVectorConstantsPreserveExactBitsWithGCC) {
  auto GCC = llvm::sys::findProgramByName("gcc");
  if (!GCC)
    GTEST_SKIP() << "GCC is unavailable for the additional C11 compiler check";
  for (bool Double : {false, true})
    compileAndRun(floatingVectorConstantSource(Double), *GCC);
}

TEST(LLVMCValues, IntegerVectorsAreScalarizedWithoutMutatingTheInputModule) {
  llvm::LLVMContext Context;
  llvm::Module Module("integer-vector-projection", Context);
  Module.setDataLayout("e-p:64:64");
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Vector = llvm::FixedVectorType::get(llvm::Type::getInt8Ty(Context), 4);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I64, {I64}, false),
      llvm::GlobalValue::ExternalLinkage, "packed", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Lanes = llvm::PoisonValue::get(Vector);
  for (unsigned Lane = 0; Lane < 4; ++Lane)
    Lanes = Builder.CreateInsertElement(
        Lanes,
        Builder.CreateTrunc(Builder.CreateLShr(Function->getArg(0), Lane * 8),
                            Builder.getInt8Ty()),
        Lane);
  auto *Mask =
      Builder.CreateICmpNE(Lanes, llvm::Constant::getNullValue(Vector));
  Builder.CreateRet(Builder.CreateOr(
      Builder.CreateZExt(Builder.CreateBitCast(Mask, Builder.getIntNTy(4)),
                         I64),
      Builder.CreateZExt(Builder.CreateOrReduce(Lanes), I64)));

  std::string Before, After, Source;
  llvm::raw_string_ostream BeforeOut(Before), AfterOut(After), Out(Source);
  Module.print(BeforeOut, nullptr);
  ASSERT_TRUE(
      neverd::LLVMCEmitter().emit(Module, Out, {}, nullptr, nullptr, Function));
  Module.print(AfterOut, nullptr);
  EXPECT_EQ(Before, After);
  compileAndRun(Source + R"(
static uint64_t expected(uint64_t value) {
  uint64_t mask = 0, reduced = 0;
  for (unsigned lane = 0; lane < 4; ++lane) {
    uint64_t byte = (value >> (lane * 8)) & 0xff;
    if (byte) mask |= UINT64_C(1) << lane;
    reduced |= byte;
  }
  return mask | reduced;
}
int main(void) {
  const uint64_t values[] = {
      0, 1, UINT64_C(0x01020304), UINT64_C(0xff000000),
      UINT64_C(0x1020304050607080), UINT64_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    if (packed(values[i]) != expected(values[i])) return (int)i + 1;
  return 0;
}
)");
}

TEST(LLVMCValues, FloatingVectorPackingPreservesBitsAndSourceLaneOrder) {
  for (bool BigEndian : {false, true}) {
    for (bool Double : {false, true}) {
      SCOPED_TRACE(BigEndian ? "big endian" : "little endian");
      SCOPED_TRACE(Double ? "double" : "float");
      llvm::LLVMContext Context;
      llvm::Module Module("floating-vector-packing", Context);
      Module.setDataLayout(BigEndian ? "E-p:64:64" : "e-p:64:64");
      auto *Float = Double ? llvm::Type::getDoubleTy(Context)
                           : llvm::Type::getFloatTy(Context);
      const unsigned Bits = Double ? 64 : 32;
      auto *Integer = llvm::IntegerType::get(Context, Bits);
      auto *Packed = llvm::IntegerType::get(Context, Bits * 2);
      auto *Vector = llvm::FixedVectorType::get(Float, 2);
      auto *Pack = llvm::Function::Create(
          llvm::FunctionType::get(Packed, {Integer, Integer}, false),
          llvm::GlobalValue::ExternalLinkage, "pack_lanes", Module);
      llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Pack));
      llvm::Value *Lanes = llvm::PoisonValue::get(Vector);
      for (unsigned Lane = 0; Lane < 2; ++Lane)
        Lanes = B.CreateInsertElement(
            Lanes, B.CreateBitCast(Pack->getArg(Lane), Float), Lane);
      B.CreateRet(B.CreateBitCast(Lanes, Packed));
      auto *First = llvm::Function::Create(
          llvm::FunctionType::get(Integer, {Packed}, false),
          llvm::GlobalValue::ExternalLinkage, "first_lane", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", First));
      B.CreateRet(B.CreateBitCast(
          B.CreateExtractElement(B.CreateBitCast(First->getArg(0), Vector),
                                 uint64_t(0)),
          Integer));
      // Unlike scalar packing, a change of vector lane width introduces new
      // scalar/vector casts inside Scalarizer itself.
      auto *IntegerVector = llvm::FixedVectorType::get(Packed, 2);
      auto *FirstWord = llvm::Function::Create(
          llvm::FunctionType::get(Integer, {IntegerVector}, false),
          llvm::GlobalValue::ExternalLinkage, "first_word", Module);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", FirstWord));
      B.CreateRet(B.CreateBitCast(
          B.CreateExtractElement(
              B.CreateBitCast(FirstWord->getArg(0),
                              llvm::FixedVectorType::get(Float, 4)),
              uint64_t(0)),
          Integer));
      // 128-bit vector elements have no supported C boundary representation.
      // The 32-bit float case exercises the supported <2 x i64> interface.
      if (Double)
        FirstWord->eraseFromParent();
      ASSERT_FALSE(llvm::verifyModule(Module));
      std::string Before, After, Source;
      llvm::raw_string_ostream BeforeOut(Before), AfterOut(After), Out(Source);
      Module.print(BeforeOut, nullptr);
      ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
      Module.print(AfterOut, nullptr);
      EXPECT_EQ(Before, After);
      const std::string LaneType = Double ? "uint64_t" : "uint32_t";
      const std::string PackedType = Double ? "__uint128_t" : "uint64_t";
      const std::string Values =
          Double
              ? "0, UINT64_C(0x8000000000000000), "
                "UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8123456789abc), "
                "UINT64_C(0xfff0000000000001), UINT64_MAX"
              : "0, UINT32_C(0x80000000), UINT32_C(0x7f800000), "
                "UINT32_C(0x7fc12345), UINT32_C(0xff800001), UINT32_MAX";
      std::string Driver =
          "int main(void) {\n  const " + LaneType + " values[] = {" + Values +
          "};\n  for (unsigned i = 0; i < 6; ++i) {\n" + "    " + LaneType +
          " a = values[i], b = values[5 - i];\n" + "    " + PackedType +
          " expected = ((" + PackedType + ")" + (BigEndian ? "a" : "b") +
          " << " + std::to_string(Bits) + ") | " + (BigEndian ? "b" : "a") +
          ";\n" +
          "    if (pack_lanes(a, b) != expected || first_lane(expected) != a) "
          "return i + 1;\n";
      if (!Double)
        Driver += "    uint64_t __attribute__((vector_size(16))) words = "
                  "{expected, ~expected};\n"
                  "    if (first_word(words) != a) return i + 10;\n";
      compileAndRun(Source + Driver + "  }\n  return 0;\n}\n");
    }
  }
}

TEST(LLVMCValues, UnalignedVectorMemoryPreservesLanesAndExactSpan) {
  llvm::LLVMContext Context;
  llvm::Module Module("vector-memory", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Vector = llvm::FixedVectorType::get(llvm::Type::getFloatTy(Context), 4);
  auto *Ptr = llvm::PointerType::getUnqual(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {Ptr, Ptr},
                              false),
      llvm::GlobalValue::ExternalLinkage, "add_lanes", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Input =
      B.CreateAlignedLoad(Vector, Function->getArg(0), llvm::Align(1));
  auto *Offsets =
      llvm::ConstantVector::get({llvm::ConstantFP::get(B.getFloatTy(), 1),
                                 llvm::ConstantFP::get(B.getFloatTy(), 2),
                                 llvm::ConstantFP::get(B.getFloatTy(), 4),
                                 llvm::ConstantFP::get(B.getFloatTy(), 8)});
  B.CreateAlignedStore(B.CreateFAdd(Input, Offsets), Function->getArg(1),
                       llvm::Align(1));
  B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
#include <string.h>
int main(void) {
  unsigned char input[20], output[24];
  const float values[] = {0.5f, -4.0f, 16.0f, -128.0f};
  const float expected[] = {1.5f, -2.0f, 20.0f, -120.0f};
  memset(input, 0xa5, sizeof(input));
  memset(output, 0x5a, sizeof(output));
  memcpy(input + 1, values, sizeof(values));
  add_lanes(input + 1, output + 3);
  if (memcmp(output + 3, expected, sizeof(expected))) return 1;
  for (unsigned i = 0; i < sizeof(output); ++i)
    if ((i < 3 || i >= 19) && output[i] != 0x5a) return 2;
  return memcmp(input + 1, values, sizeof(values)) != 0;
}
)");
}

TEST(LLVMCValues, NestedGEPsPreserveLayoutAndSignedIndicesInBothRenderPaths) {
  llvm::LLVMContext Context;
  llvm::Module Module("nested-gep-offsets", Context);
  Module.setDataLayout("e-p:64:64-i64:64");
  auto *I8 = llvm::Type::getInt8Ty(Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Record =
      llvm::StructType::get(Context, {I8, llvm::ArrayType::get(I32, 3), I64});
  auto *Rows = llvm::ArrayType::get(Record, 5);
  for (bool Materialized : {false, true}) {
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(
            I32, {llvm::PointerType::getUnqual(Context), I32, I64}, false),
        llvm::GlobalValue::ExternalLinkage,
        Materialized ? "nested_sum" : "nested_read", Module);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Pointer = B.CreateGEP(Rows, Function->getArg(0),
                                {B.getInt32(0), Function->getArg(1),
                                 B.getInt32(1), Function->getArg(2)});
    llvm::Value *Result = B.CreateAlignedLoad(I32, Pointer, llvm::Align(1));
    if (Materialized)
      Result = B.CreateAdd(Result,
                           B.CreateAlignedLoad(I32, Pointer, llvm::Align(1)));
    B.CreateRet(Result);
  }
  ASSERT_EQ(Module.getDataLayout().getTypeAllocSize(Record), 24u);
  ASSERT_FALSE(llvm::verifyModule(Module));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
#include <string.h>
int main(void) {
  unsigned char bytes[160];
  for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = i * 37 + 11;
  for (int32_t row = -1; row <= 1; ++row)
    for (int64_t column = -1; column <= 2; ++column) {
      uint32_t expected;
      memcpy(&expected, bytes + 73 + row * 24 + 4 + column * 4, sizeof(expected));
      if (nested_read(bytes + 73, row, column) != expected) return 1;
      if (nested_sum(bytes + 73, row, column) != expected + expected) return 2;
    }
  return 0;
}
)");
}

TEST(LLVMCValues, ConstantGEPKeepsNonzeroGlobalOffset) {
  llvm::LLVMContext Context;
  llvm::Module Module("constant-gep-offset", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Bytes = llvm::ConstantDataArray::getString(Context, "vector-offset");
  auto *Global = new llvm::GlobalVariable(Module, Bytes->getType(), true,
                                          llvm::GlobalValue::InternalLinkage,
                                          Bytes, "text_bytes");
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt8Ty(Context), {}, false),
      llvm::GlobalValue::ExternalLinkage, "offset_byte", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Constant *Indices[] = {B.getInt32(0), B.getInt32(7)};
  auto *Address =
      llvm::ConstantExpr::getGetElementPtr(Bytes->getType(), Global, Indices);
  B.CreateRet(B.CreateLoad(B.getInt8Ty(), Address));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + "int main(void) { return offset_byte() != 'o'; }\n");
}

TEST(LLVMCValues, UnsupportedGEPPointerLayoutsFailClosed) {
  llvm::LLVMContext Context;
  llvm::Module Module("unsupported-gep-index-width", Context);
  Module.setDataLayout("e-p:64:64:64:32");
  auto *Ptr = llvm::PointerType::getUnqual(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Ptr, {Ptr}, false),
      llvm::GlobalValue::ExternalLinkage, "unsupported_gep", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
  B.CreateRet(B.CreateGEP(B.getInt32Ty(), Function->getArg(0), B.getInt32(1)));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
               std::runtime_error);
}

TEST(LLVMCValues, UnalignedIntegerStorePreservesPointerAddressRepresentation) {
  llvm::LLVMContext Context;
  llvm::Module Module("integer-store-pointer-view", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(Context);
  auto *Ptr = B.getPtrTy();
  auto *Observe = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {Ptr, Ptr}, false),
      llvm::GlobalValue::ExternalLinkage, "observe_pointer", Module);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {Ptr}, false),
      llvm::GlobalValue::ExternalLinkage, "store_frame_address", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Frame =
      B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 96), nullptr, "frame");
  auto *End = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt64(72), "frame_end");
  auto *Base = B.CreatePtrToInt(End, B.getInt64Ty(), "rsp_init");
  auto *Bits = B.CreateAdd(Base, B.getInt64(-8));
  B.CreateAlignedStore(Bits, Function->getArg(0), llvm::Align(1));
  B.CreateCall(Observe, {Function->getArg(0), B.CreateIntToPtr(Bits, Ptr)});
  B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  // The checks read the portable byte-copy spelling.
  Options.UseUnalignedPointers = false;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  // The frame printer deliberately gives this integer IR value a pointer
  // expression. Capturing it for byte copying must keep the integer view.
  EXPECT_NE(Source.find("memory_value"), std::string::npos) << Source;
  compileAndRun(Source + R"(
#include <string.h>
static int mismatch;
void observe_pointer(void *storage, void *address) {
  uint64_t stored;
  memcpy(&stored, storage, sizeof(stored));
  mismatch = stored != (uint64_t)(uintptr_t)address;
}
int main(void) {
  unsigned char output[16];
  memset(output, 0x5a, sizeof(output));
  store_frame_address(output + 1);
  for (unsigned i = 0; i < sizeof(output); ++i)
    if ((i < 1 || i >= 9) && output[i] != 0x5a) return 2;
  return mismatch;
}
)");
}

TEST(LLVMCValues, MultiUsePointerCastKeepsOneMaterializedValue) {
  llvm::LLVMContext Context;
  llvm::Module Module("shared-memory-pointer", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(Context);
  auto *Word = B.getInt64Ty();
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {Word, Word, Word}, false),
      llvm::GlobalValue::ExternalLinkage, "store_words", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Address = B.CreateIntToPtr(Function->getArg(0), B.getPtrTy());
  B.CreateAlignedStore(Function->getArg(1), Address, llvm::Align(8));
  B.CreateAlignedStore(Function->getArg(2),
                       B.CreateGEP(Word, Address, B.getInt64(1)),
                       llvm::Align(8));
  B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
int main(void) {
  uint64_t output[] = {UINT64_C(0xa5), 0, 0, UINT64_C(0x5a)};
  store_words((uintptr_t)(output + 1), UINT64_MAX,
              UINT64_C(0x123456789abcdef0));
  return output[0] != UINT64_C(0xa5) || output[1] != UINT64_MAX ||
         output[2] != UINT64_C(0x123456789abcdef0) ||
         output[3] != UINT64_C(0x5a);
}
)");
}

TEST(LLVMCValues, VectorMemoryUsesCCarrierAlignmentWithWeakerSourceABI) {
  llvm::LLVMContext Context;
  llvm::Module Module("vector-memory-carrier-alignment", Context);
  Module.setDataLayout("e-p:64:64-i64:32");
  auto *Vector = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(Context), 2);
  auto *Ptr = llvm::PointerType::getUnqual(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), {Ptr, Ptr},
                              false),
      llvm::GlobalValue::ExternalLinkage, "add_words", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Input =
      B.CreateAlignedLoad(Vector, Function->getArg(0), llvm::Align(4));
  auto *Offsets = llvm::ConstantVector::get({B.getInt64(1), B.getInt64(3)});
  B.CreateAlignedStore(B.CreateAdd(Input, Offsets), Function->getArg(1),
                       llvm::Align(4));
  B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
#include <string.h>
int main(void) {
  _Alignas(16) unsigned char input[32], output[32];
  const uint64_t values[] = {UINT64_MAX, UINT64_C(0x123456789abcdef0)};
  const uint64_t expected[] = {0, UINT64_C(0x123456789abcdef3)};
  memset(input, 0xa5, sizeof(input));
  memset(output, 0x5a, sizeof(output));
  memcpy(input + 4, values, sizeof(values));
  add_words(input + 4, output + 4);
  if (memcmp(output + 4, expected, sizeof(expected))) return 1;
  for (unsigned i = 0; i < sizeof(output); ++i)
    if ((i < 4 || i >= 20) && output[i] != 0x5a) return 2;
  return 0;
}
)");
}

TEST(LLVMCValues, SelectedFunctionRejectsReferencedVectorGlobalsBeforeOutput) {
  for (bool Store : {false, true}) {
    for (bool ScalarAccess : {false, true}) {
      llvm::LLVMContext Context;
      llvm::Module Module("selected-vector-global", Context);
      Module.setDataLayout("e-p:64:64");
      auto *Integer = llvm::Type::getInt64Ty(Context);
      auto *Vector = llvm::FixedVectorType::get(Integer, 2);
      auto *Array = llvm::ArrayType::get(Vector, 2);
      auto *Global = new llvm::GlobalVariable(
          Module, Array, false, llvm::GlobalValue::InternalLinkage,
          llvm::Constant::getNullValue(Array), "vector_data");
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(Integer, {Integer}, false),
          llvm::GlobalValue::ExternalLinkage, "selected", Module);
      llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Address =
          B.CreateGEP(Array, Global, {B.getInt32(0), B.getInt32(1)});
      if (Store) {
        llvm::Value *Value = Function->getArg(0);
        if (!ScalarAccess)
          Value = B.CreateInsertElement(llvm::Constant::getNullValue(Vector),
                                        Value, uint64_t(0));
        B.CreateStore(Value, Address);
        B.CreateRet(Function->getArg(0));
      } else {
        auto *Type = ScalarAccess ? static_cast<llvm::Type *>(Integer) : Vector;
        llvm::Value *Value = B.CreateLoad(Type, Address);
        if (!ScalarAccess)
          Value = B.CreateExtractElement(Value, uint64_t(0));
        B.CreateRet(Value);
      }
      ASSERT_FALSE(llvm::verifyModule(Module));
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}, nullptr,
                                               nullptr, Function),
                   std::runtime_error);
      EXPECT_TRUE(Source.empty());
    }
  }
}

TEST(LLVMCValues, SelectedScalarFunctionIgnoresUnrelatedVectorGlobal) {
  llvm::LLVMContext Context;
  llvm::Module Module("unrelated-vector-global", Context);
  auto *Integer = llvm::Type::getInt64Ty(Context);
  auto *Vector = llvm::FixedVectorType::get(Integer, 2);
  new llvm::GlobalVariable(Module, Vector, false,
                           llvm::GlobalValue::InternalLinkage,
                           llvm::Constant::getNullValue(Vector), "unrelated");
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Integer, {Integer}, false),
      llvm::GlobalValue::ExternalLinkage, "selected", Module);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
  B.CreateRet(B.CreateAdd(Function->getArg(0), B.getInt64(1)));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(
      neverd::LLVMCEmitter().emit(Module, Out, {}, nullptr, nullptr, Function));
  EXPECT_EQ(Source.find("unrelated"), std::string::npos);
  compileAndRun(Source +
                "int main(void) { return selected(UINT64_MAX) != 0; }\n");
}

TEST(LLVMCValues, SurvivingVectorAllocationsFailClosedBeforeOutput) {
  for (bool Selected : {false, true}) {
    for (bool ScalarAccess : {false, true}) {
      llvm::LLVMContext Context;
      llvm::Module Module("vector-allocation", Context);
      auto *Integer = llvm::Type::getInt64Ty(Context);
      auto *Vector = llvm::FixedVectorType::get(Integer, 2);
      auto *Fill = llvm::Function::Create(
          llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                  {llvm::PointerType::getUnqual(Context)},
                                  false),
          llvm::GlobalValue::ExternalLinkage, "external_fill", Module);
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(Integer, {}, false),
          llvm::GlobalValue::ExternalLinkage, "vector_storage", Module);
      llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Storage = B.CreateAlloca(Vector);
      B.CreateCall(Fill, {Storage});
      auto *Type = ScalarAccess ? static_cast<llvm::Type *>(Integer) : Vector;
      llvm::Value *Value = B.CreateLoad(Type, Storage);
      if (!ScalarAccess)
        Value = B.CreateExtractElement(Value, uint64_t(0));
      B.CreateRet(Value);
      ASSERT_FALSE(llvm::verifyModule(Module));
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}, nullptr,
                                               nullptr,
                                               Selected ? Function : nullptr),
                   std::runtime_error);
      EXPECT_TRUE(Source.empty());
    }
  }
}

TEST(LLVMCValues, VolatileVectorMemoryStillFailsClosed) {
  for (bool Store : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module Module("volatile-vector-memory", Context);
    auto *Vector =
        llvm::FixedVectorType::get(llvm::Type::getInt32Ty(Context), 4);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                {llvm::PointerType::getUnqual(Context)}, false),
        llvm::GlobalValue::ExternalLinkage, "unsupported", Module);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
    if (Store)
      B.CreateStore(llvm::Constant::getNullValue(Vector), Function->getArg(0),
                    true);
    else
      B.CreateLoad(Vector, Function->getArg(0), true);
    B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(Module));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
                 std::runtime_error);
    EXPECT_TRUE(Source.empty());
  }
}

TEST(LLVMCValues, NativeVectorIntrinsicRequiresExactTargetAndSignature) {
  for (bool Malformed : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module Module("native-vector-intrinsic", Context);
    Module.setDataLayout("e-p:64:64");
    auto *Integer = llvm::Type::getInt128Ty(Context);
    auto *Accumulator =
        llvm::FixedVectorType::get(llvm::Type::getFloatTy(Context), 4);
    auto *Input =
        llvm::FixedVectorType::get(Malformed ? llvm::Type::getInt16Ty(Context)
                                             : llvm::Type::getBFloatTy(Context),
                                   8);
    auto *Intrinsic = llvm::Function::Create(
        llvm::FunctionType::get(Accumulator, {Accumulator, Input, Input},
                                false),
        llvm::GlobalValue::ExternalLinkage, "llvm.aarch64.neon.bfmmla", Module);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Integer, {Integer, Integer, Integer}, false),
        llvm::GlobalValue::ExternalLinkage, "native_matrix", Module);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Function));
    B.CreateRet(B.CreateBitCast(
        B.CreateCall(Intrinsic,
                     {B.CreateBitCast(Function->getArg(0), Accumulator),
                      B.CreateBitCast(Function->getArg(1), Input),
                      B.CreateBitCast(Function->getArg(2), Input)}),
        Integer));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    neverd::CEmitterOptions Options;
    Options.TheArch = Malformed ? neverd::Arch::AArch64 : neverd::Arch::X64;
    if (Malformed)
      EXPECT_FALSE(neverd::LLVMCEmitter().emit(Module, Out, Options));
    else {
      ASSERT_FALSE(llvm::verifyModule(Module));
      EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, Options),
                   std::runtime_error);
    }
    EXPECT_TRUE(Source.empty());
  }
}

TEST(LLVMCValues, UnsupportedVectorPackingIsRejected) {
  llvm::LLVMContext Context;
  llvm::Module Module("float-vector-packing", Context);
  Module.setDataLayout("e-p:64:64");
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Vector = llvm::FixedVectorType::get(llvm::Type::getFloatTy(Context), 2);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I64, {Vector}, false),
      llvm::GlobalValue::ExternalLinkage, "unsupported", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRet(Builder.CreateBitCast(Function->getArg(0), I64));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
               std::runtime_error);
}

TEST(LLVMCValues, IntegerVectorSignaturesAndCallsPreserveEveryLane) {
  llvm::LLVMContext Context;
  llvm::Module Module("integer-vector-boundary", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Vector = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(Context), 2);
  auto *Signature = llvm::FunctionType::get(Vector, {Vector}, false);
  auto *External = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "external_vector", Module);
  auto *Function = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "vector_boundary", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Called = Builder.CreateCall(External, {Function->getArg(0)});
  auto *Offsets =
      llvm::ConstantVector::get({Builder.getInt64(3), Builder.getInt64(7)});
  auto *Added = Builder.CreateAdd(Called, Offsets);
  Builder.CreateRet(Builder.CreateShuffleVector(Added, Added, {1, 0}));
  ASSERT_FALSE(llvm::verifyModule(Module));

  std::string Before, After, Source;
  llvm::raw_string_ostream BeforeOut(Before), AfterOut(After), Out(Source);
  Module.print(BeforeOut, nullptr);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  Module.print(AfterOut, nullptr);
  EXPECT_EQ(Before, After);
  const std::string Runtime = R"(
typedef uint64_t lanes __attribute__((vector_size(16)));
lanes external_vector(lanes value) {
  return (lanes){value[1] ^ UINT64_C(0xfedcba9876543210), value[0]};
}
int main(void) {
  const uint64_t values[] = {0, 1, UINT64_C(0x123456789abcdef0), UINT64_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    lanes input = {values[i], values[sizeof(values) / sizeof(values[0]) - i - 1]};
    lanes result = vector_boundary(input);
    if (result[0] != input[0] + 7 ||
        result[1] != (input[1] ^ UINT64_C(0xfedcba9876543210)) + 3)
      return (int)i + 1;
  }
  return 0;
}
)";
  compileAndRun(Source + Runtime);
  std::string Selected;
  llvm::raw_string_ostream SelectedOut(Selected);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, SelectedOut, {}, nullptr,
                                          nullptr, Function));
  compileAndRun(Selected + Runtime);
}

TEST(LLVMCValues, IndirectIntegerVectorCallPreservesSignatureAndLanes) {
  llvm::LLVMContext Context;
  llvm::Module Module("indirect-vector-boundary", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Vector = llvm::FixedVectorType::get(llvm::Type::getInt64Ty(Context), 2);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Signature = llvm::FunctionType::get(Vector, {Vector}, false);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Vector, {Pointer, Vector}, false),
      llvm::GlobalValue::ExternalLinkage, "indirect_vector", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Called =
      Builder.CreateCall(Signature, Function->getArg(0), {Function->getArg(1)});
  Builder.CreateRet(Builder.CreateShuffleVector(Called, Called, {1, 0}));
  ASSERT_FALSE(llvm::verifyModule(Module));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
typedef uint64_t lanes __attribute__((vector_size(16)));
lanes vector_callback(lanes value) {
  return (lanes){value[0] + 17, value[1] ^ UINT64_C(0xfedcba9876543210)};
}
int main(void) {
  const uint64_t values[] = {0, 1, UINT64_C(0x123456789abcdef0), UINT64_MAX};
  for (unsigned i = 0; i < 4; ++i) {
    lanes input = {values[i], values[3 - i]};
    lanes result = indirect_vector(vector_callback, input);
    if (result[0] != (input[1] ^ UINT64_C(0xfedcba9876543210)) ||
        result[1] != input[0] + 17)
      return i + 1;
  }
  return 0;
}
)");
}

TEST(LLVMCValues, SelectedIntegerVectorFunctionKeepsConstantsAndLaneUpdates) {
  llvm::LLVMContext Context;
  llvm::Module Module("selected-vector-boundary", Context);
  Module.setDataLayout("e-p:64:64");
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *Vector = llvm::FixedVectorType::get(I32, 4);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Vector, {I32}, false),
      llvm::GlobalValue::ExternalLinkage, "updated_vector", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Initial =
      llvm::ConstantVector::get({Builder.getInt32(11), Builder.getInt32(13),
                                 Builder.getInt32(17), Builder.getInt32(19)});
  Builder.CreateRet(
      Builder.CreateInsertElement(Initial, Function->getArg(0), 2));
  auto *Unrelated = llvm::FixedVectorType::get(Builder.getFloatTy(), 2);
  llvm::Function::Create(llvm::FunctionType::get(Unrelated, {}, false),
                         llvm::GlobalValue::ExternalLinkage, "unrelated",
                         Module);
  ASSERT_FALSE(llvm::verifyModule(Module));

  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(
      neverd::LLVMCEmitter().emit(Module, Out, {}, nullptr, nullptr, Function));
  compileAndRun(Source + R"(
int main(void) {
  uint32_t __attribute__((vector_size(16))) result = updated_vector(UINT32_MAX);
  return result[0] != 11 || result[1] != 13 ||
         result[2] != UINT32_MAX || result[3] != 19;
}
)");
}

TEST(LLVMCValues, UnsupportedVectorSignaturesStillFailClosed) {
  llvm::LLVMContext Context;
  llvm::Type *Types[] = {
      llvm::FixedVectorType::get(llvm::Type::getInt1Ty(Context), 8),
      llvm::FixedVectorType::get(llvm::Type::getInt64Ty(Context), 3),
      llvm::FixedVectorType::get(llvm::Type::getInt64Ty(Context), 16),
      llvm::ScalableVectorType::get(llvm::Type::getInt64Ty(Context), 2)};
  for (auto *Type : Types) {
    llvm::Module Module("unsupported-vector-boundary", Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(Type, {Type}, false),
        llvm::GlobalValue::ExternalLinkage, "unsupported", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Builder.CreateRet(Function->getArg(0));
    ASSERT_FALSE(llvm::verifyModule(Module));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
                 std::runtime_error);
  }
}

} // namespace
