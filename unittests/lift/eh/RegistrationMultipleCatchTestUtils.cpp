//===- RegistrationMultipleCatchTestUtils.cpp - Ordered catch rejection
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationMultipleCatchTestUtils.h"

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Verifier.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {
void checkMultipleCatchEdits(const llvm::Function &Parent,
                             const ExceptionFunction &Source,
                             const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    std::vector<llvm::CatchPadInst *> Pads;
    std::vector<llvm::CatchReturnInst *> Returns;
    std::vector<llvm::AllocaInst *> Stacks;
    std::vector<llvm::StoreInst *> Seeds;
    llvm::AllocaInst *Frame = nullptr;
    for (auto &Block : *Function) {
      for (auto &I : Block) {
        if (auto *Pad = llvm::dyn_cast<llvm::CatchPadInst>(&I))
          Pads.push_back(Pad);
        if (auto *Return = llvm::dyn_cast<llvm::CatchReturnInst>(&I))
          Returns.push_back(Return);
        if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
          Frame = llvm::cast<llvm::AllocaInst>(&I);
        if (I.getMetadata(windows_eh_md::RegistrationCatchStackAttachment))
          Stacks.push_back(llvm::cast<llvm::AllocaInst>(&I));
        if (I.getMetadata(windows_eh_md::RegistrationRootAttachment))
          Seeds.push_back(llvm::cast<llvm::StoreInst>(&I));
      }
    }
    ASSERT_EQ(Pads.size(), 3u);
    ASSERT_EQ(Returns.size(), 3u);
    ASSERT_EQ(Stacks.size(), 3u);
    ASSERT_EQ(Seeds.size(), 3u);
    ASSERT_TRUE(Frame);
    llvm::IRBuilder<> B(Pads[0]->getNextNode());
    switch (Mutation) {
    case 0: {
      auto *Switch = Pads[0]->getCatchSwitch();
      Switch->setSuccessor(0, Pads[1]->getParent());
      Switch->setSuccessor(1, Pads[0]->getParent());
      break;
    }
    case 1:
      Pads[1]->setMetadata(
          llvm::mc_rewrite::RewriteWinEHSemanticAttachment,
          Pads[0]->getMetadata(
              llvm::mc_rewrite::RewriteWinEHSemanticAttachment));
      break;
    case 2:
      Pads[1]->setMetadata(
          llvm::RewriteWinX86CxxCatchObjectAttachment,
          Pads[0]->getMetadata(llvm::RewriteWinX86CxxCatchObjectAttachment));
      break;
    case 3:
      Pads[1]->setArgOperand(1, B.getInt32(0));
      break;
    case 4:
      Stacks[1]->setMetadata(
          windows_eh_md::RegistrationCatchStackAttachment,
          Stacks[0]->getMetadata(
              windows_eh_md::RegistrationCatchStackAttachment));
      break;
    case 5: {
      auto *Value = llvm::cast<llvm::PtrToIntInst>(Seeds[1]->getValueOperand());
      llvm::cast<llvm::GetElementPtrInst>(Value->getPointerOperand())
          ->setOperand(0, Stacks[0]);
      break;
    }
    case 6:
      Returns[1]->setSuccessor(Returns[0]->getSuccessor());
      break;
    case 7: {
      // A sibling value catch does not receive the reference catch's implicit
      // initialization. The source control receipts remain unmodified.
      auto Home = llvm::getRewriteWinX86CxxCatchFrameObject(*Pads[1]);
      ASSERT_TRUE(bool(Home)) << llvm::toString(Home.takeError());
      B.SetInsertPoint(Pads[0]->getNextNode()->getNextNode());
      B.CreateLoad(B.getInt32Ty(),
                   B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(Home->Offset)))
          ->setVolatile(true);
      break;
    }
    case 8:
      B.SetInsertPoint(Pads[0]->getNextNode()->getNextNode());
      B.CreateStore(B.getInt32(0), Stacks[1]);
      break;
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    if (Mutation == 7 || Mutation == 8) {
      auto Control =
          validateCOFFRegistrationCxxControlIR(*Function, Source, Image);
      ASSERT_FALSE(bool(Control)) << llvm::toString(std::move(Control));
    }
    auto Error = validateCOFFRegistrationCxxIR(*Function, Source, Image);
    EXPECT_TRUE(bool(Error));
    if (Error)
      llvm::consumeError(std::move(Error));
  }

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
      llvm::Mangler().getNameWithPrefix(Name, &Candidate, false);
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
  auto Compiled =
      compileImageForPatch(*Module, Arch::X86, BinaryFormat::COFF,
                           llvm::alignTo(Base, 0x1000), Resolve, Image.Base);
  ASSERT_TRUE(Compiled.Success);
  ASSERT_TRUE(Compiled.Unresolved.empty());
  std::vector<size_t> Catches;
  for (size_t I = 0; I < Compiled.WinEHSemanticRecords.size(); ++I)
    if (Compiled.WinEHSemanticRecords[I].Token.Kind ==
        llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCatch)
      Catches.push_back(I);
  ASSERT_EQ(Catches.size(), 3u);
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Compiled;
    auto &Row = Changed.WinEHSemanticRecords[Catches[1]];
    const auto &Sibling = Changed.WinEHSemanticRecords[Catches[0]];
    switch (Mutation) {
    case 0:
      Row.Token = Sibling.Token;
      break;
    case 1:
      Row.RecordVA = Sibling.RecordVA;
      break;
    case 2:
      Changed.WinEHSemanticRecords.erase(Changed.WinEHSemanticRecords.begin() +
                                         Catches[1]);
      break;
    case 3:
      Row.X86CxxLayout->Frame = Sibling.X86CxxLayout->Frame;
      break;
    case 4:
      llvm::support::endian::write32le(
          Changed.Bytes.data() + Row.RecordVA - Changed.BaseVA + 4,
          Source.Cxx->TryBlocks[0].Handlers[0].TypeDescriptorVA);
      break;
    case 5:
      for (auto &Section : Changed.Sections)
        for (auto &Fixup : Section.FixupReferences)
          if (Section.VA + Fixup.Offset == Row.RecordVA + 12)
            Fixup.Symbol = Sibling.HandlerSymbol;
      break;
    }
    auto Receipt = getCheckedCOFFRegistrationCxxTableReceipt(*Function, Source,
                                                             Image, Changed);
    EXPECT_FALSE(bool(Receipt));
    if (!Receipt)
      llvm::consumeError(Receipt.takeError());
  }
#endif
}
} // namespace neverd::registration_test
