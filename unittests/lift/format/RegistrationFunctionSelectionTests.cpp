//===- RegistrationFunctionSelectionTests.cpp - PE32 callback roots ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A selected parent keeps runtime callbacks distinct from ordinary entries.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace {
using namespace neverd;

class SourcePreparation : public BinaryPatcher {
public:
  using BinaryPatcher::prepareSourceFunctionsForPatch;
};

class RegistrationFunctionSelection : public ::testing::Test {
protected:
  std::unique_ptr<BinaryImage> Image;
  llvm::LLVMContext Context;
  va_t Entry = 0;
  va_t Callback = 0;
  std::string Name;

  void SetUp() override {
    const char *Path = std::getenv("NEVERD_REGISTRATION_GUARDED_PE32");
    if (!Path)
      GTEST_SKIP() << "requires the authored guarded cleanup PE32";
    auto Loaded = COFFLoader().load(Path);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    Image = std::make_unique<BinaryImage>(std::move(*Loaded));
    const auto Export = llvm::find_if(Image->Exports, [](const auto &E) {
      return E.Name == "cleanup_only_parent";
    });
    ASSERT_NE(Export, Image->Exports.end());
    Entry = Export->Addr;
    Name = Image->getFunctionNameAt(Entry);
    const auto *EH = Image->ExceptionMetadata.findFunction(Entry);
    ASSERT_TRUE(EH && EH->Cxx && EH->Registration);
    ASSERT_EQ(EH->Cxx->UnwindMap.size(), 1u);
    Callback = EH->Cxx->UnwindMap.front().ActionVA;
    ASSERT_NE(Callback, 0u);
    ASSERT_FALSE(EH->CodeRange.contains(Callback));
    ASSERT_FALSE(Image->hasAuthenticatedFunctionEntryAt(Callback));
    const auto Sources =
        coff_loader::getCheckedX86CxxCallbackPointerSources(*Image, *EH);
    ASSERT_TRUE(Sources);
    ASSERT_TRUE(llvm::any_of(*Sources, [&](const auto &Source) {
      return Source.second == Callback;
    }));
  }

  PipelineResult run(std::set<va_t> Entries) {
    PipelineOptions Options;
    Options.LiftMode = true;
    Options.NoOpt = true;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = std::move(Entries);
    return Pipeline().run(*Image, Context, Options);
  }

  bool hasCallback(const PipelineResult &Result) const {
    return llvm::any_of(Result.LowFuncs, [&](const auto &Function) {
      return Function.Entry == Callback;
    });
  }
};

TEST_F(RegistrationFunctionSelection, RuntimeCleanupStaysWithSelectedParent) {
  auto Result = run({Entry});
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.LlvmModule);
  EXPECT_FALSE(hasCallback(Result));
  ASSERT_EQ(Result.LowFuncs.size(), 1u);
  const auto *Parent = Result.LlvmModule->getFunction(Name);
  ASSERT_TRUE(Parent);
  EXPECT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
  ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
  SourceFunctionPreparation Prepared;
  std::string Detail;
  ASSERT_TRUE(SourcePreparation::prepareSourceFunctionsForPatch(
      *Result.LlvmModule, Image.get(), Prepared, Detail))
      << Detail;
  ASSERT_EQ(Prepared.ReplaceableOriginalVAs.size(), 1u);
  EXPECT_EQ(Prepared.ReplaceableOriginalVAs.at(Name), Entry);
}

TEST_F(RegistrationFunctionSelection, WholeImageKeepsHandlerRuntimeRoles) {
  const auto Result = run({});
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.LlvmModule);
  for (const auto &EH : Image->ExceptionMetadata.Functions) {
    if (!EH.Cxx || !EH.Registration)
      continue;
    EXPECT_FALSE(llvm::any_of(Result.LowFuncs, [&](const auto &Function) {
      return Function.Entry == EH.PersonalityVA;
    })) << llvm::utohexstr(EH.PersonalityVA);
  }
  const auto *Parent = Result.LlvmModule->getFunction(Name);
  ASSERT_TRUE(Parent);
  EXPECT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
  const auto *Personality =
      Result.LlvmModule->getFunction("__CxxFrameHandler3");
  ASSERT_TRUE(Personality);
  EXPECT_TRUE(Personality->getReturnType()->isIntegerTy(32));
  EXPECT_TRUE(Personality->isVarArg());
  EXPECT_TRUE(Personality->isDeclaration());
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
}

TEST_F(RegistrationFunctionSelection, HandlerRoleRequiresCurrentInstallation) {
  const auto *Source = Image->ExceptionMetadata.findFunction(Entry);
  ASSERT_TRUE(Source);
  const auto Pointer =
      coff_loader::getCheckedX86CxxHandlerPointerSource(*Image, *Source);
  ASSERT_TRUE(Pointer);
  ASSERT_TRUE(Source->Registration->RealignedFrame);
  EXPECT_EQ(*Pointer, std::make_pair(Entry + 9, Source->PersonalityVA));
  EXPECT_FALSE(coff_loader::getCheckedX86CxxPersonalityABI(*Image, *Source));
  for (unsigned Mutation = 0; Mutation != 16; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = *Image;
    auto EH = *Source;
    auto WriteByte = [&](va_t Address, uint8_t Value) {
      bool Found = false;
      for (auto &Segment : Changed.Segments)
        if (Address >= Segment.VA &&
            Address - Segment.VA < Segment.Data.size()) {
          Segment.Data[Address - Segment.VA] = Value;
          Found = true;
        }
      ASSERT_TRUE(Found);
    };
    switch (Mutation) {
    case 0:
      WriteByte(Pointer->first, Image->readVA(Pointer->first, 1)[0] ^ 1);
      break;
    case 1:
      WriteByte(Entry + 7, 0); // No longer seeds the inactive state.
      break;
    case 2:
      WriteByte(EH.Registration->ChainInstallVA, 0x90);
      break;
    case 3:
      ++EH.Registration->ChainInstallVA;
      break;
    case 4:
      EH.Registration->RegistrationOffset = -24;
      break;
    case 5:
      EH.Registration->TryLevelOffset = -8;
      break;
    case 6:
      EH.Registration->RealignedFrame->Alignment = 8;
      break;
    case 7:
      EH.Registration->SeededTryLevel = 0;
      break;
    case 8:
      EH.CodeRange.End = EH.Registration->ChainInstallVA;
      break;
    case 9:
      WriteByte(EH.PersonalityVA, 0xc3);
      break;
    case 10:
      ++EH.Cxx->MaxState;
      break;
    case 11:
      for (auto &Segment : Changed.Segments)
        if (Segment.contains(Entry))
          Segment.Flags = Segment.Flags | SegmentFlags::Writable;
      break;
    case 12:
      Changed.BaseRelocations.push_back(
          {Entry + 3, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
      break;
    case 13:
      Changed.Arch = Arch::X64;
      break;
    case 14:
      Changed.Format = BinaryFormat::ELF;
      break;
    case 15:
      Changed.Bits = Bitness::Bits64;
      break;
    }
    EXPECT_FALSE(
        coff_loader::getCheckedX86CxxHandlerPointerSource(Changed, EH));
  }
}

TEST_F(RegistrationFunctionSelection, IndependentHandlerReferencesRemain) {
  const auto *EH = Image->ExceptionMetadata.findFunction(Entry);
  ASSERT_TRUE(EH);
  const va_t Handler = EH->PersonalityVA;
  for (unsigned OrdinarySource = 0; OrdinarySource != 5; ++OrdinarySource) {
    SCOPED_TRACE(OrdinarySource);
    auto Changed = *Image;
    auto Sym = Symbol::makeFunc(Handler);
    if (OrdinarySource == 1)
      Sym.Origin = NameOrigin::Stated;
    Changed.Symbols.push_back(Sym);
    if (OrdinarySource == 2)
      Changed.Exports.push_back({"ordinary_handler", 0, Handler});
    if (OrdinarySource == 3 || OrdinarySource == 4) {
      const auto Throw = llvm::find_if(Changed.Exports, [](const auto &E) {
        return E.Name == "cleanup_only_throw";
      });
      ASSERT_NE(Throw, Changed.Exports.end());
      for (auto &Segment : Changed.Segments)
        if (Throw->Addr >= Segment.VA &&
            rangeInBounds(Throw->Addr - Segment.VA, 6, Segment.Data.size())) {
          auto *Code = Segment.Data.data() + Throw->Addr - Segment.VA;
          Code[0] = OrdinarySource == 3 ? 0xb8 : 0xe8;
          writeLE<uint32_t>(Code + 1, OrdinarySource == 3
                                          ? Handler
                                          : Handler - Throw->Addr - 5);
          Code[5] = 0xc3;
        }
      if (OrdinarySource == 3)
        Changed.CodePtrRelocSlots.insert(Throw->Addr + 1);
    }
    const auto Roles =
        coff_loader::getCheckedX86RegistrationPointerRoles(Changed);
    ASSERT_TRUE(Roles);
    EXPECT_EQ(Roles->RuntimeOnlyPointerTargets.count(Handler),
              OrdinarySource != 3);
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Changed));
    const auto Entries = FuncDetector().detect(Changed, Decode);
    EXPECT_EQ(llvm::any_of(Entries,
                           [&](const auto &Function) {
                             return Function.first == Handler;
                           }),
              OrdinarySource != 0);
  }
}

TEST_F(RegistrationFunctionSelection, ExplicitCallbackRequestIsRetained) {
  const auto Result = run({Entry, Callback});
  ASSERT_TRUE(Result.Success) << Result.Error;
  EXPECT_TRUE(hasCallback(Result));
  EXPECT_EQ(Result.LowFuncs.size(), 2u);
}

TEST_F(RegistrationFunctionSelection, IndependentCallableEntryIsRetained) {
  Image->VerifiedFunctionEntries.insert(Callback);
  ASSERT_TRUE(Image->hasAuthenticatedFunctionEntryAt(Callback));
  const auto Result = run({Entry});
  ASSERT_TRUE(Result.Success) << Result.Error;
  EXPECT_TRUE(hasCallback(Result));
  EXPECT_EQ(Result.LowFuncs.size(), 2u);
}

TEST_F(RegistrationFunctionSelection, StaleFuncInfoCannotSuppressAnEntry) {
  const auto *EH = Image->ExceptionMetadata.findFunction(Entry);
  ASSERT_TRUE(EH);
  const auto Sources =
      coff_loader::getCheckedX86CxxCallbackPointerSources(*Image, *EH);
  ASSERT_TRUE(Sources);
  const auto Found = llvm::find_if(
      *Sources, [&](const auto &Source) { return Source.second == Callback; });
  ASSERT_NE(Found, Sources->end());
  bool Changed = false;
  for (auto &Segment : Image->Segments)
    if (Found->first >= Segment.VA &&
        rangeInBounds(Found->first - Segment.VA, 4, Segment.Data.size())) {
      writeLE<uint32_t>(Segment.Data.data() + Found->first - Segment.VA, 0);
      Changed = true;
    }
  ASSERT_TRUE(Changed);
  ASSERT_FALSE(
      coff_loader::getCheckedX86CxxCallbackPointerSources(*Image, *EH));
  const auto Result = run({Entry});
  EXPECT_TRUE(hasCallback(Result));
}
} // namespace
