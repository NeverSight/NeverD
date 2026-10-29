//===- ConstantBranchSourceTests.cpp - Direct-edge state in source C -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute both source routes after an exact dispatch crosses a direct edge.
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
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

BinaryImage dispatchImage(Arch Architecture, BinaryFormat Format, bool Join) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Image.Format = Format;
  Image.Base = 0x1000;
  Image.Entry = 0x1000;
  const unsigned Continuation = Join ? 0x20 : 0x10;
  const unsigned Destination = Join ? 0x50 : 0x30;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (Architecture == Arch::X64) {
    if (Join) {
      Text.Data = {0x85, uint8_t(Format == BinaryFormat::COFF ? 0xc9 : 0xff),
                   0x74, 0x0c, // test arg,arg; jz 0x1010
                   0x4c, 0x8d,
                   0x1d, 0x35,
                   0,    0,
                   0,           // lea r11,0x1040
                   0xeb, 0x13}; // jmp 0x1020
      Text.Data.resize(0x10, 0xcc);
      Text.Data.insert(Text.Data.end(),
                       {0x4c, 0x8d, 0x1d, 0x29, 0, 0, 0, 0xeb, 7});
    } else
      Text.Data = {0x4c, 0x8d, 0x1d, 0x19, 0, 0, 0, // lea r11,0x1020
                   0xeb, 7};                        // jmp 0x1010
    Text.Data.resize(Continuation, 0xcc);
    Text.Data.insert(Text.Data.end(), {0x49, 0x83, 0xc3, 0x10, // add r11,16
                                       0x41, 0xff, 0xe3});     // jmp r11
    Text.Data.resize(Destination, 0xcc);
    Text.Data.insert(Text.Data.end(),
                     {0x89, uint8_t(Format == BinaryFormat::COFF ? 0xc8 : 0xf8),
                      0x83, 0xf0, 0x5a, 0xc3}); // mov eax,arg; xor eax,90; ret
  } else {
    if (Join) {
      Text.Data = {0x80, 0x00, 0x00, 0xb4,  // cbz x0,0x1010
                   0xf0, 0x01, 0x00, 0x10,  // adr x16,0x1040
                   0x06, 0x00, 0x00, 0x14}; // b 0x1020
      Text.Data.resize(0x10, 0);
      Text.Data.insert(Text.Data.end(),
                       {0x90, 0x01, 0x00, 0x10,   // adr x16,0x1040
                        0x03, 0x00, 0x00, 0x14}); // b 0x1020
    } else
      Text.Data = {0x10, 0x01, 0x00, 0x10,  // adr x16,0x1020
                   0x03, 0x00, 0x00, 0x14}; // b 0x1010
    Text.Data.resize(Continuation, 0);
    Text.Data.insert(Text.Data.end(), {0x10, 0x42, 0x00, 0x91, // add x16,x16,16
                                       0x00, 0x02, 0x1f, 0xd6}); // br x16
    Text.Data.resize(Destination, 0);
    // Use a scratch register outside the ABI's two-register return carrier.
    Text.Data.insert(Text.Data.end(), {0x49, 0x0b, 0x80, 0x52,   // mov w9,90
                                       0x00, 0x00, 0x09, 0x4a,   // eor w0,w0,w9
                                       0xc0, 0x03, 0x5f, 0xd6}); // ret
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  auto Function = Symbol::makeFunc(Image.Entry);
  Function.Name = "dispatch_probe";
  Function.Size = Image.Segments.front().Size;
  Image.Symbols.push_back(std::move(Function));
  return Image;
}

TEST(ConstantBranchSource, DirectAndJoinedEdgesKeepTheTargetInsideItsFunction) {
  for (bool Join : {false, true})
    for (auto Architecture : {Arch::X64, Arch::AArch64})
      for (auto Format :
           {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
        for (bool LLVM : {false, true})
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << unsigned(Architecture) << ':' << unsigned(Format)
                         << ':' << LLVM << ':' << NoOpt << ':' << Join);
            auto Image = dispatchImage(Architecture, Format, Join);
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.SourceProjection = true;
            Options.LiftMode = LLVM;
            Options.NoOpt = NoOpt;
            Options.OnlyFunctionEntries = {Image.Entry};
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            // Execute the scalar body on the host without requiring target
            // platform headers (notably Windows intrin.h on Linux).
            std::string Source = "#include <stdint.h>\n#include <string.h>\n";
            llvm::raw_string_ostream Output(Source);
            CEmitterOptions COptions;
            COptions.EmitIncludes = false;
            COptions.TheArch = Architecture;
            COptions.Format = Format;
            COptions.Image = &Image;
            if (LLVM) {
              ASSERT_TRUE(Result.LlvmModule);
              ASSERT_FALSE(
                  llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
              ASSERT_TRUE(
                  LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
            } else
              ASSERT_TRUE(
                  HighCEmitter().emit(Result.HighFuncs, Output, COptions));
            ASSERT_NE(Source.find("dispatch_probe("), std::string::npos)
                << Source;
            EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos)
                << Source;
            for (const auto *Optimization : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  Source + R"C(
int main(void) {
    const uint32_t values[] = {0, 1, 90, 255, 0x80000000u, 0xffffffffu};
    for (unsigned i = 0; i != sizeof(values) / sizeof(values[0]); ++i)
        if ((uint32_t)dispatch_probe(values[i]) != (values[i] ^ 90u))
            return 1;
    return 0;
}
)C",
                  {Optimization, "-fsanitize=undefined",
                   "-fno-sanitize-recover=all"});
          }
}
} // namespace
