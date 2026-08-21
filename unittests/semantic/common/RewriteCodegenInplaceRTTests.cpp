//===- RewriteCodegenInplaceRTTests.cpp -----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// In-place rewriter and binary-patcher plumbing: Thumb tail-patch safety and
// the trampoline/mapping accounting reported back to callers.  Uses stub
// RelocResolver / BinaryPatcher / InplaceRewriter implementations rather than
// real binaries.
//
//===----------------------------------------------------------------------===//

#include "RewriteCodegenHarness.h"
#include "UnicornSemanticFixture.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryUtils.h"

using namespace neverd;
using namespace rwcg;

namespace {

class TestRelocResolver final : public RelocResolver {
public:
  bool parse(const std::vector<uint8_t> &, Arch) override { return true; }
};

struct TestPatcherStats {
  size_t PatchCalls = 0;
  size_t AppendCalls = 0;
  bool PatchHadImageContext = false;
};

class TestBinaryPatcher final : public BinaryPatcher {
public:
  static constexpr size_t FormatPatchCodeSize = 0x4558;

  explicit TestBinaryPatcher(TestPatcherStats *Stats = nullptr)
      : Stats(Stats) {}

  static bool validatePatchPlan(const CompiledImage &Compiled,
                                const BinaryImage *Image, std::string &Detail) {
    return validateSourceFunctionPatchPlan(Compiled, Image, Detail);
  }

  static SourceTrampolinePlan makePatchPlan(const CompiledImage &Compiled,
                                            const BinaryImage *Image) {
    return makeSourceTrampolinePlan(Compiled, Image);
  }

  static bool prepareSources(llvm::Module &Module, const BinaryImage *Image,
                             SourceFunctionPreparation &Preparation,
                             std::string &Detail) {
    return prepareSourceFunctionsForPatch(Module, Image, Preparation, Detail);
  }

  PatchResult patch(const std::filesystem::path &,
                    const std::filesystem::path &, llvm::Module &,
                    Arch) override {
    if (Stats) {
      ++Stats->PatchCalls;
      Stats->PatchHadImageContext = CachedImage != nullptr;
    }
    PatchResult Result;
    Result.Success = true;
    Result.CodeSize = FormatPatchCodeSize;
    return Result;
  }

  uint64_t plannedExecSegmentVA(const std::vector<uint8_t> &, Arch) override {
    return 0x2000;
  }

  uint64_t appendExecSegment(std::vector<uint8_t> &Binary,
                             llvm::ArrayRef<uint8_t> Code, llvm::StringRef,
                             Arch) override {
    if (Stats)
      ++Stats->AppendCalls;
    Binary.insert(Binary.end(), Code.begin(), Code.end());
    return 0x2000;
  }

private:
  TestPatcherStats *Stats = nullptr;
};

class TestInplaceRewriter final : public InplaceRewriter {
public:
  explicit TestInplaceRewriter(TestPatcherStats *Stats = nullptr)
      : Stats(Stats) {}

  static PatchResult
  writeSyntheticResult(const std::filesystem::path &OutputPath,
                       size_t MappingCount, size_t TrampolineCount) {
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
    return std::make_unique<TestBinaryPatcher>(Stats);
  }

private:
  TestPatcherStats *Stats = nullptr;
};

struct InplaceRunResult {
  PatchResult Result;
  bool OutputExists = false;
};

static InplaceRunResult runInplaceWithSpan(uint64_t OrigSize, Arch TargetArch,
                                           InstructionMode Mode,
                                           const char *Triple,
                                           TestPatcherStats *Stats = nullptr,
                                           bool HasExceptionMetadata = false) {
  ensureLLVMTargets();

  llvm::SmallString<128> InputPath;
  if (auto EC = llvm::sys::fs::createTemporaryFile("neverd-inplace", "bin",
                                                   InputPath)) {
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
  if (HasExceptionMetadata) {
    ExceptionFunction EH;
    EH.CodeRange = {0x1000, 0x1000 + OrigSize};
    Image.ExceptionMetadata.Functions.push_back(std::move(EH));
    Image.ExceptionMetadata.rebuildIndex();
  }

  TestInplaceRewriter Rewriter(Stats);
  PatchResult Result = Rewriter.rewrite(InputPath.str().str(), OutputPath, *Mod,
                                        Image, TargetArch);
  return {std::move(Result), llvm::sys::fs::exists(OutputPath)};
}

static std::unique_ptr<llvm::Module>
buildDefinedFunctionAddressIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto Module = std::make_unique<llvm::Module>("defined-function-address", Ctx);
  Module->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *Function = llvm::Function::Create(llvm::FunctionType::get(I32, false),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "return_self_address", Module.get());
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Ctx, "entry", Function));
  Builder.CreateRet(llvm::ConstantExpr::getPtrToInt(Function, I32));
  return Module;
}

static std::optional<uint32_t>
runSymbolizedFunctionAddressCodegen(BinaryFormat Format) {
  constexpr uint32_t OriginalVA = 0x1000;
  constexpr uint32_t EmittedVA = 0x2000;
  constexpr uint32_t ReturnVA = 0xf000;

  ensureLLVMTargets();
  const char *Triple = Format == BinaryFormat::COFF
                           ? "thumbv7-pc-windows-msvc"
                           : "arm-unknown-linux-gnueabihf";
  llvm::LLVMContext Ctx;
  auto Module = buildDefinedFunctionAddressIR(Ctx, Triple);
  const std::string AddressSymbol = makeNdDataSymbol(OriginalVA);
  auto *PredeclaredAddress = new llvm::GlobalVariable(
      *Module, llvm::Type::getInt8Ty(Ctx), /*isConstant=*/false,
      llvm::GlobalValue::ExternalLinkage, /*Initializer=*/nullptr,
      AddressSymbol);
  PredeclaredAddress->setDSOLocal(false);

  BinaryImage Image;
  Image.Arch = Arch::ARM;
  Image.Mode = Format == BinaryFormat::COFF ? InstructionMode::Thumb
                                            : InstructionMode::ARM;
  Image.Format = Format;
  Image.Bits = Bitness::Bits32;

  Segment Text;
  Text.Name = ".text";
  Text.VA = OriginalVA;
  Text.Size = 64;
  Text.FileSz = 64;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xaa);
  Image.Segments.push_back(std::move(Text));

  Symbol Function = Symbol::makeFunc(OriginalVA, 64);
  Function.Name = "return_self_address";
  Image.Symbols.push_back(std::move(Function));
  EXPECT_TRUE(Image.CodeRefTargets.empty());

  symbolizeImageAbsolutePointers(*Module, Image);
  llvm::GlobalVariable *OriginalAddress =
      Module->getNamedGlobal(makeNdDataSymbol(OriginalVA));
  EXPECT_NE(OriginalAddress, nullptr);
  if (!OriginalAddress)
    return std::nullopt;
  EXPECT_TRUE(OriginalAddress->isDeclaration());
  EXPECT_TRUE(OriginalAddress->isDSOLocal());

  const uint32_t SerializedAddress =
      Format == BinaryFormat::COFF ? OriginalVA | 1u : OriginalVA;
  auto Result = compileRewriteWithVAs(
      *Module, Arch::ARM, Format, EmittedVA, EmittedVA + 0x1000,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == AddressSymbol || Symbol == "_" + AddressSymbol)
          return SerializedAddress;
        return std::nullopt;
      });
  EXPECT_TRUE(Result.Unresolved.empty());
  if (!Result.Unresolved.empty())
    return std::nullopt;
  const llvm::mc_rewrite::RewriteSection *TextSection = findTextSection(Result);
  EXPECT_NE(TextSection, nullptr);
  if (!TextSection)
    return std::nullopt;

  uc_engine *UC = nullptr;
  const uc_mode Mode =
      Format == BinaryFormat::COFF ? UC_MODE_THUMB : UC_MODE_ARM;
  if (uc_open(UC_ARCH_ARM, Mode, &UC) != UC_ERR_OK)
    return std::nullopt;
  struct UCClose {
    void operator()(uc_engine *Engine) const {
      if (Engine)
        uc_close(Engine);
    }
  };
  std::unique_ptr<uc_engine, UCClose> Close(UC);

  if (uc_mem_map(UC, EmittedVA, 0x3000, UC_PROT_ALL) != UC_ERR_OK ||
      uc_mem_map(UC, STACK_BASE, STACK_SIZE, UC_PROT_ALL) != UC_ERR_OK ||
      uc_mem_map(UC, ReturnVA, 0x1000, UC_PROT_ALL) != UC_ERR_OK)
    return std::nullopt;
  for (const auto &Section : Result.Sections)
    if (Section.IsAllocated && !Section.Bytes.empty() &&
        uc_mem_write(UC, Section.VA, Section.Bytes.data(),
                     Section.Bytes.size()) != UC_ERR_OK)
      return std::nullopt;

  uint32_t SP = uint32_t(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t LR = ReturnVA;
  uc_reg_write(UC, UC_ARM_REG_SP, &SP);
  uc_reg_write(UC, UC_ARM_REG_LR, &LR);
  uint64_t Entry = findSymbolVA(Result, "return_self_address", TextSection->VA);
  if (Format == BinaryFormat::COFF)
    Entry |= 1u;
  if (uc_emu_start(UC, Entry, ReturnVA, /*timeout=*/0, /*count=*/200) !=
      UC_ERR_OK)
    return std::nullopt;

  uint32_t R0 = 0;
  if (uc_reg_read(UC, UC_ARM_REG_R0, &R0) != UC_ERR_OK)
    return std::nullopt;
  return R0;
}

} // namespace

TEST(BinaryPatcher_ThumbSafety, ZeroSizeTailWithoutSuccessorIsSkipped) {
  std::vector<uint8_t> Binary(16, 0xaa);
  const std::vector<uint8_t> Original = Binary;
  std::map<std::string, uint64_t> NewSymbols{{"sub_1000", 0x1100}};
  std::vector<Symbol> Symbols{Symbol::makeFunc(0x1000)};

  EXPECT_EQ(BinaryPatcher::installTrampolines(Binary, NewSymbols, 0x1000, 16, 0,
                                              0, Arch::ARM,
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

  EXPECT_EQ(BinaryPatcher::installTrampolines(Binary, NewSymbols, 0x1000, 16, 0,
                                              0, Arch::ARM,
                                              InstructionMode::Thumb, &Symbols),
            0u);
  EXPECT_EQ(Binary, Original);
}

TEST(BinaryPatcher_TrampolineSafety, X86EntryBeforeAdjacentFunctionIsSkipped) {
  std::vector<uint8_t> Binary(16, 0xaa);
  const std::vector<uint8_t> Original = Binary;
  std::map<std::string, uint64_t> NewSymbols{{"sub_1000", 0x1100}};
  std::vector<Symbol> Symbols{Symbol::makeFunc(0x1000),
                              Symbol::makeFunc(0x1002)};

  EXPECT_EQ(BinaryPatcher::installTrampolines(
                Binary, NewSymbols, 0x1000, 16, 0, 0, Arch::X64,
                InstructionMode::Default, &Symbols),
            0u);
  EXPECT_EQ(Binary, Original);
}

TEST(BinaryPatcher_TrampolineSafety,
     PatchPlanPreservesCodeBoundaryAndAdjacentEntrySources) {
  CompiledImage Compiled;
  Compiled.SourceFunctionOriginalVAs["source"] = 0x1000;
  Compiled.SourceFunctionOwners.push_back({"source", "source", 0x2000, false});

  BinaryImage Image;
  Image.Format = BinaryFormat::ELF;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;

  Segment Load;
  Load.Name = "LOAD";
  Load.VA = 0x1000;
  Load.Size = 0x20;
  Load.FileSz = Load.Size;
  Load.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Load.Data.resize(Load.Size);
  Image.Segments.push_back(std::move(Load));

  auto setSections = [&](uint64_t CodeSize) {
    Image.Sections.clear();
    Section Code;
    Code.Name = ".text";
    Code.SegmentName = "LOAD";
    Code.VA = 0x1000;
    Code.Size = CodeSize;
    Code.FileSz = CodeSize;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Sections.push_back(std::move(Code));
    Section Data;
    Data.Name = ".rodata";
    Data.SegmentName = "LOAD";
    Data.VA = 0x1000 + CodeSize;
    Data.Size = 0x20 - CodeSize;
    Data.FileSz = Data.Size;
    Data.Flags = SegmentFlags::Readable;
    Image.Sections.push_back(std::move(Data));
  };

  Image.Symbols = {Symbol::makeFunc(0x1000)};
  std::string Detail;
  setSections(/*CodeSize=*/4);
  EXPECT_TRUE(TestBinaryPatcher::validatePatchPlan(Compiled, &Image, Detail))
      << Detail;
  SourceTrampolinePlan Plan =
      TestBinaryPatcher::makePatchPlan(Compiled, &Image);
  EXPECT_TRUE(Plan.OriginalVAs.empty());
  EXPECT_EQ(Plan.PreservedCount, 1u);

  setSections(/*CodeSize=*/0x10);
  Image.Symbols.push_back(Symbol::makeFunc(0x1002));
  EXPECT_TRUE(TestBinaryPatcher::validatePatchPlan(Compiled, &Image, Detail))
      << Detail;
  Plan = TestBinaryPatcher::makePatchPlan(Compiled, &Image);
  EXPECT_TRUE(Plan.OriginalVAs.empty());
  EXPECT_EQ(Plan.PreservedCount, 1u);

  Image.Symbols.resize(1);
  EXPECT_TRUE(TestBinaryPatcher::validatePatchPlan(Compiled, &Image, Detail))
      << Detail;
  Plan = TestBinaryPatcher::makePatchPlan(Compiled, &Image);
  EXPECT_EQ(Plan.OriginalVAs.size(), 1u);
  EXPECT_EQ(Plan.Owners.size(), 1u);
  EXPECT_EQ(Plan.PreservedCount, 0u);
}

TEST(BinaryPatcher_TrampolineSafety,
     ThumbPreparationClassifiesReplaceableAndExactNoOpSources) {
  llvm::LLVMContext Context;
  llvm::FunctionType *Type =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto makeModule = [&](llvm::StringRef Name) {
    auto Result = std::make_unique<llvm::Module>(Name, Context);
    llvm::Function *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "source", *Result);
    llvm::BasicBlock *Entry =
        llvm::BasicBlock::Create(Context, "entry", Function);
    llvm::ReturnInst::Create(Context, Entry);
    rewrite_source::setOriginalVA(*Function, 0x1000);
    return Result;
  };

  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::ARM;
  Image.Mode = InstructionMode::Thumb;
  Image.Bits = Bitness::Bits32;
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x10;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.SegmentName = ".text";
  Code.VA = 0x1000;
  Code.Size = 0x10;
  Code.FileSz = Code.Size;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Sections.push_back(std::move(Code));
  Image.Symbols = {Symbol::makeFunc(0x1000, 4)};

  SourceFunctionPreparation Preparation;
  std::string Detail;
  auto SafeModule = makeModule("thumb-safe-source");
  ASSERT_TRUE(TestBinaryPatcher::prepareSources(*SafeModule, &Image,
                                                Preparation, Detail))
      << Detail;
  EXPECT_TRUE(Preparation.HasExactSources);
  EXPECT_EQ(Preparation.ReplaceableOriginalVAs.at("source"), 0x1000u);
  EXPECT_TRUE(Preparation.PreservedOriginalVAs.empty());
  EXPECT_FALSE(Preparation.isExactNoOp());
  EXPECT_FALSE(SafeModule->getFunction("source")->isDeclaration());

  Image.Symbols = {Symbol::makeFunc(0x1000), Symbol::makeFunc(0x1002)};
  auto UnsafeModule = makeModule("thumb-unsafe-source");
  ASSERT_TRUE(TestBinaryPatcher::prepareSources(*UnsafeModule, &Image,
                                                Preparation, Detail))
      << Detail;
  EXPECT_EQ(Preparation.PreservedOriginalVAs.at("source"), 0x1000u);
  EXPECT_TRUE(Preparation.ReplaceableOriginalVAs.empty());
  EXPECT_TRUE(Preparation.isExactNoOp());
  EXPECT_FALSE(UnsafeModule->getFunction("source")->isDeclaration());
}

TEST(BinaryPatcher_TrampolineSafety,
     ExactNoOpIgnoresUnannotatedHelperDefinitions) {
  llvm::LLVMContext Context;
  llvm::Module Module("thumb-exact-noop-helper", Context);
  llvm::FunctionType *Type =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto makeBody = [&](llvm::StringRef Name) {
    llvm::Function *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::BasicBlock *Entry =
        llvm::BasicBlock::Create(Context, "entry", Function);
    llvm::ReturnInst::Create(Context, Entry);
    return Function;
  };
  llvm::Function *Source = makeBody("source");
  llvm::Function *Helper = makeBody("helper_without_source_identity");
  rewrite_source::setOriginalVA(*Source, 0x1000);

  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::ARM;
  Image.Mode = InstructionMode::Thumb;
  Image.Bits = Bitness::Bits32;
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x10;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.SegmentName = ".text";
  Code.VA = 0x1000;
  Code.Size = 0x10;
  Code.FileSz = Code.Size;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Sections.push_back(std::move(Code));
  Image.Symbols = {Symbol::makeFunc(0x1000), Symbol::makeFunc(0x1002)};

  SourceFunctionPreparation Preparation;
  std::string Detail;
  ASSERT_TRUE(
      TestBinaryPatcher::prepareSources(Module, &Image, Preparation, Detail))
      << Detail;
  EXPECT_TRUE(Preparation.isExactNoOp());
  EXPECT_EQ(Preparation.PreservedOriginalVAs.at("source"), 0x1000u);
  EXPECT_FALSE(Source->isDeclaration());
  EXPECT_FALSE(Helper->isDeclaration());
}

TEST(BinaryPatcher_TrampolineSafety,
     RejectsEscapingBlockAddressBeforeExternalizingSource) {
  llvm::LLVMContext Context;
  llvm::Module Module("thumb-preserved-blockaddress", Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *Type = llvm::FunctionType::get(I32, false);

  llvm::Function *Preserved = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "preserved", Module);
  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Preserved);
  llvm::BasicBlock *Target =
      llvm::BasicBlock::Create(Context, "target", Preserved);
  llvm::UncondBrInst::Create(Target, Entry);
  llvm::ReturnInst::Create(Context, llvm::ConstantInt::get(I32, 7), Target);
  rewrite_source::setOriginalVA(*Preserved, 0x1000);

  llvm::Function *Replaceable = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "replaceable", Module);
  llvm::BasicBlock *Use =
      llvm::BasicBlock::Create(Context, "entry", Replaceable);
  llvm::BlockAddress *Address = llvm::BlockAddress::get(Preserved, Target);
  llvm::Constant *AddressInt = llvm::ConstantExpr::getPtrToInt(Address, I32);
  llvm::ReturnInst *Return = llvm::ReturnInst::Create(Context, AddressInt, Use);
  rewrite_source::setOriginalVA(*Replaceable, 0x1008);

  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::ARM;
  Image.Mode = InstructionMode::Thumb;
  Image.Bits = Bitness::Bits32;
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x20;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.SegmentName = ".text";
  Code.VA = 0x1000;
  Code.Size = 0x20;
  Code.FileSz = Code.Size;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Sections.push_back(std::move(Code));
  Image.Symbols = {Symbol::makeFunc(0x1000), Symbol::makeFunc(0x1002),
                   Symbol::makeFunc(0x1008, 4)};

  SourceFunctionPreparation Preparation;
  std::string Detail;
  EXPECT_FALSE(
      TestBinaryPatcher::prepareSources(Module, &Image, Preparation, Detail));
  EXPECT_NE(Detail.find("escaping blockaddress"), std::string::npos);
  EXPECT_FALSE(Preserved->isDeclaration());
  EXPECT_FALSE(Replaceable->isDeclaration());
  EXPECT_EQ(llvm::BlockAddress::lookup(Target), Address);
  EXPECT_EQ(Return->getReturnValue(), AddressInt);
}

TEST(BinaryPatcher_TrampolineSafety,
     RejectsGlobalBlockAddressBeforeExternalizingSource) {
  llvm::LLVMContext Context;
  llvm::Module Module("thumb-preserved-global-blockaddress", Context);
  llvm::Type *I32 = llvm::Type::getInt32Ty(Context);
  llvm::FunctionType *Type = llvm::FunctionType::get(I32, false);

  llvm::Function *Preserved = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "preserved", Module);
  llvm::BasicBlock *Entry =
      llvm::BasicBlock::Create(Context, "entry", Preserved);
  llvm::BasicBlock *Target =
      llvm::BasicBlock::Create(Context, "target", Preserved);
  llvm::UncondBrInst::Create(Target, Entry);
  llvm::ReturnInst::Create(Context, llvm::ConstantInt::get(I32, 7), Target);
  rewrite_source::setOriginalVA(*Preserved, 0x1000);

  llvm::BlockAddress *Address = llvm::BlockAddress::get(Preserved, Target);
  llvm::Constant *AddressInt = llvm::ConstantExpr::getPtrToInt(Address, I32);
  auto *AddressGlobal = new llvm::GlobalVariable(
      Module, I32, /*isConstant=*/true, llvm::GlobalValue::ExternalLinkage,
      AddressInt, "escaped_block_address");

  llvm::Function *Replaceable = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "replaceable", Module);
  llvm::BasicBlock *Use =
      llvm::BasicBlock::Create(Context, "entry", Replaceable);
  llvm::ReturnInst::Create(Context, llvm::ConstantInt::get(I32, 0), Use);
  rewrite_source::setOriginalVA(*Replaceable, 0x1008);

  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::ARM;
  Image.Mode = InstructionMode::Thumb;
  Image.Bits = Bitness::Bits32;
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x20;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.SegmentName = ".text";
  Code.VA = 0x1000;
  Code.Size = 0x20;
  Code.FileSz = Code.Size;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Sections.push_back(std::move(Code));
  Image.Symbols = {Symbol::makeFunc(0x1000), Symbol::makeFunc(0x1002),
                   Symbol::makeFunc(0x1008, 4)};

  SourceFunctionPreparation Preparation;
  std::string Detail;
  EXPECT_FALSE(
      TestBinaryPatcher::prepareSources(Module, &Image, Preparation, Detail));
  EXPECT_NE(Detail.find("escaping blockaddress"), std::string::npos);
  EXPECT_FALSE(Preserved->isDeclaration());
  EXPECT_FALSE(Replaceable->isDeclaration());
  EXPECT_EQ(llvm::BlockAddress::lookup(Target), Address);
  EXPECT_EQ(AddressGlobal->getInitializer(), AddressInt);
}

TEST(RewriteCodegen_CodePointerIdentity,
     SyntheticDataSymbolUsesTypedFunctionIdentityWithoutCodeRef) {
  BinaryImage Image;
  Image.Format = BinaryFormat::COFF;
  Image.Arch = Arch::ARM;
  Image.Mode = InstructionMode::Thumb;
  Image.Bits = Bitness::Bits32;
  Image.Symbols = {Symbol::makeFunc(0x1000, 4)};

  ASSERT_TRUE(Image.CodeRefTargets.empty());
  EXPECT_TRUE(isAuthenticatedCodeDataSymbol(Image, 0x1000));
  EXPECT_EQ(serializeImageDataSymbolAddress(Image, 0x1000), 0x1001u);
  EXPECT_FALSE(isAuthenticatedCodeDataSymbol(Image, 0x1004));
  EXPECT_EQ(serializeImageDataSymbolAddress(Image, 0x1004), 0x1004u);
}

TEST(BinaryPatcher_TrampolineSafety,
     PreservedSourceAliasesRespectObjectFormatMangling) {
  const std::map<std::string, uint64_t> Preserved{{"source", 0x1000},
                                                  {"_source", 0x2000}};
  EXPECT_EQ(resolveSourceFunctionAlias("_source", Preserved,
                                       BinaryFormat::MachO, Arch::ARM),
            "source");
  EXPECT_EQ(resolveSourceFunctionAlias("__source", Preserved,
                                       BinaryFormat::MachO, Arch::ARM),
            "_source");
  EXPECT_EQ(resolveSourceFunctionAlias("_source", Preserved, BinaryFormat::COFF,
                                       Arch::X86),
            "source");
  EXPECT_EQ(resolveSourceFunctionAlias("_source", Preserved, BinaryFormat::ELF,
                                       Arch::X64),
            "_source");
  const std::map<std::string, uint64_t> OnlySource{{"source", 0x1000}};
  EXPECT_TRUE(resolveSourceFunctionAlias("_source", OnlySource,
                                         BinaryFormat::ELF, Arch::X64)
                  .empty());
  EXPECT_TRUE(resolveSourceFunctionAlias("_source", OnlySource,
                                         BinaryFormat::COFF, Arch::X64)
                  .empty());
  EXPECT_TRUE(resolveSourceFunctionAlias("__imp_source", OnlySource,
                                         BinaryFormat::COFF, Arch::X86)
                  .empty());
}

TEST(InplaceRewriter_ThumbSafety, ShortGrowerSpanFailsWithoutOutput) {
  InplaceRunResult Run = runInplaceWithSpan(
      2, Arch::ARM, InstructionMode::Thumb, "thumbv7-unknown-linux-gnueabihf");
  EXPECT_FALSE(Run.Result.Success);
  EXPECT_EQ(Run.Result.TrampolineCount, 0u);
  EXPECT_FALSE(Run.OutputExists);
}

TEST(InplaceRewriter_ThumbSafety, InstalledGrowerReportsOneTrampoline) {
  InplaceRunResult Run = runInplaceWithSpan(
      4, Arch::ARM, InstructionMode::Thumb, "thumbv7-unknown-linux-gnueabihf");
  ASSERT_TRUE(Run.Result.Success);
  EXPECT_EQ(Run.Result.TrampolineCount, 1u);
  EXPECT_TRUE(Run.OutputExists);
}

TEST(InplaceRewriter_ResultAccuracy, TrampolineCountIsNotMappingCount) {
  llvm::SmallString<128> OutputPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-inplace-result",
                                                  "bin", OutputPath));
  llvm::FileRemover RemoveOutput(OutputPath);

  PatchResult Result =
      TestInplaceRewriter::writeSyntheticResult(OutputPath.str().str(), 2, 1);
  ASSERT_TRUE(Result.Success);
  EXPECT_EQ(Result.TrampolineCount, 1u);
}

TEST(InplaceRewriter_ExceptionSafety,
     RoutesELFExceptionFunctionsThroughFormatPatcher) {
  TestPatcherStats Stats;
  InplaceRunResult Run =
      runInplaceWithSpan(64, Arch::X64, InstructionMode::Default,
                         "x86_64-unknown-linux-gnu", &Stats, true);

  ASSERT_TRUE(Run.Result.Success);
  EXPECT_EQ(Stats.PatchCalls, 1u);
  EXPECT_EQ(Stats.AppendCalls, 0u);
  EXPECT_TRUE(Stats.PatchHadImageContext);
  EXPECT_EQ(Run.Result.CodeSize, TestBinaryPatcher::FormatPatchCodeSize);
}

TEST(RewriteCodegen_CodePointerIdentity,
     TypedFunctionSymbolKeepsTargetModeAcrossELFAndCOFFCodegen) {
  for (const auto &[Format, Expected] :
       {std::pair{BinaryFormat::ELF, uint32_t{0x1000}},
        std::pair{BinaryFormat::COFF, uint32_t{0x1001}}}) {
    SCOPED_TRACE(Format == BinaryFormat::ELF ? "ELF ARM" : "COFF Thumb");
    const std::optional<uint32_t> Returned =
        runSymbolizedFunctionAddressCodegen(Format);
    ASSERT_TRUE(Returned.has_value());
    EXPECT_EQ(*Returned, Expected);
  }
}
