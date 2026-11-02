//===- X87WorkloadTests.cpp - Deterministic x87 batch replay
//---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replay bounded raw-binary80 streams through native and generated loops.
/// Timing is a separate opt-in run of the retained, verified executables.
//===----------------------------------------------------------------------===//

#include "../../LLVMHostFixture.h"
#include "X87ProjectionFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <vector>

using namespace neverd;

namespace {
std::vector<uint8_t> batchProgram(Arch A, bool Transport, bool Microsoft) {
  // One packet pointer, i386 cdecl or x64 System V/Microsoft. The loop has an
  // empty x87 stack at every backedge; each record has independent sticky
  // status.
  std::vector<uint8_t> Bytes =
      A == Arch::X86
          ? std::vector<uint8_t>{0x8b, 0x54, 0x24, 4}
          : std::vector<uint8_t>{0x48, 0x89, uint8_t(Microsoft ? 0xca : 0xfa)};
  Bytes.insert(Bytes.end(), {0xdb, 0xe3,    // FNINIT
                             0xd9, 0x6a, 4, // FLDCW [DX+4]
                             0x8b, 0x0a});  // MOV ECX,[DX]
  if (A == Arch::X64)
    Bytes.push_back(0x48);
  Bytes.insert(Bytes.end(), {0x83, 0xc2, 16, 0x85, 0xc9, 0x74, 0});
  const auto EmptyJump = Bytes.size() - 1;
  const auto Loop = Bytes.size();
  Bytes.insert(Bytes.end(), {0xdb, 0xe2, 0xdb, 0x2a}); // FNCLEX; FLD m80
  if (!Transport)
    Bytes.insert(Bytes.end(),
                 {0xd9, 0xfe,                          // FSIN
                  0xdf, 0xe0,                          // FNSTSW AX
                  0x25, 0x3f, 6,    0,  0,             // AND EAX,063f
                  0x66, 0x89, 0x42, 28, 0xf6, 0xc4, 4, // TEST AH,4 (C2)
                  0x0f, 0x95, 0x42, 30});              // SETNE [DX+30]
  Bytes.insert(Bytes.end(), {0xdb, 0x7a, 16});         // FSTP [DX+16]
  if (A == Arch::X64)
    Bytes.push_back(0x48);
  Bytes.insert(Bytes.end(), {0x83, 0xc2, 32, 0xff, 0xc9, 0x75, 0});
  Bytes.back() = uint8_t(int(Loop) - int(Bytes.size()));
  Bytes[EmptyJump] = uint8_t(Bytes.size() - (EmptyJump + 1));
  Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3}); // XOR EAX,EAX; RET
  return Bytes;
}

BinaryImage batchImage(Arch A, bool Owned, bool Microsoft,
                       const std::vector<uint8_t> &Bytes) {
  BinaryImage Image;
  Image.Arch = A;
  Image.Bits = A == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Format = Microsoft ? BinaryFormat::COFF : BinaryFormat::ELF;
  Image.X87ContextOverride = Owned;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = Bytes;
  Text.Size = Text.FileSz = Bytes.size();
  Image.Segments.push_back(Text);
  auto Symbol = Symbol::makeFunc(Image.Entry, Bytes.size());
  Symbol.Name = "batch_probe";
  Image.Symbols.push_back(Symbol);
  Image.KnownCodeRanges.emplace_back(Image.Entry, Image.Entry + Bytes.size());
  return Image;
}
} // namespace

class X87Workload : public X87ProjectionFixture {
protected:
  void check(Arch A, bool Owned, bool Microsoft = false);
};

void X87Workload::check(Arch A, bool Owned, bool Microsoft) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires native Linux x64 and i386 C runtimes";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang x86 targets";
  for (bool Transport : {false, true}) {
    SCOPED_TRACE(Transport);
    selectCase(A, unsigned(Transport) + 2 * unsigned(Owned));
    const auto Bytes = batchProgram(A, Transport, Microsoft);
    auto Image = batchImage(A, Owned, Microsoft, Bytes);
    const std::string Target = A == Arch::X86 ? "--target=i386-linux-gnu"
                                              : "--target=x86_64-linux-gnu";
    const auto NativePath = tmpFile("original.s");
    std::ofstream Native(NativePath);
    Native << ".text\n.globl native_probe\nnative_probe:\n.byte ";
    for (size_t I = 0; I != Bytes.size(); ++I)
      Native << (I ? "," : "") << unsigned(Bytes[I]);
    Native << "\n.section .note.GNU-stack,\"\",@progbits\n";
    Native.close();
    const auto NativeObject = tmpFile("original.o");
    const auto NativeBuild =
        exec(NEVERD_TEST_CLANG,
             {Target, "-c", NativePath.string(), "-o", NativeObject.string()});
    ASSERT_TRUE(NativeBuild.ok()) << NativeBuild.err;
    const auto HarnessPath = tmpFile("harness.c");
    std::ifstream HarnessSource(fs::path(TEST_SOURCE_DIR) /
                                "x86_32/X87WorkloadHarness.c");
    ASSERT_TRUE(HarnessSource.good());
    std::ofstream Harness(HarnessPath);
    Harness << "#define ORIGINAL_MICROSOFT_ABI " << unsigned(Microsoft) << '\n'
            << HarnessSource.rdbuf();
    Harness.close();
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(NoOpt);
      const std::string Prefix = NoOpt ? "noopt-" : "default-";
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = true;
      Options.NoOpt = NoOpt;
      auto Lift = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Lift.Success) << Lift.Error;
      ASSERT_NE(Lift.LlvmModule, nullptr);
      ASSERT_FALSE(llvm::verifyModule(*Lift.LlvmModule, &llvm::errs()));
      const auto IRPath = tmpFile(Prefix + "generated.ll");
      std::string IR;
      llvm::raw_string_ostream OS(IR);
      neverd::test::printHostCompilerFixture(*Lift.LlvmModule, OS);
      std::ofstream(IRPath) << IR;
      CEmitterOptions Emit;
      Emit.TheArch = A;
      // The original uses the image's ABI. The generated C targets the
      // GNU/Linux host, so it needs GNU assembly and headers, not MSVC syntax.
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &Image;
      const auto LLVMCPath = tmpFile(Prefix + "llvm.c");
      std::string LLVMC;
      llvm::raw_string_ostream LOS(LLVMC);
      ASSERT_TRUE(
          LLVMCEmitter().emit(*Lift.LlvmModule, LOS, Emit, nullptr, &Image));
      std::ofstream(LLVMCPath) << LLVMC;
      Options.LiftMode = false;
      auto High = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(High.Success) << High.Error;
      const auto CPath = tmpFile(Prefix + "high.c");
      std::string C;
      llvm::raw_string_ostream COS(C);
      ASSERT_TRUE(HighCEmitter().emit(High.HighFuncs, COS, Emit));
      std::ofstream(CPath) << C;
      for (const auto &Source : {IRPath, CPath, LLVMCPath})
        for (const std::string Opt : {"-O0", "-O2"}) {
          SCOPED_TRACE(Source.string());
          SCOPED_TRACE(Opt);
          const auto Stem = Source.stem().string() + Opt;
          const auto Binary = tmpFile(Stem + ".exe");
          const auto Build =
              exec(NEVERD_TEST_CLANG,
                   {Target, Opt, "-fno-strict-aliasing", "-fuse-ld=lld",
                    "-no-pie", Source.string(), HarnessPath.string(),
                    NativeObject.string(), "-o", Binary.string()});
          ASSERT_TRUE(Build.ok()) << Build.err;
          const auto Run = exec(Binary.string(), {tmpFile(Stem).string()});
          ASSERT_EQ(Run.exitCode, 0) << Run.out << Run.err;
        }
    }
  }
#endif
}

TEST_F(X87Workload, I386TypedBatch) { check(Arch::X86, false); }
TEST_F(X87Workload, X64TypedBatch) { check(Arch::X64, false); }
TEST_F(X87Workload, X64OwnedBatch) { check(Arch::X64, true); }
TEST_F(X87Workload, MicrosoftX64TypedBatch) { check(Arch::X64, false, true); }
TEST_F(X87Workload, MicrosoftX64OwnedBatch) { check(Arch::X64, true, true); }
