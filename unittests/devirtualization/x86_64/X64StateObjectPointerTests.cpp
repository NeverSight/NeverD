//===- X64StateObjectPointerTests.cpp - State pointer proofs -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify source-only object projection without executing generated code.
//===----------------------------------------------------------------------===//

#include "../../../lib/pipeline/X86/X64StateObjectPointers.h"
#include "../../lift/NeverDLiftFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <utility>

namespace {

TEST(X64StateObjectPointers, RetainsExactObjectBoundsAndGuestArithmetic) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"IR(
target datalayout = "e-p:64:64"
define void @access(ptr %state, ptr %foreign, i64 %index) {
entry:
  %bits = ptrtoint ptr %state to i64
  %start_ptr = inttoptr i64 %bits to ptr
  %start = load i8, ptr %start_ptr, align 1
  %last_bits = add i64 %bits, 24
  %last_ptr = inttoptr i64 %last_bits to ptr
  %last = load i64, ptr %last_ptr, align 1
  store i64 %last, ptr %last_ptr, align 1
  %commuted_bits = add i64 8, %bits
  %commuted_ptr = inttoptr i64 %commuted_bits to ptr
  %commuted = load i16, ptr %commuted_ptr, align 1
  %sub_bits = sub i64 %last_bits, 16
  %sub_ptr = inttoptr i64 %sub_bits to ptr
  %sub = load i32, ptr %sub_ptr, align 1
  %wide_bits = add i64 %bits, 28
  %wide_ptr = inttoptr i64 %wide_bits to ptr
  %wide = load i64, ptr %wide_ptr, align 1
  %end_bits = add i64 %bits, 32
  %end_ptr = inttoptr i64 %end_bits to ptr
  %end = load i8, ptr %end_ptr, align 1
  %negative_bits = sub i64 %bits, 1
  %negative_ptr = inttoptr i64 %negative_bits to ptr
  %negative = load i8, ptr %negative_ptr, align 1
  %wrapped_bits = add i64 %bits, -1
  %wrapped_ptr = inttoptr i64 %wrapped_bits to ptr
  %wrapped = load i8, ptr %wrapped_ptr, align 1
  %dynamic_bits = add i64 %bits, %index
  %dynamic_ptr = inttoptr i64 %dynamic_bits to ptr
  %dynamic = load i8, ptr %dynamic_ptr, align 1
  %narrow_bits = trunc i64 %bits to i32
  %narrow_ptr = inttoptr i32 %narrow_bits to ptr
  %narrow = load i8, ptr %narrow_ptr, align 1
  %signed_bits = add nsw i64 %bits, 8
  %signed_ptr = inttoptr i64 %signed_bits to ptr
  %signed = load i8, ptr %signed_ptr, align 1
  %guest_ptr = inttoptr i64 %last to ptr
  %guest = load i64, ptr %guest_ptr, align 1
  %other = load i64, ptr %foreign, align 1
  %ordered_bits = add i64 %bits, 16
  %ordered_ptr = inttoptr i64 %ordered_bits to ptr
  %ordered = load atomic i64, ptr %ordered_ptr acquire, align 8
  %volatile = load volatile i64, ptr %ordered_ptr, align 8
  store volatile i64 %last, ptr %ordered_ptr, align 8
  store ptr %commuted_ptr, ptr %foreign, align 1
  ret void
}
)IR",
                                          Diagnostic, Context);
  ASSERT_NE(Module, nullptr);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  neverd::preserveX64StateObjectPointers(*Module, 32);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  auto &Function = *Module->getFunction("access");
  const auto Pointer = [&](const char *Name) {
    return llvm::cast<llvm::LoadInst>(
               Function.getValueSymbolTable()->lookup(Name))
        ->getPointerOperand();
  };
  EXPECT_EQ(Pointer("start"), Function.getArg(0));
  for (const auto &[Name, Offset] :
       {std::pair{"last", 24}, {"commuted", 8}, {"sub", 8}}) {
    SCOPED_TRACE(Name);
    const auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Pointer(Name));
    ASSERT_NE(GEP, nullptr);
    EXPECT_EQ(GEP->getPointerOperand(), Function.getArg(0));
    EXPECT_TRUE(GEP->isInBounds());
    EXPECT_EQ(llvm::cast<llvm::ConstantInt>(GEP->getOperand(1))->getZExtValue(),
              uint64_t(Offset));
  }
  for (const char *Name :
       {"wide", "end", "negative", "wrapped", "dynamic", "narrow", "signed",
        "guest", "ordered", "volatile"}) {
    SCOPED_TRACE(Name);
    EXPECT_TRUE(llvm::isa<llvm::IntToPtrInst>(Pointer(Name)));
  }
  EXPECT_EQ(Pointer("other"), Function.getArg(1));
  unsigned Stores = 0;
  for (const auto &Instruction : llvm::instructions(Function))
    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction)) {
      ++Stores;
      if (Store->getPointerOperand() == Function.getArg(1))
        EXPECT_TRUE(llvm::isa<llvm::IntToPtrInst>(Store->getValueOperand()));
      else if (Store->isVolatile())
        EXPECT_TRUE(llvm::isa<llvm::IntToPtrInst>(Store->getPointerOperand()));
      else {
        EXPECT_TRUE(
            llvm::isa<llvm::GetElementPtrInst>(Store->getPointerOperand()));
        EXPECT_EQ(Store->getAlign().value(), 1u);
      }
    }
  EXPECT_EQ(Stores, 3u);
}

TEST(X64StateObjectPointers, ProjectsOnlyTheDeclaredEventObject) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"IR(
target datalayout = "e-p:64:64"
define void @declared_event(ptr %state, ptr %event, i64 %index) {
  %bits = ptrtoint ptr %event to i64
  %last_bits = add i64 %bits, 24
  %last_ptr = inttoptr i64 %last_bits to ptr
  %last = load i64, ptr %last_ptr, align 1
  %byte_bits = add i64 %bits, 31
  %byte_ptr = inttoptr i64 %byte_bits to ptr
  %byte = load i8, ptr %byte_ptr, align 1
  %end_bits = add i64 %bits, 32
  %end_ptr = inttoptr i64 %end_bits to ptr
  %end = load i8, ptr %end_ptr, align 1
  %wide_bits = add i64 %bits, 28
  %wide_ptr = inttoptr i64 %wide_bits to ptr
  %wide = load i64, ptr %wide_ptr, align 1
  %dynamic_bits = add i64 %bits, %index
  %dynamic_ptr = inttoptr i64 %dynamic_bits to ptr
  %dynamic = load i8, ptr %dynamic_ptr, align 1
  %guest_ptr = inttoptr i64 %last to ptr
  %guest = load i64, ptr %guest_ptr, align 1
  %ordered = load atomic i64, ptr %last_ptr acquire, align 8
  %volatile = load volatile i64, ptr %last_ptr, align 8
  ret void
}
define i64 @other(ptr %state, ptr %foreign) {
  %bits = ptrtoint ptr %foreign to i64
  %field_bits = add i64 %bits, 24
  %field_ptr = inttoptr i64 %field_bits to ptr
  %field = load i64, ptr %field_ptr, align 1
  ret i64 %field
}
)IR",
                                          Diagnostic, Context);
  ASSERT_NE(Module, nullptr);
  auto &Event = *Module->getFunction("declared_event");
  auto &Other = *Module->getFunction("other");
  const auto Load = [](llvm::Function &Function, const char *Name) {
    return llvm::cast<llvm::LoadInst>(
        Function.getValueSymbolTable()->lookup(Name));
  };
  neverd::preserveX64StateObjectPointers(*Module, 32);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  for (const char *Name : {"last", "byte"})
    EXPECT_TRUE(
        llvm::isa<llvm::IntToPtrInst>(Load(Event, Name)->getPointerOperand()));

  neverd::preserveX64StateObjectPointers(*Module, 32, "declared_event");
  ASSERT_FALSE(llvm::verifyModule(*Module));
  for (const auto &[Name, Offset] : {std::pair{"last", 24}, {"byte", 31}}) {
    SCOPED_TRACE(Name);
    const auto *Access = Load(Event, Name);
    const auto *GEP =
        llvm::dyn_cast<llvm::GetElementPtrInst>(Access->getPointerOperand());
    ASSERT_NE(GEP, nullptr);
    EXPECT_TRUE(GEP->isInBounds());
    EXPECT_EQ(GEP->getPointerOperand(), Event.getArg(1));
    EXPECT_EQ(llvm::cast<llvm::ConstantInt>(GEP->getOperand(1))->getZExtValue(),
              uint64_t(Offset));
    EXPECT_EQ(Access->getType()->getIntegerBitWidth(), Offset == 24 ? 64u : 8u);
    EXPECT_EQ(Access->getAlign().value(), 1u);
  }
  for (const char *Name :
       {"end", "wide", "dynamic", "guest", "ordered", "volatile"}) {
    SCOPED_TRACE(Name);
    EXPECT_TRUE(
        llvm::isa<llvm::IntToPtrInst>(Load(Event, Name)->getPointerOperand()));
  }
  EXPECT_TRUE(
      llvm::isa<llvm::IntToPtrInst>(Load(Other, "field")->getPointerOperand()));
}

TEST(X64StateObjectPointers, PromotesOnlyScaffoldingBeforeProjection) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"IR(
target datalayout = "e-p:64:64"
define i64 @access(ptr %state, i1 %choose) {
entry:
  %slot = alloca i64
  %bits = ptrtoint ptr %state to i64
  store i64 %bits, ptr %slot
  br i1 %choose, label %left, label %right
left:
  %left_bits = load i64, ptr %slot
  %left_offset = add i64 %left_bits, 8
  br label %join
right:
  %right_bits = load i64, ptr %slot
  %right_offset = add i64 %right_bits, 16
  br label %join
join:
  %choice = phi i64 [ %left_offset, %left ], [ %right_offset, %right ]
  %mixed_ptr = inttoptr i64 %choice to ptr
  %mixed = load i64, ptr %mixed_ptr, align 1
  %root_bits = load i64, ptr %slot
  %root_ptr = inttoptr i64 %root_bits to ptr
  store i64 %mixed, ptr %root_ptr, align 1
  ret i64 %mixed
}
)IR",
                                          Diagnostic, Context);
  ASSERT_NE(Module, nullptr);
  neverd::preserveX64StateObjectPointers(*Module, 32);
  ASSERT_FALSE(llvm::verifyModule(*Module));
  auto &Function = *Module->getFunction("access");
  EXPECT_EQ(Function.getValueSymbolTable()->lookup("slot"), nullptr);
  const auto *Mixed = llvm::cast<llvm::LoadInst>(
      Function.getValueSymbolTable()->lookup("mixed"));
  EXPECT_TRUE(llvm::isa<llvm::IntToPtrInst>(Mixed->getPointerOperand()));
  unsigned Stores = 0;
  for (const auto &Instruction : llvm::instructions(Function))
    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction)) {
      ++Stores;
      EXPECT_EQ(Store->getPointerOperand(), Function.getArg(0));
    }
  EXPECT_EQ(Stores, 1u);
}

class StateObjectSourceTest : public NeverDLiftTest {};

TEST_F(StateObjectSourceTest, ConstantInboundsGEPsKeepTheirObject) {
  for (unsigned Bits : {32u, 64u}) {
    SCOPED_TRACE(Bits);
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Diagnostic;
    auto Module = llvm::parseAssemblyString(R"IR(
@image_view = external global [32 x i8]
define ptr @named_view() {
  %field = getelementptr inbounds [32 x i8], ptr @image_view, i64 0, i64 8
  ret ptr %field
}
define void @positive(ptr %base, i64 %value) {
  %field = getelementptr inbounds [4 x i64], ptr %base, i64 0, i64 2
  store i64 %value, ptr %field, align 1
  ret void
}
define void @negative(ptr %middle, i64 %value) {
  %field = getelementptr inbounds i64, ptr %middle, i64 -1
  store i64 %value, ptr %field, align 1
  ret void
}
define ptr @zero(ptr %base) {
  %field = getelementptr inbounds i8, ptr %base, i64 0
  ret ptr %field
}
define ptr @dynamic(ptr %dynamic_base, i8 %index) {
  %field = getelementptr inbounds i8, ptr %dynamic_base, i8 %index
  ret ptr %field
}
define ptr @wrapping(ptr %wrapping_base) {
  %field = getelementptr i8, ptr %wrapping_base, i64 -16
  ret ptr %field
}
)IR",
                                            Diagnostic, Context);
    ASSERT_NE(Module, nullptr);
    Module->setDataLayout(Bits == 32 ? "e-p:32:32" : "e-p:64:64");
    neverd::CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = true;
    Options.UseUnalignedPointers = false;
    // The test environment supplies this external storage declaration.
    std::string Source =
        "#include <stdint.h>\nextern uint8_t image_view[32];\n";
    llvm::raw_string_ostream OS(Source);
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(*Module, OS, Options));
    // Named storage, dynamic and wrapping addresses retain their numeric owner.
    for (const char *Base : {"image_view", "dynamic_base", "wrapping_base"})
      EXPECT_NE(Source.find("(uint" + std::to_string(Bits) + "_t)(uintptr_t)(" +
                            Base + ")"),
                std::string::npos);
    Source += R"C(
int proof_object(uint64_t a,uint64_t b) {
    uint64_t object[4]={11,22,33,44};
    positive(object,a);
    if(object[2]!=a)return 1;
    negative(&object[3],b);
    return object[0]!=11 || object[1]!=22 || object[2]!=b || object[3]!=44 ||
        zero(object)!=(void *)object;
}
)C";
    const auto File = tmpFile("constant-object.c");
    std::ofstream(File) << Source;
    for (const char *Level : {"-O0", "-O2"}) {
      const auto Built = exec(NEVERD_TEST_CLANG,
                              {"-std=c11", Level, "-Werror=uninitialized",
                               "-Werror=return-type", "-c", File.string(), "-o",
                               tmpFile("constant-object.o").string()});
      ASSERT_TRUE(Built.ok()) << Built.err;
    }
    const auto IR = tmpFile("constant-object.ll");
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-std=c11", "-O2", "-S", "-emit-llvm",
                                 File.string(), "-o", IR.string()});
    ASSERT_TRUE(Built.ok()) << Built.err;
    const auto Optimized = llvm::parseIRFile(IR.string(), Diagnostic, Context);
    ASSERT_NE(Optimized, nullptr);
    const auto *Proof = Optimized->getFunction("proof_object");
    ASSERT_NE(Proof, nullptr);
    ASSERT_EQ(Proof->size(), 1u);
    const auto *Return =
        llvm::dyn_cast<llvm::ReturnInst>(Proof->front().getTerminator());
    ASSERT_NE(Return, nullptr);
    const auto *Value =
        llvm::dyn_cast<llvm::ConstantInt>(Return->getReturnValue());
    ASSERT_NE(Value, nullptr);
    EXPECT_TRUE(Value->isZero());
  }
}

} // namespace
