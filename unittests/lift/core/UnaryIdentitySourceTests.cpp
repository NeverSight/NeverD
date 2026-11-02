//===- UnaryIdentitySourceTests.cpp - Execute unary identities -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compare original machine words and recovered C with independent bit oracles.
///
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>
#include <utility>

namespace {
using namespace neverd;

BinaryImage unaryImage(Arch Architecture, BinaryFormat Format, unsigned Count,
                       bool Narrow, bool Negation, bool Flags) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Image.Format = Format;
  Image.Base = Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (Architecture == Arch::X64) {
    Text.Data = {0x48, 0x89,
                 uint8_t(Format == BinaryFormat::COFF ? 0xc8 : 0xf8)};
    for (unsigned I = 0; I < Count; ++I) {
      // NEG changes arithmetic flags; NOT preserves them.
      Text.Data.insert(Text.Data.end(),
                       {0x48, 0xf7, uint8_t(Negation ? 0xd8 : 0xd0)});
      if (Narrow && I == 0)
        Text.Data.insert(Text.Data.end(), {0x89, 0xc0}); // mov eax,eax
    }
    if (Flags)
      // pushfq; pop rdx; and edx,0x801; xor rax,rdx. Observe final CF/OF
      // independently of the value fold; the captured flags must survive.
      Text.Data.insert(Text.Data.end(), {0x9c, 0x5a, 0x81, 0xe2, 0x01, 0x08, 0,
                                         0, 0x48, 0x31, 0xd0});
    Text.Data.push_back(0xc3);
  } else {
    for (unsigned I = 0; I < Count; ++I) {
      if (Negation)
        Text.Data.insert(Text.Data.end(),
                         {0xe0, 0x03, 0x00, 0xcb}); // neg x0,x0
      else
        Text.Data.insert(Text.Data.end(),
                         {0xe0, 0x03, 0x20, 0xaa}); // mvn x0,x0
      if (Narrow && I == 0)
        Text.Data.insert(Text.Data.end(),
                         {0xe0, 0x03, 0x00, 0x2a}); // mov w0,w0
    }
    Text.Data.insert(Text.Data.end(), {0xc0, 0x03, 0x5f, 0xd6});
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  auto Function = Symbol::makeFunc(Image.Entry);
  Function.Name = "complement_probe";
  Function.Size = Image.Segments.front().Size;
  Image.Symbols.push_back(std::move(Function));
  return Image;
}

std::string oracle(const BinaryImage &Image, unsigned Count, bool Narrow,
                   bool Negation, bool Flags) {
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  Out << "\n#include <stdint.h>\n#include <string.h>\n";
#if defined(__linux__) && defined(__x86_64__)
  const bool Native = Image.Arch == Arch::X64;
#else
  const bool Native = false;
#endif
  if (Native) {
    Out << "#include <sys/mman.h>\n"
           "typedef uint64_t ("
        << (Image.abiFormat() == BinaryFormat::COFF ? "__attribute__((ms_abi)) "
                                                    : "")
        << "*original_function)(uint64_t);\n";
  }
  Out << "int main(void) {\n";
  if (Native) {
    Out << "static const unsigned char bytes[] = {";
    for (auto Byte : Image.Segments.front().Data)
      Out << unsigned(Byte) << ',';
    Out << "};\nvoid *code = mmap(0, 4096, PROT_READ | PROT_WRITE | "
           "PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);\n"
           "if (code == MAP_FAILED) return 1;\n"
           "memcpy(code, bytes, sizeof(bytes));\n"
           "original_function original = (original_function)code;\n";
  }
  Out << "static const uint64_t edges[] = {0, 1, 0xffffffffULL, "
         "0x8000000000000000ULL, 0x123456789abcdef0ULL, "
         "0xffffffffffffffffULL};\n"
         "uint64_t state = 0xa0761d6478bd642fULL;\n"
         "for (unsigned i = 0; i < 262; ++i) {\n"
         "state ^= state << 13; state ^= state >> 7; state ^= state << 17;\n"
         "uint64_t input = i < 6 ? edges[i] : state;\n"
         "uint64_t expected = input;\n";
  for (unsigned I = 0; I < Count; ++I) {
    if (Flags && I + 1 == Count)
      Out << "uint64_t flags = (expected != 0) | "
             "((uint64_t)(expected == UINT64_C(0x8000000000000000)) << 11);\n";
    Out << (Negation ? "expected = UINT64_C(0) - expected;\n"
                     : "expected = ~expected;\n");
    if (Narrow && I == 0)
      Out << "expected = (uint32_t)expected;\n";
  }
  if (Flags)
    Out << "expected ^= flags;\n";
  if (Native)
    Out << "if (original(input) != expected) return 3;\n";
  Out << "if ((uint64_t)complement_probe((int64_t)input) != expected) return "
         "2;\n";
  Out << "}\n";
  if (Native)
    Out << "if (munmap(code, 4096)) return 4;\n";
  Out << "return 0;\n}\n";
  return Text;
}

void checkUnarySource(Arch Architecture, bool Negation, bool Flags = false) {
  for (auto Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (unsigned Count : {2u, 3u, 4u})
      for (bool Narrow : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << unsigned(Architecture) << ':' << unsigned(Format) << ':'
                     << Count << ':' << Narrow);
        auto Image =
            unaryImage(Architecture, Format, Count, Narrow, Negation, Flags);
        for (bool Optimized : {false, true}) {
          for (bool LLVM : {false, true}) {
            SCOPED_TRACE(testing::Message()
                         << "optimized=" << Optimized << ":llvm=" << LLVM);
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.SourceProjection = true;
            Options.NoOpt = !Optimized;
            Options.LiftMode = LLVM;
            Options.OnlyFunctionEntries = {Image.Entry};
            llvm::LLVMContext Context;
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            if (LLVM) {
              ASSERT_TRUE(Result.LlvmModule);
              ASSERT_FALSE(
                  llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            } else {
              ASSERT_EQ(Result.HighFuncs.size(), 1u);
            }
            CEmitterOptions COptions;
            COptions.TheArch = Architecture;
            COptions.Format = Format;
            COptions.Image = &Image;
            std::string Source;
            llvm::raw_string_ostream Out(Source);
            if (LLVM)
              ASSERT_TRUE(
                  LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions));
            else
              ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, COptions));
            if (Optimized && !Narrow && !Negation)
              EXPECT_EQ(Source.find("~~"), std::string::npos) << Source;
            if (Optimized && Negation && !Narrow && !Flags && Count % 2 == 0)
              EXPECT_NE(Source.find("return arg0;"), std::string::npos)
                  << Source;
            EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos)
                << Source;
            if (Flags) {
              // Preserve target-format emission above. Execute the same IR
              // with Linux's intrinsic header rather than Windows intrin.h.
              std::string Native;
              llvm::raw_string_ostream NativeOut(Native);
              COptions.Format = BinaryFormat::ELF;
              if (LLVM)
                ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, NativeOut,
                                                COptions));
              else
                ASSERT_TRUE(
                    HighCEmitter().emit(Result.HighFuncs, NativeOut, COptions));
              Source = std::move(Native);
            }
            for (const auto *Optimization : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  Source + oracle(Image, Count, Narrow, Negation, Flags),
                  {Optimization, "-D_GNU_SOURCE=1", "-fsanitize=undefined",
                   "-fno-sanitize-recover=all"});
          }
        }
      }
}
TEST(UnaryIdentitySource, X64ComplementsPreserveOriginalWordsAndNarrowing) {
  checkUnarySource(Arch::X64, false);
}
TEST(UnaryIdentitySource, ARM64ComplementsPreserveWordsAndNarrowing) {
  checkUnarySource(Arch::AArch64, false);
}
TEST(UnaryIdentitySource, X64NegationsPreserveWordsAndNarrowing) {
  checkUnarySource(Arch::X64, true);
}
TEST(UnaryIdentitySource, NativeX64NegationsPreserveArithmeticFlags) {
#if defined(__linux__) && defined(__x86_64__)
  checkUnarySource(Arch::X64, true, true);
#else
  GTEST_SKIP() << "original x64 flag execution requires a Linux x64 host";
#endif
}
TEST(UnaryIdentitySource, ARM64NegationsPreserveWordsAndNarrowing) {
  checkUnarySource(Arch::AArch64, true);
}
} // namespace
