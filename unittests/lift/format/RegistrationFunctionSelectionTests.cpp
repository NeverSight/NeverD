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
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
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
