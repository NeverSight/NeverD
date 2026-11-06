//===- X86_64_SourceAccuracyTests.cpp - CPU accuracy regressions ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../HostCompiler.h"
#include "LLVMHostFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace neverd;
constexpr va_t Entry = 0x1000;

struct CommandResult {
  int Status = -1;
  std::string Out;
  std::string Error;
};

class X86SourceAccuracy : public testing::Test {
protected:
  llvm::SmallString<128> Directory;
  std::string Compiler;
  std::vector<std::string> Files;

  void SetUp() override {
    auto Clang = llvm::sys::findProgramByName("clang");
    if (!Clang)
      GTEST_SKIP() << "clang is required for native and generated-code oracles";
    Compiler = *Clang;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-isa-regression",
                                                      Directory));
  }

  void TearDown() override {
    for (const auto &File : Files)
      (void)llvm::sys::fs::remove(File);
    if (!Directory.empty())
      (void)llvm::sys::fs::remove(Directory);
  }

  std::string file(llvm::StringRef Name) {
    llvm::SmallString<128> Path(Directory);
    llvm::sys::path::append(Path, Name);
    Files.push_back(Path.str().str());
    return Files.back();
  }

  static std::string contents(llvm::StringRef Path) {
    auto Buffer = llvm::MemoryBuffer::getFile(Path);
    return Buffer ? (*Buffer)->getBuffer().str() : "";
  }

  void write(llvm::StringRef Path, llvm::StringRef Text) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Out << Text;
  }

  // Optional retained evidence is useful while investigating a failing case.
  // Normal CTest runs use only the disposable directory above.
  void retain(llvm::StringRef Name, llvm::StringRef Text) {
    const char *Root = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR");
    if (!Root || !*Root)
      return;
    llvm::SmallString<128> Path(Root);
    llvm::sys::path::append(
        Path, testing::UnitTest::GetInstance()->current_test_info()->name());
    ASSERT_FALSE(llvm::sys::fs::create_directories(Path));
    llvm::sys::path::append(Path, Name);
    write(Path, Text);
  }

  CommandResult command(llvm::StringRef Program,
                        const std::vector<std::string> &Arguments) {
    const auto Stdout = file("stdout-" + std::to_string(Files.size()));
    const auto Stderr = file("stderr-" + std::to_string(Files.size()));
    llvm::SmallVector<llvm::StringRef, 16> Args{Program};
    for (const auto &Arg : Arguments)
      Args.push_back(Arg);
    const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Stdout,
                                                        Stderr};
    std::string Error;
    const int Status = llvm::sys::ExecuteAndWait(Program, Args, std::nullopt,
                                                 Redirects, 30, 0, &Error);
    return {Status, contents(Stdout), Error + contents(Stderr)};
  }

  static bool nativeX64() {
    return llvm::Triple(llvm::sys::getDefaultTargetTriple()).getArch() ==
           llvm::Triple::x86_64;
  }

  static BinaryFormat hostFormat() {
#if defined(_WIN32)
    return BinaryFormat::COFF;
#elif defined(__APPLE__)
    return BinaryFormat::MachO;
#else
    return BinaryFormat::ELF;
#endif
  }

  // The memory probes use the native first integer argument register. This
  // keeps the original byte fixture and recovered source ABI identical.
  static uint8_t memoryModRM() {
    return hostFormat() == BinaryFormat::COFF ? 0x01 : 0x07;
  }

  BinaryImage image(llvm::ArrayRef<uint8_t> Bytes,
                    BinaryFormat Format = hostFormat()) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = Format;
    Image.Base = Image.Entry = Entry;
    Segment Text;
    Text.Name = ".text";
    Text.VA = Entry;
    Text.Size = Text.FileSz = Bytes.size();
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.assign(Bytes.begin(), Bytes.end());
    Image.Segments.push_back(Text);
    Section Code;
    Code.Name = Text.Name;
    Code.VA = Entry;
    Code.Size = Code.FileSz = Text.Size;
    Code.Flags = Text.Flags;
    Code.Data = Text.Data;
    Image.Sections.push_back(Code);
    auto FunctionSymbol = Symbol::makeFunc(Entry, Bytes.size());
    FunctionSymbol.Name = "isa_probe";
    Image.Symbols.push_back(std::move(FunctionSymbol));
    return Image;
  }

  static std::string assembly(llvm::ArrayRef<uint8_t> Bytes) {
#if defined(__APPLE__)
    std::string Text = ".text\n.globl _isa_probe\n_isa_probe:\n.byte ";
#else
    std::string Text = ".text\n.globl isa_probe\nisa_probe:\n.byte ";
#endif
    for (unsigned I = 0; I < Bytes.size(); ++I) {
      if (I)
        Text += ',';
      Text += std::to_string(Bytes[I]);
    }
    return Text + '\n';
  }

  // Save and restore x87/SSE state before any C observer can use it. The
  // red-zone fixture also uses AVX in an isolated, feature-gated subprocess.
  static std::string driver(llvm::StringRef Check, bool IntegerReturn = false,
                            bool DeclareFunction = true,
                            bool PointerArgument = false) {
    std::string Text = R"(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
)";
    if (DeclareFunction)
      Text += IntegerReturn     ? "extern uint64_t isa_probe(void);\n"
              : PointerArgument ? "extern void isa_probe(void *);\n"
                                : "extern void isa_probe(uintptr_t);\n";
    Text += R"(
int main(void) {
  _Alignas(16) unsigned char saved[512];
  _Alignas(16) unsigned char output[2048] = {0};
  for (unsigned i = 1024; i < 1536; i += 8) {
    uint64_t seed = UINT64_C(0x3ff000003f800000);
    memcpy(output + i, &seed, sizeof(seed));
  }
  __asm__ volatile("fxsave64 %0" : "=m"(saved) : : "memory");
)";
    Text += IntegerReturn     ? "  uint64_t value = isa_probe();\n"
            : PointerArgument ? "  isa_probe(output);\n"
                              : "  isa_probe((uintptr_t)output);\n";
    Text +=
        "  __asm__ volatile(\"fxrstor64 %0\" : : \"m\"(saved) : \"memory\");\n";
    Text += Check.str();
    return Text + "\n}\n";
  }

  void compareLLVM(llvm::ArrayRef<uint8_t> Bytes, llvm::StringRef Check,
                   bool IntegerReturn = false, bool AllowContextRefusal = false,
                   BinaryFormat Format = hostFormat()) {
    if (!nativeX64())
      GTEST_SKIP() << "native x86_64 host required";
    const auto Original = file("original.s");
    const auto Harness = file("driver.c");
    const auto OriginalExecutable = file("original.exe");
    write(Original, assembly(Bytes));
    write(Harness, driver(Check, IntegerReturn));
    retain("original.s", contents(Original));
    retain("driver.c", contents(Harness));
    auto Compiled =
        command(Compiler, {"-O0", Original, Harness, "-o", OriginalExecutable});
    ASSERT_EQ(Compiled.Status, 0) << Compiled.Error;
    auto Native = command(OriginalExecutable, {});
    retain("original-run.txt", Native.Out + Native.Error +
                                   "\nexit=" + std::to_string(Native.Status));
    if (AllowContextRefusal && Native.Status == 77)
      GTEST_SKIP()
          << "current shadow-stack context does not exhibit NOP behavior";
    ASSERT_EQ(Native.Status, 0) << Native.Out << Native.Error;

    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(NoOpt ? "NoOpt" : "default optimization");
      auto Image = image(Bytes, Format);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.LiftMode = true;
      Options.NoOpt = NoOpt;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      std::string LowText, MedText;
      llvm::raw_string_ostream LowOut(LowText), MedOut(MedText);
      Pipeline::dumpLowIR(Result.LowFuncs, LowOut);
      Pipeline::dumpMedIR(Result.MedFuncs, MedOut);
      retain(NoOpt ? "noopt.low" : "default.low", LowText);
      retain(NoOpt ? "noopt.med" : "default.med", MedText);
      if (AllowContextRefusal && !Result.Success) {
        EXPECT_FALSE(Result.Error.empty());
        EXPECT_EQ(Result.LlvmModule, nullptr);
        continue;
      }
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_NE(Result.LlvmModule, nullptr);
      ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      auto *Function = Result.LlvmModule->getFunction("isa_probe");
      ASSERT_NE(Function, nullptr);
      ASSERT_EQ(Function->arg_size(), IntegerReturn ? 0u : 1u);
      if (IntegerReturn)
        ASSERT_TRUE(Function->getReturnType()->isIntegerTy())
            << "an observed integer return must retain a return carrier";
      const auto IR = file(NoOpt ? "noopt.ll" : "default.ll");
      write(IR, neverd::test::printHostCompilerFixture(*Result.LlvmModule));
      retain(NoOpt ? "noopt.ll" : "default.ll", contents(IR));
      // Exercise the bundled LLVM code generator used by NeverD itself.
      // Host Clang only compiles the independent observer and links the object.
      auto CompiledObject =
          Codegen().compile(*Result.LlvmModule, Arch::X64, Format);
      ASSERT_TRUE(CompiledObject.Success);
      const auto Object = file(NoOpt ? "noopt.o" : "default.o");
      const llvm::StringRef ObjectBytes(
          reinterpret_cast<const char *>(CompiledObject.ObjectData.data()),
          CompiledObject.ObjectData.size());
      write(Object, ObjectBytes);
      retain(NoOpt ? "noopt.o" : "default.o", ObjectBytes);
      const auto Executable = file(NoOpt ? "noopt.exe" : "default.exe");
      Compiled = command(Compiler, {"-O0", Object, Harness, "-o", Executable});
      ASSERT_EQ(Compiled.Status, 0) << Compiled.Error << contents(IR);
      const auto Actual = command(Executable, {});
      retain(NoOpt ? "noopt-run.txt" : "default-run.txt",
             Actual.Out + Actual.Error +
                 "\nexit=" + std::to_string(Actual.Status));
      EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error;
    }
  }

  void compareC(llvm::ArrayRef<uint8_t> Bytes, llvm::StringRef Check,
                bool IntegerReturn = false) {
    if (!nativeX64())
      GTEST_SKIP() << "native x86_64 host required";
    for (bool UseLLVM : {false, true}) {
      SCOPED_TRACE(UseLLVM ? "LLVMC" : "HighC");
      auto Image = image(Bytes);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.LiftMode = UseLLVM;
      Options.SourceProjection = UseLLVM;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      CEmitterOptions EmitOptions;
      EmitOptions.TheArch = Arch::X64;
      EmitOptions.Format = Image.Format;
      EmitOptions.Image = &Image;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      ASSERT_TRUE(
          UseLLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Out, EmitOptions)
                  : HighCEmitter().emit(Result.HighFuncs, Out, EmitOptions));
      const auto C = file(UseLLVM ? "llvm.c" : "high.c");
      // Append a caller to the untouched generated definition. In particular,
      // an inferred-void projection cannot pass through a mismatched foreign
      // declaration and accidentally return a leftover host register value.
      write(C, Source + driver(Check, IntegerReturn, false, !IntegerReturn));
      retain(UseLLVM ? "llvm.c" : "high.c", Source);
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        const auto Executable = file(std::string(UseLLVM ? "llvm" : "high") +
                                     Optimization + ".exe");
        auto Compiled = command(Compiler, {Optimization, C, "-o", Executable});
        retain(std::string(UseLLVM ? "llvm" : "high") + Optimization +
                   "-compile.txt",
               Compiled.Error);
        EXPECT_EQ(Compiled.Status, 0) << Compiled.Error << Source;
        if (Compiled.Status != 0)
          continue;
        const auto Actual = command(Executable, {});
        EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error;
      }
    }
  }
};

TEST_F(X86SourceAccuracy,
       WideSlicesShiftInTheSourceCarrierWithoutUndefinedBehavior) {
  std::vector<HighFunc> Functions;
  std::string Checks = "int main(void) {\n";
  for (unsigned Width : {16u, 32u, 64u})
    for (unsigned Offset : {1u, 7u, 8u, 15u, 16u, 31u, 32u, 63u}) {
      if (Offset >= Width)
        continue;
      for (uint64_t Bits : {UINT64_C(0), UINT64_C(0xfedcba9876543210)}) {
        HighFunc Function;
        Function.Name = "slice_" + std::to_string(Functions.size());
        Function.ReturnType = NdType::makeInt(8, false);
        auto Value = HighExpr::makeConst(Bits, Width);
        auto Slice = HighExpr::makeBinop(NdOp::SUBBYTES, Value,
                                         HighExpr::makeConst(Offset, 4));
        Slice->Type = Function.ReturnType;
        HighStmt Return;
        Return.Kind = StmtKind::Return;
        Return.RetVal = Slice;
        Function.Body.push_back(std::move(Return));
        const uint64_t Expected = Offset >= 8 ? 0 : Bits >> (Offset * 8);
        Checks += "if (" + Function.Name + "() != UINT64_C(" +
                  std::to_string(Expected) + ")) return 1;\n";
        Functions.push_back(std::move(Function));
      }
    }
  Checks += "return 0; }\n";
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
  const auto Path = file("wide-slices.c");
  write(Path, Source + Checks);
  retain("wide-slices.c", Source + Checks);
  if (!neverd::test::compilerHasWideBitInt(Compiler))
    GTEST_SKIP() << "this Clang lacks the _BitInt width the C spells";
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable = file(std::string("wide") + Optimization + ".exe");
    const auto Compiled = command(
        Compiler, {Optimization, "-fsanitize=shift", "-fsanitize-trap=shift",
                   "-Werror=shift-count-overflow", Path, "-o", Executable});
    ASSERT_EQ(Compiled.Status, 0) << Compiled.Error << Source;
    const auto Actual = command(Executable, {});
    EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error;
  }
}

TEST_F(X86SourceAccuracy, StmxcsrCStoresTheLoadedControlValue) {
  const uint8_t M = memoryModRM();
  // LDMXCSR [arg0]; STMXCSR [arg0+4]; RET. O0 and O2 must preserve
  // the store and the non-default rounding-control bits, not print a mnemonic.
  for (uint8_t Offset : {0, 1}) {
    SCOPED_TRACE(static_cast<unsigned>(Offset));
    std::vector<uint8_t> Code{0xc7,
                              static_cast<uint8_t>(0x40 | M),
                              static_cast<uint8_t>(Offset + 8),
                              0x12,
                              0xa0,
                              0xfe,
                              0x39};
    Code.insert(Code.end(),
                {0xc7, static_cast<uint8_t>((Offset ? 0x40 : 0) | M)});
    if (Offset)
      Code.push_back(Offset);
    Code.insert(Code.end(), {0x80, 0x5f, 0, 0, 0x0f, 0xae,
                             static_cast<uint8_t>((Offset ? 0x50 : 0x10) | M)});
    if (Offset)
      Code.push_back(Offset);
    Code.insert(Code.end(), {0x0f, 0xae, static_cast<uint8_t>(0x58 | M),
                             static_cast<uint8_t>(Offset + 4), 0xc3});
    compareC(Code,
             "  uint32_t csr, adjacent; memcpy(&csr, output + " +
                 std::to_string(Offset + 4) +
                 ", 4); memcpy(&adjacent, output + " +
                 std::to_string(Offset + 8) + ", 4);\n" +
                 "  return csr == 0x5f80 && adjacent == 0x39fea012 ? 0 : 1;\n");
  }
}

TEST_F(X86SourceAccuracy, MxcsrAddressesEvaluateOnceAndDoNotShadowSourceNames) {
  if (!nativeX64())
    GTEST_SKIP() << "native x86_64 host required";
  llvm::LLVMContext Context;
  llvm::Module Module("csr_memory", Context);
  Module.setTargetTriple(llvm::Triple(llvm::sys::getDefaultTargetTriple()));
  Module.setDataLayout("e-p:64:64-i64:64-n8:16:32:64-S128");
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Void = llvm::Type::getVoidTy(Context);
  auto *GetAddress = llvm::Function::Create(
      llvm::FunctionType::get(I64, {}, false), llvm::Function::ExternalLinkage,
      "neverd_mxcsr", Module);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Void, {}, false), llvm::Function::ExternalLinkage,
      "csr_memory", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *AsmType = llvm::FunctionType::get(Void, {I64}, false);
  auto *Load =
      llvm::InlineAsm::get(AsmType, "ldmxcsr ($0)", "r,~{memory}", true);
  auto *Store =
      llvm::InlineAsm::get(AsmType, "stmxcsr ($0)", "r,~{memory}", true);
  Builder.CreateCall(Load, {Builder.CreateCall(GetAddress)});
  Builder.CreateCall(Store, {Builder.CreateAdd(Builder.CreateCall(GetAddress),
                                               Builder.getInt64(4))});
  Builder.CreateRetVoid();
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.Format = hostFormat();
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
  Source += R"(
#include <string.h>
static unsigned char storage[16];
static unsigned calls;
uint64_t neverd_mxcsr(void) { ++calls; return (uintptr_t)(storage + 1); }
int main(void) {
  uint32_t saved = _mm_getcsr(), configured = 0x5f80, guard = 0x39fea012;
  memcpy(storage + 1, &configured, 4);
  memcpy(storage + 9, &guard, 4);
  csr_memory();
  _mm_setcsr(saved);
  uint32_t stored, adjacent;
  memcpy(&stored, storage + 5, 4);
  memcpy(&adjacent, storage + 9, 4);
  return calls == 2 && stored == configured && adjacent == guard ? 0 : 1;
}
)";
  const auto Path = file("csr-addresses.c");
  write(Path, Source);
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    const auto Executable =
        file(std::string("csr-addresses") + Optimization + ".exe");
    const auto Compiled =
        command(Compiler, {Optimization, Path, "-o", Executable});
    ASSERT_EQ(Compiled.Status, 0) << Compiled.Error << Source;
    const auto Actual = command(Executable, {});
    EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error << Source;
  }
}

TEST_F(X86SourceAccuracy, AhWriteSurvivesLLVMReturnRecovery) {
  // MOV RAX,0; MOV AH,0xff; RET. AH updates RAX[15:8] even if no
  // full-width physical-register write follows it.
  compareLLVM({0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xb4, 0xff, 0xc3}, R"(
  printf("returned=%llu\n", (unsigned long long)value);
  return value == 0xff00 ? 0 : 1;
)",
              true);
}

TEST_F(X86SourceAccuracy, AhWriteSurvivesBothCReturnRoutes) {
  compareC({0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xb4, 0xff, 0xc3}, R"(
  printf("returned=%llu\n", (unsigned long long)value);
  return value == 0xff00 ? 0 : 1;
)",
           true);
}

TEST_F(X86SourceAccuracy, AhWriteCanBeObservedThroughMemory) {
  // The same merged RAX value is correct when used as a store operand. This
  // distinguishes register-byte lifting from return-carrier recovery.
  compareLLVM({0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xb4, 0xff, 0x48, 0x89,
               memoryModRM(), 0xc3},
              R"(
  uint64_t value; memcpy(&value, output, 8);
  return value == 0xff00 ? 0 : 1;
)");
}

TEST_F(X86SourceAccuracy, NewWideReturnDominatesEarlierAhWrite) {
  compareLLVM({0x48, 0xb8, 0,  0, 0, 0, 0, 0, 0, 0, 0xb4, 0xff,
               0x48, 0xb8, 42, 0, 0, 0, 0, 0, 0, 0, 0xc3},
              R"(
  return value == 42 ? 0 : 1;
)",
              true);
}

TEST_F(X86SourceAccuracy, EaxWriteDefinesZeroExtendedReturn) {
  compareLLVM({0x48, 0xb8, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xb8,
               0x78, 0x56, 0x34, 0x12, 0xc3},
              R"(
  return value == UINT64_C(0x12345678) ? 0 : 1;
)",
              true);
}

} // namespace
