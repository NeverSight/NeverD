//===- X86_64_EVEXPackedConvertEmitterTests.cpp --------------------------===//

#include "gtest/gtest.h"
#include "neverd/backend/llvm/MedLLVMFloatConvertLowering.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>

using namespace neverd;

namespace {

struct FixtureIR {
  llvm::LLVMContext Context;
  llvm::Module Module{"packed-convert-lowering", Context};
  llvm::Function *Function = nullptr;
  llvm::IRBuilder<> Builder{Context};

  explicit FixtureIR(bool FloatReturn) {
    llvm::Type *ReturnType = FloatReturn ? llvm::Type::getFloatTy(Context)
                                        : llvm::Type::getInt32Ty(Context);
    llvm::Type *ArgumentType = llvm::Type::getInt32Ty(Context);
    auto *Type = llvm::FunctionType::get(ReturnType, {ArgumentType}, false);
    Function = llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                      "convert", Module);
    Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  }

  testing::AssertionResult valid() {
    std::string Error;
    llvm::raw_string_ostream OS(Error);
    if (llvm::verifyModule(Module, &OS)) {
      OS.flush();
      return testing::AssertionFailure() << Error;
    }
    return testing::AssertionSuccess();
  }
};

TEST(X86EVEXPackedConvertEmitter, UnsignedInvalidUsesAllOnesIndefinite) {
  const struct {
    uint32_t Raw;
    uint32_t Expected;
  } Cases[] = {
      {UINT32_C(0xbf800000), UINT32_MAX},
      {UINT32_C(0x7fc00000), UINT32_MAX},
      {UINT32_C(0x4f800000), UINT32_MAX},
      {UINT32_C(0xbf000000), 0},
      {UINT32_C(0x3f800000), 1},
  };

  for (const auto &Case : Cases) {
    FixtureIR IR(/*FloatReturn=*/false);
    auto *SourceBits = llvm::ConstantInt::get(
        llvm::Type::getInt32Ty(IR.Context), Case.Raw);
    auto *Source = IR.Builder.CreateBitCast(
        SourceBits, llvm::Type::getFloatTy(IR.Context));
    auto *Result = llvm_detail::emitFPToInt(
        IR.Builder, IR.Module, Source, llvm::Type::getInt32Ty(IR.Context),
        true, Arch::X64);
    IR.Builder.CreateRet(Result);
    ASSERT_TRUE(IR.valid());
    const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Result);
    ASSERT_NE(Select, nullptr);
    const auto *Invalid =
        llvm::dyn_cast<llvm::ConstantInt>(Select->getCondition());
    ASSERT_NE(Invalid, nullptr);
    EXPECT_EQ(Invalid->isOne(), Case.Expected == UINT32_MAX);
    const auto *Indefinite =
        llvm::dyn_cast<llvm::ConstantInt>(Select->getTrueValue());
    ASSERT_NE(Indefinite, nullptr);
    EXPECT_TRUE(Indefinite->isAllOnesValue());
    const auto *Saturating = llvm::dyn_cast<llvm::CallBase>(
        Select->getFalseValue());
    ASSERT_NE(Saturating, nullptr);
    ASSERT_NE(Saturating->getCalledFunction(), nullptr);
    EXPECT_TRUE(Saturating->getCalledFunction()->getName().starts_with(
        "llvm.fptoui.sat"));
  }
}

TEST(X86EVEXPackedConvertEmitter, IntToFloatUsesDynamicRoundingEnvironment) {
  for (bool IsUnsigned : {false, true}) {
    FixtureIR IR(/*FloatReturn=*/true);
    auto *Result = llvm_detail::emitIntToFP(
        IR.Builder, IR.Function->getArg(0),
        llvm::Type::getFloatTy(IR.Context), IsUnsigned, Arch::X64);
    IR.Builder.CreateRet(Result);
    ASSERT_TRUE(IR.valid());

    const auto *Call = llvm::dyn_cast<llvm::CallBase>(Result);
    ASSERT_NE(Call, nullptr);
    ASSERT_NE(Call->getCalledFunction(), nullptr);
    EXPECT_TRUE(Call->getCalledFunction()->getName().starts_with(
        IsUnsigned ? "llvm.experimental.constrained.uitofp"
                   : "llvm.experimental.constrained.sitofp"));
    const auto *Rounding = llvm::dyn_cast<llvm::MetadataAsValue>(
        Call->getArgOperand(1));
    ASSERT_NE(Rounding, nullptr);
    const auto *Text = llvm::dyn_cast<llvm::MDString>(Rounding->getMetadata());
    ASSERT_NE(Text, nullptr);
    EXPECT_EQ(Text->getString(), "round.dynamic");
    EXPECT_TRUE(IR.Function->hasFnAttribute(llvm::Attribute::StrictFP));
    EXPECT_TRUE(Call->hasFnAttr(llvm::Attribute::StrictFP));
  }
}

} // namespace
