//===- CallStackRangeSourceTests.cpp - Execute indexed local reads --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compare the original Win64 instructions and both generated C routes.
///
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace {
using namespace neverd;

TEST(CallStackRangeSource, IndexedLocalsMatchNativeReadsAndWrites) {
#if !defined(__x86_64__) || !defined(__linux__)
  GTEST_SKIP() << "the original Win64 instructions require Linux x64 mmap";
#endif
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0x48, 0x89, 0x4c, 0x24, 8,            // mov [rsp+8],rcx
               0x48, 0x83, 0xec, 0x28,               // sub rsp,40
               0xc7, 0x44, 0x24, 0x10, 17,  0, 0, 0, // mov [rsp+16],17
               0xc7, 0x44, 0x24, 0x14, 29,  0, 0, 0, // mov [rsp+20],29
               0x83, 0xe2, 1,                        // and edx,1
               0x8b, 0x44, 0x94, 0x10,               // mov eax,[rsp+rdx*4+16]
               0x48, 0x83, 0xc4, 0x28, 0xc3};        // add rsp,40; ret
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  auto FunctionSymbol = Symbol::makeFunc(Image.Entry);
  FunctionSymbol.Name = "indexed_local_probe";
  FunctionSymbol.Size = Image.Segments.front().Size;
  Image.Symbols.push_back(std::move(FunctionSymbol));
  const auto Original = Image.Segments.front().Data;
  for (bool Write : {false, true})
    for (bool NoOpt : {false, true})
      for (bool LLVM : {false, true})
        for (bool Named : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << Write << ':' << NoOpt << ':' << LLVM << ':' << Named);
          auto &Code = Image.Segments.front().Data;
          Code = Original;
          if (Write) {
            Code.resize(28);
            const uint8_t Tail[] = {
                0xc7, 0x44, 0x94, 0x10, 43,  0, 0, 0, // mov [rsp+rdx*4+16],43
                0x8b, 0x44, 0x24, 0x10,               // mov eax,[rsp+16]
                0x03, 0x44, 0x24, 0x14,               // add eax,[rsp+20]
                0x48, 0x83, 0xc4, 0x28, 0xc3};        // add rsp,40; ret
            Code.insert(Code.end(), std::begin(Tail), std::end(Tail));
          }
          Image.Segments.front().Size = Image.Segments.front().FileSz =
              Code.size();
          Image.Symbols.front().Size = Code.size();
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.SourceProjection = true;
          Options.NoOpt = NoOpt;
          Options.LiftMode = LLVM;
          Options.OnlyFunctionEntries = {Image.Entry};
          auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          ASSERT_TRUE(Result.CallEntryReadGPRs.count(Image.Entry));
          EXPECT_EQ(Result.CallEntryReadGPRs.at(Image.Entry)[x86reg::RCX / 8],
                    0u);
          EXPECT_EQ(Result.CallEntryStackArgs.at(Image.Entry), 0);
          std::string Source = "#define _GNU_SOURCE\n#include <stdint.h>\n";
          llvm::raw_string_ostream Output(Source);
          CEmitterOptions COptions;
          COptions.EmitIncludes = false;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          const std::map<int64_t, CEmitterOptions::FrameVariable> FrameNames = {
              {-24, {"first_element", NdType::makeInt(4, false)}},
              {-20, {"second_element", NdType::makeInt(4, false)}}};
          if (Named)
            COptions.FrameVariables = &FrameNames;
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            ASSERT_TRUE(
                LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
          } else
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Output, COptions));
          EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
          Output << "\n#include <sys/mman.h>\n#include <string.h>\n"
                    "static const unsigned char original[] = {";
          for (uint8_t Byte : Image.Segments.front().Data)
            Output << unsigned(Byte) << ',';
          Output << "\n};\n#define WRITE_CASE " << unsigned(Write) << "\n";
          Output << R"C(
int main(void) {
    void *code = mmap(0, sizeof(original), PROT_READ|PROT_WRITE|PROT_EXEC,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 1;
    memcpy(code, original, sizeof(original));
    __builtin___clear_cache((char *)code, (char *)code + sizeof(original));
    typedef uint64_t (__attribute__((ms_abi)) *native_fn)(uint64_t, uint64_t);
    for (uint64_t i = 0; i != 1024; ++i) {
        uint64_t salt = i * UINT64_C(0x9e3779b97f4a7c15);
        uint64_t index = i ^ UINT64_C(0xfedcba9876543200);
        uint64_t expected = WRITE_CASE ? (index & 1 ? 60 : 72) : (index & 1 ? 29 : 17);
        if (((native_fn)code)(salt, index) != expected) return 2;
        if ((uint64_t)indexed_local_probe(salt, index) != expected) return 3;
    }
    return munmap(code, sizeof(original)) != 0;
}
)C";
          for (const char *Optimization : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                Source, {Optimization, "-fsanitize=undefined",
                         "-fno-sanitize-recover=all"});
        }
}
} // namespace
