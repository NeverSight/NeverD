//===- X86FPStateAccuracyFixture.h - Native FP state test harness -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_X86FPSTATEACCURACYFIXTURE_H
#define NEVERD_UNITTESTS_X86FPSTATEACCURACYFIXTURE_H

#include "LLVMHostFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/LLVMX86FPStateAsm.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/X86FPState.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdlib>
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
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

class X86FPStateFixture : public testing::Test {
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
    const auto Out = contents(Stdout);
    const auto Diagnostics = Error + contents(Stderr);
    if (Status != 0) {
      std::string Invocation = Program.str();
      for (const auto &Arg : Arguments)
        Invocation += "\n" + Arg;
      retain("command-failure-" + std::to_string(Files.size()) + ".txt",
             Invocation + "\nstdout:\n" + Out + "\nstderr:\n" + Diagnostics);
    }
    return {Status, Out, Diagnostics + (Status != 0 ? Out : "")};
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

  static std::string floatingDriver(bool IsDouble, bool DeclareProbe) {
    std::string Text = R"(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <immintrin.h>
extern void native_probe(uintptr_t);
)";
    if (DeclareProbe)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += IsDouble ? R"(
static const uint64_t values[] = {
  0, UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000),
  UINT64_C(0xbff0000000000000), UINT64_C(0x3ff0000000000001),
  UINT64_C(0x3fb999999999999a), 1, UINT64_C(0x000fffffffffffff),
  UINT64_C(0x0010000000000000), UINT64_C(0x7fefffffffffffff),
  UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
  UINT64_C(0x7ff8000000000011), UINT64_C(0x7ff8000000000077),
  UINT64_C(0x7ff0000000000031), UINT64_C(0x7ff0000000000071),
  UINT64_C(0x3ca0000000000000), UINT64_C(0x4000000000000000)
};
)"
                     : R"(
static const uint32_t values[] = {
  0, 0x80000000, 0x3f800000, 0xbf800000, 0x3f800001, 0x3dcccccd,
  1, 0x007fffff, 0x00800000, 0x7f7fffff, 0x7f800000, 0xff800000,
  0x7fc00011, 0x7fc00077, 0x7f800031, 0x7f800071, 0x33800000, 0x40000000
};
)";
    Text += R"(
int main(void) {
  uint32_t saved = _mm_getcsr();
  unsigned count = 0;
  for (unsigned rounding = 0; rounding < 4; ++rounding)
    for (unsigned environment = 0; environment < 4; ++environment)
      for (unsigned sticky = 0; sticky < 2; ++sticky)
        for (unsigned a = 0; a < sizeof(values)/sizeof(values[0]); ++a)
          for (unsigned b = 0; b < sizeof(values)/sizeof(values[0]); ++b) {
            unsigned char expected[80], actual[80];
            for (unsigned i = 0; i < sizeof(expected); ++i)
              expected[i] = (unsigned char)(0x59u + i * 37u);
            memcpy(expected, &values[a], sizeof(values[0]));
            memcpy(expected + 16, &values[b], sizeof(values[0]));
            memcpy(actual, expected, sizeof(expected));
            uint32_t state = 0x1f80 | (rounding << 13) |
                ((environment & 1) ? 0x40 : 0) |
                ((environment & 2) ? 0x8000 : 0) | (sticky ? 0x25 : 0);
            _mm_setcsr(state);
            native_probe((uintptr_t)expected);
            uint32_t expected_state = _mm_getcsr();
            _mm_setcsr(state);
            isa_probe((uintptr_t)actual);
            uint32_t actual_state = _mm_getcsr();
            _mm_setcsr(saved);
            if (expected_state != actual_state ||
                memcmp(expected, actual, sizeof(expected)) != 0) {
              printf("a=%u b=%u state=%08x expected=%08x actual=%08x\n",
                  a, b, state, expected_state, actual_state);
              for (unsigned i = 0; i < sizeof(expected); ++i)
                if (expected[i] != actual[i])
                  printf("byte %u: expected=%02x actual=%02x\n",
                      i, expected[i], actual[i]);
              return 1;
            }
            ++count;
          }
  printf("%u full numerical/state comparisons passed\n", count);
  return 0;
}
)";
    return Text;
  }

  static std::string faultDriver(bool IsDouble, bool DeclareProbe,
                                 bool NativeCall = false) {
    std::string Text = R"(
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif
extern void native_probe(uintptr_t);
static unsigned char actual[80];
static int numerical_output_untouched(void) {
  for (unsigned i = 48; i < 64; ++i)
    if (actual[i] != (unsigned char)(0x59u + i * 37u)) return 0;
  return 1;
}
#if defined(_WIN32)
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_FLT_DIVIDE_BY_ZERO)
    ExitProcess(numerical_output_untouched() ? 0 : 1);
  return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void on_fault(int signal_number) {
  _exit(signal_number == SIGFPE && numerical_output_untouched() ? 0 : 1);
}
#endif
)";
    if (DeclareProbe)
      Text += "extern void isa_probe(uintptr_t);\n";
    Text += R"(
int main(void) {
#if defined(_WIN32)
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (!AddVectoredExceptionHandler(1, on_fault)) return 77;
#else
  if (signal(SIGFPE, on_fault) == SIG_ERR) return 77;
#endif
  for (unsigned i = 0; i < sizeof(actual); ++i)
    actual[i] = (unsigned char)(0x59u + i * 37u);
)";
    Text += IsDouble ? R"(
  uint64_t one = UINT64_C(0x3ff0000000000000), zero = 0;
)"
                     : R"(
  uint32_t one = 0x3f800000, zero = 0;
)";
    Text += R"(
  memcpy(actual, &one, sizeof(one));
  memcpy(actual + 16, &zero, sizeof(zero));
  _mm_setcsr(0x1d80); /* divide-by-zero unmasked */
)";
    Text += NativeCall ? "  native_probe((uintptr_t)actual);\n"
                       : "  isa_probe((uintptr_t)actual);\n";
    Text += "  _mm_setcsr(0x1f80);\n  return 99; /* missing exception */\n}\n";
    return Text;
  }

  virtual std::string comparisonDriver(bool IsDouble, bool DeclareProbe,
                                       bool Fault, bool NativeCall = false) {
    return Fault ? faultDriver(IsDouble, DeclareProbe, NativeCall)
                 : floatingDriver(IsDouble, DeclareProbe);
  }

  void compareFloating(llvm::ArrayRef<uint8_t> Bytes, bool IsDouble,
                       bool Fault = false) {
    ++ProbeNumber;
    if (!nativeX64())
      GTEST_SKIP() << "native x86_64 host required";
    const auto Original = file("native.s");
    const auto OriginalObject = file("native.o");
    std::string NativeText = assembly(Bytes);
    for (size_t Index = 0;
         (Index = NativeText.find("isa_probe", Index)) != std::string::npos;)
      NativeText.replace(Index, 9, "native_probe");
    write(Original, NativeText);
    auto Built = command(Compiler, {"-c", Original, "-o", OriginalObject});
    ASSERT_EQ(Built.Status, 0) << Built.Error;
    if (Fault) {
      const auto ReferenceDriver = file("fault-reference.c");
      const auto Reference = file("fault-reference.exe");
      write(ReferenceDriver, comparisonDriver(IsDouble, true, true, true));
      Built = command(
          Compiler, {"-O2", OriginalObject, ReferenceDriver, "-o", Reference});
      ASSERT_EQ(Built.Status, 0) << Built.Error;
      const auto OriginalRun = command(Reference, {});
      ASSERT_EQ(OriginalRun.Status, 0) << OriginalRun.Out << OriginalRun.Error;
    }
    const auto Harness = file("driver.c");
    write(Harness, comparisonDriver(IsDouble, true, Fault));
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(NoOpt ? "NoOpt" : "default");
      auto Image = image(Bytes);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.LiftMode = true;
      Options.NoOpt = NoOpt;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_NE(Result.LlvmModule, nullptr);
      ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      std::string Module;
      llvm::raw_string_ostream ModuleOut(Module);
      Result.LlvmModule->print(ModuleOut, nullptr);
      retain(NoOpt ? "noopt.ll" : "default.ll", Module);
      auto Object =
          Codegen().compile(*Result.LlvmModule, Arch::X64, hostFormat());
      ASSERT_TRUE(Object.Success);
      const auto LiftedObject = file(NoOpt ? "noopt.o" : "default.o");
      write(LiftedObject, llvm::StringRef(reinterpret_cast<const char *>(
                                              Object.ObjectData.data()),
                                          Object.ObjectData.size()));
      const auto Executable = file(NoOpt ? "noopt.exe" : "default.exe");
      Built = command(Compiler, {"-O2", OriginalObject, LiftedObject, Harness,
                                 "-o", Executable});
      ASSERT_EQ(Built.Status, 0) << Built.Error;
      auto Actual = command(Executable, {});
      retain(NoOpt ? "noopt-native.txt" : "default-native.txt",
             Actual.Out + Actual.Error);
      EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error;
    }
    for (const auto [LLVM, SourceNoOpt] :
         {std::pair{false, false}, std::pair{false, true},
          std::pair{true, false}, std::pair{true, true}}) {
      SCOPED_TRACE(LLVM ? "LLVMC" : "HighC");
      SCOPED_TRACE(SourceNoOpt ? "source NoOpt" : "source default");
      auto Image = image(Bytes);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.LiftMode = LLVM;
      Options.SourceProjection = LLVM;
      Options.NoOpt = SourceNoOpt;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      CEmitterOptions EmitterOptions;
      EmitterOptions.TheArch = Arch::X64;
      EmitterOptions.Format = hostFormat();
      EmitterOptions.Image = &Image;
      EmitterOptions.PreserveLLVMFunctionTypes = true;
      EmitterOptions.UseUnalignedPointers = false;
      std::string Source;
      llvm::raw_string_ostream SourceOut(Source);
      if (LLVM) {
        ASSERT_NE(Result.LlvmModule, nullptr);
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, SourceOut,
                                        EmitterOptions, nullptr, &Image));
      } else {
        ASSERT_TRUE(
            HighCEmitter().emit(Result.HighFuncs, SourceOut, EmitterOptions));
      }
      retain(std::string(LLVM ? "llvm" : "high") +
                 (SourceNoOpt ? "-noopt.c" : ".c"),
             Source);
      const auto Standalone = file(LLVM ? "llvm-unit.c" : "high-unit.c");
      write(Standalone, Source);
      Built =
          command(Compiler, {"-O2", "-c", Standalone, "-o", file("unit.o")});
      ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
      const auto C = file(LLVM ? "llvm.c" : "high.c");
      std::string CDriver = comparisonDriver(IsDouble, false, Fault);
      bool PointerProbe = false;
      if (LLVM) {
        const auto *Function = Result.LlvmModule->getFunction("isa_probe");
        ASSERT_NE(Function, nullptr);
        ASSERT_EQ(Function->arg_size(), 1U);
        PointerProbe = Function->getArg(0)->getType()->isPointerTy();
      } else {
        ASSERT_EQ(Result.HighFuncs.size(), 1U);
        ASSERT_EQ(Result.HighFuncs[0].Params.size(), 1U);
        PointerProbe =
            Result.HighFuncs[0].Params[0].Type->Kind == NdTypeKind::Ptr;
      }
      if (PointerProbe) {
        const auto Position = CDriver.find("isa_probe((uintptr_t)actual)");
        ASSERT_NE(Position, std::string::npos);
        CDriver.replace(Position, 28, "isa_probe((void *)actual)");
      }
      write(C, Source + CDriver);
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        const auto Executable =
            file(std::string(LLVM ? "llvm" : "high") + Optimization + ".exe");
        Built = command(Compiler,
                        {Optimization, OriginalObject, C, "-o", Executable});
        ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
        auto Actual = command(Executable, {});
        retain(std::string(LLVM ? "llvm" : "high") + Optimization + ".txt",
               Actual.Out + Actual.Error);
        EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error;
      }
    }
  }

  void executeSource(const std::string &Source, llvm::StringRef Driver) {
    const auto Unit = file("standalone.c");
    write(Unit, Source);
    auto Built = command(Compiler, {"-O2", "-c", Unit, "-o", file("unit.o")});
    ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
    const auto C = file("source-driver.c");
    write(C, Source + Driver.str());
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Executable =
          file(std::string("source") + Optimization + ".exe");
      Built =
          command(Compiler, {Optimization, "-fsanitize=undefined",
                             "-fsanitize-trap=undefined", C, "-o", Executable});
      ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
      const auto Actual = command(Executable, {});
      EXPECT_EQ(Actual.Status, 0) << Actual.Out << Actual.Error << Source;
    }
  }
};

} // namespace

#endif
