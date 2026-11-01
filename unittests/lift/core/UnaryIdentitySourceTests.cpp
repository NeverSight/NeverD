//===- UnaryIdentitySourceTests.cpp - Execute complement identities -------===//
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

BinaryImage complementImage(Arch Architecture, BinaryFormat Format,
                            unsigned Count, bool Narrow) {
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
      Text.Data.insert(Text.Data.end(), {0x48, 0xf7, 0xd0}); // not rax
      if (Narrow && I == 0)
        Text.Data.insert(Text.Data.end(), {0x89, 0xc0}); // mov eax,eax
    }
    Text.Data.push_back(0xc3);
  } else {
    for (unsigned I = 0; I < Count; ++I) {
      Text.Data.insert(Text.Data.end(), {0xe0, 0x03, 0x20, 0xaa}); // mvn x0,x0
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

std::string oracle(const BinaryImage &Image, unsigned Count, bool Narrow) {
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  Out << "\n#include <stdint.h>\n#include <string.h>\n";
  const bool Native = Image.Arch == Arch::X64;
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
    Out << "expected = ~expected;\n";
    if (Narrow && I == 0)
      Out << "expected = (uint32_t)expected;\n";
  }
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

void checkComplements(Arch Architecture) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "original x64 execution requires a Linux x64 host";
#else
  for (auto Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (unsigned Count : {2u, 3u, 4u})
      for (bool Narrow : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << unsigned(Architecture) << ':' << unsigned(Format) << ':'
                     << Count << ':' << Narrow);
        auto Image = complementImage(Architecture, Format, Count, Narrow);
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
            if (Optimized && !Narrow)
              EXPECT_EQ(Source.find("~~"), std::string::npos) << Source;
            EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos)
                << Source;
            for (const auto *Optimization : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  Source + oracle(Image, Count, Narrow),
                  {Optimization, "-D_GNU_SOURCE=1", "-fsanitize=undefined",
                   "-fno-sanitize-recover=all"});
          }
        }
      }
#endif
}
TEST(UnaryIdentitySource, X64ComplementsPreserveOriginalWordsAndNarrowing) {
  checkComplements(Arch::X64);
}
TEST(UnaryIdentitySource, ARM64ComplementsPreserveWordsAndNarrowing) {
  checkComplements(Arch::AArch64);
}
} // namespace
