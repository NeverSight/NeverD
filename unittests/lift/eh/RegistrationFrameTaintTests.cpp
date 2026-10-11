//===- RegistrationFrameTaintTests.cpp - PE32 pointer-byte proof ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise independent allocation ownership, overlap and bounded IR replay.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationFrameProof.h"
#include "../../../lib/backend/codegen/COFF/COFFRegistrationFrameTaint.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <array>

namespace {
using namespace neverd;
using namespace neverd::coff_registration;

TEST(RegistrationFrameTaint, IntervalsMatchIndependentByteCoverage) {
  llvm::LLVMContext Context;
  auto *Root = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 1);
  auto *Other = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 2);
  RegistrationFrameTaint Taint;
  std::array<bool, 128> Bytes{};
  size_t Work = 0;
  for (unsigned I = 0; I != 64; ++I) {
    const unsigned Begin = (I * 37) % 112;
    const unsigned Width = 1 + (I * 5) % 16;
    bool Grew = false;
    for (unsigned Byte = Begin; Byte < Begin + Width; ++Byte) {
      Grew |= !Bytes[Byte];
      Bytes[Byte] = true;
    }
    EXPECT_EQ(Taint.insert(Root, Begin, Width, Work), Grew);
    for (unsigned Offset = 0; Offset < Bytes.size(); ++Offset)
      EXPECT_EQ(Taint.overlaps(Root, Offset, 1, Work), Bytes[Offset]);
    EXPECT_EQ(Taint.overlaps(Other, Begin, Width, Work), false);
  }
  EXPECT_EQ(Taint.overlaps(Root, 128, 8, Work), false);
}

TEST(RegistrationFrameTaint, RejectsWrappingRangesAndExhaustedWork) {
  llvm::LLVMContext Context;
  auto *Root = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 1);
  RegistrationFrameTaint Taint;
  size_t Work = 0;
  EXPECT_FALSE(Taint.insert(Root, -1, 4, Work));
  EXPECT_FALSE(Taint.insert(Root, INT64_MAX - 1, 4, Work));
  EXPECT_FALSE(Taint.insert(Root, 0, UINT64_MAX, Work));
  EXPECT_FALSE(Taint.insert(Root, 0, 0, Work));
  ASSERT_EQ(Taint.insert(Root, 0, 4, Work), true);
  Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(Taint.overlaps(Root, 0, 4, Work));
  EXPECT_FALSE(Taint.insert(Root, 4, 4, Work));
}

TEST(RegistrationFrameTaint, ReplaysManyPrivateSlotsAndRetainsPartialEscapes) {
  for (bool Escape : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module Module("private-slots", Context);
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Frame = Builder.CreateAlloca(Builder.getInt32Ty());
    auto *Address = Builder.CreatePtrToInt(Frame, Builder.getInt64Ty());
    llvm::Value *Last = nullptr;
    for (unsigned I = 0; I != 1024; ++I) {
      auto *Slot = Builder.CreateAlloca(Builder.getInt64Ty());
      Builder.CreateStore(Address, Slot);
      Last = Builder.CreateLoad(Builder.getInt32Ty(), Slot);
    }
    Builder.CreateRet(Escape ? Last : Builder.getInt32(0));
    auto Error = validateFramePrivacy(*Function, {}, Frame, {}, {});
    EXPECT_EQ(bool(Error), Escape) << llvm::toString(std::move(Error));
    llvm::consumeError(std::move(Error));
  }
}
} // namespace
