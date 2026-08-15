//===- RewriteFunctionProvenanceTests.cpp ---------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/CodeGen.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <gtest/gtest.h>
#include <limits>

using namespace neverd;

namespace {

TEST(RewriteFunctionProvenance, PreservesPrivateUnwindOwner) {
  llvm::LLVMContext Context;
  llvm::Module Module("private-owner", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);

  auto *Helper =
      llvm::Function::Create(FunctionType, llvm::GlobalValue::PrivateLinkage,
                             "private_helper", Module);
  Helper->addFnAttr(llvm::Attribute::NoInline);
  Helper->setUWTableKind(llvm::UWTableKind::Default);
  llvm::IRBuilder<> HelperBuilder(
      llvm::BasicBlock::Create(Context, "entry", Helper));
  HelperBuilder.CreateRetVoid();

  auto *Entry = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "entry", Module);
  llvm::IRBuilder<> EntryBuilder(
      llvm::BasicBlock::Create(Context, "entry", Entry));
  EntryBuilder.CreateCall(Helper);
  EntryBuilder.CreateRetVoid();

  CompiledImage Image = compileImageForPatch(
      Module, Arch::X64, BinaryFormat::ELF, 0x100000,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      });

  ASSERT_TRUE(Image.Success);
  ASSERT_TRUE(Image.FunctionRangesValid);
  ASSERT_EQ(Image.SourceFunctionOwners.size(), 2u);
  const llvm::mc_rewrite::RewriteSourceFunctionOwner *PrivateOwner = nullptr;
  for (const auto &SourceOwner : Image.SourceFunctionOwners)
    if (SourceOwner.SourceFunction == "private_helper")
      PrivateOwner = &SourceOwner;
  ASSERT_NE(PrivateOwner, nullptr);

  const CompiledFunctionRange *PrivateRange = nullptr;
  for (const CompiledFunctionRange &Candidate : Image.FunctionRanges)
    if (Candidate.OwnerSymbol == PrivateOwner->OwnerSymbol) {
      ASSERT_EQ(PrivateRange, nullptr);
      PrivateRange = &Candidate;
    }
  ASSERT_NE(PrivateRange, nullptr);
  const CompiledFunctionRange &Range = *PrivateRange;
  EXPECT_FALSE(Range.OwnerSymbol.empty());
  EXPECT_EQ(Image.SymbolAddrs.count(Range.OwnerSymbol), 0u);
  const auto Owner = Image.FunctionOwnerAddrs.find(Range.OwnerSymbol);
  ASSERT_NE(Owner, Image.FunctionOwnerAddrs.end());
  EXPECT_EQ(Owner->second, Range.OwnerVA);
  EXPECT_TRUE(llvm::mc_rewrite::validateRewriteFunctionRanges(
      Image.FunctionRanges, Image.FunctionOwnerAddrs));

  EXPECT_EQ(PrivateOwner->OwnerSymbol, Range.OwnerSymbol);
  EXPECT_EQ(PrivateOwner->OwnerVA, Range.OwnerVA);

  exception_rewrite::Requirements Requirements;
  Requirements.RequiresRegisteredUnwind = true;
  Requirements.Functions.push_back({"private_helper", true});
  auto Resolved =
      exception_rewrite::resolveRequiredFunctionOwners(Requirements, Image);
  ASSERT_TRUE(static_cast<bool>(Resolved))
      << llvm::toString(Resolved.takeError());
  ASSERT_EQ(Resolved->size(), 1u);
  EXPECT_EQ(Resolved->front().OwnerSymbol, Range.OwnerSymbol);
  EXPECT_EQ(Resolved->front().OwnerVA, Range.OwnerVA);
}

TEST(RewriteFunctionProvenance,
     SourceOwnerIdentityIsBijectiveButAddressesMayAlias) {
  using llvm::mc_rewrite::RewriteSourceFunctionOwner;
  const std::vector<RewriteSourceFunctionOwner> SameAddress = {
      {"first", "_first", 0x1000}, {"second", "_second", 0x1000}};
  EXPECT_TRUE(
      llvm::mc_rewrite::validateRewriteSourceFunctionOwners(SameAddress));

  auto DuplicateSource = SameAddress;
  DuplicateSource[1].SourceFunction = "first";
  EXPECT_FALSE(
      llvm::mc_rewrite::validateRewriteSourceFunctionOwners(DuplicateSource));

  auto DuplicateOwner = SameAddress;
  DuplicateOwner[1].OwnerSymbol = "_first";
  EXPECT_FALSE(
      llvm::mc_rewrite::validateRewriteSourceFunctionOwners(DuplicateOwner));
}

TEST(RewriteFunctionProvenance, PreservesExactOriginalSourceEntry) {
  llvm::LLVMContext Context;
  llvm::Module Module("source-entry", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function =
      llvm::Function::Create(FunctionType, llvm::GlobalValue::ExternalLinkage,
                             "name_without_an_address", Module);
  rewrite_source::setOriginalVA(*Function, 0x401000);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();

  CompiledImage Image = compileImageForPatch(
      Module, Arch::X64, BinaryFormat::ELF, 0x800000,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      });

  ASSERT_TRUE(Image.Success);
  ASSERT_EQ(Image.SourceFunctionOriginalVAs.size(), 1u);
  EXPECT_EQ(Image.SourceFunctionOriginalVAs.at("name_without_an_address"),
            0x401000u);
  ASSERT_EQ(Image.SourceFunctionOwners.size(), 1u);
  EXPECT_EQ(Image.SourceFunctionOwners.front().SourceFunction,
            "name_without_an_address");
}

TEST(RewriteFunctionProvenance,
     RejectsDuplicateOrMalformedOriginalSourceEntries) {
  for (bool Malformed : {false, true}) {
    SCOPED_TRACE(Malformed);
    llvm::LLVMContext Context;
    llvm::Module Module("invalid-source-entry", Context);
    auto *FunctionType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
    auto AddFunction = [&](llvm::StringRef Name) {
      auto *Function = llvm::Function::Create(
          FunctionType, llvm::GlobalValue::ExternalLinkage, Name, Module);
      rewrite_source::setOriginalVA(*Function, 0x401000);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      Builder.CreateRetVoid();
      return Function;
    };
    llvm::Function *First = AddFunction("first");
    if (Malformed) {
      First->setMetadata(rewrite_source::FunctionAttachment,
                         llvm::MDNode::get(Context, {}));
    } else {
      AddFunction("second");
    }

    CompiledImage Image = compileImageForPatch(
        Module, Arch::X64, BinaryFormat::ELF, 0x800000,
        [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
          return std::nullopt;
        });
    EXPECT_FALSE(Image.Success);
    EXPECT_TRUE(Image.Bytes.empty());
    EXPECT_TRUE(Image.SourceFunctionOriginalVAs.empty());
  }
}

TEST(RewriteFunctionProvenance, ResolvesPrivateMachOOwnerWithoutPublishingIt) {
  llvm::LLVMContext Context;
  llvm::Module Module("private-macho-owner", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::PrivateLinkage, "private_macho", Module);
  Function->setUWTableKind(llvm::UWTableKind::Default);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();

  CompiledImage Image = compileImageForPatch(
      Module, Arch::AArch64, BinaryFormat::MachO, 0x300000,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      });

  ASSERT_TRUE(Image.Success);
  ASSERT_TRUE(Image.FunctionRangesValid);
  ASSERT_EQ(Image.SourceFunctionOwners.size(), 1u);
  const auto &Owner = Image.SourceFunctionOwners.front();
  EXPECT_EQ(Owner.SourceFunction, "private_macho");
  EXPECT_FALSE(Owner.OwnerSymbol.empty());
  EXPECT_EQ(Image.SymbolAddrs.count(Owner.OwnerSymbol), 0u);
  ASSERT_EQ(Image.FunctionRanges.size(), 1u);
  EXPECT_EQ(Image.FunctionRanges.front().OwnerSymbol, Owner.OwnerSymbol);
  EXPECT_EQ(Image.FunctionRanges.front().OwnerVA, Owner.OwnerVA);

  exception_rewrite::Requirements Requirements;
  Requirements.RequiresRegisteredUnwind = true;
  Requirements.Functions.push_back({"private_macho", true});
  auto Resolved =
      exception_rewrite::resolveRequiredFunctionOwners(Requirements, Image);
  ASSERT_TRUE(static_cast<bool>(Resolved))
      << llvm::toString(Resolved.takeError());
  ASSERT_EQ(Resolved->size(), 1u);
  EXPECT_EQ(Resolved->front().OwnerSymbol, Owner.OwnerSymbol);
  EXPECT_EQ(Resolved->front().OwnerVA, Owner.OwnerVA);
}

TEST(RewriteFunctionProvenance, PreservesARMSourceOwnerWithoutDwarfCFI) {
  llvm::LLVMContext Context;
  llvm::Module Module("arm-ehabi-owner", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "f", Module);
  Function->setUWTableKind(llvm::UWTableKind::Default);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();

  CompiledImage Image = compileImageForPatch(
      Module, Arch::ARM, BinaryFormat::ELF, 0x400000,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      });

  ASSERT_TRUE(Image.Success);
  ASSERT_TRUE(Image.FunctionRangesValid);
  ASSERT_EQ(Image.SourceFunctionOwners.size(), 1u);
  EXPECT_EQ(Image.SourceFunctionOwners.front().SourceFunction, "f");
  EXPECT_EQ(Image.SourceFunctionOwners.front().OwnerSymbol, "f");
  EXPECT_EQ(Image.FunctionRanges.size(), 0u);
}

TEST(RewriteFunctionProvenance,
     ExplicitARMv7kMachOTargetAuthenticatesUnwindRange) {
  llvm::LLVMContext Context;
  llvm::Module Module("armv7k-macho-owner", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "f", Module);
  Function->setUWTableKind(llvm::UWTableKind::Default);
  Function->addFnAttr("frame-pointer", "all");
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::AllocaInst *Slot = Builder.CreateAlloca(Builder.getInt32Ty());
  llvm::StoreInst *Store = Builder.CreateStore(Builder.getInt32(1), Slot);
  Store->setVolatile(true);
  Builder.CreateRetVoid();

  CompiledImage Image = compileImageForPatch(
      Module, Arch::ARM, BinaryFormat::MachO, 0x400000,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      /*ImageBaseVA=*/0, "thumbv7k-apple-watchos");

  ASSERT_TRUE(Image.Success);
  EXPECT_EQ(Image.TargetTriple, "thumbv7k-apple-watchos");
  EXPECT_EQ(Image.TargetMode, InstructionMode::Thumb);
  ASSERT_TRUE(Image.FunctionRangesValid);
  ASSERT_TRUE(Image.Unresolved.empty());
  ASSERT_EQ(Image.SourceFunctionOwners.size(), 1u);
  ASSERT_EQ(Image.FunctionRanges.size(), 1u);
  const auto &Owner = Image.SourceFunctionOwners.front();
  const auto &Range = Image.FunctionRanges.front();
  EXPECT_EQ(Owner.SourceFunction, "f");
  EXPECT_EQ(Range.OwnerSymbol, Owner.OwnerSymbol);
  EXPECT_EQ(Range.OwnerVA, Owner.OwnerVA);
  EXPECT_LT(Range.BeginVA, Range.EndVA);
  EXPECT_TRUE(llvm::mc_rewrite::validateRewriteFunctionRanges(
      Image.FunctionRanges, Image.FunctionOwnerAddrs));
}

TEST(RewriteFunctionProvenance, ARMv7kRejectsARMv8OnlyIRRequirements) {
  llvm::LLVMContext Context;
  llvm::Module Module("armv7k-feature-ceiling", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "f", Module);
  auto *RoundType =
      llvm::FunctionType::get(llvm::Type::getFloatTy(Context),
                              {llvm::Type::getFloatTy(Context)}, false);
  auto *Round = llvm::Function::Create(
      RoundType, llvm::GlobalValue::ExternalLinkage, "llvm.round.f32", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateCall(
      Round, llvm::ConstantFP::get(llvm::Type::getFloatTy(Context), 1.5));
  Builder.CreateRetVoid();

  testing::internal::CaptureStderr();
  CompiledImage Image = compileImageForPatch(
      Module, Arch::ARM, BinaryFormat::MachO, 0x400000,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      /*ImageBaseVA=*/0, "thumbv7k-apple-watchos");
  const std::string Diagnostic = testing::internal::GetCapturedStderr();

  EXPECT_FALSE(Image.Success);
  EXPECT_TRUE(Image.Bytes.empty());
  EXPECT_NE(Diagnostic.find("exceeding the input CPU feature ceiling"),
            std::string::npos)
      << Diagnostic;
}

TEST(RewriteFunctionProvenance,
     DirectRewriteRejectsSameArchitectureWrongObjectFormat) {
  llvm::LLVMContext Context;
  llvm::Module Module("wrong-format-rewrite-triple", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "f", Module);
  llvm::IRBuilder<>(llvm::BasicBlock::Create(Context, "entry", Function))
      .CreateRetVoid();

  llvm::mc_rewrite::RewriteOptions Options;
  Codegen Compiler;
  testing::internal::CaptureStderr();
  llvm::mc_rewrite::RewriteResult Result = Compiler.compileForRewrite(
      Module, Arch::ARM, Options, BinaryFormat::MachO,
      "armv7k-unknown-linux-gnueabihf");
  const std::string Diagnostic = testing::internal::GetCapturedStderr();

  EXPECT_FALSE(Result.ImageValid);
  EXPECT_TRUE(Result.Sections.empty());
  EXPECT_NE(Diagnostic.find("does not match the requested architecture and "
                            "object format"),
            std::string::npos)
      << Diagnostic;
}

TEST(RewriteFunctionProvenance, RejectsMismatchedExplicitTargetTriple) {
  llvm::LLVMContext Context;
  llvm::Module Module("mismatched-rewrite-triple", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "f", Module);
  llvm::IRBuilder<>(llvm::BasicBlock::Create(Context, "entry", Function))
      .CreateRetVoid();

  CompiledImage Image = compileImageForPatch(
      Module, Arch::ARM, BinaryFormat::MachO, 0x400000,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      /*ImageBaseVA=*/0, "x86_64-apple-macos14.0");

  EXPECT_FALSE(Image.Success);
  EXPECT_TRUE(Image.Bytes.empty());
  EXPECT_TRUE(Image.Sections.empty());
  EXPECT_TRUE(Image.SourceFunctionOwners.empty());
  EXPECT_TRUE(Image.FunctionRanges.empty());
}

TEST(RewriteFunctionProvenance, DefersOnlyProvisionalCrossSectionOverlap) {
  llvm::LLVMContext Context;
  llvm::Module Module("multi-code-section", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);

  auto AddFunction = [&](llvm::StringRef Name, llvm::StringRef Section) {
    auto *Function = llvm::Function::Create(
        FunctionType, llvm::GlobalValue::ExternalLinkage, Name, Module);
    Function->setSection(Section);
    Function->setUWTableKind(llvm::UWTableKind::Default);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    Builder.CreateRetVoid();
  };
  AddFunction("first", ".text.first");
  AddFunction("second", ".text.second");

  CompiledImage Image = compileImageForPatch(
      Module, Arch::X64, BinaryFormat::ELF, 0x200000,
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      });

  ASSERT_TRUE(Image.Success);
  ASSERT_TRUE(Image.FunctionRangesValid);
  ASSERT_EQ(Image.FunctionRanges.size(), 2u);
  EXPECT_TRUE(llvm::mc_rewrite::validateRewriteFunctionRanges(
      Image.FunctionRanges, Image.FunctionOwnerAddrs));
  for (const CompiledFunctionRange &Range : Image.FunctionRanges) {
    const auto Owner = Image.FunctionOwnerAddrs.find(Range.OwnerSymbol);
    ASSERT_NE(Owner, Image.FunctionOwnerAddrs.end());
    EXPECT_EQ(Owner->second, Range.OwnerVA);
  }
}

TEST(RewriteFunctionProvenance, RejectsOverflowingFinalSectionAddress) {
  llvm::LLVMContext Context;
  llvm::Module Module("overflowing-section", Context);
  auto *FunctionType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Function = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "entry", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();

  llvm::mc_rewrite::RewriteOptions Options;
  Options.Model.TextVA = std::numeric_limits<uint64_t>::max();
  Options.Model.getSectionVA = [](llvm::StringRef) {
    return std::numeric_limits<uint64_t>::max();
  };
  Codegen Compiler;
  auto DirectModule = llvm::CloneModule(Module);
  llvm::mc_rewrite::RewriteResult Result = Compiler.compileForRewrite(
      *DirectModule, Arch::X64, Options, BinaryFormat::ELF);
  EXPECT_FALSE(Result.ImageValid);
  EXPECT_TRUE(Result.FunctionRangesValid);
  EXPECT_TRUE(Result.Sections.empty());
  EXPECT_TRUE(Result.SymbolAddrs.empty());
  EXPECT_TRUE(Result.FunctionOwnerAddrs.empty());
  EXPECT_TRUE(Result.FunctionRanges.empty());

  CompiledImage Image = compileImageForPatch(
      Module, Arch::X64, BinaryFormat::ELF,
      std::numeric_limits<uint64_t>::max(),
      [](llvm::StringRef, uint32_t) -> std::optional<uint64_t> {
        return std::nullopt;
      });

  EXPECT_FALSE(Image.Success);
  EXPECT_TRUE(Image.FunctionRangesValid);
  EXPECT_TRUE(Image.Sections.empty());
  EXPECT_TRUE(Image.SymbolAddrs.empty());
  EXPECT_TRUE(Image.FunctionOwnerAddrs.empty());
  EXPECT_TRUE(Image.FunctionRanges.empty());
}

} // namespace
