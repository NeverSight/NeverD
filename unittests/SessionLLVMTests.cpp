//===- SessionLLVMTests.cpp - Private LLVM query/cache contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Verifier.h"

#include <optional>
#include <string>
#include <utility>

namespace neverd::sdk {
namespace {

class SessionLLVMTest : public ::testing::Test {
protected:
  void SetUp() override {
    State.Loaded = true;
    State.PipeRan = true;
    State.PipeResult.Success = true;
    State.Img.Arch = Arch::X64;
    State.Img.Bits = Bitness::Bits64;
    State.Img.Format = BinaryFormat::ELF;
    State.LLVMCtx = std::make_unique<llvm::LLVMContext>();
  }

  llvm::Function *addFunction(const std::string &Name,
                              std::optional<va_t> Address,
                              bool Declaration = false) {
    if (!State.PipeResult.LlvmModule)
      State.PipeResult.LlvmModule =
          std::make_unique<llvm::Module>("identity_fixture", *State.LLVMCtx);
    auto *Type =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*State.LLVMCtx), false);
    auto *Function =
        llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage, Name,
                               *State.PipeResult.LlvmModule);
    if (Address)
      rewrite_source::setOriginalVA(*Function, *Address);
    if (!Declaration) {
      auto *Block = llvm::BasicBlock::Create(*State.LLVMCtx, "entry", Function);
      llvm::ReturnInst::Create(*State.LLVMCtx, Block);
    }
    return Function;
  }

  MedFunc sourceFunction() {
    MedFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "source_function";
    Function.ReturnType = NdType::makeVoid();
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Function.Entry;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Return);
    Function.Blocks.push_back(std::move(Block));
    return Function;
  }

  Session State;
};

TEST_F(SessionLLVMTest, ExactAddressZeroIgnoresHelperNamesAndDeclarations) {
  addFunction("sub_0", std::nullopt);
  addFunction("external_declaration", 0, true);
  const auto *Source = addFunction("debug_named_entry", 0);
  addFunction("another_source", 0x1000);

  EXPECT_EQ(State.findNativeLlvmFunction(0), Source);
  EXPECT_TRUE(State.LastError.empty());
}

TEST_F(SessionLLVMTest, MissingIdentityNeverFallsBackToSymbolSpelling) {
  addFunction("sub_1000", std::nullopt);
  addFunction("sub_100", 0x2000);

  EXPECT_EQ(State.findNativeLlvmFunction(0x1000), nullptr);
  EXPECT_NE(State.LastError.find("not found at 0x1000"), std::string::npos);
}

TEST_F(SessionLLVMTest, DuplicateOriginalAddressIsAmbiguous) {
  addFunction("first_source", 0x1000);
  addFunction("second_source", 0x1000);

  EXPECT_EQ(State.findNativeLlvmFunction(0x1000), nullptr);
  EXPECT_NE(State.LastError.find("ambiguous"), std::string::npos);
  EXPECT_NE(State.LastError.find("0x1000"), std::string::npos);
  EXPECT_NE(State.LastError.find("first_source"), std::string::npos);
  EXPECT_NE(State.LastError.find("second_source"), std::string::npos);
}

TEST_F(SessionLLVMTest, MalformedIdentityCannotBeHiddenByAnEarlierMatch) {
  addFunction("matching_source", 0x1000);
  auto *Malformed = addFunction("malformed_source", std::nullopt);
  Malformed->setMetadata(rewrite_source::FunctionAttachment,
                         llvm::MDNode::get(*State.LLVMCtx, {}));

  EXPECT_EQ(State.findNativeLlvmFunction(0x1000), nullptr);
  EXPECT_NE(State.LastError.find("invalid LLVM source identity"),
            std::string::npos);
  EXPECT_NE(State.LastError.find("malformed_source"), std::string::npos);
  EXPECT_NE(State.LastError.find("invalid operand count"), std::string::npos);
}

TEST_F(SessionLLVMTest, InvalidCandidateIsNeverCachedAndAnalysisRemainsUsable) {
  MedFunc Function = sourceFunction();
  // Duplicate block identity makes the real emitter produce a block with two
  // terminators. This tests cache publication, not a valid-binary lifting path.
  MedBlock Duplicate = Function.Blocks.front();
  Duplicate.StartAddr += 1;
  Function.Blocks.push_back(std::move(Duplicate));
  State.PipeResult.MedFuncs.push_back(std::move(Function));
  HighFunc High;
  High.Entry = 0x1000;
  High.Name = "available_high_ir";
  State.PipeResult.HighFuncs.push_back(std::move(High));
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "available_low_ir";
  State.PipeResult.LowFuncs.push_back(std::move(Low));
  const auto *Context = State.LLVMCtx.get();
  const auto *HighIR = State.findHighFunc(0x1000);
  const auto *LowIR = State.findLowFunc(0x1000);

  std::string Diagnostic;
  for (unsigned Attempt = 0; Attempt < 2; ++Attempt) {
    SCOPED_TRACE(Attempt);
    State.clearError();
    EXPECT_FALSE(State.ensureLlvmModule());
    EXPECT_EQ(State.PipeResult.LlvmModule, nullptr);
    EXPECT_NE(State.LastError.find("native LLVM verification failed"),
              std::string::npos);
    EXPECT_NE(State.LastError.find("Terminator found in the middle"),
              std::string::npos);
    if (Attempt == 0)
      Diagnostic = State.LastError;
    else
      EXPECT_EQ(State.LastError, Diagnostic);
    EXPECT_TRUE(State.ensurePipeline());
    EXPECT_TRUE(State.PipeResult.Success);
    EXPECT_EQ(State.LLVMCtx.get(), Context);
    EXPECT_EQ(State.findHighFunc(0x1000), HighIR);
    EXPECT_EQ(State.findLowFunc(0x1000), LowIR);
    EXPECT_TRUE(State.PipeResult.Error.empty());
  }

  State.PipeResult.MedFuncs.front().Blocks.pop_back();
  State.clearError();
  ASSERT_TRUE(State.ensureLlvmModule()) << State.LastError;
  ASSERT_NE(State.PipeResult.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*State.PipeResult.LlvmModule));
  EXPECT_TRUE(State.LastError.empty());
  const auto *Module = State.PipeResult.LlvmModule.get();
  EXPECT_NE(State.findNativeLlvmFunction(0x1000), nullptr);
  EXPECT_TRUE(State.ensureLlvmModule());
  EXPECT_EQ(State.PipeResult.LlvmModule.get(), Module);
}

TEST_F(SessionLLVMTest, RejectedEmissionDoesNotPublishAModule) {
  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x1000;
  Text.Size = 0x100;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  State.Img.Segments.push_back(std::move(Text));
  MedFunc Function = sourceFunction();
  Function.ReturnType = NdType::makeInt(8);
  Function.Blocks.front().Ops.front().addInput(
      MedVar::makeConst(0x1080, 8, ConstantAddressProvenance::CodeAddress));
  State.PipeResult.MedFuncs.push_back(std::move(Function));

  EXPECT_FALSE(State.ensureLlvmModule());
  EXPECT_EQ(State.PipeResult.LlvmModule, nullptr);
  EXPECT_EQ(State.LastError, "native LLVM emission failed");
  EXPECT_TRUE(State.PipeResult.Success);
}

} // namespace
} // namespace neverd::sdk
