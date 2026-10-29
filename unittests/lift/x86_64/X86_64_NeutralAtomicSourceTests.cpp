//===- X86_64_NeutralAtomicSourceTests.cpp - Neutral RMW source -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute generated neutral locked operations with live logical flags.
//===----------------------------------------------------------------------===//

#include "../core/SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>
#include <utility>

namespace {
using namespace neverd;

TEST(X86NeutralAtomicSource, BothRoutesKeepMemoryAndAllDefinedLogicalFlags) {
#if !defined(__x86_64__) && !defined(_M_X64)
  GTEST_SKIP() << "executing x64 atomic source requires an x64 host";
#endif
  for (auto Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (unsigned Width : {1u, 2u, 4u, 8u})
      for (unsigned Group : {1u, 4u, 6u})
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(testing::Message() << unsigned(Format) << '/' << Width
                                          << '/' << Group << '/' << LLVM);
          BinaryImage Image;
          Image.Arch = Arch::X64;
          Image.Bits = Bitness::Bits64;
          Image.Format = Format;
          Image.Base = Image.Entry = 0x1000;
          Segment Text;
          Text.VA = Image.Entry;
          Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
          Text.Data = {0xf0};
          if (Width == 2)
            Text.Data.push_back(0x66);
          if (Width == 8)
            Text.Data.push_back(0x48);
          Text.Data.push_back(Width == 1 ? 0x80 : 0x83);
          Text.Data.push_back(static_cast<uint8_t>(
              (Group << 3) | (Format == BinaryFormat::COFF ? 1 : 7)));
          Text.Data.push_back(Group == 4 ? 0xff : 0);
          // Capture ZF, SF, PF, CF and OF before any arithmetic changes them.
          // AF is undefined by logic and deliberately is not an observation.
          Text.Data.insert(
              Text.Data.end(),
              {0x0f, 0x94, 0xc0, 0x0f, 0x98, 0xc2, 0x41, 0x0f, 0x9a, 0xc0, 0x41,
               0x0f, 0x92, 0xc1, 0x41, 0x0f, 0x90, 0xc2, 0x0f, 0xb6, 0xc0, 0x0f,
               0xb6, 0xd2, 0x45, 0x0f, 0xb6, 0xc0, 0x45, 0x0f, 0xb6, 0xc9, 0x45,
               0x0f, 0xb6, 0xd2, 0xd1, 0xe2, 0x41, 0xc1, 0xe0, 2,    0x41, 0xc1,
               0xe1, 3,    0x41, 0xc1, 0xe2, 4,    0x09, 0xd0, 0x44, 0x09, 0xc0,
               0x44, 0x09, 0xc8, 0x44, 0x09, 0xd0, 0xc3});
          Text.Size = Text.FileSz = Text.Data.size();
          auto Function = Symbol::makeFunc(Image.Entry, Text.Size);
          Function.Name = "neutral_logic";
          Image.Symbols.push_back(std::move(Function));
          Image.Segments.push_back(std::move(Text));
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.SourceProjection = true;
          Options.LiftMode = LLVM;
          Options.OnlyFunctionEntries = {Image.Entry};
          llvm::LLVMContext Context;
          auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          CEmitterOptions COptions;
          COptions.TheArch = Arch::X64;
          COptions.Format = Format;
          COptions.Image = &Image;
          std::string Source;
          llvm::raw_string_ostream Output(Source);
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            ASSERT_TRUE(
                LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
          } else
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Output, COptions));
          Output.flush();
          const auto Oracle = "\n#include <string.h>\n#define WIDTH " +
                              std::to_string(Width) + R"C(
int main(void) {
  const uint64_t sign = UINT64_C(1) << (WIDTH * 8 - 1);
  const uint64_t mask = UINT64_MAX >> (64 - WIDTH * 8);
  const uint64_t values[] = {0, 1, 15, 16, sign - 1, sign, mask};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    for (unsigned offset = 0; offset < 8; ++offset) {
      unsigned char storage[32], before[32];
      memset(storage, 0xa5, sizeof(storage));
      memcpy(storage + offset, &values[i], WIDTH);
      memcpy(before, storage, sizeof(storage));
      const unsigned expected = (values[i] == 0) |
          ((values[i] & sign) != 0) << 1 |
          (!__builtin_parity((unsigned char)values[i])) << 2;
      if (neutral_logic(storage + offset) != expected)
        return 1;
      if (memcmp(storage, before, sizeof(storage)))
        return 2;
    }
  return 0;
}
)C";
          for (const auto *Optimization : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                Source + Oracle,
                {Optimization, "-fsanitize=undefined", "-fsanitize-trap=all"});
        }
}
} // namespace
