//===- X86_64_StateAccuracyTests.cpp - CPU accuracy regressions ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMHostFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

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

class X86StateAccuracy : public testing::Test {
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
        auto Compiled = command(Compiler, {Optimization, "-include",
                                           "immintrin.h", C, "-o", Executable});
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

TEST_F(X86StateAccuracy, FcmovSelfKeepsSt0InsteadOfCopyingSt1) {
  // FNINIT; FLD1; FLDPI; XOR EAX,EAX; INC EAX; FCMOVNBE ST0,ST0;
  // FSTP qword [arg0]; RET. CF=ZF=0 makes the condition true; copying
  // ST0 to itself must retain PI, whereas the incorrect default ST1 is 1.0.
  compareLLVM({0xdb, 0xe3, 0xd9, 0xe8, 0xd9, 0xeb, 0x31, 0xc0, 0xff, 0xc0, 0xdb,
               0xd0, 0xdd, static_cast<uint8_t>(0x18 | memoryModRM()), 0xc3},
              R"(
  uint64_t bits; memcpy(&bits, output, 8);
  printf("st0=%016llx\n", (unsigned long long)bits);
  return bits == UINT64_C(0x400921fb54442d18) ? 0 : 1;
)");
}

TEST_F(X86StateAccuracy, FcmovNonSelfCopiesSt1WhenConditionIsTrue) {
  // The same condition with the real ST1 source must copy 1.0. This rejects
  // an overcorrection that treats every FCMOV as an unchanged destination.
  compareLLVM({0xdb, 0xe3, 0xd9, 0xe8, 0xd9, 0xeb, 0x31, 0xc0, 0xff, 0xc0, 0xdb,
               0xd1, 0xdd, static_cast<uint8_t>(0x18 | memoryModRM()), 0xc3},
              R"(
  uint64_t bits; memcpy(&bits, output, 8);
  return bits == UINT64_C(0x3ff0000000000000) ? 0 : 1;
)");
}

TEST_F(X86StateAccuracy, PoppingX87OperationsPreserveOtherLiveStackValues) {
  for (uint8_t Opcode : {0xf1, 0xf3, 0xf9}) {
    SCOPED_TRACE(static_cast<unsigned>(Opcode));
    // Two copies of 1,PI keep the earlier PI live across a binary pop.
    // Only observe that retained value; precision of the computed first value
    // is a separate constant/rounding contract.
    compareLLVM({0xdb, 0xe3, 0xd9, 0xe8, 0xd9, 0xeb, 0xd9, 0xe8, 0xd9, 0xeb,
                 0xd9, Opcode, 0xdd, static_cast<uint8_t>(0x18 | memoryModRM()),
                 0xdd, static_cast<uint8_t>(0x58 | memoryModRM()), 8, 0xc3},
                R"(
  uint64_t retained; memcpy(&retained, output + 8, 8);
  return retained == UINT64_C(0x400921fb54442d18) ? 0 : 1;
)");
  }
}

TEST_F(X86StateAccuracy, StackWritingFlagsRequireACompilerFrame) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x9c, 0x58, 0xc3},
        std::vector<uint8_t>{0x68, 0x02, 0x02, 0, 0, 0x9d, 0xc3}}) {
    auto Image = image(Bytes, BinaryFormat::ELF);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.LiftMode = true;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry};
    const auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    const auto *Function = Result.LlvmModule->getFunction("isa_probe");
    ASSERT_NE(Function, nullptr);
    EXPECT_TRUE(Function->hasFnAttribute(llvm::Attribute::NoRedZone));
  }
}

TEST_F(X86StateAccuracy, InlinedStackWritingFlagsStillRequireACompilerFrame) {
  for (const std::vector<uint8_t> Bytes :
       {std::vector<uint8_t>{0x9c, 0x58, 0xc3},
        std::vector<uint8_t>{0x68, 0x02, 0x02, 0, 0, 0x9d, 0xc3}}) {
    auto Image = image(Bytes, BinaryFormat::ELF);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.LiftMode = true;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Entry};
    auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_NE(Result.LlvmModule, nullptr);
    auto *Callee = Result.LlvmModule->getFunction("isa_probe");
    ASSERT_NE(Callee, nullptr);
    auto *Caller = llvm::Function::Create(
        Callee->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
        "inlined_flags", Result.LlvmModule.get());
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Caller));
    std::vector<llvm::Value *> Arguments;
    for (llvm::Argument &Argument : Caller->args())
      Arguments.push_back(&Argument);
    auto *Call = Builder.CreateCall(Callee, Arguments);
    if (Callee->getReturnType()->isVoidTy())
      Builder.CreateRetVoid();
    else
      Builder.CreateRet(Call);
    llvm::InlineFunctionInfo Info;
    ASSERT_TRUE(llvm::InlineFunction(*Call, Info).isSuccess());
    ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    Codegen Generator;
    const auto Object =
        Generator.compile(*Result.LlvmModule, Arch::X64, BinaryFormat::ELF);
    ASSERT_TRUE(Object.Success);
    // LLVM's flags intrinsics reserve their own frame when it selects them. The
    // assembly fallback relies on the attribute following its body here.
    const bool LLVMReservesFrame = llvm::any_of(
        llvm::instructions(*Caller), [](const llvm::Instruction &Instruction) {
          const auto *Intrinsic =
              llvm::dyn_cast<llvm::IntrinsicInst>(&Instruction);
          return Intrinsic && (Intrinsic->getIntrinsicID() ==
                                   llvm::Intrinsic::x86_flags_read_u64 ||
                               Intrinsic->getIntrinsicID() ==
                                   llvm::Intrinsic::x86_flags_write_u64);
        });
    EXPECT_TRUE(LLVMReservesFrame ||
                Caller->hasFnAttribute(llvm::Attribute::NoRedZone));
  }
}

TEST_F(X86StateAccuracy, PackedFlagsDoNotOverwriteRedZoneSpill) {
#if defined(__linux__) && defined(__x86_64__)
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP()
        << "AVX and enabled OS vector state are required by the fixture";
  // Bounded PSUBSB fixture from the CPU campaign. Several live temporaries
  // make codegen spill the saved MXCSR address. PUSHF/POPF must not overwrite
  // that compiler-owned slot in the SysV red zone.
  const std::vector<uint8_t> Bytes = {
#include "../fixtures/X86FlagsSpillFixture.inc"
  };
  compareLLVM(Bytes, R"(
  uint32_t csr; memcpy(&csr, output + 408, 4);
  printf("stored=%08x\n", csr);
  return csr == 0x1f80 ? 0 : 1;
)",
              false, false, BinaryFormat::ELF);
#else
  GTEST_SKIP() << "SysV red-zone native execution requires Linux x86_64";
#endif
}

} // namespace
