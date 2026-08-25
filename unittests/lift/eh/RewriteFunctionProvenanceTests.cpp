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
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <limits>

using namespace neverd;

namespace {

class ProvenanceTestMCAsmInfo final : public llvm::MCAsmInfo {
public:
  explicit ProvenanceTestMCAsmInfo(const llvm::MCTargetOptions &Options)
      : MCAsmInfo(Options) {}
};

class SourceOwnerRegistrationFixture {
  llvm::Triple TT;
  llvm::MCTargetOptions Options;
  ProvenanceTestMCAsmInfo MAI;
  llvm::MCRegisterInfo MRI;
  llvm::MCSubtargetInfo STI;
  llvm::MCContext Context;

public:
  SourceOwnerRegistrationFixture()
      : TT("x86_64-pc-windows-msvc"), MAI(Options),
        STI(TT, "", "", "", {}, {}, {}, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr),
        Context(TT, MAI, MRI, STI),
        Assembler(Context, nullptr, nullptr, nullptr) {}

  llvm::MCSymbol *symbol(llvm::StringRef Name) {
    return Context.getOrCreateSymbol(Name);
  }

  llvm::MCAssembler Assembler;
};

enum class SourceOwnerRegistrationCase {
  ExpectThenReceipt,
  ReceiptThenExpect,
  MissingReceipt,
  ReceiptWithoutExpectation,
  DuplicateExpectation,
  DuplicateReceipt,
};

bool validateSourceOwnerRegistration(SourceOwnerRegistrationCase TestCase) {
  using Kind = llvm::mc_rewrite::RewriteSourceFunctionOwnerKind;
  SourceOwnerRegistrationFixture Fixture;
  Fixture.Assembler.registerRewriteSourceFunctionOwner(
      "parent", Fixture.symbol("parent$entry"), /*IsPrivate=*/false);
  auto Expect = [&]() {
    Fixture.Assembler.expectRewriteSourceFunctionOwner(
        "source_catch", Kind::WinCxxCatchFunclet, "parent");
  };
  auto Receipt = [&](llvm::StringRef Symbol = "parent$catch") {
    Fixture.Assembler.registerRewriteSourceFunctionOwner(
        "source_catch", Fixture.symbol(Symbol), /*IsPrivate=*/true,
        Kind::WinCxxCatchFunclet, "parent");
  };

  switch (TestCase) {
  case SourceOwnerRegistrationCase::ExpectThenReceipt:
    Expect();
    Receipt();
    break;
  case SourceOwnerRegistrationCase::ReceiptThenExpect:
    Receipt();
    Expect();
    break;
  case SourceOwnerRegistrationCase::MissingReceipt:
    Expect();
    break;
  case SourceOwnerRegistrationCase::ReceiptWithoutExpectation:
    Receipt();
    break;
  case SourceOwnerRegistrationCase::DuplicateExpectation:
    Expect();
    Expect();
    Receipt();
    break;
  case SourceOwnerRegistrationCase::DuplicateReceipt:
    Expect();
    Receipt();
    Receipt("parent$catch.duplicate");
    break;
  }
  return Fixture.Assembler.validateRewriteSourceFunctionOwnerRegistrations();
}

std::unique_ptr<llvm::Module>
makeWinCxxCatchOwnerModule(llvm::LLVMContext &Context, bool MarkSourceCatch,
                           llvm::StringRef SourceParent, bool AttachCatchPad) {
  auto Module = std::make_unique<llvm::Module>("win-cxx-catch-owner", Context);
  auto *VoidType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *PersonalityType = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(Context), /*isVarArg=*/true);
  auto *Personality = llvm::Function::Create(PersonalityType,
                                             llvm::GlobalValue::ExternalLinkage,
                                             "__CxxFrameHandler3", *Module);
  auto *MayThrow = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "may_throw", *Module);

  // Deliberately emit the delegated source before its parent so this real
  // CodeGen path exercises expectation-before-receipt ordering.
  auto *SourceCatch = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "source_catch", *Module);
  rewrite_source::setOriginalVA(*SourceCatch, 0x140002000);
  if (MarkSourceCatch)
    SourceCatch->addFnAttr(llvm::mc_rewrite::RewriteWinCxxCatchParentAttribute,
                           SourceParent);
  llvm::IRBuilder<>(llvm::BasicBlock::Create(Context, "entry", SourceCatch))
      .CreateRetVoid();

  auto *Parent = llvm::Function::Create(
      VoidType, llvm::GlobalValue::ExternalLinkage, "parent_frame", *Module);
  rewrite_source::setOriginalVA(*Parent, 0x140001000);
  Parent->setPersonalityFn(Personality);
  Parent->setUWTableKind(llvm::UWTableKind::Default);

  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Parent);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", Parent);
  auto *Dispatch = llvm::BasicBlock::Create(Context, "catch.dispatch", Parent);
  auto *Pad = llvm::BasicBlock::Create(Context, "catch.pad", Parent);
  llvm::IRBuilder<>(Entry).CreateInvoke(MayThrow, Exit, Dispatch);
  llvm::IRBuilder<>(Exit).CreateRetVoid();
  llvm::IRBuilder<> DispatchBuilder(Dispatch);
  auto *CatchSwitch = DispatchBuilder.CreateCatchSwitch(
      llvm::ConstantTokenNone::get(Context), nullptr, 1);
  CatchSwitch->addHandler(Pad);
  llvm::IRBuilder<> PadBuilder(Pad);
  auto *PtrType = llvm::PointerType::get(Context, 0);
  auto *CatchPad = PadBuilder.CreateCatchPad(
      CatchSwitch,
      {llvm::ConstantPointerNull::get(PtrType), PadBuilder.getInt32(0),
       llvm::ConstantPointerNull::get(PtrType)});
  if (AttachCatchPad)
    CatchPad->setMetadata(
        llvm::mc_rewrite::RewriteWinCxxCatchSourceAttachment,
        llvm::MDNode::get(Context,
                          {llvm::MDString::get(Context, "source_catch")}));
  PadBuilder.CreateCatchRet(CatchPad, Exit);
  return Module;
}

CompiledImage compileWinCxxCatchOwnerModule(llvm::Module &Module) {
  return compileImageForPatch(
      Module, Arch::X64, BinaryFormat::COFF, 0x140004000,
      [](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return 0x140000800;
        if (Symbol == "__CxxFrameHandler3")
          return 0x140000900;
        return std::nullopt;
      },
      0x140000000);
}

llvm::mc_rewrite::RewriteResult
compileWinCxxCatchOwnerModuleRaw(llvm::Module &Module) {
  llvm::mc_rewrite::RewriteOptions Options;
  Options.DeferGlobalFunctionRangeOverlap = true;
  Options.Model.TextVA = 0x140004000;
  Options.Model.ImageBaseVA = 0x140000000;
  Options.Model.getSectionVA = [](llvm::StringRef Section) {
    if (Section.starts_with(".text"))
      return 0x140004000ull;
    if (Section.starts_with(".xdata"))
      return 0x140014000ull;
    if (Section.starts_with(".pdata"))
      return 0x140024000ull;
    return 0x140034000ull;
  };
  Options.Model.resolve = [](llvm::StringRef Symbol,
                             uint32_t) -> std::optional<uint64_t> {
    if (Symbol == "may_throw")
      return 0x140000800;
    if (Symbol == "__CxxFrameHandler3")
      return 0x140000900;
    return std::nullopt;
  };
  Codegen Compiler;
  auto DirectModule = llvm::CloneModule(Module);
  return Compiler.compileForRewrite(*DirectModule, Arch::X64, Options,
                                    BinaryFormat::COFF);
}

void expectVerifierClean(const llvm::Module &Module) {
  std::string Verification;
  llvm::raw_string_ostream OS(Verification);
  EXPECT_FALSE(llvm::verifyModule(Module, &OS)) << Verification;
}

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

TEST(RewriteFunctionProvenance,
     DelegatedOwnerRequiresExactlyOneExpectationAndReceipt) {
  using Case = SourceOwnerRegistrationCase;
  EXPECT_TRUE(validateSourceOwnerRegistration(Case::ExpectThenReceipt));
  EXPECT_TRUE(validateSourceOwnerRegistration(Case::ReceiptThenExpect));
  EXPECT_FALSE(validateSourceOwnerRegistration(Case::MissingReceipt));
  EXPECT_FALSE(
      validateSourceOwnerRegistration(Case::ReceiptWithoutExpectation));
  EXPECT_FALSE(validateSourceOwnerRegistration(Case::DuplicateExpectation));
  EXPECT_FALSE(validateSourceOwnerRegistration(Case::DuplicateReceipt));
}

TEST(RewriteFunctionProvenance,
     BindsDelegatedCxxSourceToExactPrivateCatchFunclet) {
  using Kind = llvm::mc_rewrite::RewriteSourceFunctionOwnerKind;
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = makeWinCxxCatchOwnerModule(
      Context, /*MarkSourceCatch=*/true, "parent_frame",
      /*AttachCatchPad=*/true);
  expectVerifierClean(*Module);

  CompiledImage Image = compileWinCxxCatchOwnerModule(*Module);
  ASSERT_TRUE(Image.Success);
  ASSERT_TRUE(Image.FunctionRangesValid);
  ASSERT_TRUE(Image.Unresolved.empty());
  ASSERT_EQ(Image.SourceFunctionOwners.size(), 2u);
  EXPECT_TRUE(llvm::mc_rewrite::validateRewriteSourceFunctionOwners(
      Image.SourceFunctionOwners));

  const llvm::mc_rewrite::RewriteSourceFunctionOwner *Parent = nullptr;
  const llvm::mc_rewrite::RewriteSourceFunctionOwner *Catch = nullptr;
  for (const auto &Owner : Image.SourceFunctionOwners) {
    if (Owner.SourceFunction == "parent_frame")
      Parent = &Owner;
    if (Owner.SourceFunction == "source_catch")
      Catch = &Owner;
  }
  ASSERT_NE(Parent, nullptr);
  ASSERT_NE(Catch, nullptr);
  EXPECT_EQ(Parent->Kind, Kind::FunctionEntry);
  EXPECT_TRUE(Parent->ParentSourceFunction.empty());
  EXPECT_EQ(Catch->Kind, Kind::WinCxxCatchFunclet);
  EXPECT_EQ(Catch->ParentSourceFunction, "parent_frame");
  EXPECT_TRUE(Catch->IsPrivate);
  EXPECT_NE(Catch->OwnerSymbol, "source_catch");
  EXPECT_NE(Catch->OwnerSymbol.find("?catch$"), std::string::npos);
  EXPECT_EQ(Image.SymbolAddrs.count(Catch->OwnerSymbol), 0u);
  const auto PhysicalOwner = Image.FunctionOwnerAddrs.find(Catch->OwnerSymbol);
  ASSERT_NE(PhysicalOwner, Image.FunctionOwnerAddrs.end());
  EXPECT_EQ(PhysicalOwner->second, Catch->OwnerVA);
  ASSERT_EQ(Image.SourceFunctionOriginalVAs.size(), 2u);
  EXPECT_EQ(Image.SourceFunctionOriginalVAs.at("source_catch"), 0x140002000u);
}

TEST(RewriteFunctionProvenance,
     AuthenticatesCompilerCreatedWinCxxFuncletParent) {
  llvm::LLVMContext Context;
  std::unique_ptr<llvm::Module> Module = makeWinCxxCatchOwnerModule(
      Context, /*MarkSourceCatch=*/false, /*SourceParent=*/"",
      /*AttachCatchPad=*/false);
  expectVerifierClean(*Module);

  CompiledImage Image = compileWinCxxCatchOwnerModule(*Module);
  ASSERT_TRUE(Image.Success);
  ASSERT_TRUE(Image.FunctionRangesValid);
  ASSERT_TRUE(Image.Unresolved.empty());

  const llvm::mc_rewrite::RewriteSourceFunctionOwner *ParentOwner = nullptr;
  for (const auto &Owner : Image.SourceFunctionOwners)
    if (Owner.SourceFunction == "parent_frame") {
      ASSERT_EQ(ParentOwner, nullptr);
      ParentOwner = &Owner;
    }
  ASSERT_NE(ParentOwner, nullptr);

  const CompiledFunctionRange *ParentRange = nullptr;
  const CompiledFunctionRange *DerivedRange = nullptr;
  for (const CompiledFunctionRange &Range : Image.FunctionRanges) {
    if (Range.OwnerSymbol == ParentOwner->OwnerSymbol) {
      ASSERT_EQ(ParentRange, nullptr);
      ParentRange = &Range;
    }
    if (!Range.ParentOwnerSymbol.empty()) {
      ASSERT_EQ(DerivedRange, nullptr);
      DerivedRange = &Range;
    }
  }
  ASSERT_NE(ParentRange, nullptr);
  ASSERT_NE(DerivedRange, nullptr);
  EXPECT_NE(DerivedRange->OwnerSymbol, ParentRange->OwnerSymbol);
  EXPECT_EQ(DerivedRange->ParentOwnerSymbol, ParentRange->OwnerSymbol);
  EXPECT_EQ(DerivedRange->ParentOwnerVA, ParentRange->OwnerVA);
  EXPECT_TRUE(llvm::mc_rewrite::validateRewriteFunctionRanges(
      Image.FunctionRanges, Image.FunctionOwnerAddrs));

  auto RejectMutation = [&](auto Mutate) {
    std::vector<CompiledFunctionRange> Corrupt = Image.FunctionRanges;
    auto Child = std::find_if(Corrupt.begin(), Corrupt.end(),
                              [](const CompiledFunctionRange &Range) {
                                return !Range.ParentOwnerSymbol.empty();
                              });
    ASSERT_NE(Child, Corrupt.end());
    Mutate(*Child);
    EXPECT_FALSE(llvm::mc_rewrite::validateRewriteFunctionRanges(
        Corrupt, Image.FunctionOwnerAddrs));
  };
  RejectMutation([](CompiledFunctionRange &Range) { ++Range.ParentOwnerVA; });
  RejectMutation(
      [](CompiledFunctionRange &Range) { Range.ParentOwnerSymbol.clear(); });
  RejectMutation([](CompiledFunctionRange &Range) {
    Range.ParentOwnerSymbol = Range.OwnerSymbol;
    Range.ParentOwnerVA = Range.OwnerVA;
  });
  RejectMutation([](CompiledFunctionRange &Range) {
    Range.ParentOwnerSymbol = "missing.parent";
  });
}

TEST(RewriteFunctionProvenance,
     RejectsIncompleteOrMismatchedCxxCatchOwnerProtocol) {
  struct Case {
    bool MarkSourceCatch;
    const char *SourceParent;
    bool AttachCatchPad;
  };
  const Case Cases[] = {
      {/*MarkSourceCatch=*/true, "parent_frame", /*AttachCatchPad=*/false},
      {/*MarkSourceCatch=*/false, "", /*AttachCatchPad=*/true},
      {/*MarkSourceCatch=*/true, "wrong_parent", /*AttachCatchPad=*/true},
      {/*MarkSourceCatch=*/true, "", /*AttachCatchPad=*/true},
  };
  for (const Case &TestCase : Cases) {
    SCOPED_TRACE(TestCase.SourceParent);
    SCOPED_TRACE(TestCase.MarkSourceCatch);
    SCOPED_TRACE(TestCase.AttachCatchPad);
    llvm::LLVMContext Context;
    std::unique_ptr<llvm::Module> Module = makeWinCxxCatchOwnerModule(
        Context, TestCase.MarkSourceCatch, TestCase.SourceParent,
        TestCase.AttachCatchPad);
    expectVerifierClean(*Module);

    CompiledImage Image = compileWinCxxCatchOwnerModule(*Module);
    EXPECT_FALSE(Image.Success);
    EXPECT_FALSE(Image.FunctionRangesValid);
    EXPECT_TRUE(Image.SymbolAddrs.empty());
    EXPECT_TRUE(Image.SourceFunctionOwners.empty());
    EXPECT_TRUE(Image.FunctionOwnerAddrs.empty());

    // An expected delegated catch with no receipt still causes the Windows
    // backend to create a private physical funclet.  Exercise the raw writer
    // result so the upper-level CompiledImage fail-closed path cannot mask a
    // leaked private symbol identity.
    if (TestCase.MarkSourceCatch && !TestCase.AttachCatchPad) {
      llvm::mc_rewrite::RewriteResult Raw =
          compileWinCxxCatchOwnerModuleRaw(*Module);
      EXPECT_TRUE(Raw.ImageValid);
      EXPECT_FALSE(Raw.FunctionRangesValid);
      EXPECT_TRUE(Raw.SymbolAddrs.empty());
      EXPECT_TRUE(Raw.SourceFunctionOwners.empty());
      EXPECT_TRUE(Raw.FunctionOwnerAddrs.empty());
    }
  }
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
