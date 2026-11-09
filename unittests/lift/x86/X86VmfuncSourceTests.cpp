//===- X86VmfuncSourceTests.cpp - Proven VMFUNC native source -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify EPTP-switch selection and exact source transport. Optional complete
/// source/object exports feed a controlled oracle; no EPT switch executes here.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86Virtualization.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <array>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

namespace {

TEST(X86VmfuncSource, OnlyProvenZeroSelectsTheNativeEptpInterface) {
  for (auto A : {Arch::X86, Arch::X64}) {
    for (const auto Leaf :
         {std::optional<uint64_t>{}, std::optional<uint64_t>{1},
          std::optional<uint64_t>{0x80000000},
          std::optional<uint64_t>{0x100000000}})
      EXPECT_FALSE(x86VirtualizationNative(
          Intrinsic::VmxFunction, A, NdMemoryAddressSpace::Default, 0, Leaf));
    const auto Plan = x86VirtualizationNative(
        Intrinsic::VmxFunction, A, NdMemoryAddressSpace::Default, 0, 0);
    ASSERT_TRUE(Plan);
    EXPECT_EQ(Plan->LLVMName, "llvm.x86.vmfunc.eptp.switch");
    EXPECT_EQ(Plan->Builtin, "__builtin_ia32_vmfunc_eptp_switch");
    EXPECT_EQ(Plan->FirstArgument, 1u);
    EXPECT_EQ(Plan->Result, X86VirtualizationResult::None);
    EXPECT_EQ(Plan->Assembly.InputBytes, (std::vector<uint16_t>{4, 4}));
  }
}

TEST(X86VmfuncSource, HighCRequiresACompleteConstantLeafAndEvaluatesIndexOnce) {
  for (auto A : {Arch::X86, Arch::X64})
    for (int Kind = 0; Kind != 4; ++Kind) {
      auto Leaf = HighExpr::makeConst(Kind == 1 ? 1 : 0, 4);
      if (Kind == 2) {
        MedVar V;
        V.Id = 1;
        V.Size = 4;
        Leaf = HighExpr::makeVar(V);
      }
      if (Kind == 3)
        Leaf = HighExpr::makeConst(0, 2);
      auto E = HighExpr::makeCall("vmfunc", 0,
                                  {Leaf, HighExpr::makeConst(0xffffffff, 4)});
      E->IntrinsicId = Intrinsic::VmxFunction;
      E->Type = NdType::makeVoid();
      if (Kind == 3) {
        EXPECT_THROW(renderX86VirtualizationExpression(A, *E, {"0", "next()"}),
                     std::invalid_argument);
        continue;
      }
      const auto Text = renderX86VirtualizationExpression(
          A, *E, {Kind == 2 ? "leaf" : std::to_string(Kind), "next()"});
      const auto Native = Text.find("__builtin_ia32_vmfunc_eptp_switch");
      EXPECT_EQ(Native != std::string::npos, Kind == 0) << Text;
      if (Kind == 0) {
        const auto End = Text.find("}),", Native);
        const auto First = Text.find("next()", Native);
        ASSERT_LT(First, End);
        EXPECT_GT(Text.find("next()", First + 1), End);
      }
      EXPECT_NE(Text.find("\"a\"((uint32_t)("), std::string::npos);
      EXPECT_NE(Text.find("\"c\"((uint32_t)(next()))"), std::string::npos);
    }
}

llvm::CallInst *call(llvm::Module &M, bool Native, unsigned Mutation) {
  llvm::IRBuilder<> B(M.getContext());
  auto *IndexType = Mutation == 1 ? B.getInt64Ty() : B.getInt32Ty();
  auto *Owner = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {IndexType}, false),
      llvm::GlobalValue::ExternalLinkage, "probe", M);
  B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", Owner));
  llvm::CallInst *Call;
  if (Native) {
    const auto Name = Mutation == 2 ? "llvm.x86.vmfunc.unowned"
                                    : "llvm.x86.vmfunc.eptp.switch";
    auto Callee = M.getOrInsertFunction(
        Name, llvm::FunctionType::get(B.getVoidTy(), {IndexType}, false));
    auto *Function = llvm::cast<llvm::Function>(Callee.getCallee());
    Function->addFnAttr(llvm::Attribute::NoUnwind);
    if (Mutation == 3) {
      llvm::IRBuilder<> Body(
          llvm::BasicBlock::Create(M.getContext(), "body", Function));
      Body.CreateRetVoid();
    }
    Call = B.CreateCall(Callee, {Owner->getArg(0)});
  } else {
    const auto Plan = x86VirtualizationAsm(Intrinsic::VmxFunction, Arch::X64,
                                           NdMemoryAddressSpace::Default);
    auto *Type = llvm::FunctionType::get(B.getVoidTy(),
                                         {B.getInt32Ty(), IndexType}, false);
    auto *Assembly = llvm::InlineAsm::get(Type, Plan->Text, Plan->Constraints,
                                          Mutation != 2);
    Call = B.CreateCall(Assembly,
                        {B.getInt32(Mutation == 3 ? 1 : 0), Owner->getArg(0)});
  }
  if (Mutation == 4)
    Call->setMemoryEffects(llvm::MemoryEffects::readOnly());
  if (Mutation == 5)
    Call->setMemoryEffects(llvm::MemoryEffects::writeOnly());
  if (Mutation == 6)
    Call->setCallingConv(llvm::CallingConv::Fast);
  const std::array Attributes = {
      llvm::Attribute::NoReturn, llvm::Attribute::WillReturn,
      llvm::Attribute::Speculatable, llvm::Attribute::NoSync};
  if (Mutation >= 7 && Mutation <= 10)
    Call->addFnAttr(Attributes[Mutation - 7]);
  B.CreateRetVoid();
  return Call;
}

TEST(X86VmfuncSource, NativeAndAssemblyContractsCannotLoseTypesOrEffects) {
  for (auto A : {Arch::X86, Arch::X64})
    for (bool Native : {false, true})
      for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
        SCOPED_TRACE(Native);
        SCOPED_TRACE(Mutation);
        llvm::LLVMContext Context;
        llvm::Module M("vmfunc", Context);
        auto *Call = call(M, Native, Mutation);
        auto Render = [&] {
          return Native ? renderX86VirtualizationNativeCall(A, *Call, "", false,
                                                            {"index"})
                        : renderX86VirtualizationCall(
                              A, *Call, "", false,
                              {Mutation == 3 ? "1" : "0", "index"});
        };
        if (!Mutation || (!Native && Mutation == 3)) {
          const auto Text = Render();
          ASSERT_TRUE(Text);
          EXPECT_EQ(Text->find("__builtin_ia32_vmfunc_eptp_switch") !=
                        std::string::npos,
                    Mutation == 0);
          EXPECT_NE(Text->find("\"memory\""), std::string::npos);
        } else
          EXPECT_THROW((void)Render(), std::invalid_argument);
      }
}

std::vector<uint8_t> original(unsigned Mode, uint32_t Index,
                              unsigned LeafKind) {
  std::vector<uint8_t> Bytes;
  if (LeafKind == 3 && Mode == 64)
    Bytes = {0x48, 0xb8, 0, 0, 0, 0, 0x10, 0x32, 0x54, 0x76};
  else if (LeafKind == 2)
    Bytes = {0xb8, 0xff, 0xff, 0xff, 0xff, 0x66, 0xb8, 0, 0};
  else
    Bytes = {0xb8, uint8_t(LeafKind == 1), 0, 0, 0};
  Bytes.push_back(0xb9);
  for (unsigned I = 0; I != 4; ++I)
    Bytes.push_back(uint8_t(Index >> (8 * I)));
  // VMFUNC preserves ECX. Returning it also checks the complete index carrier.
  Bytes.insert(Bytes.end(), {0x0f, 0x01, 0xd4, 0x89, 0xc8, 0xc3});
  return Bytes;
}

void write(llvm::StringRef Path, llvm::StringRef Text) {
  std::error_code Error;
  llvm::raw_fd_ostream Out(Path, Error);
  ASSERT_FALSE(Error) << Error.message();
  Out << Text;
}

TEST(X86VmfuncSource, CompletePipelineRetainsLeafIndexAndNativeObjects) {
  const char *Directory = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR");
  if (Directory)
    ASSERT_FALSE(llvm::sys::fs::create_directories(Directory));
  unsigned Cases = 0;
  for (auto A : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool NoOpt : {false, true})
        for (uint32_t Index : {0u, 511u, 512u, 0x800001ffu, 0xffffffffu})
          for (unsigned LeafKind = 0; LeafKind != 4; ++LeafKind) {
            if (LeafKind == 3 && A == Arch::X86)
              continue;
            SCOPED_TRACE(unsigned(A));
            SCOPED_TRACE(unsigned(Format));
            SCOPED_TRACE(NoOpt);
            SCOPED_TRACE(Index);
            SCOPED_TRACE(LeafKind);
            BinaryImage Image;
            Image.Arch = A;
            Image.Format = Format;
            Image.Bits = A == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
            Image.Base = Image.Entry = 0x1000;
            Segment Text;
            Text.VA = Image.Entry;
            Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
            Text.Data = original(A == Arch::X64 ? 64 : 32, Index, LeafKind);
            Text.Size = Text.FileSz = Text.Data.size();
            auto Symbol = Symbol::makeFunc(Text.VA, Text.Size);
            Symbol.Name = "vmfunc_pipeline";
            Image.Symbols.push_back(std::move(Symbol));
            Image.Segments.push_back(Text);
            PipelineOptions O;
            O.NoOpt = NoOpt;
            O.EmitDumpOutput = false;
            O.OnlyFunctionEntries = {Image.Entry};
            llvm::LLVMContext Context;
            auto R = Pipeline().run(Image, Context, O);
            ASSERT_TRUE(R.Success) << R.Error;
            CEmitterOptions Options;
            Options.TheArch = A;
            Options.Format = Format;
            Options.Image = &Image;
            std::string High, LLVM, IR;
            llvm::raw_string_ostream HighOS(High), LLVMOS(LLVM), IROS(IR);
            ASSERT_TRUE(HighCEmitter().emit(R.HighFuncs, HighOS, Options));
            MedLLVMEmitter Emitter;
            auto M = Emitter.emit(R.MedFuncs, Context, "vmfunc", A, {}, &Image,
                                  Format);
            ASSERT_NE(M, nullptr);
            ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
            if (NoOpt)
              Pipeline::promoteScaffoldingAllocas(*M);
            else
              (void)Pipeline::optimizeOrPromoteModule(*M);
            ASSERT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
            ASSERT_TRUE(LLVMCEmitter().emit(*M, LLVMOS, Options));
            M->print(IROS, nullptr);
            const bool ZeroLeaf = LeafKind == 0 || LeafKind == 3;
            for (const auto &Source : {High, LLVM}) {
              // The declaration-only capability preamble also names the API.
              EXPECT_EQ(Source.find("__builtin_ia32_vmfunc_eptp_switch((") !=
                            std::string::npos,
                        ZeroLeaf)
                  << Source;
            }
            const bool NativeAvailable =
                llvm::Intrinsic::lookupIntrinsicID("llvm.x86.vmread.address") &&
                llvm::Intrinsic::lookupIntrinsicID(
                    "llvm.x86.vmfunc.eptp.switch");
            EXPECT_EQ(IR.find("@llvm.x86.vmfunc.eptp.switch(") !=
                          std::string::npos,
                      ZeroLeaf && NativeAvailable)
                << IR;
            auto Copy = llvm::CloneModule(*M);
            const auto Object = Codegen().compile(*Copy, A, Format);
            ASSERT_TRUE(Object.Success);
            ASSERT_FALSE(Object.ObjectData.empty());
            if (Directory) {
              const auto Stem = std::string(Directory) + "/vmfunc-" +
                                (A == Arch::X64 ? "64" : "32") + "-" +
                                std::to_string(unsigned(Format)) + "-" +
                                std::to_string(NoOpt) + "-" +
                                std::to_string(Index) + "-" +
                                std::to_string(LeafKind);
              write(Stem + "-high.c", High);
              write(Stem + "-llvm.c", LLVM);
              write(Stem + ".ll", IR);
              write(Stem + ".bin",
                    llvm::StringRef(
                        reinterpret_cast<const char *>(Text.Data.data()),
                        Text.Data.size()));
              write(Stem + ".o", llvm::StringRef(reinterpret_cast<const char *>(
                                                     Object.ObjectData.data()),
                                                 Object.ObjectData.size()));
            }
            ++Cases;
            if (::testing::Test::HasFailure())
              return;
          }
  EXPECT_EQ(Cases, 210u);
  RecordProperty("complete_vmfunc_pipeline_profiles", Cases);
}

TEST(X86VmfuncSource, EffectfulIndexAndPostSwitchMemoryRemainOrdered) {
  const char *Directory = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR");
  for (auto A : {Arch::X86, Arch::X64}) {
    auto E = HighExpr::makeCall(
        "vmfunc", 0, {HighExpr::makeConst(0, 4), HighExpr::makeConst(0, 4)});
    E->IntrinsicId = Intrinsic::VmxFunction;
    E->Type = NdType::makeVoid();
    const auto High =
        std::string("#include <stdint.h>\n") +
        X86VirtualizationCompilerDispatch +
        "uint32_t vmfunc_effects(void) {\n"
        "uint32_t *memory = (uint32_t *)(uintptr_t)0x40000;\n" +
        renderX86VirtualizationExpression(A, *E, {"0", "(*memory)++"}) +
        ";\nreturn *memory;\n}\n";
    llvm::LLVMContext Context;
    llvm::Module M("vmfunc-effects", Context);
    M.setDataLayout(A == Arch::X64 ? "e-p:64:64" : "e-p:32:32");
    llvm::IRBuilder<> B(Context);
    auto *Owner = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), {}, false),
        llvm::GlobalValue::ExternalLinkage, "vmfunc_effects", M);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Owner));
    auto *Pointer = B.CreateIntToPtr(
        B.getIntN(A == Arch::X64 ? 64 : 32, 0x40000), B.getPtrTy());
    auto *Index = B.CreateLoad(B.getInt32Ty(), Pointer);
    B.CreateStore(B.CreateAdd(Index, B.getInt32(1)), Pointer);
    auto Callee = M.getOrInsertFunction(
        "llvm.x86.vmfunc.eptp.switch",
        llvm::FunctionType::get(B.getVoidTy(), {B.getInt32Ty()}, false));
    llvm::cast<llvm::Function>(Callee.getCallee())
        ->addFnAttr(llvm::Attribute::NoUnwind);
    B.CreateCall(Callee, {Index});
    B.CreateRet(B.CreateLoad(B.getInt32Ty(), Pointer));
    ASSERT_FALSE(llvm::verifyModule(M, &llvm::errs()));
    std::string LLVM;
    llvm::raw_string_ostream LLVMOS(LLVM);
    CEmitterOptions Options;
    Options.TheArch = A;
    Options.Format = BinaryFormat::ELF;
    ASSERT_TRUE(LLVMCEmitter().emit(M, LLVMOS, Options));
    EXPECT_NE(LLVM.find("__builtin_ia32_vmfunc_eptp_switch(("),
              std::string::npos);
    if (Directory) {
      const auto Stem =
          std::string(Directory) + "/effects-" + (A == Arch::X64 ? "64" : "32");
      write(Stem + "-high.c", High);
      write(Stem + "-llvm.c", LLVM);
      // mov edx,0x40000; mov ecx,[edx]; add dword [edx],1; xor eax,eax;
      // vmfunc; mov eax,[edx]; ret. Address stays identical in both code modes.
      const char Raw[] = {char(0xba), 0,          0,          4,    0,
                          char(0x8b), 0x0a,       char(0x83), 0x02, 1,
                          0x31,       char(0xc0), 0x0f,       1,    char(0xd4),
                          char(0x8b), 0x02,       char(0xc3)};
      write(Stem + ".bin", llvm::StringRef(Raw, sizeof Raw));
    }
  }
}

} // namespace
