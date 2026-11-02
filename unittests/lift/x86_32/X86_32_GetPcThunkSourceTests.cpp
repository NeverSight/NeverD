//===- X86_32_GetPcThunkSourceTests.cpp - Preserved get-PC helper bodies
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "NeverDLiftFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/loader/X86/X86GetPcThunk.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <stdexcept>

using namespace neverd;
namespace {
class X86GetPcSource : public NeverDLiftTest {};

TEST_F(X86GetPcSource, RecompiledBodiesPreserveEverySelectedRegisterAndFlags) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang's i386 target";
  const auto Assembly = tmpFile("helpers.s");
  const auto Object = tmpFile("helpers.o");
  std::ofstream Asm(Assembly);
  for (unsigned Reg : {0u, 1u, 2u, 3u, 5u, 6u, 7u}) {
    const auto Name = "getpc_" + std::to_string(Reg);
    Asm << ".globl " << Name << "\n.type " << Name << ",@function\n"
        << Name << ":\n.rept 8\nnop\n.endr\n"
        << x86GetPcThunkAssembly(Reg) << "\n.size " << Name << ",.-" << Name
        << "\n";
  }
  Asm.close();
  auto Compile =
      exec(NEVERD_TEST_CLANG, {"--target=i386-linux-gnu", "-c",
                               Assembly.string(), "-o", Object.string()});
  ASSERT_TRUE(Compile.ok()) << Compile.err;
  auto Image = loadBinary(Object);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;

  for (unsigned Reg : {0u, 1u, 2u, 3u, 5u, 6u, 7u}) {
    auto *Symbol = Image->findSymbol("getpc_" + std::to_string(Reg));
    ASSERT_NE(Symbol, nullptr);
    EXPECT_EQ(x86GetPcThunkRegister(*Image, Symbol->Addr), Reg);
    Options.OnlyFunctionEntries.insert(Symbol->Addr);
  }
  auto Result = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.HighFuncs.size(), 7u);
  Options.LiftMode = true;
  Options.SourceProjection = true;
  for (bool NoOpt : {false, true}) {
    SCOPED_TRACE(NoOpt);
    Options.NoOpt = NoOpt;
    auto LLVMResult = Pipeline().run(*Image, Context, Options);
    ASSERT_TRUE(LLVMResult.Success) << LLVMResult.Error;
    ASSERT_NE(LLVMResult.LlvmModule, nullptr);
    ASSERT_FALSE(llvm::verifyModule(*LLVMResult.LlvmModule, &llvm::errs()));
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(Route);
      std::string Text;
      llvm::raw_string_ostream Stream(Text);
      CEmitterOptions Emit;
      Emit.Image = &*Image;
      Emit.TheArch = Arch::X86;
      Emit.Format = BinaryFormat::ELF;
      Emit.EmitIncludes = false;
      if (Route < 2)
        Stream << "#include <stdint.h>\n";
      if (Route == 0)
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Stream, Emit));
      else if (Route == 1)
        ASSERT_TRUE(LLVMCEmitter().emit(*LLVMResult.LlvmModule, Stream, Emit,
                                        nullptr, &*Image));
      else
        LLVMResult.LlvmModule->print(Stream, nullptr);
      const auto Source = tmpFile("roundtrip" + std::to_string(Route) +
                                  (Route == 2 ? ".ll" : ".c"));
      const auto Rebuilt = tmpFile("roundtrip" + std::to_string(Route) + ".o");
      std::ofstream(Source) << Text;
      const auto Build =
          exec(NEVERD_TEST_CLANG,
               {"--target=i386-linux-gnu", "-O2", "-ffreestanding", "-c",
                Source.string(), "-o", Rebuilt.string()});
      ASSERT_TRUE(Build.ok()) << Build.err << Text;
      auto Roundtrip = loadBinary(Rebuilt);
      ASSERT_TRUE(bool(Roundtrip)) << llvm::toString(Roundtrip.takeError());
      for (unsigned Reg : {0u, 1u, 2u, 3u, 5u, 6u, 7u}) {
        auto *Symbol = Roundtrip->findSymbol("getpc_" + std::to_string(Reg));
        ASSERT_NE(Symbol, nullptr) << Text;
        const auto *Bytes = Roundtrip->readVA(Symbol->Addr, 4);
        ASSERT_NE(Bytes, nullptr);
        // Exact machine bytes prove stack behavior, the selected destination,
        // and preservation of every other register and the arithmetic flags.
        EXPECT_EQ(Bytes[0], 0x8b);
        EXPECT_EQ(Bytes[1], 0x04 | (Reg << 3));
        EXPECT_EQ(Bytes[2], 0x24);
        EXPECT_EQ(Bytes[3], 0xc3);
      }
    }
    // Metadata cannot conceal an edited or simplified-away assembly body.
    LLVMResult.LlvmModule->getFunction("getpc_0")
        ->getEntryBlock()
        .front()
        .eraseFromParent();
    CEmitterOptions Emit;
    Emit.TheArch = Arch::X86;
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    EXPECT_THROW(LLVMCEmitter().emit(*LLVMResult.LlvmModule, Stream, Emit),
                 std::invalid_argument);
  }
}

TEST_F(X86GetPcSource, NamesAndNearMatchesDoNotCertifyAHelper) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = 16;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = {0x8b, 0x44, 0x24, 0x04, 0xc3}; // Reads an argument, not the PC.
  Image.Segments.push_back(Code);
  EXPECT_FALSE(x86GetPcThunkRegister(Image, Code.VA));
  Image.Segments[0].Data = {0x8b, 0x24, 0x24, 0xc3}; // Overwrites ESP.
  EXPECT_FALSE(x86GetPcThunkRegister(Image, Code.VA));
  Image.Segments[0].Data = {0x8b, 0x04, 0x24, 0xc3};
  Image.Arch = Arch::X64;
  EXPECT_FALSE(x86GetPcThunkRegister(Image, Code.VA));
}
} // namespace
