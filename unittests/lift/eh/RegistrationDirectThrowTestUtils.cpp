//===- RegistrationDirectThrowTestUtils.cpp - CRT throw edits ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify callback boundaries and current object arguments of x86 C++ throws.
//===----------------------------------------------------------------------===//
#include "RegistrationDirectThrowTestUtils.h"

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {
namespace {
void checkCallbackBoundaries(const MedFunc &Med, const BinaryImage &Image) {
  const auto &Source = *Med.ExceptionMetadata;
  const auto Boundary = Source.Cxx->TryBlocks[0].Handlers[0].HandlerVA + 1;
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Image;
    Changed.ExceptionMetadata.Functions.clear();
    Changed.ExceptionMetadata.rebuildIndex();
    auto BoundarySymbol = Symbol::makeFunc(Boundary);
    BoundarySymbol.IsBoundaryGuess = true;
    if (Mutation == 1) {
      BoundarySymbol.Name = "independent_entry";
      BoundarySymbol.Origin = NameOrigin::Stated;
    }
    Changed.Symbols.push_back(BoundarySymbol);
    if (Mutation == 2) {
      Export Entry;
      Entry.Name = "independent_entry";
      Entry.Addr = Boundary;
      Changed.Exports.push_back(Entry);
    }
    if (Mutation >= 3) {
      auto Segment =
          llvm::find_if(Changed.Segments, [&](const auto &Candidate) {
            return Boundary > Candidate.VA &&
                   Boundary - 1 - Candidate.VA < Candidate.Data.size();
          });
      ASSERT_NE(Segment, Changed.Segments.end());
      // A real return, trap or unresolved jump keeps even a weak boundary.
      const uint8_t Terminators[] = {0xc3, 0xcc, 0xeb};
      Segment->Data[Boundary - 1 - Segment->VA] = Terminators[Mutation - 3];
    }
    coff_loader::parseX86RegistrationExceptions(Changed);
    const auto Found = llvm::find_if(
        Changed.ExceptionMetadata.Functions, [&](const auto &Candidate) {
          return Candidate.CodeRange.Begin == Source.CodeRange.Begin;
        });
    ASSERT_NE(Found, Changed.ExceptionMetadata.Functions.end());
    EXPECT_EQ(Found->CodeRange.End,
              Mutation == 0 ? Source.CodeRange.End : Boundary);
  }
}
} // namespace
void checkDirectThrowEdits(const MedFunc &Med, const llvm::Function &Parent,
                           const BinaryImage &Image) {
  checkCallbackBoundaries(Med, Image);
  for (bool Callback : {false, true})
    for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
      SCOPED_TRACE(Callback);
      SCOPED_TRACE(Mutation);
      auto Module = llvm::CloneModule(*Parent.getParent());
      auto *Function = Module->getFunction(Parent.getName());
      std::vector<llvm::CallBase *> Calls;
      for (auto &Block : *Function)
        for (auto &I : Block)
          if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I))
            if (const auto *Target = Call->getCalledFunction();
                Target &&
                Target->getName().starts_with(
                    "__nd_registration_runtime_throw_") &&
                !llvm::isa<llvm::ConstantPointerNull>(Call->getArgOperand(1)))
              Calls.push_back(Call);
      ASSERT_GE(Calls.size(), 3u);
      auto Selected = llvm::find_if(Calls, [&](const auto *Call) {
        return Call->getOperandBundle(llvm::LLVMContext::OB_funclet)
                   .has_value() == Callback;
      });
      if (Selected == Calls.end()) {
        ASSERT_TRUE(Callback);
        continue;
      }
      auto *Call = *Selected;
      llvm::IRBuilder<> Builder(Call);
      if (Mutation == 0)
        Call->setArgOperand(0,
                            llvm::ConstantPointerNull::get(Builder.getPtrTy()));
      if (Mutation == 1 || Mutation == 2) {
        const unsigned Argument = Mutation - 1;
        Call->setArgOperand(Argument,
                            Builder.CreateGEP(Builder.getInt8Ty(),
                                              Call->getArgOperand(Argument),
                                              Builder.getInt32(1)));
      }
      if (Mutation == 3)
        Call->setArgOperand(1, Calls[1]->getArgOperand(1));
      if (Mutation == 4)
        Call->addParamAttr(0, llvm::Attribute::InReg);
      if (Mutation == 5)
        Call->getCalledFunction()->addParamAttr(1, llvm::Attribute::NonNull);
      if (Mutation == 6)
        Call->setMemoryEffects(llvm::MemoryEffects::none());
      if (Mutation == 7)
        Call->getCalledFunction()->setMemoryEffects(
            llvm::MemoryEffects::none());
      if (Mutation == 8 || Mutation == 9) {
        // The three source objects hold int(7), unsigned(7), and float(7).
        // Destroy their definite initialization without changing the call or
        // its receipt. The float bits must not count as integer seven.
        unsigned Stores = 0;
        llvm::DominatorTree Dominators(*Function);
        for (auto &Block : *Function)
          for (auto It = Block.begin(); It != Block.end();) {
            auto *Store = llvm::dyn_cast<llvm::StoreInst>(&*It++);
            const auto *Value = Store ? llvm::dyn_cast<llvm::ConstantInt>(
                                            Store->getValueOperand())
                                      : nullptr;
            if (!Value || !Dominators.dominates(Store, Call) ||
                !Value->getType()->isIntegerTy(32) ||
                (Value->getZExtValue() != 7 &&
                 Value->getZExtValue() != 0x40e00000))
              continue;
            ++Stores;
            if (Mutation == 8)
              Store->eraseFromParent();
            else
              Store->setOperand(0, llvm::UndefValue::get(Value->getType()));
          }
        ASSERT_GE(Stores, 1u);
      }
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      auto Error = validateCOFFRegistrationCxxIR(*Function,
                                                 *Med.ExceptionMetadata, Image);
      EXPECT_TRUE(bool(Error));
      llvm::consumeError(std::move(Error));
    }
  const auto &States = *Med.RegistrationStates;
  const auto Effect = llvm::find_if(States.CallFrameEffects, [](const auto &C) {
    return C.RuntimeThrow && !C.RuntimeThrow->isRethrow();
  });
  ASSERT_NE(Effect, States.CallFrameEffects.end());
  for (unsigned Argument = 0; Argument != 2; ++Argument) {
    SCOPED_TRACE(Argument);
    auto Changed = Med;
    auto Call = llvm::find_if(Changed.CallInfos, [&](const auto &Candidate) {
      return Candidate.TargetAddr == Effect->Target;
    });
    ASSERT_NE(Call, Changed.CallInfos.end());
    ASSERT_EQ(Call->Args.size(), 2u);
    Call->Args[Argument] = MedVar::makeConst(1, 4);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Changed}, Context, "edited-throw", Arch::X86, {},
                              &Image, BinaryFormat::COFF);
    ASSERT_TRUE(Module);
    const auto *Function = Module->getFunction(Changed.Name);
    ASSERT_TRUE(Function);
    EXPECT_FALSE(Function->getMetadata(windows_eh_md::NativeAttachment));
  }
}
} // namespace neverd::registration_test
