//===- RewriteCodegenRTTests.cpp - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// Verifies that the rewrite backend (AddressModelBackend) produces correct
// machine code by:
//   1. Building LLVM IR programmatically for a simple function
//   2. Compiling via Codegen::compileForRewrite() to get fixed-up bytes
//   3. Loading the bytes into Unicorn Engine at the target VA
//   4. Emulating and checking register results
//
// This lets us verify rewrite-backend output for architectures we cannot
// natively run (x86-64 on arm64 macOS, ARM32 on arm64 macOS, etc.).
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/pass/mir/MIRPass.h"
#include "neverd/pass/mir/NOPPass.h"

#include "UnicornSemanticFixture.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/TargetSelect.h"

using namespace neverd;

static constexpr uint64_t CODE_VA = 0x400000;
static constexpr uint64_t DATA_VA = 0x500000;

static void ensureLLVMTargets() {
  static bool Done = false;
  if (Done)
    return;
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
  Done = true;
}

// Build IR: int add(int a, int b) { return a + b; }
// AArch64/x86-64 ABI: a=first int reg, b=second int reg, return in first.
static std::unique_ptr<llvm::Module>
buildAddIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32, I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "add", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Sum = B.CreateAdd(F->getArg(0), F->getArg(1));
  B.CreateRet(Sum);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR: int compute(int x) { return x * 3 + 7; }
static std::unique_ptr<llvm::Module>
buildComputeIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "compute", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Mul = B.CreateMul(F->getArg(0), B.getInt32(3));
  auto *Add = B.CreateAdd(Mul, B.getInt32(7));
  B.CreateRet(Add);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with data reference:
//   @counter = global i32 42
//   int get_and_inc() { counter++; return counter; }
static std::unique_ptr<llvm::Module>
buildDataRefIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *GV = new llvm::GlobalVariable(
      *M, I32, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantInt::get(I32, 42), "counter");
  GV->setDSOLocal(true);

  auto *FTy = llvm::FunctionType::get(I32, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "get_and_inc", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Val = B.CreateLoad(I32, GV);
  auto *Inc = B.CreateAdd(Val, B.getInt32(1));
  B.CreateStore(Inc, GV);
  B.CreateRet(Inc);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with cross-function call:
//   int helper(int x) { return x * 2 + 1; }
//   int caller(int a, int b) { return helper(a) + helper(b); }
static std::unique_ptr<llvm::Module>
buildCrossCallIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);

  auto *HelperTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *Helper = llvm::Function::Create(HelperTy,
      llvm::Function::InternalLinkage, "helper", M.get());
  Helper->setDSOLocal(true);
  {
    auto *BB = llvm::BasicBlock::Create(Ctx, "entry", Helper);
    llvm::IRBuilder<> B(BB);
    auto *Mul = B.CreateMul(Helper->getArg(0), B.getInt32(2));
    auto *Add = B.CreateAdd(Mul, B.getInt32(1));
    B.CreateRet(Add);
  }

  auto *CallerTy = llvm::FunctionType::get(I32, {I32, I32}, false);
  auto *Caller = llvm::Function::Create(CallerTy,
      llvm::Function::ExternalLinkage, "caller", M.get());
  {
    auto *BB = llvm::BasicBlock::Create(Ctx, "entry", Caller);
    llvm::IRBuilder<> B(BB);
    auto *R1 = B.CreateCall(Helper, {Caller->getArg(0)});
    auto *R2 = B.CreateCall(Helper, {Caller->getArg(1)});
    auto *Sum = B.CreateAdd(R1, R2);
    B.CreateRet(Sum);
  }

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with external call:
//   declare i32 @external_fn(i32)
//   int wrap_ext(int x) { return external_fn(x + 10); }
// DSOLocal controls whether the call goes direct (dso_local=true) or via @PLT.
static std::unique_ptr<llvm::Module>
buildExternalCallIR(llvm::LLVMContext &Ctx, const char *Triple,
                    bool DSOLocal = true) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *ExtTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *ExtFn = llvm::Function::Create(ExtTy,
      llvm::Function::ExternalLinkage, "external_fn", M.get());
  if (DSOLocal)
    ExtFn->setDSOLocal(true);

  auto *WrapTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *Wrap = llvm::Function::Create(WrapTy,
      llvm::Function::ExternalLinkage, "wrap_ext", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", Wrap);
  llvm::IRBuilder<> B(BB);
  auto *Added = B.CreateAdd(Wrap->getArg(0), B.getInt32(10));
  auto *Result = B.CreateCall(ExtFn, {Added});
  B.CreateRet(Result);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with an external function address materialized in data:
//   int external_fn(int);
//   int (*external_slot)(int) = external_fn;
//   int call_external_ptr(int x) { return external_slot(x); }
static std::unique_ptr<llvm::Module>
buildExternalFunctionPointerIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *ExtTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *ExtFn = llvm::Function::Create(
      ExtTy, llvm::Function::ExternalLinkage, "external_fn", M.get());
  ExtFn->setDSOLocal(true);
  auto *Slot = new llvm::GlobalVariable(
      *M, ExtFn->getType(), false, llvm::GlobalValue::ExternalLinkage, ExtFn,
      "external_slot");
  Slot->setDSOLocal(true);

  auto *Wrap = llvm::Function::Create(
      ExtTy, llvm::Function::ExternalLinkage, "call_external_ptr", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", Wrap);
  llvm::IRBuilder<> B(BB);
  auto *Fn = B.CreateLoad(ExtFn->getType(), Slot);
  B.CreateRet(B.CreateCall(ExtTy, Fn, {Wrap->getArg(0)}));

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with a loop: int sum_to(int n) { int s=0; for(int i=1;i<=n;i++) s+=i; return s; }
static std::unique_ptr<llvm::Module>
buildLoopIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "sum_to", M.get());
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", F);
  auto *Loop = llvm::BasicBlock::Create(Ctx, "loop", F);
  auto *Exit = llvm::BasicBlock::Create(Ctx, "exit", F);

  llvm::IRBuilder<> B(Entry);
  B.CreateBr(Loop);

  B.SetInsertPoint(Loop);
  auto *IPhi = B.CreatePHI(I32, 2, "i");
  auto *SPhi = B.CreatePHI(I32, 2, "s");
  auto *NewS = B.CreateAdd(SPhi, IPhi);
  auto *NewI = B.CreateAdd(IPhi, B.getInt32(1));
  auto *Cond = B.CreateICmpSLE(NewI, F->getArg(0));
  B.CreateCondBr(Cond, Loop, Exit);
  IPhi->addIncoming(B.getInt32(1), Entry);
  IPhi->addIncoming(NewI, Loop);
  SPhi->addIncoming(B.getInt32(0), Entry);
  SPhi->addIncoming(NewS, Loop);

  B.SetInsertPoint(Exit);
  B.CreateRet(NewS);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with switch:
//   int classify(int x) {
//     switch(x) {
//       case 0: return 10;
//       case 1: return 20;
//       case 2: return 30;
//       case 3: return 40;
//       default: return -1;
//     }
//   }
static std::unique_ptr<llvm::Module>
buildSwitchIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));

  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "classify", M.get());
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", F);
  auto *Case0 = llvm::BasicBlock::Create(Ctx, "case0", F);
  auto *Case1 = llvm::BasicBlock::Create(Ctx, "case1", F);
  auto *Case2 = llvm::BasicBlock::Create(Ctx, "case2", F);
  auto *Case3 = llvm::BasicBlock::Create(Ctx, "case3", F);
  auto *Default = llvm::BasicBlock::Create(Ctx, "default", F);

  llvm::IRBuilder<> B(Entry);
  auto *SW = B.CreateSwitch(F->getArg(0), Default, 4);
  SW->addCase(B.getInt32(0), Case0);
  SW->addCase(B.getInt32(1), Case1);
  SW->addCase(B.getInt32(2), Case2);
  SW->addCase(B.getInt32(3), Case3);

  B.SetInsertPoint(Case0);  B.CreateRet(B.getInt32(10));
  B.SetInsertPoint(Case1);  B.CreateRet(B.getInt32(20));
  B.SetInsertPoint(Case2);  B.CreateRet(B.getInt32(30));
  B.SetInsertPoint(Case3);  B.CreateRet(B.getInt32(40));
  B.SetInsertPoint(Default); B.CreateRet(B.getInt32(-1));

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// Build IR with multiple globals:
//   @g1 = global i32 111
//   @g2 = global i32 222
//   int sum_globals() { g1 += 1; g2 += 2; return g1 + g2; }
// Expected result: (111+1) + (222+2) = 336
static std::unique_ptr<llvm::Module>
buildMultiGlobalIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *G1 = new llvm::GlobalVariable(
      *M, I32, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantInt::get(I32, 111), "g1");
  G1->setDSOLocal(true);
  auto *G2 = new llvm::GlobalVariable(
      *M, I32, false, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantInt::get(I32, 222), "g2");
  G2->setDSOLocal(true);

  auto *FTy = llvm::FunctionType::get(I32, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "sum_globals", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *V1 = B.CreateLoad(I32, G1);
  auto *Inc1 = B.CreateAdd(V1, B.getInt32(1));
  B.CreateStore(Inc1, G1);
  auto *V2 = B.CreateLoad(I32, G2);
  auto *Inc2 = B.CreateAdd(V2, B.getInt32(2));
  B.CreateStore(Inc2, G2);
  B.CreateRet(B.CreateAdd(Inc1, Inc2));

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

using ResolveCallback =
    std::function<std::optional<uint64_t>(llvm::StringRef, uint32_t)>;

static llvm::mc_rewrite::RewriteResult
compileRewrite(llvm::Module &Mod, Arch Ar, BinaryFormat Fmt,
          ResolveCallback Resolve = nullptr) {
  ensureLLVMTargets();

  llvm::mc_rewrite::RewriteOptions Opts;
  Opts.Model.TextVA = CODE_VA;
  Opts.Model.getSectionVA = [](llvm::StringRef Name) -> uint64_t {
    if (Name.contains("text") || Name.contains("TEXT"))
      return CODE_VA;
    if (Name.contains("data") || Name.contains("DATA") ||
        Name.contains("bss") || Name.contains("BSS"))
      return DATA_VA;
    return DATA_VA + 0x1000;
  };
  if (Resolve)
    Opts.Model.resolve = Resolve;
  else
    Opts.Model.resolve = [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
      return std::nullopt;
    };

  Codegen CG;
  return CG.compileForRewrite(Mod, Ar, Opts, Fmt);
}

static llvm::mc_rewrite::RewriteResult
compileRewriteWithVAs(llvm::Module &Mod, Arch Ar, BinaryFormat Fmt,
                 uint64_t CodeVA, uint64_t DataVA,
                 ResolveCallback Resolve = nullptr) {
  ensureLLVMTargets();

  llvm::mc_rewrite::RewriteOptions Opts;
  Opts.Model.TextVA = CodeVA;
  Opts.Model.getSectionVA = [CodeVA, DataVA](llvm::StringRef Name) -> uint64_t {
    if (Name.contains("text") || Name.contains("TEXT"))
      return CodeVA;
    if (Name.contains("data") || Name.contains("DATA") ||
        Name.contains("bss") || Name.contains("BSS"))
      return DataVA;
    return DataVA + 0x1000;
  };
  if (Resolve)
    Opts.Model.resolve = Resolve;
  else
    Opts.Model.resolve = [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
      return std::nullopt;
    };

  Codegen CG;
  return CG.compileForRewrite(Mod, Ar, Opts, Fmt);
}

static const llvm::mc_rewrite::RewriteSection *
findDataSection(const llvm::mc_rewrite::RewriteResult &R) {
  for (auto &S : R.Sections) {
    llvm::StringRef N(S.Name);
    if (N.contains("data") || N.contains("DATA") || N.contains("bss"))
      return &S;
  }
  return nullptr;
}

static const llvm::mc_rewrite::RewriteSection *
findTextSection(const llvm::mc_rewrite::RewriteResult &R) {
  for (auto &S : R.Sections) {
    llvm::StringRef N(S.Name);
    if (N.contains("text") || N.contains("TEXT"))
      return &S;
  }
  return nullptr;
}

// MachO prefixes symbols with '_'; this helper checks both variants.
static uint64_t findSymbolVA(const llvm::mc_rewrite::RewriteResult &R,
                             const char *Name, uint64_t Default) {
  if (R.SymbolAddrs.count(Name))
    return R.SymbolAddrs.at(Name);
  std::string Prefixed = std::string("_") + Name;
  if (R.SymbolAddrs.count(Prefixed))
    return R.SymbolAddrs.at(Prefixed);
  return Default;
}

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

// ========================= x86-64 Tests =========================

TEST(RewriteCodegen_x64, AddFunction) {
  LLVMMCAssembler::initTargets();
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections";
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr) << "No .text section";
  ASSERT_GT(Text->Bytes.size(), 0u) << ".text is empty";

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // x86-64 SysV ABI: edi=a, esi=b, return in eax
  uint64_t a = 17, b = 25;
  uc_reg_write(uc, UC_X86_REG_RDI, &a);
  uc_reg_write(uc, UC_X86_REG_RSI, &b);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  // Write a fake return address that we'll use as end point
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 42u) << "add(17,25) should be 42";

  uc_close(uc);
}

TEST(RewriteCodegen_x64, ComputeFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 5;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 22u) << "compute(5) = 5*3+7 = 22";

  uc_close(uc);
}

// ========================= AArch64 Tests =========================

TEST(RewriteCodegen_AArch64, AddFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 17, b = 25;
  uc_reg_write(uc, UC_ARM64_REG_X0, &a);
  uc_reg_write(uc, UC_ARM64_REG_X1, &b);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  // AArch64 returns via BX LR — set LR to a sentinel end address
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 42u) << "add(17,25) should be 42";

  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, ComputeFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 22u) << "compute(5) = 5*3+7 = 22";

  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, DataReference) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  // Map a contiguous region covering code (0x400000) through data (0x510000)
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);

  for (auto &S : RR.Sections) {
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());
  }

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "get_and_inc() should return 43 (42+1)";

  uc_close(uc);
}

TEST(RewriteCodegen_x64, DataReference) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);

  for (auto &S : RR.Sections) {
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());
  }

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "get_and_inc() should return 43 (42+1)";

  uc_close(uc);
}

// ========================= Cross-Function Call Tests =========================

TEST(RewriteCodegen_x64, CrossFunctionCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 3, b = 7;
  uc_reg_write(uc, UC_X86_REG_RDI, &a);
  uc_reg_write(uc, UC_X86_REG_RSI, &b);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t callerVA = findSymbolVA(RR, "caller", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, callerVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  // caller(3,7) = helper(3) + helper(7) = (3*2+1) + (7*2+1) = 7 + 15 = 22
  EXPECT_EQ(rax & 0xFFFFFFFF, 22u) << "caller(3,7) should be 22";

  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, CrossFunctionCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 3, b = 7;
  uc_reg_write(uc, UC_ARM64_REG_X0, &a);
  uc_reg_write(uc, UC_ARM64_REG_X1, &b);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t callerVA = findSymbolVA(RR, "caller", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, callerVA, retAddr, 0, 500);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 22u) << "caller(3,7) should be 22";

  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, CrossFunctionCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t a = 3, b = 7;
  uc_reg_write(uc, UC_ARM_REG_R0, &a);
  uc_reg_write(uc, UC_ARM_REG_R1, &b);
  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint32_t callerVA = (uint32_t)findSymbolVA(RR, "caller", Text->VA);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, callerVA, retAddr, 0, 500);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 22u) << "caller(3,7) should be 22";

  uc_close(uc);
}

// ========================= External Call Tests =========================
// Validates that the resolve callback correctly wires external symbols.
// We place a tiny "external_fn" stub in Unicorn at a known VA, then
// compile wrap_ext() which calls it. The stub doubles its argument:
//   x86-64 stub:  lea eax, [rdi+rdi]; ret
//   AArch64 stub: add w0, w0, w0; ret
//   ARM32 stub:   add r0, r0, r0; bx lr

static constexpr uint64_t EXT_FN_VA = 0x600000;

TEST(RewriteCodegen_x64, ExternalCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // x86-64 stub: lea eax, [rdi+rdi]; ret  (doubles arg)
  uint8_t stub_x64[] = { 0x8D, 0x04, 0x3F, 0xC3 };
  uc_mem_write(uc, EXT_FN_VA, stub_x64, sizeof(stub_x64));

  uint64_t x = 5;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  // wrap_ext(5) = external_fn(5+10) = double(15) = 30
  EXPECT_EQ(rax & 0xFFFFFFFF, 30u) << "wrap_ext(5) should be 30";

  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, ExternalCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // AArch64 stub: add w0, w0, w0; ret
  uint32_t stub_a64[] = { 0x0B000000, 0xD65F03C0 };
  uc_mem_write(uc, EXT_FN_VA, stub_a64, sizeof(stub_a64));

  uint64_t x = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr, 0, 500);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 30u) << "wrap_ext(5) should be 30";

  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, ExternalCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // ARM32 stub: add r0, r0, r0; bx lr
  uint32_t stub_arm[] = { 0xE0800000, 0xE12FFF1E };
  uc_mem_write(uc, EXT_FN_VA, stub_arm, sizeof(stub_arm));

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);

  uint32_t wrapVA = (uint32_t)findSymbolVA(RR, "wrap_ext", Text->VA);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr, 0, 500);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u) << "wrap_ext(5) should be 30";

  uc_close(uc);
}

// ========================= ARM32 Tests =========================

TEST(RewriteCodegen_ARM32, AddFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for ARM32";
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t a = 17, b = 25;
  uc_reg_write(uc, UC_ARM_REG_R0, &a);
  uc_reg_write(uc, UC_ARM_REG_R1, &b);
  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 42u) << "add(17,25) should be 42";

  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, ComputeFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for ARM32";
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 22u) << "compute(5) = 5*3+7 = 22";

  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, DataReference) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for ARM32";

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);

  for (auto &S : RR.Sections) {
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());
  }

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 200);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "get_and_inc() should return 43 (42+1)";

  uc_close(uc);
}

// ========================= i386 (X86 32-bit) Tests =========================
// i386 cdecl ABI: args on stack (right-to-left push), return in EAX.
// At function entry: [ESP]=retAddr, [ESP+4]=arg0, [ESP+8]=arg1, ...

TEST(RewriteCodegen_i386, AddFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for i386";
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 17, 25 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 42u) << "add(17,25) should be 42";

  uc_close(uc);
}

TEST(RewriteCodegen_i386, ComputeFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 5 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 22u) << "compute(5) = 5*3+7 = 22";

  uc_close(uc);
}

TEST(RewriteCodegen_i386, DataReference) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);

  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);

  for (auto &S : RR.Sections) {
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());
  }

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 43u) << "get_and_inc() should return 43 (42+1)";

  uc_close(uc);
}

TEST(RewriteCodegen_i386, CrossFunctionCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t callerVA = (uint32_t)findSymbolVA(RR, "caller", Text->VA);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 3, 7 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  uc_err err = uc_emu_start(uc, callerVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 22u) << "caller(3,7) should be 22";

  uc_close(uc);
}

TEST(RewriteCodegen_i386, ExternalCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // i386 stub: mov eax, [esp+4]; add eax, eax; ret  (doubles arg from stack)
  uint8_t stub_i386[] = {
    0x8B, 0x44, 0x24, 0x04,  // mov eax, [esp+4]
    0x01, 0xC0,              // add eax, eax
    0xC3                     // ret
  };
  uc_mem_write(uc, EXT_FN_VA, stub_i386, sizeof(stub_i386));

  uint32_t wrapVA = (uint32_t)findSymbolVA(RR, "wrap_ext", Text->VA);

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 5 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  // wrap_ext(5) = external_fn(5+10) = double(15) = 30
  EXPECT_EQ(eax, 30u) << "wrap_ext(5) should be 30";

  uc_close(uc);
}

// ========================= COFF x86-64 Tests =========================
// Microsoft x64 ABI: args in RCX, RDX, R8, R9 (not RDI, RSI like SysV).
// 32 bytes shadow space required on stack before args.

TEST(RewriteCodegen_COFF_x64, AddFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for COFF x64";
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 17, b = 25;
  uc_reg_write(uc, UC_X86_REG_RCX, &a);
  uc_reg_write(uc, UC_X86_REG_RDX, &b);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 42u) << "add(17,25) should be 42";

  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, ComputeFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 5;
  uc_reg_write(uc, UC_X86_REG_RCX, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 22u) << "compute(5) = 5*3+7 = 22";

  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, DataReference) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);

  for (auto &S : RR.Sections) {
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());
  }

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "get_and_inc() should return 43 (42+1)";

  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, CrossFunctionCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 3, b = 7;
  uc_reg_write(uc, UC_X86_REG_RCX, &a);
  uc_reg_write(uc, UC_X86_REG_RDX, &b);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t callerVA = findSymbolVA(RR, "caller", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, callerVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 22u) << "caller(3,7) should be 22";

  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, ExternalCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // MSVC x64 stub: lea eax, [rcx+rcx]; ret  (doubles arg in RCX)
  uint8_t stub_win64[] = { 0x8D, 0x04, 0x09, 0xC3 };
  uc_mem_write(uc, EXT_FN_VA, stub_win64, sizeof(stub_win64));

  uint64_t x = 5;
  uc_reg_write(uc, UC_X86_REG_RCX, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 30u) << "wrap_ext(5) should be 30";

  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t n = 10;
  uc_reg_write(uc, UC_X86_REG_RCX, &n);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t x = 2;
  uc_reg_write(uc, UC_X86_REG_RCX, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

// ========================= MachO AArch64 Tests =========================
// Same AAPCS ABI as ELF AArch64, but exercises MachO-specific fixups
// (@PAGE/@PAGEOFF specifiers, LOH, Mach-O streamer paths).

TEST(RewriteCodegen_MachO_AArch64, AddFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty()) << "rewrite backend produced no sections for MachO AArch64";
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 17, b = 25;
  uc_reg_write(uc, UC_ARM64_REG_X0, &a);
  uc_reg_write(uc, UC_ARM64_REG_X1, &b);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 42u) << "add(17,25) should be 42";

  uc_close(uc);
}

TEST(RewriteCodegen_MachO_AArch64, ComputeFunction) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 22u) << "compute(5) = 5*3+7 = 22";

  uc_close(uc);
}

TEST(RewriteCodegen_MachO_AArch64, DataReference) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);

  for (auto &S : RR.Sections) {
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());
  }

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, Text->VA, retAddr, 0, 100);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "get_and_inc() should return 43 (42+1)";

  uc_close(uc);
}

TEST(RewriteCodegen_MachO_AArch64, CrossFunctionCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 3, b = 7;
  uc_reg_write(uc, UC_ARM64_REG_X0, &a);
  uc_reg_write(uc, UC_ARM64_REG_X1, &b);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t callerVA = findSymbolVA(RR, "caller", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, callerVA, retAddr, 0, 500);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 22u) << "caller(3,7) should be 22";

  uc_close(uc);
}

TEST(RewriteCodegen_MachO_AArch64, ExternalCall) {
  ensureLLVMTargets();

  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn" || Sym == "_external_fn")
          return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);

  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // AArch64 stub: add w0, w0, w0; ret
  uint32_t stub_a64[] = { 0x0B000000, 0xD65F03C0 };
  uc_mem_write(uc, EXT_FN_VA, stub_a64, sizeof(stub_a64));

  uint64_t x = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);

  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr, 0, 500);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn emulation failed: " << uc_strerror(err);

  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 30u) << "wrap_ext(5) should be 30";

  uc_close(uc);
}

TEST(RewriteCodegen_MachO_AArch64, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "aarch64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t n = 10;
  uc_reg_write(uc, UC_ARM64_REG_X0, &n);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 5000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_AArch64, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "aarch64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 2;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

// ========================= COFF AArch64 (Windows ARM64) Tests =========================

TEST(RewriteCodegen_COFF_AArch64, AddFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty()) << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 17, b = 25;
  uc_reg_write(uc, UC_ARM64_REG_X0, &a);
  uc_reg_write(uc, UC_ARM64_REG_X1, &b);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 100), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 42u);
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, DataReference) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 100), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u);
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, CrossFunctionCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 3, b = 7;
  uc_reg_write(uc, UC_ARM64_REG_X0, &a);
  uc_reg_write(uc, UC_ARM64_REG_X1, &b);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t callerVA = findSymbolVA(RR, "caller", Text->VA);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, callerVA, retAddr, 0, 500), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 22u);
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, ExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty()) << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());
  uint32_t stub_a64[] = { 0x0B000000, 0xD65F03C0 };
  uc_mem_write(uc, EXT_FN_VA, stub_a64, sizeof(stub_a64));

  uint64_t x = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, wrapVA, retAddr, 0, 500), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 30u);
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, ComputeFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 100), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 22u) << "compute(5) = 5*3+7 = 22";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t n = 10;
  uc_reg_write(uc, UC_ARM64_REG_X0, &n);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 5000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 2;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

// ========================= MachO x86-64 Tests =========================

TEST(RewriteCodegen_MachO_x64, AddFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty()) << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 17, b = 25;
  uc_reg_write(uc, UC_X86_REG_RDI, &a);
  uc_reg_write(uc, UC_X86_REG_RSI, &b);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 42u);
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, DataReference) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u);
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, CrossFunctionCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t a = 3, b = 7;
  uc_reg_write(uc, UC_X86_REG_RDI, &a);
  uc_reg_write(uc, UC_X86_REG_RSI, &b);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t callerVA = findSymbolVA(RR, "caller", Text->VA);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, callerVA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 22u);
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, ComputeFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 5;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 22u) << "compute(5) = 5*3+7 = 22";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, ExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "_external_fn" || Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty()) << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // SysV stub: lea eax, [rdi+rdi]; ret  (doubles arg in RDI)
  uint8_t stub_sysv[] = { 0x8D, 0x04, 0x3F, 0xC3 };
  uc_mem_write(uc, EXT_FN_VA, stub_sysv, sizeof(stub_sysv));

  uint64_t x = 5;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 30u) << "wrap_ext(5) should be 30";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t n = 10;
  uc_reg_write(uc, UC_X86_REG_RDI, &n);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t x = 2;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

// ========================= Loop Tests (Control Flow) =========================

TEST(RewriteCodegen_x64, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t n = 10;
  uc_reg_write(uc, UC_X86_REG_RDI, &n);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t n = 10;
  uc_reg_write(uc, UC_ARM64_REG_X0, &n);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 5000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t n = 10;
  uc_reg_write(uc, UC_ARM_REG_R0, &n);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 5000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 10 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

// ========================= Switch Tests (Jump Tables) =========================

TEST(RewriteCodegen_x64, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t x = 2;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t x = 2;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t x = 2;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 2 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

// ===================== Edge Case: Multi-Global =====================

TEST(RewriteCodegen_x64, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_AArch64, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

// ===================== Edge Case: Negative Displacement =====================
// Data at 0x200000, code at 0x400000 — tests negative PC-relative / ADRP delta

TEST(RewriteCodegen_x64, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X64, BinaryFormat::ELF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "counter=42, inc to 43";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewriteWithVAs(*Mod, Arch::AArch64, BinaryFormat::ELF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = 0x400000 + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "counter=42, inc to 43";
  uc_close(uc);
}

// ===================== Edge Case: Large Displacement =====================
// Code at 0x400000, data at 0x10000000 (252MB gap, ~64k pages)

TEST(RewriteCodegen_AArch64, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewriteWithVAs(*Mod, Arch::AArch64, BinaryFormat::ELF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = 0x400000 + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}

TEST(RewriteCodegen_x64, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X64, BinaryFormat::ELF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}

// --- MachO / COFF NegativeDisplacement ---

TEST(RewriteCodegen_MachO_AArch64, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewriteWithVAs(*Mod, Arch::AArch64, BinaryFormat::MachO,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = 0x400000 + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (negative ADRP delta, MachO)";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_AArch64, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "arm64-apple-macos14.0");
  auto RR = compileRewriteWithVAs(*Mod, Arch::AArch64, BinaryFormat::MachO,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = 0x400000 + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (252MB ADRP delta, MachO)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::AArch64, BinaryFormat::COFF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = 0x400000 + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (negative ADRP delta, COFF)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::AArch64, BinaryFormat::COFF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = 0x400000 + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uint64_t x0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (252MB ADRP delta, COFF)";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-apple-macos14.0");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X64, BinaryFormat::MachO,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (negative RIP delta, MachO)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_x64, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X64, BinaryFormat::COFF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (negative RIP delta, COFF)";
  uc_close(uc);
}

// ===================== Edge Case: COFF ARM32 (Thumb mode) =====================

TEST(RewriteCodegen_COFF_ARM32, AddFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t a = 5, b = 8;
  uc_reg_write(uc, UC_ARM_REG_R0, &a);
  uc_reg_write(uc, UC_ARM_REG_R1, &b);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 1000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 13u) << "5 + 8 = 13 (Thumb mode)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, CrossFunctionCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint64_t callerVA = findSymbolVA(RR, "caller", Text->VA);
  uint32_t a = 3, b = 7;
  uc_reg_write(uc, UC_ARM_REG_R0, &a);
  uc_reg_write(uc, UC_ARM_REG_R1, &b);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, callerVA | 1, retAddr, 0, 1000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 22u) << "helper(3)+helper(7) = 7+15 = 22 (Thumb)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, DataReference) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 1000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "counter=42, inc to 43 (Thumb)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t n = 10;
  uc_reg_write(uc, UC_ARM_REG_R0, &n);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 2000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 55u) << "sum_to(10)=55 (Thumb)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, ComputeFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 1000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 22u) << "compute(5) = 5*3+7 = 22 (Thumb)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, ExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        // Bit 0 = 1 indicates Thumb mode target → backend uses BL (stays in Thumb)
        if (Sym == "external_fn") return EXT_FN_VA | 1;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // Thumb stub: adds r0, r0, r0; bx lr
  uint8_t stub[] = { 0x00, 0x44, 0x70, 0x47 };
  uc_mem_write(uc, EXT_FN_VA, stub, sizeof(stub));

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, wrapVA | 1, retAddr, 0, 2000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u) << "wrap_ext(5): external_fn(15)=30 (Thumb)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, ExternalFunctionPointer) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod =
      buildExternalFunctionPointerIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(
      *Mod, Arch::ARM, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn")
          return EXT_FN_VA | 1;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  auto *Data = findDataSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(Data, nullptr);

  uint64_t SlotVA = findSymbolVA(RR, "external_slot", Data->VA);
  ASSERT_GE(SlotVA, Data->VA);
  ASSERT_LE(SlotVA - Data->VA + sizeof(uint32_t), Data->Bytes.size());
  uint32_t StoredFn = 0;
  std::memcpy(&StoredFn, Data->Bytes.data() + (SlotVA - Data->VA),
              sizeof(StoredFn));
  EXPECT_EQ(StoredFn, uint32_t(EXT_FN_VA | 1));

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL),
            UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL),
            UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size()),
            UC_ERR_OK);
  ASSERT_EQ(uc_mem_write(uc, Data->VA, Data->Bytes.data(), Data->Bytes.size()),
            UC_ERR_OK);

  // Thumb stub: add r0, r0; bx lr.
  uint8_t Stub[] = {0x00, 0x44, 0x70, 0x47};
  ASSERT_EQ(uc_mem_write(uc, EXT_FN_VA, Stub, sizeof(Stub)), UC_ERR_OK);

  uint32_t R0 = 7;
  uint32_t SP = uint32_t(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t RetAddr = uint32_t(CODE_VA + 0xf000);
  uc_reg_write(uc, UC_ARM_REG_R0, &R0);
  uc_reg_write(uc, UC_ARM_REG_SP, &SP);
  uc_reg_write(uc, UC_ARM_REG_LR, &RetAddr);

  uint64_t WrapVA = findSymbolVA(RR, "call_external_ptr", Text->VA);
  uc_err Err = uc_emu_start(uc, WrapVA | 1, RetAddr, 0, 2000);
  ASSERT_EQ(Err, UC_ERR_OK)
      << "Unicorn Thumb function-pointer call failed: " << uc_strerror(Err);
  uc_reg_read(uc, UC_ARM_REG_R0, &R0);
  EXPECT_EQ(R0, 14u);
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t x = 2;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 1000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u) << "classify(2) should be 30 (Thumb)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 1000), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 336u) << "(111+1)+(222+2) = 336 (Thumb)";
  uc_close(uc);
}

// ===================== @PLT External Call Tests =====================
// These test non-dso_local external calls. On ELF, the compiler generates
// call instructions with @PLT specifiers. The backend must resolve @PLT to the
// actual target VA via the resolve callback (Specifier != 0).

TEST(RewriteCodegen_x64, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "x86_64-unknown-linux-elf", false);
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // x86-64 stub: lea eax, [rdi+rdi]; ret  (doubles arg)
  uint8_t stub[] = { 0x8D, 0x04, 0x3F, 0xC3 };
  uc_mem_write(uc, EXT_FN_VA, stub, sizeof(stub));

  uint64_t x = 5;
  uc_reg_write(uc, UC_X86_REG_RDI, &x);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn @PLT emulation failed: " << uc_strerror(err);

  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 30u)
      << "wrap_ext(5): external_fn(5+10)=15*2=30 via @PLT";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "aarch64-unknown-linux-elf", false);
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // AArch64 stub: add w0, w0, w0; ret  (doubles arg)
  uint8_t stub[] = { 0x00, 0x00, 0x00, 0x0B, 0xC0, 0x03, 0x5F, 0xD6 };
  uc_mem_write(uc, EXT_FN_VA, stub, sizeof(stub));

  uint64_t x = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint32_t retInstr = 0xD65F03C0; // ret
  uc_mem_write(uc, retAddr, &retInstr, 4);
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr + 4, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn @PLT emulation failed: " << uc_strerror(err);

  uint64_t w0 = 0;
  uc_reg_read(uc, UC_ARM64_REG_X0, &w0);
  EXPECT_EQ(w0 & 0xFFFFFFFF, 30u)
      << "wrap_ext(5): external_fn(5+10)=15*2=30 via @PLT";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "armv7-unknown-linux-gnueabihf", false);
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // ARM stub: add r0, r0, r0; bx lr  (doubles arg)
  uint8_t stub[] = {
    0x00, 0x00, 0x80, 0xE0, // add r0, r0, r0
    0x1E, 0xFF, 0x2F, 0xE1  // bx lr
  };
  uc_mem_write(uc, EXT_FN_VA, stub, sizeof(stub));

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn @PLT emulation failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u)
      << "wrap_ext(5): external_fn(5+10)=15*2=30 via @PLT";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "i386-unknown-linux-gnu", false);
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn") return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // i386 stub: mov eax, [esp+4]; add eax, eax; ret  (doubles arg from stack)
  uint8_t stub[] = { 0x8B, 0x44, 0x24, 0x04, 0x01, 0xC0, 0xC3 };
  uc_mem_write(uc, EXT_FN_VA, stub, sizeof(stub));

  uint32_t wrapVA = (uint32_t)findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);

  // cdecl stack layout at function entry: [esp]=retAddr, [esp+4]=arg
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 5 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn @PLT emulation failed: " << uc_strerror(err);

  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 30u)
      << "wrap_ext(5): external_fn(5+10)=15*2=30 via @PLT";
  uc_close(uc);
}

// ===================== ARM↔Thumb Interworking Tests =====================
// Thumb code calling ARM-mode external function.
// The resolve callback returns an even VA (ARM mode), so the backend must emit
// BLX (branch-and-exchange) instead of BL (which stays in Thumb).

TEST(RewriteCodegen_ARM32, ThumbToArmInterwork) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "armv7-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn")
          return EXT_FN_VA;  // even address = ARM mode
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // ARM stub at even address: add r0, r0, r0; bx lr
  uint8_t armStub[] = {
    0x00, 0x00, 0x80, 0xE0,  // add r0, r0, r0
    0x1E, 0xFF, 0x2F, 0xE1   // bx lr
  };
  uc_mem_write(uc, EXT_FN_VA, armStub, sizeof(armStub));

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA, retAddr, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn ARM interwork failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u)
      << "wrap_ext(5): external_fn(15)=30 via ARM interwork";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, ThumbToArmInterwork) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn")
          return EXT_FN_VA;  // even = ARM mode
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty())
      << "Unresolved: " << RR.Unresolved[0];

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  // ARM stub at even address: add r0, r0, r0; bx lr
  uint8_t armStub[] = {
    0x00, 0x00, 0x80, 0xE0,  // add r0, r0, r0
    0x1E, 0xFF, 0x2F, 0xE1   // bx lr
  };
  uc_mem_write(uc, EXT_FN_VA, armStub, sizeof(armStub));

  uint32_t x = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &x);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);

  uint64_t wrapVA = findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uc_err err = uc_emu_start(uc, wrapVA | 1, retAddr, 0, 2000);
  ASSERT_EQ(err, UC_ERR_OK) << "Unicorn Thumb→ARM interwork failed: " << uc_strerror(err);

  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 30u)
      << "wrap_ext(5): external_fn(15)=30 via Thumb→ARM interwork";
  uc_close(uc);
}

// ==================== Float via integer wrapper ====================
// int float_add_trunc(int a, int b) { return (int)((float)a + (float)b); }
// Tests FP codegen while keeping Unicorn I/O in integer regs.
static std::unique_ptr<llvm::Module>
buildFloatAddTruncIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *F32 = llvm::Type::getFloatTy(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32, I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "float_add_trunc", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *FA = B.CreateSIToFP(F->getArg(0), F32);
  auto *FB = B.CreateSIToFP(F->getArg(1), F32);
  auto *Sum = B.CreateFAdd(FA, FB);
  auto *Res = B.CreateFPToSI(Sum, I32);
  B.CreateRet(Res);
  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// ==================== Chained functions (10 deep) ====================
// f0(x)=x+1, f1(x)=f0(x)+1, ..., f9(x)=f8(x)+1
// chain_entry(x) = f9(x) → x + 10
static std::unique_ptr<llvm::Module>
buildChainedFunctionsIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32}, false);

  llvm::Function *Prev = nullptr;
  for (int I = 0; I < 10; ++I) {
    std::string Name = "f" + std::to_string(I);
    auto *F = llvm::Function::Create(FTy, llvm::Function::InternalLinkage,
                                     Name, M.get());
    F->setDSOLocal(true);
    auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
    llvm::IRBuilder<> B(BB);
    llvm::Value *V = F->getArg(0);
    if (Prev)
      V = B.CreateCall(Prev, {V});
    V = B.CreateAdd(V, B.getInt32(1));
    B.CreateRet(V);
    Prev = F;
  }

  auto *Entry = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                       "chain_entry", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", Entry);
  llvm::IRBuilder<> B(BB);
  B.CreateRet(B.CreateCall(Prev, {Entry->getArg(0)}));

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// ==================== 64-bit wide arithmetic ====================
// int64_t wide_mul(int64_t a, int64_t b) { return a * b + 7; }
// On i386 this exercises register pairs for 64-bit values.
static std::unique_ptr<llvm::Module>
buildWideMulIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I64 = llvm::Type::getInt64Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I64, {I64, I64}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "wide_mul", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Prod = B.CreateMul(F->getArg(0), F->getArg(1));
  auto *Res = B.CreateAdd(Prod, B.getInt64(7));
  B.CreateRet(Res);
  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// ==================== Conditional with multiple BBs ====================
// int abs_diff(int a, int b) {
//   int d = a - b;
//   return d >= 0 ? d : -d;
// }
static std::unique_ptr<llvm::Module>
buildAbsDiffIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("rwtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32, I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "abs_diff", M.get());
  auto *Entry = llvm::BasicBlock::Create(Ctx, "entry", F);
  auto *Pos = llvm::BasicBlock::Create(Ctx, "pos", F);
  auto *Neg = llvm::BasicBlock::Create(Ctx, "neg", F);
  auto *Merge = llvm::BasicBlock::Create(Ctx, "merge", F);

  llvm::IRBuilder<> B(Entry);
  auto *D = B.CreateSub(F->getArg(0), F->getArg(1));
  auto *Cmp = B.CreateICmpSGE(D, B.getInt32(0));
  B.CreateCondBr(Cmp, Pos, Neg);

  B.SetInsertPoint(Pos);
  B.CreateBr(Merge);

  B.SetInsertPoint(Neg);
  auto *NegD = B.CreateNeg(D);
  B.CreateBr(Merge);

  B.SetInsertPoint(Merge);
  auto *Phi = B.CreatePHI(I32, 2);
  Phi->addIncoming(D, Pos);
  Phi->addIncoming(NegD, Neg);
  B.CreateRet(Phi);

  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

// ==================== MIR Pass Rewrite Tests ====================

TEST(RewriteCodegen_MIR, NopPassOnRewriteResult_x64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  size_t OrigTextSize = 0;
  for (auto &S : RR.Sections)
    if (llvm::StringRef(S.Name).contains("text"))
      OrigTextSize = S.Bytes.size();
  ASSERT_GT(OrigTextSize, 0u);

  MIRPassRunner Runner;
  NopPass NP;
  Runner.addPass(&NP);
  // NopPass appends a NOP (size change). runOnRewriteResult must refuse to
  // apply it in the in-place path (the change would break already-resolved
  // cross-function references) and emit a warning. Capture stderr so the
  // expected warning stays out of the test log, and assert it actually fired.
  testing::internal::CaptureStderr();
  Runner.runOnRewriteResult(RR, Arch::X64);
  std::string Warn = testing::internal::GetCapturedStderr();
  EXPECT_NE(Warn.find("size change"), std::string::npos)
      << "expected a size-change rejection warning, got: " << Warn;

  // The rejected size change must leave .text byte-for-byte unchanged.
  auto *TextAfter = findTextSection(RR);
  ASSERT_NE(TextAfter, nullptr);
  EXPECT_EQ(TextAfter->Bytes.size(), OrigTextSize)
      << "size-change pass must not corrupt .text";

  // Verify function still works after pass infrastructure touched it.
  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uint64_t rdi = 3, rsi = 4;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, TextAfter->VA, retAddr + 1, 0, 500), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 7u) << "add(3,4)=7 after MIR pass";
  uc_close(uc);
}

TEST(RewriteCodegen_MIR, NopPassOnRewriteResult_AArch64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());

  size_t OrigSize = 0;
  for (auto &S : RR.Sections)
    if (llvm::StringRef(S.Name).contains("text"))
      OrigSize = S.Bytes.size();

  MIRPassRunner Runner;
  NopPass NP;
  Runner.addPass(&NP);
  // See the x64 variant: the size change must be rejected with a warning and
  // .text left unchanged. Capture stderr to keep the expected warning out of
  // the test log while asserting it was emitted.
  testing::internal::CaptureStderr();
  Runner.runOnRewriteResult(RR, Arch::AArch64);
  std::string Warn = testing::internal::GetCapturedStderr();
  EXPECT_NE(Warn.find("size change"), std::string::npos)
      << "expected a size-change rejection warning, got: " << Warn;

  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  EXPECT_EQ(Text->Bytes.size(), OrigSize);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 3, x1 = 4;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 500), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 7u) << "add(3,4)=7 after MIR pass";
  uc_close(uc);
}

// ==================== Float Tests ====================

TEST(RewriteCodegen_x64, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uint64_t rdi = 10, rsi = 25;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 10, x1 = 25;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "armv7-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, DATA_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  // Enable VFP: CPACR allows CP10/CP11, FPEXC.EN=1
  uint32_t cpacr = 0x00F00000;
  uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &cpacr);
  uint32_t fpexc = (1u << 30);
  uc_reg_write(uc, UC_ARM_REG_FPEXC, &fpexc);

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  uint32_t r0 = 10, r1 = 25;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uc_reg_write(uc, UC_ARM_REG_R1, &r1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "i386-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);
  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  // i386 cdecl: args on stack [ret_addr, arg0, arg1]
  uint32_t stackData[] = {retAddr, 10, 25};
  uc_mem_write(uc, esp, stackData, sizeof(stackData));
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

// ==================== Chained Functions (Large Module) ====================

TEST(RewriteCodegen_x64, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x1000;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint64_t rdi = 100;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 110u) << "chain_entry(100) = 100 + 10 = 110";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x100000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x1000;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint64_t x0 = 100;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 110u) << "chain_entry(100) = 110";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "armv7-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x100000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x1000);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF0000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint32_t r0 = 100;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 110u) << "chain_entry(100) = 110";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "i386-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x2000);
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);
  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  // cdecl: [ret_addr, arg0]
  uint32_t stackData[] = {retAddr, 100};
  uc_mem_write(uc, esp, stackData, sizeof(stackData));

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 110u) << "chain_entry(100) = 110";
  uc_close(uc);
}

// ==================== 64-bit Wide Arithmetic ====================

TEST(RewriteCodegen_x64, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  // wide_mul(1000000000LL, 3LL) = 3000000000 + 7 = 3000000007
  uint64_t rdi = 1000000000ULL, rsi = 3;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 1000000000ULL, x1 = 3;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

// ==================== AbsDiff (Multiple BB + PHI) ====================

TEST(RewriteCodegen_x64, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);

  // abs_diff(3, 10) = |3-10| = 7
  uint64_t rdi = 3, rsi = 10;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

TEST(RewriteCodegen_AArch64, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "aarch64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 3, x1 = 10;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "armv7-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  uint32_t r0 = 3, r1 = 10;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uc_reg_write(uc, UC_ARM_REG_R1, &r1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "i386-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);
  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t stackData[] = {retAddr, 3, 10};
  uc_mem_write(uc, esp, stackData, sizeof(stackData));
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

// ==================== COFF/MachO Matrix Fill ====================

// --- COFF x64: LargeDisplacement ---
TEST(RewriteCodegen_COFF_x64, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X64, BinaryFormat::COFF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}

// --- COFF x64: FloatAddTrunc ---
TEST(RewriteCodegen_COFF_x64, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_mem_map(uc, retAddr & ~0xFFFULL, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rcx = 10, rdx = 25;
  uc_reg_write(uc, UC_X86_REG_RCX, &rcx);
  uc_reg_write(uc, UC_X86_REG_RDX, &rdx);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

// --- COFF x64: ChainedFunctions10 ---
TEST(RewriteCodegen_COFF_x64, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rcx = 100;
  uc_reg_write(uc, UC_X86_REG_RCX, &rcx);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 110u) << "chain_entry(100)=110";
  uc_close(uc);
}

// --- COFF x64: AbsDiff ---
TEST(RewriteCodegen_COFF_x64, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rcx = 3, rdx = 10;
  uc_reg_write(uc, UC_X86_REG_RCX, &rcx);
  uc_reg_write(uc, UC_X86_REG_RDX, &rdx);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

// --- COFF x64: PLTExternalCall ---
TEST(RewriteCodegen_COFF_x64, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "x86_64-pc-windows-msvc", false);
  uint64_t ExtStub = CODE_VA + 0x2000;
  auto Resolve = [ExtStub](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
    if (Sym == "external_fn")
      return ExtStub;
    return std::nullopt;
  };
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF, Resolve);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint8_t stub[] = {0x89, 0xC8, 0xC3};
  uc_mem_write(uc, ExtStub, stub, sizeof(stub));

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rcx = 5;
  uc_reg_write(uc, UC_X86_REG_RCX, &rcx);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 15u) << "wrap_ext(5): external_fn(15)=15";
  uc_close(uc);
}

// --- COFF AArch64: FloatAddTrunc ---
TEST(RewriteCodegen_COFF_AArch64, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 10, x1 = 25;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

// --- COFF AArch64: ChainedFunctions10 ---
TEST(RewriteCodegen_COFF_AArch64, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 100;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 110u) << "chain_entry(100)=110";
  uc_close(uc);
}

// --- COFF AArch64: AbsDiff ---
TEST(RewriteCodegen_COFF_AArch64, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 3, x1 = 10;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

// --- COFF AArch64: PLTExternalCall ---
TEST(RewriteCodegen_COFF_AArch64, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "aarch64-pc-windows-msvc", false);
  uint64_t ExtStub = CODE_VA + 0x2000;
  auto Resolve = [ExtStub](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
    if (Sym == "external_fn")
      return ExtStub;
    return std::nullopt;
  };
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF, Resolve);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t stubCode[] = {0xAA0003E0, 0xD65F03C0};
  uc_mem_write(uc, ExtStub, stubCode, sizeof(stubCode));

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 15u) << "wrap_ext(5): external_fn(15)=15";
  uc_close(uc);
}

// --- COFF ARM32: FloatAddTrunc ---
TEST(RewriteCodegen_COFF_ARM32, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t cpacr = 0x00F00000;
  uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &cpacr);
  uint32_t fpexc = (1u << 30);
  uc_reg_write(uc, UC_ARM_REG_FPEXC, &fpexc);

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  uint32_t r0 = 10, r1 = 25;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uc_reg_write(uc, UC_ARM_REG_R1, &r1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

// --- COFF ARM32: ChainedFunctions10 ---
TEST(RewriteCodegen_COFF_ARM32, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  uint32_t r0 = 100;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  ASSERT_EQ(uc_emu_start(uc, entryVA | 1, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 110u) << "chain_entry(100)=110";
  uc_close(uc);
}

// --- COFF ARM32: AbsDiff ---
TEST(RewriteCodegen_COFF_ARM32, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  uint32_t r0 = 3, r1 = 10;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uc_reg_write(uc, UC_ARM_REG_R1, &r1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

// --- COFF ARM32: PLTExternalCall ---
TEST(RewriteCodegen_COFF_ARM32, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "thumbv7-pc-windows-msvc", false);
  uint64_t ExtStub = CODE_VA + 0x2000;
  auto Resolve = [ExtStub](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
    if (Sym == "external_fn")
      return ExtStub | 1;
    return std::nullopt;
  };
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF, Resolve);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint16_t stubThumb[] = {0x4600, 0x4770};
  uc_mem_write(uc, ExtStub, stubThumb, sizeof(stubThumb));

  uint32_t sp_val = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp_val);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  uint32_t r0 = 5;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 15u) << "wrap_ext(5): external_fn(15)=15";
  uc_close(uc);
}

// --- MachO AArch64: FloatAddTrunc ---
TEST(RewriteCodegen_MachO_AArch64, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "aarch64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 10, x1 = 25;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

// --- MachO AArch64: ChainedFunctions10 ---
TEST(RewriteCodegen_MachO_AArch64, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "aarch64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 100;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 110u) << "chain_entry(100)=110";
  uc_close(uc);
}

// --- MachO AArch64: AbsDiff ---
TEST(RewriteCodegen_MachO_AArch64, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "aarch64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 3, x1 = 10;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

// --- MachO AArch64: PLTExternalCall ---
TEST(RewriteCodegen_MachO_AArch64, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "aarch64-apple-darwin", false);
  uint64_t ExtStub = CODE_VA + 0x2000;
  auto Resolve = [ExtStub](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
    if (Sym == "_external_fn")
      return ExtStub;
    return std::nullopt;
  };
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO, Resolve);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t stubCode[] = {0xAA0003E0, 0xD65F03C0};
  uc_mem_write(uc, ExtStub, stubCode, sizeof(stubCode));

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 5;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0 & 0xFFFFFFFF, 15u) << "wrap_ext(5): external_fn(15)=15";
  uc_close(uc);
}

// --- MachO x64: LargeDisplacement ---
TEST(RewriteCodegen_MachO_x64, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "x86_64-apple-darwin");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X64, BinaryFormat::MachO,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}

// --- MachO x64: FloatAddTrunc ---
TEST(RewriteCodegen_MachO_x64, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "x86_64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rdi = 10, rsi = 25;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

// --- MachO x64: ChainedFunctions10 ---
TEST(RewriteCodegen_MachO_x64, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "x86_64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t entryVA = findSymbolVA(RR, "chain_entry", Text->VA);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rdi = 100;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 110u) << "chain_entry(100)=110";
  uc_close(uc);
}

// --- MachO x64: AbsDiff ---
TEST(RewriteCodegen_MachO_x64, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "x86_64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rdi = 3, rsi = 10;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 7u) << "abs_diff(3,10)=7";
  uc_close(uc);
}

// ==================== i386 WideMul64 / NegDisp / LargeDisp ====================

TEST(RewriteCodegen_i386, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  // i386 cdecl: push args right-to-left, then retAddr
  // wide_mul(int64_t a, int64_t b): a=[ESP+4..11], b=[ESP+12..19]
  uint32_t stack[5];
  stack[0] = retAddr;                 // return address
  stack[1] = 1000000000U; stack[2] = 0; // a = 1000000000 (lo, hi)
  stack[3] = 3;            stack[4] = 0; // b = 3 (lo, hi)
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint32_t eax = 0, edx = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  uc_reg_read(uc, UC_X86_REG_EDX, &edx);
  uint64_t result = (uint64_t)edx << 32 | eax;
  EXPECT_EQ(result, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X86, BinaryFormat::ELF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t retAddr = 0x4FF000;
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t stackData[] = { retAddr };
  uc_mem_write(uc, esp, stackData, sizeof(stackData));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 43u) << "counter=42, inc to 43 (negative delta)";
  uc_close(uc);
}

TEST(RewriteCodegen_i386, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "i386-unknown-linux-gnu");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X86, BinaryFormat::ELF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t retAddr = 0x4FF000;
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t stackData[] = { retAddr };
  uc_mem_write(uc, esp, stackData, sizeof(stackData));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}

// ==================== ARM32 WideMul64 / NegDisp / LargeDisp ====================

TEST(RewriteCodegen_ARM32, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  // ARM32 AAPCS: i64 in r0:r1 (lo:hi), second i64 in r2:r3
  uint32_t r0 = 1000000000U, r1 = 0; // a = 1000000000
  uint32_t r2 = 3, r3 = 0;            // b = 3
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uc_reg_write(uc, UC_ARM_REG_R1, &r1);
  uc_reg_write(uc, UC_ARM_REG_R2, &r2);
  uc_reg_write(uc, UC_ARM_REG_R3, &r3);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  uc_reg_read(uc, UC_ARM_REG_R1, &r1);
  uint64_t result = (uint64_t)r1 << 32 | r0;
  EXPECT_EQ(result, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewriteWithVAs(*Mod, Arch::ARM, BinaryFormat::ELF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = (uint32_t)(0x400000 + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 200), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "counter=42, inc to 43 (negative delta)";
  uc_close(uc);
}

TEST(RewriteCodegen_ARM32, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "arm-unknown-linux-gnueabihf");
  auto RR = compileRewriteWithVAs(*Mod, Arch::ARM, BinaryFormat::ELF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = (uint32_t)(0x400000 + 0xF000);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 200), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}

// ==================== COFF ARM32 WideMul / NegDisp / LargeDisp ====================

TEST(RewriteCodegen_COFF_ARM32, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::ARM, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  uint32_t r0 = 1000000000U, r1 = 0;
  uint32_t r2 = 3, r3 = 0;
  uc_reg_write(uc, UC_ARM_REG_R0, &r0);
  uc_reg_write(uc, UC_ARM_REG_R1, &r1);
  uc_reg_write(uc, UC_ARM_REG_R2, &r2);
  uc_reg_write(uc, UC_ARM_REG_R3, &r3);
  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 5000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  uc_reg_read(uc, UC_ARM_REG_R1, &r1);
  uint64_t result = (uint64_t)r1 << 32 | r0;
  EXPECT_EQ(result, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::ARM, BinaryFormat::COFF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x200000, 0x300000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = 0x40F000;
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 200), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "counter=42, inc to 43 (negative delta)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_ARM32, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "thumbv7-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::ARM, BinaryFormat::COFF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t sp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uc_reg_write(uc, UC_ARM_REG_SP, &sp);
  uint32_t retAddr = 0x40F000;
  uc_reg_write(uc, UC_ARM_REG_LR, &retAddr);
  ASSERT_EQ(uc_emu_start(uc, Text->VA | 1, retAddr, 0, 200), UC_ERR_OK);
  uint32_t r0 = 0;
  uc_reg_read(uc, UC_ARM_REG_R0, &r0);
  EXPECT_EQ(r0, 43u) << "counter=42, inc to 43 (large delta)";
  uc_close(uc);
}

// ==================== COFF/MachO WideMul64 ====================

TEST(RewriteCodegen_COFF_x64, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "x86_64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  // Win64 ABI: rcx=a, rdx=b
  uint64_t rcx = 1000000000ULL, rdx = 3;
  uc_reg_write(uc, UC_X86_REG_RCX, &rcx);
  uc_reg_write(uc, UC_X86_REG_RDX, &rdx);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_AArch64, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "aarch64-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 1000000000ULL, x1 = 3;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_AArch64, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "aarch64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t sp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
  uint64_t retAddr = CODE_VA + 0xF000;
  uc_reg_write(uc, UC_ARM64_REG_LR, &retAddr);
  uint64_t x0 = 1000000000ULL, x1 = 3;
  uc_reg_write(uc, UC_ARM64_REG_X0, &x0);
  uc_reg_write(uc, UC_ARM64_REG_X1, &x1);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 1000), UC_ERR_OK);
  uc_reg_read(uc, UC_ARM64_REG_X0, &x0);
  EXPECT_EQ(x0, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

TEST(RewriteCodegen_MachO_x64, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "x86_64-apple-darwin");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rdi = 1000000000ULL, rsi = 3;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  uc_reg_write(uc, UC_X86_REG_RSI, &rsi);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

// --- MachO x64: PLTExternalCall ---
TEST(RewriteCodegen_MachO_x64, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "x86_64-apple-darwin", false);
  uint64_t ExtStub = CODE_VA + 0x2000;
  auto Resolve = [ExtStub](llvm::StringRef Sym,
                           uint32_t) -> std::optional<uint64_t> {
    if (Sym == "_external_fn")
      return ExtStub;
    return std::nullopt;
  };
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::MachO, Resolve);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint8_t stub[] = {0x89, 0xF8, 0xC3};
  uc_mem_write(uc, ExtStub, stub, sizeof(stub));

  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uc_mem_write(uc, rsp, &retAddr, 8);
  uint64_t rdi = 5;
  uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint64_t rax = 0;
  uc_reg_read(uc, UC_X86_REG_RAX, &rax);
  EXPECT_EQ(rax & 0xFFFFFFFF, 15u) << "wrap_ext(5): external_fn(15)=15";
  uc_close(uc);
}

// ========================= COFF i386 Tests =========================

TEST(RewriteCodegen_COFF_i386, AddFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAddIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty()) << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);
  ASSERT_GT(Text->Bytes.size(), 0u);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 17, 25 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 42u) << "add(17,25) should be 42";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, ComputeFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildComputeIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 5 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 22u) << "compute(5) = 5*3+7 = 22";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, DataReference) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 43u) << "get_and_inc() should return 43 (42+1)";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, CrossFunctionCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildCrossCallIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint32_t callerVA = (uint32_t)findSymbolVA(RR, "caller", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 3, 7 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, callerVA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 22u) << "caller(3,7) should be 22";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, ExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn" || Sym == "_external_fn")
          return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_FALSE(RR.Sections.empty());
  ASSERT_TRUE(RR.Unresolved.empty()) << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, CODE_VA & ~0xFFFULL, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  uc_mem_write(uc, Text->VA, Text->Bytes.data(), Text->Bytes.size());

  uint8_t stub_i386[] = {
    0x8B, 0x44, 0x24, 0x04,  // mov eax, [esp+4]
    0x01, 0xC0,              // add eax, eax
    0xC3                     // ret
  };
  uc_mem_write(uc, EXT_FN_VA, stub_i386, sizeof(stub_i386));

  uint32_t wrapVA = (uint32_t)findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = (uint32_t)(CODE_VA + 0xF000);
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 5 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 30u) << "wrap_ext(5) should be 30";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, LoopFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildLoopIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 10 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 5000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 55u) << "sum_to(10) should be 55";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, SwitchFunction) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSwitchIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_FALSE(RR.Sections.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 2 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 30u) << "classify(2) should be 30";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, MultiGlobal) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildMultiGlobalIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 336u) << "(111+1)+(222+2) = 336";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, NegativeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X86, BinaryFormat::COFF,
                              0x400000, 0x200000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x100000, 0x400000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 43u) << "get_and_inc() = 43 with data below code";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, LargeDisplacement) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildDataRefIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewriteWithVAs(*Mod, Arch::X86, BinaryFormat::COFF,
                              0x400000, 0x10000000);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, 0x10000000, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 43u) << "get_and_inc() = 43 with ~252MB delta";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, FloatAddTrunc) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildFloatAddTruncIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack_data[] = { retAddr, 10, 25 };
  uc_mem_write(uc, esp, stack_data, sizeof(stack_data));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 35u) << "(int)(10.0f + 25.0f) = 35";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, ChainedFunctions10) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildChainedFunctionsIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t entryVA = (uint32_t)findSymbolVA(RR, "chain_entry", Text->VA);
  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 100 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, entryVA, retAddr + 1, 0, 10000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 110u) << "chain_entry(100) = 100+10 = 110";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, AbsDiff) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildAbsDiffIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 3, 10 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 1000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 7u) << "abs_diff(3,10) = 7";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, PLTExternalCall) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildExternalCallIR(Ctx, "i386-pc-windows-msvc", false);
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF,
      [](llvm::StringRef Sym, uint32_t) -> std::optional<uint64_t> {
        if (Sym == "external_fn" || Sym == "_external_fn")
          return EXT_FN_VA;
        return std::nullopt;
      });
  ASSERT_TRUE(RR.Unresolved.empty()) << "Unresolved: " << RR.Unresolved[0];
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_map(uc, EXT_FN_VA, 0x1000, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint8_t stub_i386[] = {
    0x8B, 0x44, 0x24, 0x04,
    0x01, 0xC0,
    0xC3
  };
  uc_mem_write(uc, EXT_FN_VA, stub_i386, sizeof(stub_i386));

  uint32_t wrapVA = (uint32_t)findSymbolVA(RR, "wrap_ext", Text->VA);
  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  uint32_t stack[] = { retAddr, 5 };
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, wrapVA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint32_t eax = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  EXPECT_EQ(eax, 30u) << "wrap_ext(5)=30 via non-dso_local external";
  uc_close(uc);
}

TEST(RewriteCodegen_COFF_i386, WideMul64) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildWideMulIR(Ctx, "i386-pc-windows-msvc");
  auto RR = compileRewrite(*Mod, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_32, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());

  uint32_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr & ~0xFFFU, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint32_t esp = (uint32_t)(STACK_BASE + STACK_SIZE - 0x100);
  // i386: i64 args as (lo, hi) pairs on stack
  uint32_t stack[5];
  stack[0] = retAddr;
  stack[1] = 1000000000U; stack[2] = 0; // a = 1000000000 (lo, hi)
  stack[3] = 3;            stack[4] = 0; // b = 3 (lo, hi)
  uc_mem_write(uc, esp, stack, sizeof(stack));
  uc_reg_write(uc, UC_X86_REG_ESP, &esp);

  ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr + 1, 0, 2000), UC_ERR_OK);
  uint32_t eax = 0, edx = 0;
  uc_reg_read(uc, UC_X86_REG_EAX, &eax);
  uc_reg_read(uc, UC_X86_REG_EDX, &edx);
  uint64_t result = (uint64_t)edx << 32 | eax;
  EXPECT_EQ(result, 3000000007ULL) << "wide_mul(1B, 3) = 3B + 7";
  uc_close(uc);
}

// ---------------------------------------------------------------------------
// Isolated smax/smin clamp test — diagnose LLVM fork codegen
// ---------------------------------------------------------------------------
static std::unique_ptr<llvm::Module>
buildSmaxSminClampIR(llvm::LLVMContext &Ctx, const char *Triple) {
  auto M = std::make_unique<llvm::Module>("smaxtest", Ctx);
  M->setTargetTriple(llvm::Triple(Triple));
  auto *I32 = llvm::Type::getInt32Ty(Ctx);
  auto *I16 = llvm::Type::getInt16Ty(Ctx);
  auto *I8 = llvm::Type::getInt8Ty(Ctx);
  auto *FTy = llvm::FunctionType::get(I32, {I32}, false);
  auto *F = llvm::Function::Create(FTy, llvm::Function::ExternalLinkage,
                                   "clamp_i16", M.get());
  auto *BB = llvm::BasicBlock::Create(Ctx, "entry", F);
  llvm::IRBuilder<> B(BB);
  auto *Trunc16 = B.CreateTrunc(F->getArg(0), I16);
  auto *SmaxFn = llvm::Intrinsic::getOrInsertDeclaration(
      M.get(), llvm::Intrinsic::smax, {I16});
  auto *SminFn = llvm::Intrinsic::getOrInsertDeclaration(
      M.get(), llvm::Intrinsic::smin, {I16});
  auto *Clamped1 = B.CreateCall(SmaxFn, {Trunc16, B.getInt16(-128)});
  auto *Clamped2 = B.CreateCall(SminFn, {Clamped1, B.getInt16(127)});
  auto *Narrow = B.CreateTrunc(Clamped2, I8);
  auto *Result = B.CreateZExt(Narrow, I32);
  B.CreateRet(Result);
  assert(!llvm::verifyModule(*M, &llvm::errs()));
  return M;
}

TEST(RewriteCodegen_SmaxSmin, ClampI16_viaRewrite) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSmaxSminClampIR(Ctx, "x86_64-unknown-linux-elf");
  auto RR = compileRewrite(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(RR.Unresolved.empty());
  auto *Text = findTextSection(RR);
  ASSERT_NE(Text, nullptr);

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uc_mem_map(uc, 0x400000, 0x200000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  for (auto &S : RR.Sections)
    if (!S.Bytes.empty())
      uc_mem_write(uc, S.VA, S.Bytes.data(), S.Bytes.size());
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);
  uint64_t rsp = STACK_BASE + STACK_SIZE - 0x100;
  uc_mem_write(uc, rsp, &retAddr, 8);
  uc_reg_write(uc, UC_X86_REG_RSP, &rsp);

  struct { int64_t input; uint64_t expected; } cases[] = {
    {42, 42}, {200, 127}, {-200, 128}, {127, 127}, {-128, 128}, {0, 0},
  };
  for (auto &tc : cases) {
    uint64_t rdi = (uint64_t)tc.input;
    uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
    rsp = STACK_BASE + STACK_SIZE - 0x100;
    uc_mem_write(uc, rsp, &retAddr, 8);
    uc_reg_write(uc, UC_X86_REG_RSP, &rsp);
    ASSERT_EQ(uc_emu_start(uc, Text->VA, retAddr, 0, 200), UC_ERR_OK);
    uint64_t rax = 0;
    uc_reg_read(uc, UC_X86_REG_RAX, &rax);
    EXPECT_EQ(rax, tc.expected)
        << "clamp_i16(" << tc.input << ") rewrite = " << rax;
  }
  uc_close(uc);
}

TEST(RewriteCodegen_SmaxSmin, ClampI16_viaCompile) {
  ensureLLVMTargets();
  llvm::LLVMContext Ctx;
  auto Mod = buildSmaxSminClampIR(Ctx, "x86_64-unknown-linux-elf");

  Codegen CG;
  auto CR = CG.compile(*Mod, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(CR.Success) << "compile failed";
  ASSERT_FALSE(CR.ObjectData.empty());

  auto ObjBuf = llvm::MemoryBuffer::getMemBuffer(
      llvm::StringRef(reinterpret_cast<const char *>(CR.ObjectData.data()),
                      CR.ObjectData.size()),
      "", false);
  auto ObjOrErr =
      llvm::object::ObjectFile::createObjectFile(ObjBuf->getMemBufferRef());
  ASSERT_TRUE(!!ObjOrErr);
  auto &Obj = **ObjOrErr;
  std::vector<uint8_t> TextBytes;
  for (auto &Sec : Obj.sections()) {
    auto NameOrErr = Sec.getName();
    if (!NameOrErr) continue;
    if (*NameOrErr == ".text") {
      auto ContOrErr = Sec.getContents();
      if (ContOrErr)
        TextBytes.assign(ContOrErr->begin(), ContOrErr->end());
      break;
    }
  }
  ASSERT_FALSE(TextBytes.empty());

  uc_engine *uc = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_X86, UC_MODE_64, &uc), UC_ERR_OK);
  uint64_t codeVA = 0x400000;
  uc_mem_map(uc, codeVA, 0x10000, UC_PROT_ALL);
  uc_mem_map(uc, STACK_BASE, STACK_SIZE, UC_PROT_ALL);
  uc_mem_write(uc, codeVA, TextBytes.data(), TextBytes.size());
  uint64_t retAddr = 0x700000;
  uc_mem_map(uc, retAddr, 0x1000, UC_PROT_ALL);
  uint8_t hlt = 0xF4;
  uc_mem_write(uc, retAddr, &hlt, 1);

  struct { int64_t input; uint64_t expected; } cases[] = {
    {42, 42}, {200, 127}, {-200, 128}, {127, 127}, {-128, 128}, {0, 0},
  };
  for (auto &tc : cases) {
    uint64_t rdi = (uint64_t)tc.input;
    uc_reg_write(uc, UC_X86_REG_RDI, &rdi);
    uint64_t rsp2 = STACK_BASE + STACK_SIZE - 0x100;
    uc_mem_write(uc, rsp2, &retAddr, 8);
    uc_reg_write(uc, UC_X86_REG_RSP, &rsp2);
    ASSERT_EQ(uc_emu_start(uc, codeVA, retAddr, 0, 200), UC_ERR_OK);
    uint64_t rax = 0;
    uc_reg_read(uc, UC_X86_REG_RAX, &rax);
    EXPECT_EQ(rax, tc.expected)
        << "clamp_i16(" << tc.input << ") compile = " << rax;
  }
  uc_close(uc);
}
