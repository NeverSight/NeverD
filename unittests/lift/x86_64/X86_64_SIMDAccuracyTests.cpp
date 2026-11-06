//===- X86_64_SIMDAccuracyTests.cpp - CPU accuracy regressions ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LLVMHostFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
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

class X86SIMDAccuracy : public testing::Test {
protected:
  llvm::SmallString<128> Directory;
  std::string Compiler;
  std::vector<std::string> Files;
  bool CompareOutput = false;
  bool PositiveInput = false;
  unsigned ProbeNumber = 0;

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
    llvm::sys::path::append(Path, std::to_string(ProbeNumber));
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
                            bool PointerArgument = false,
                            bool PatternInput = false, bool ClearSign = false) {
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
)";
    if (PatternInput)
      Text += R"(
  for (unsigned i = 1024; i < 1536; ++i)
    output[i] = (unsigned char)((i * 37u + (i >> 4) * 13u) ^ 0xa5u);
)";
    if (ClearSign)
      Text +=
          "  for (unsigned i = 1027; i < 1536; i += 4) output[i] &= 0x7f;\n";
    Text +=
        "  __asm__ volatile(\"fxsave64 %0\" : \"=m\"(saved) : : \"memory\");\n";
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
    write(Harness, driver(Check, IntegerReturn, true, false, CompareOutput,
                          PositiveInput));
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
      if (CompareOutput)
        EXPECT_EQ(Actual.Out, Native.Out);
    }
  }

  void compareSIMD(llvm::ArrayRef<uint8_t> Instruction, unsigned Width = 16,
                   bool MMX = false, bool ObserveFlags = false,
                   bool ObserveIndex = false) {
    const auto Features = llvm::sys::getHostCPUFeatures();
    if (Width == 32 && !Features.lookup("avx2"))
      GTEST_SKIP() << "native AVX2 execution is unavailable";
    // Copy the host argument to R10. Every memory access remains owned by
    // the same argument, while EAX/EDX/ECX are available for string operations.
    std::vector<uint8_t> Bytes = hostFormat() == BinaryFormat::COFF
                                     ? std::vector<uint8_t>{0x49, 0x89, 0xca}
                                     : std::vector<uint8_t>{0x49, 0x89, 0xfa};
    auto Append = [&](llvm::ArrayRef<uint8_t> Part) {
      Bytes.insert(Bytes.end(), Part.begin(), Part.end());
    };
    for (unsigned Register = 0; Register < 3; ++Register) {
      if (MMX)
        Append({0x41, 0x0f, 0x6f});
      else if (Width == 32)
        Append({0xc4, 0xc1, 0x7e, 0x6f});
      else
        Append({0xf3, 0x41, 0x0f, 0x6f});
      Append({static_cast<uint8_t>(0x82 + Register * 8),
              static_cast<uint8_t>(Register * 32), 4, 0, 0});
    }
    // Explicit string lengths; vector sources contain independent byte
    // patterns, including high-bit shuffle controls and sign bits.
    Append({0x31, 0xc0, 0xb8, 4, 0, 0, 0, 0xba, 3, 0, 0, 0});
    Append(Instruction);
    // The driver restores the x87 state, so no EMMS is needed: outside an owned
    // x87 context one is refused, which is not what these probes compare.
    if (MMX)
      Append({0x41, 0x0f, 0x7f, 0x02});
    else if (Width == 32)
      Append({0xc4, 0xc1, 0x7e, 0x7f, 0x02});
    else
      Append({0xf3, 0x41, 0x0f, 0x7f, 0x02});
    if (ObserveFlags) {
      Append({0x9c, 0x58, 0x25, 0xd5, 8, 0, 0, 0x49, 0x89, 0x42, 0x40});
      if (ObserveIndex)
        Append({0x41, 0x89, 0x4a, 0x48});
    }
    Append({0xc3});
    CompareOutput = true;
    ++ProbeNumber;
    compareLLVM(Bytes, R"(
  for (unsigned i = 0; i < 80; ++i) printf("%02x", output[i]);
  putchar('\n');
  return 0;
)");
  }
};

TEST_F(X86SIMDAccuracy, SIMDScalarMovesKeepDefinedUpperBytes) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx"))
    GTEST_SKIP() << "native AVX execution is unavailable";
  compareSIMD({0x66, 0x0f, 0xd6, 0xc8}); // MOVQ xmm0,xmm1
  compareSIMD({0xc5, 0xf9, 0xd6, 0xc8}); // VMOVQ xmm0,xmm1
  compareSIMD({0xc5, 0xf2, 0x11, 0xd0}); // VMOVSS xmm0,xmm1,xmm2
  compareSIMD({0xc5, 0xf3, 0x11, 0xd0}); // VMOVSD xmm0,xmm1,xmm2
  // CVTPI2PS preserves xmm0[127:64]. Load an independent MMX source first.
  compareSIMD({0x41, 0x0f, 0x6f, 0x8a, 0x20, 4, 0, 0, 0x0f, 0x2a, 0xc1});
}

TEST_F(X86SIMDAccuracy, SIMDWideBroadcastAndBlendRepeatWithinLanes) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx2"))
    GTEST_SKIP() << "native AVX2 execution is unavailable";
  compareSIMD({0xc4, 0xc2, 0x7d, 0x1a, 0x82, 0x20, 4, 0, 0}, 32);
  compareSIMD({0xc4, 0xc2, 0x7d, 0x5a, 0x82, 0x20, 4, 0, 0}, 32);
  for (uint8_t Control : {0, 1, 0x7e, 0x80, 0xff}) {
    SCOPED_TRACE(static_cast<unsigned>(Control));
    compareSIMD({0xc4, 0xe3, 0x75, 0x0e, 0xc2, Control}, 32);
  }
}

TEST_F(X86SIMDAccuracy, SIMDShuffleZeroControlsProduceDefinedZeros) {
  const auto Features = llvm::sys::getHostCPUFeatures();
  if (!Features.lookup("avx2") || !Features.lookup("ssse3"))
    GTEST_SKIP() << "native AVX2/SSSE3 execution is unavailable";
  for (uint8_t Control : {0, 15, 16, 17, 31, 32, 0x7e, 0xff}) {
    SCOPED_TRACE(static_cast<unsigned>(Control));
    compareSIMD({0x66, 0x0f, 0x3a, 0x0f, 0xc1, Control});
    compareSIMD({0xc4, 0xe3, 0x75, 0x0f, 0xc2, Control}, 32);
  }
  for (uint8_t Control : {0, 0x13, 0x08, 0x80, 0x88, 0x7e, 0xff}) {
    SCOPED_TRACE(static_cast<unsigned>(Control));
    compareSIMD({0xc4, 0xe3, 0x75, 0x06, 0xc2, Control}, 32);
    compareSIMD({0xc4, 0xe3, 0x75, 0x46, 0xc2, Control}, 32);
  }
}

TEST_F(X86SIMDAccuracy, SIMDStringResultsAndFlagsReadOriginalAliasedInputs) {
  const auto Features = llvm::sys::getHostCPUFeatures();
  if (!Features.lookup("avx") || !Features.lookup("sse4.2"))
    GTEST_SKIP() << "native AVX/SSE4.2 execution is unavailable";
  for (uint8_t Opcode : {0x60, 0x61, 0x62, 0x63})
    for (uint8_t Control : {0, 0x0c, 0x40, 0x7e}) {
      SCOPED_TRACE(static_cast<unsigned>(Opcode));
      SCOPED_TRACE(static_cast<unsigned>(Control));
      compareSIMD({0x66, 0x0f, 0x3a, Opcode, 0xc1, Control}, 16, false, true,
                  Opcode & 1);
      compareSIMD({0xc4, 0xe3, 0x79, Opcode, 0xc1, Control}, 16, false, true,
                  Opcode & 1);
    }
}

TEST_F(X86SIMDAccuracy, SIMDVectorTestsUseElementSignBitsAndClearFlags) {
  if (!llvm::sys::getHostCPUFeatures().lookup("avx2"))
    GTEST_SKIP() << "native AVX2 execution is unavailable";
  for (uint8_t Opcode : {0x0e, 0x0f}) {
    compareSIMD({0xc4, 0xe2, 0x79, Opcode, 0xc1}, 16, false, true);
    compareSIMD({0xc4, 0xe2, 0x7d, Opcode, 0xc1}, 32, false, true);
    PositiveInput = true;
    compareSIMD({0xc4, 0xe2, 0x79, Opcode, 0xc1}, 16, false, true);
    compareSIMD({0xc4, 0xe2, 0x7d, Opcode, 0xc1}, 32, false, true);
    PositiveInput = false;
  }
}

TEST_F(X86SIMDAccuracy, SIMDMMXQwordArithmeticAndShufflesExecute) {
  if (!llvm::sys::getHostCPUFeatures().lookup("ssse3"))
    GTEST_SKIP() << "native SSSE3 execution is unavailable";
  compareSIMD({0x0f, 0xd4, 0xc1}, 8, true);
  compareSIMD({0x0f, 0xfb, 0xc1}, 8, true);
  compareSIMD({0x0f, 0x38, 0x00, 0xc1}, 8, true);
  for (uint8_t Control : {0, 0x1b, 0x7e, 0xff})
    compareSIMD({0x0f, 0x70, 0xc1, Control}, 8, true);
}

TEST_F(X86SIMDAccuracy, SIMDVAESRoundsOperateOnBoth128BitLanes) {
  const auto Features = llvm::sys::getHostCPUFeatures();
  if (!Features.lookup("vaes") || !Features.lookup("avx2"))
    GTEST_SKIP() << "native VAES execution is unavailable";
  for (uint8_t Opcode : {0xdc, 0xdd, 0xde, 0xdf}) {
    compareSIMD({0xc4, 0xe2, 0x75, Opcode, 0xc2}, 32);
    compareSIMD({0xc5, 0xf5, 0xef, 0xc9, 0xc4, 0xe2, 0x75, Opcode, 0xc2}, 32);
    compareSIMD({0xc5, 0xed, 0xef, 0xd2, 0xc4, 0xe2, 0x75, Opcode, 0xc2}, 32);
    compareSIMD({0xc5, 0xf5, 0xef, 0xc9, 0xc5, 0xed, 0xef, 0xd2, 0xc4, 0xe2,
                 0x75, Opcode, 0xc2},
                32);
  }
}

} // namespace
