//===- RewriteCodegenInplaceRTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// In-place rewriter and binary-patcher plumbing: Thumb tail-patch safety and the trampoline/mapping accounting reported back to callers.  Uses stub RelocResolver / BinaryPatcher / InplaceRewriter implementations rather than real binaries.
//
//===----------------------------------------------------------------------===//


#include "RewriteCodegenHarness.h"

#include "UnicornSemanticFixture.h"

using namespace neverd;
using namespace rwcg;

namespace {

class TestRelocResolver final : public RelocResolver {
public:
  bool parse(const std::vector<uint8_t> &, Arch) override { return true; }
};

class TestBinaryPatcher final : public BinaryPatcher {
public:
  uint64_t plannedExecSegmentVA(const std::vector<uint8_t> &, Arch) override {
    return 0x2000;
  }

  uint64_t appendExecSegment(std::vector<uint8_t> &Binary,
                             llvm::ArrayRef<uint8_t> Code, llvm::StringRef,
                             Arch) override {
    Binary.insert(Binary.end(), Code.begin(), Code.end());
    return 0x2000;
  }
};

class TestInplaceRewriter final : public InplaceRewriter {
public:
  static PatchResult writeSyntheticResult(
      const std::filesystem::path &OutputPath, size_t MappingCount,
      size_t TrampolineCount) {
    RewriteState State;
    State.Binary.assign(4, 0xaa);
    State.Mappings.resize(MappingCount);
    State.TrampolineCount = TrampolineCount;
    return writeResult(OutputPath, State, false);
  }

protected:
  BinaryFormat getBinaryFormat() const override { return BinaryFormat::ELF; }

  bool parseTextSection(const std::vector<uint8_t> &, const BinaryImage &,
                        TextLayout &TL) override {
    TL.SectionFileoff = 0;
    TL.SectionVA = 0x1000;
    TL.SectionSize = 64;
    return true;
  }

  std::unique_ptr<RelocResolver> createRelocResolver() const override {
    return std::make_unique<TestRelocResolver>();
  }

  std::unique_ptr<BinaryPatcher> createBinaryPatcher() const override {
    return std::make_unique<TestBinaryPatcher>();
  }
};

struct InplaceRunResult {
  PatchResult Result;
  bool OutputExists = false;
};

static InplaceRunResult runInplaceWithSpan(uint64_t OrigSize, Arch TargetArch,
                                           InstructionMode Mode,
                                           const char *Triple) {
  ensureLLVMTargets();

  llvm::SmallString<128> InputPath;
  if (auto EC = llvm::sys::fs::createTemporaryFile(
          "neverd-inplace", "bin", InputPath)) {
    ADD_FAILURE() << "cannot create temporary input: " << EC.message();
    return {};
  }
  llvm::FileRemover RemoveInput(InputPath);
  std::string OutputPath = InputPath.str().str() + ".patched";
  llvm::FileRemover RemoveOutput(OutputPath);

  std::error_code EC;
  llvm::raw_fd_ostream OS(InputPath, EC, llvm::sys::fs::OF_None);
  if (EC) {
    ADD_FAILURE() << "cannot write temporary input: " << EC.message();
    return {};
  }
  std::vector<uint8_t> InputBytes(64, 0xaa);
  OS.write(reinterpret_cast<const char *>(InputBytes.data()),
           InputBytes.size());
  OS.close();

  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, Triple);
  BinaryImage Image;
  Image.Arch = TargetArch;
  Image.Mode = Mode;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = TargetArch == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Symbol Sym;
  Sym.Name = "sum_to";
  Sym.Addr = 0x1000;
  Sym.Size = OrigSize;
  Sym.IsFunc = true;
  Image.Symbols.push_back(std::move(Sym));

  TestInplaceRewriter Rewriter;
  PatchResult Result = Rewriter.rewrite(InputPath.str().str(), OutputPath, *Mod,
                                        Image, TargetArch);
  return {std::move(Result), llvm::sys::fs::exists(OutputPath)};
}

} // namespace

TEST(BinaryPatcher_ThumbSafety, ZeroSizeTailWithoutSuccessorIsSkipped) {
  std::vector<uint8_t> Binary(16, 0xaa);
  const std::vector<uint8_t> Original = Binary;
  std::map<std::string, uint64_t> NewSymbols{{"sub_1000", 0x1100}};
  std::vector<Symbol> Symbols{Symbol::makeFunc(0x1000)};

  EXPECT_EQ(BinaryPatcher::installTrampolines(
                Binary, NewSymbols, 0x1000, 16, 0, 0, Arch::ARM,
                InstructionMode::Thumb, &Symbols),
            0u);
  EXPECT_EQ(Binary, Original);
}

TEST(BinaryPatcher_ThumbSafety, ShortTailBeforeSentinelIsSkipped) {
  std::vector<uint8_t> Binary(16, 0xaa);
  const std::vector<uint8_t> Original = Binary;
  std::map<std::string, uint64_t> NewSymbols{{"sub_1000", 0x1100}};
  std::vector<Symbol> Symbols{Symbol::makeFunc(0x1000),
                              Symbol::makeFunc(0x1002)};

  EXPECT_EQ(BinaryPatcher::installTrampolines(
                Binary, NewSymbols, 0x1000, 16, 0, 0, Arch::ARM,
                InstructionMode::Thumb, &Symbols),
            0u);
  EXPECT_EQ(Binary, Original);
}

TEST(InplaceRewriter_ThumbSafety, ShortGrowerSpanFailsWithoutOutput) {
  InplaceRunResult Run = runInplaceWithSpan(
      2, Arch::ARM, InstructionMode::Thumb,
      "thumbv7-unknown-linux-gnueabihf");
  EXPECT_FALSE(Run.Result.Success);
  EXPECT_EQ(Run.Result.TrampolineCount, 0u);
  EXPECT_FALSE(Run.OutputExists);
}

TEST(InplaceRewriter_ThumbSafety, InstalledGrowerReportsOneTrampoline) {
  InplaceRunResult Run = runInplaceWithSpan(
      4, Arch::ARM, InstructionMode::Thumb,
      "thumbv7-unknown-linux-gnueabihf");
  ASSERT_TRUE(Run.Result.Success);
  EXPECT_EQ(Run.Result.TrampolineCount, 1u);
  EXPECT_TRUE(Run.OutputExists);
}

TEST(InplaceRewriter_ResultAccuracy, TrampolineCountIsNotMappingCount) {
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile(
      "neverd-inplace-result", "bin", OutputPath));
  llvm::FileRemover RemoveOutput(OutputPath);

  PatchResult Result =
      TestInplaceRewriter::writeSyntheticResult(OutputPath.str().str(), 2, 1);
  ASSERT_TRUE(Result.Success);
  EXPECT_EQ(Result.TrampolineCount, 1u);
}
