//===- RegistrationCxxCatchTestUtils.cpp - Unbound PE32 catch proofs
//-------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationCxxCatchTestUtils.h"

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86RegistrationCatch.h"
#include "neverd/backend/llvm/X86RegistrationLayout.h"
#include "neverd/ir/RegistrationState.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Verifier.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace neverd;

namespace {
TEST(WindowsRegistrationCatch, AbsenceOfObjectStillRequiresCompleteProof) {
  ExceptionFunction Source;
  Source.Registration.emplace();
  Source.Cxx.emplace();
  Source.Cxx->TryBlocks.resize(1);
  Source.Cxx->TryBlocks[0].Handlers.resize(1);
  RegistrationStateAnalysis State;
  State.CxxCatchObjectsComplete = State.RuntimeObjectAccessesComplete = true;
  const X86RegistrationFrameLayout Frame{64, 64, 60};
  auto Plan = projectX86RegistrationCatch(Source, State, Frame);
  ASSERT_TRUE(Plan);
  EXPECT_FALSE(Plan->Home);
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = State;
    switch (Mutation) {
    case 0:
      Changed.CxxCatchObjectsComplete = false;
      break;
    case 1:
      Changed.RuntimeObjectAccessesComplete = false;
      break;
    case 2:
      Changed.CxxCatchObjects.emplace_back();
      break;
    case 3:
      Changed.RuntimeObjectAccesses.emplace_back();
      break;
    case 4:
      Source.Cxx->TryBlocks[0].Handlers[0].CatchObjectOffset = -4;
      break;
    }
    EXPECT_FALSE(projectX86RegistrationCatch(Source, Changed, Frame));
  }
}
} // namespace

namespace neverd::registration_test {
void checkUnboundCxxCatchEdits(const llvm::Function &Parent,
                               const ExceptionFunction &Source,
                               const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  if (Source.Cxx->TryBlocks[0].Handlers[0].CatchObjectOffset)
    return;
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::CatchPadInst *Pad = nullptr;
    llvm::AllocaInst *Frame = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (auto *Candidate = llvm::dyn_cast<llvm::CatchPadInst>(&I))
          Pad = Candidate;
        if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
          Frame = llvm::cast<llvm::AllocaInst>(&I);
      }
    ASSERT_TRUE(Pad && Frame);
    llvm::IRBuilder<> B(Pad->getNextNode()->getNextNode());
    switch (Mutation) {
    case 0:
    case 1:
      if (Mutation == 0)
        Pad->setArgOperand(2, Frame);
      Pad->setMetadata(
          llvm::RewriteWinX86CxxCatchObjectAttachment,
          llvm::MDNode::get(B.getContext(),
                            {llvm::ConstantAsMetadata::get(B.getInt32(1)),
                             llvm::ConstantAsMetadata::get(B.getInt32(0)),
                             llvm::ConstantAsMetadata::get(B.getInt32(4))}));
      break;
    case 2:
      Pad->setArgOperand(
          0, Source.Cxx->TryBlocks[0].Handlers[0].TypeDescriptorVA
                 ? static_cast<llvm::Constant *>(
                       llvm::ConstantPointerNull::get(B.getPtrTy()))
                 : new llvm::GlobalVariable(*Module, B.getInt8Ty(), false,
                                            llvm::GlobalValue::ExternalLinkage,
                                            nullptr, "invented_rtti"));
      break;
    case 3:
      Pad->setArgOperand(1, B.getInt32(32));
      break;
    case 4:
      // A catch with no runtime object cannot initialize otherwise unread
      // parent storage. The control graph remains valid for this edit.
      B.CreateLoad(B.getInt32Ty(), Frame)->setVolatile(true);
      auto Control =
          validateCOFFRegistrationCxxControlIR(*Function, Source, Image);
      ASSERT_FALSE(bool(Control)) << llvm::toString(std::move(Control));
      break;
    }
    std::string Diagnostics;
    llvm::raw_string_ostream Out(Diagnostics);
    EXPECT_EQ(llvm::verifyModule(*Module, &Out), Mutation == 1) << Diagnostics;
    auto Rejected = validateCOFFRegistrationCxxIR(*Function, Source, Image);
    EXPECT_TRUE(bool(Rejected));
    if (Rejected)
      llvm::consumeError(std::move(Rejected));
  }

  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
  auto Module = llvm::CloneModule(*Parent.getParent());
  auto *Function = Module->getFunction(Parent.getName());
  auto Resolve = [&](llvm::StringRef Symbol,
                     uint32_t) -> std::optional<uint64_t> {
    if (auto Address = parseNdDataSymbol(Symbol))
      return *Address;
    if (auto Address = parseNdCodePtrSymbol(Symbol))
      return *Address;
    for (const auto &Candidate : *Module) {
      llvm::SmallString<64> Name;
      llvm::Mangler Mangler;
      Mangler.getNameWithPrefix(Name, &Candidate, false);
      if (Name != Symbol)
        continue;
      auto Address = rewrite_source::getOriginalVA(Candidate);
      if (Address)
        return *Address;
      llvm::consumeError(Address.takeError());
      return std::nullopt;
    }
    return std::nullopt;
  };
  va_t Base = Image.Base;
  for (const auto &Segment : Image.Segments)
    Base = std::max(Base, Segment.VA + Segment.Size);
  Base = llvm::alignTo(Base, 0x1000);
  auto Compiled = compileImageForPatch(*Module, Arch::X86, BinaryFormat::COFF,
                                       Base, Resolve, Image.Base);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_TRUE(Compiled.Unresolved.empty());
  auto Receipt = getCheckedCOFFRegistrationCxxTableReceipt(*Function, Source,
                                                           Image, Compiled);
  ASSERT_TRUE(bool(Receipt)) << llvm::toString(Receipt.takeError());
  const auto Row =
      llvm::find_if(Compiled.WinEHSemanticRecords, [](const auto &R) {
        return R.Token.Kind ==
               llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCatch;
      });
  ASSERT_NE(Row, Compiled.WinEHSemanticRecords.end());
  const auto Section = llvm::find_if(Compiled.Sections, [&](const auto &S) {
    return S.VA <= Row->RecordVA && Row->RecordVA + 16 <= S.VA + S.Size;
  });
  ASSERT_NE(Section, Compiled.Sections.end());
  const auto Fixup =
      llvm::find_if(Section->FixupReferences, [&](const auto &F) {
        return F.Offset + Section->VA == Row->RecordVA + 12;
      });
  ASSERT_NE(Fixup, Section->FixupReferences.end());
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Compiled;
    auto Write = [&](unsigned Offset, uint32_t Value) {
      llvm::support::endian::write32le(Changed.Bytes.data() + Row->RecordVA +
                                           Offset - Changed.BaseVA,
                                       Value);
    };
    switch (Mutation) {
    case 0:
      Write(8, 4);
      break;
    case 1:
      Changed.WinEHSemanticRecords[Row - Compiled.WinEHSemanticRecords.begin()]
          .X86CxxLayout->Frame = {-16, 16, 0, 4};
      break;
    case 2:
    case 3:
    case 4:
    case 5: {
      // Even a resolved zero relocation would change a literal after rebasing.
      auto Extra = *Fixup;
      Extra.Offset = Row->RecordVA +
                     (Mutation == 5   ? 4
                      : Mutation == 3 ? 7
                                      : 8) -
                     Section->VA;
      Extra.ResolvedValue = 0;
      if (Mutation == 4)
        Extra.BitWidth = 0;
      Changed.Sections[Section - Compiled.Sections.begin()]
          .FixupReferences.push_back(Extra);
      break;
    }
    case 6:
      Write(4, Source.Cxx->TryBlocks[0].Handlers[0].TypeDescriptorVA ^ 4);
      break;
    }
    auto Rejected = getCheckedCOFFRegistrationCxxTableReceipt(*Function, Source,
                                                              Image, Changed);
    EXPECT_FALSE(bool(Rejected));
    if (!Rejected)
      llvm::consumeError(Rejected.takeError());
  }
#endif
}
} // namespace neverd::registration_test
