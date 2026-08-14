//===- COFFExceptionPatchTests.cpp - Windows EH patch contract tests --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <string>

namespace {

using namespace neverd;

std::optional<uint64_t> metadataInteger(const llvm::MDNode *Node,
                                        unsigned Index, unsigned Width) {
  if (!Node || Index >= Node->getNumOperands())
    return std::nullopt;
  const auto *Metadata = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(
      Node->getOperand(Index).get());
  const auto *Integer =
      Metadata ? llvm::dyn_cast<llvm::ConstantInt>(Metadata->getValue())
               : nullptr;
  if (!Integer || Integer->getBitWidth() != Width)
    return std::nullopt;
  return Integer->getZExtValue();
}

std::string moduleIR(const llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream Stream(Text);
  Module.print(Stream, nullptr);
  return Text;
}

llvm::Function *defineVoidFunction(llvm::Module &Module, llvm::StringRef Name) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  llvm::Function *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, Name, Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateRetVoid();
  return Function;
}

void expectContractError(llvm::Expected<COFFExceptionPatchPlan> Result,
                         exception_rewrite::ContractErrorReason ExpectedReason,
                         llvm::StringRef ExpectedFunction) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool SawContractError = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const exception_rewrite::ExceptionRewriteContractError &Error) {
        SawContractError = true;
        EXPECT_EQ(Error.reason(), ExpectedReason);
        EXPECT_EQ(Error.functionName(), ExpectedFunction);
      },
      [&](const llvm::ErrorInfoBase &Error) {
        std::string Message;
        llvm::raw_string_ostream Stream(Message);
        Error.log(Stream);
        ADD_FAILURE() << "unexpected wrapped error: " << Stream.str();
      });
  EXPECT_TRUE(SawContractError);
}

TEST(COFFExceptionPatch, AcceptsCompleteX64UnwindContract) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "sub_140001000";
  MedBlock Block;
  Block.Id = 0;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x20};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  Func.ExceptionMetadata = EH;

  llvm::LLVMContext Ctx;
  auto Module = MedLLVMEmitter().emit({Func}, Ctx, "eh-patch-safe", Arch::X64,
                                      {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Image.ExceptionMetadata.rebuildIndex();

  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->ExceptionFunctionEntries.size(), 1u);
  EXPECT_EQ(Plan->ExceptionFunctionEntries[0], Func.Entry);

  Image.DynInfo.GuardFlags = 0x00800000u; // IMAGE_GUARD_XFG_ENABLED
  auto UnsupportedGuardPlan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(UnsupportedGuardPlan));
  EXPECT_NE(llvm::toString(UnsupportedGuardPlan.takeError())
                .find("guard instrumentation mode"),
            std::string::npos);
}

TEST(COFFExceptionPatch, ResolvesExecutablePersonalityThunkInsteadOfIATData) {
  BinaryImage Image;
  Image.Base = 0x140000000;
  Segment Code;
  Code.VA = Image.Base + 0x1000;
  Code.Size = 1;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = {0xc3};
  Image.Segments.push_back(std::move(Code));

  ExceptionFunction EH;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = Image.Base + 0x1000;
  Image.ExceptionMetadata.Functions.push_back(EH);

  EXPECT_EQ(findCOFFExceptionPersonalityVA(Image, "__C_specific_handler"),
            EH.PersonalityVA);
  EXPECT_EQ(findCOFFExceptionPersonalityVA(Image, "\01__C_specific_handler"),
            EH.PersonalityVA);
  const std::string AddressAlias =
      (kAutoFuncPrefix + llvm::utohexstr(EH.PersonalityVA)).str();
  EXPECT_EQ(findCOFFExceptionPersonalityVA(Image, AddressAlias),
            EH.PersonalityVA);
  EXPECT_FALSE(findCOFFExceptionPersonalityVA(Image, "sub_140002000"));
  EXPECT_FALSE(findCOFFExceptionPersonalityVA(Image, "__CxxFrameHandler3"));

  Image.Segments.front().Flags = SegmentFlags::Readable;
  EXPECT_FALSE(findCOFFExceptionPersonalityVA(Image, "__C_specific_handler"));
}

TEST(COFFExceptionPatch,
     RejectsMarkedModuleWithoutExactDefinedFunctionCoverage) {
  llvm::LLVMContext Context;
  llvm::Module Module("incomplete-common-contract", Context);
  llvm::Function *Covered = defineVoidFunction(Module, "covered");
  defineVoidFunction(Module, "uncovered");
  exception_rewrite::setContract(*Covered,
                                 exception_rewrite::SourceState::Absent,
                                 exception_rewrite::LoweringState::NotRequired);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  const std::string Before = moduleIR(Module);

  auto Plan = planCOFFExceptionPatch(Module, Image, Arch::X64);

  expectContractError(std::move(Plan),
                      exception_rewrite::ContractErrorReason::InvalidMetadata,
                      "uncovered");
  EXPECT_EQ(moduleIR(Module), Before);
}

TEST(COFFExceptionPatch, RejectsLanguageGraphWithoutNativeWinEH) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "sub_140001000";
  MedBlock Block;
  Block.Id = 0;
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x20};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.SEH.emplace();
  Func.ExceptionMetadata = EH;

  llvm::LLVMContext Ctx;
  auto Module = MedLLVMEmitter().emit({Func}, Ctx, "eh-patch-reject", Arch::X64,
                                      {}, nullptr, BinaryFormat::COFF);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Emitted = Module->getFunction(Func.Name);
  ASSERT_NE(Emitted, nullptr);
  const llvm::MDNode *Contract =
      Emitted->getMetadata(exception_rewrite::FunctionAttachment);
  ASSERT_NE(Contract, nullptr);
  EXPECT_EQ(metadataInteger(Contract, exception_rewrite::Source, 8),
            static_cast<uint8_t>(exception_rewrite::SourceState::Complete));
  EXPECT_EQ(metadataInteger(Contract, exception_rewrite::Lowering, 8),
            static_cast<uint8_t>(exception_rewrite::LoweringState::Missing));
  for (unsigned Operand : {exception_rewrite::RequiredProtectedCalls,
                           exception_rewrite::LoweredProtectedCalls,
                           exception_rewrite::SkippedLandingPads})
    EXPECT_EQ(metadataInteger(Contract, Operand, 64), 0u);
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Image.ExceptionMetadata.rebuildIndex();
  const std::string Before = moduleIR(*Module);

  auto Plan = planCOFFExceptionPatch(*Module, Image, Arch::X64);
  expectContractError(
      std::move(Plan),
      exception_rewrite::ContractErrorReason::IncompleteLowering, Func.Name);
  EXPECT_EQ(moduleIR(*Module), Before);
}

} // namespace
