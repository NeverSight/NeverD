//===- COFFExceptionPatchTests.cpp - Windows EH patch contract tests --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

void expectPatchError(llvm::Expected<COFFExceptionPatchPlan> Result,
                      llvm::StringRef ExpectedMessage) {
  ASSERT_FALSE(static_cast<bool>(Result));
  EXPECT_EQ(llvm::toString(Result.takeError()), ExpectedMessage);
}

struct CompletePatchInput {
  std::unique_ptr<llvm::Module> Module;
  BinaryImage Image;
  llvm::Function *Function = nullptr;
};

class SourcePreparationProbe : public BinaryPatcher {
public:
  static bool prepare(llvm::Module &Module, const BinaryImage *Image,
                      SourceFunctionPreparation &Preparation,
                      std::string &Detail) {
    return prepareSourceFunctionsForPatch(Module, Image, Preparation, Detail);
  }
};

CompletePatchInput makeCompletePatchInput(llvm::LLVMContext &Context) {
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
  EH.Diagnostics = {"source diagnostic"};
  Func.ExceptionMetadata = EH;

  CompletePatchInput Input;
  Input.Module =
      MedLLVMEmitter().emit({Func}, Context, "eh-patch-canonical", Arch::X64,
                            {}, nullptr, BinaryFormat::COFF);
  if (!Input.Module)
    return Input;
  Input.Function = Input.Module->getFunction(Func.Name);
  Input.Image.Arch = Arch::X64;
  Input.Image.Format = BinaryFormat::COFF;
  Input.Image.Base = 0x140000000;
  Input.Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Input.Image.ExceptionMetadata.rebuildIndex();
  return Input;
}

CompletePatchInput makeNativeSEHPatchInput(llvm::LLVMContext &Context) {
  constexpr va_t Entry = 0x140001000;
  constexpr va_t MayThrowVA = 0x140001100;

  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = "native_seh_patch_source";
  Func.ReturnType = NdType::makeVoid();

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Entry;
  Protected.EndAddr = Entry + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Entry + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(std::move(Call));
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = Entry + 8;
  Protected.Ops.push_back(std::move(ProtectedReturn));

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = Entry + 0x20;
  Handler.EndAddr = Entry + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = Entry + 0x28;
  Handler.Ops.push_back(std::move(HandlerReturn));
  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + 0x40};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = Entry + 0x100;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Entry, Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Entry + 0x20;
  Scope.ContinuationVA = Scope.HandlerVA;
  EH.SEH->Scopes.push_back(Scope);
  Func.ExceptionMetadata = EH;

  CompletePatchInput Input;
  Input.Module = MedLLVMEmitter().emit(
      {Func}, Context, "native-seh-patch-source", Arch::X64,
      {{MayThrowVA, "may_throw"}}, nullptr, BinaryFormat::COFF);
  if (!Input.Module)
    return Input;
  Input.Function = Input.Module->getFunction(Func.Name);
  Input.Image.Arch = Arch::X64;
  Input.Image.Bits = Bitness::Bits64;
  Input.Image.Format = BinaryFormat::COFF;
  Input.Image.Base = 0x140000000;
  Input.Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Input.Image.ExceptionMetadata.rebuildIndex();
  return Input;
}

llvm::MDNode *replacePayloadOperand(llvm::LLVMContext &Context,
                                    const llvm::MDNode &Payload, unsigned Index,
                                    llvm::Metadata *Replacement) {
  std::vector<llvm::Metadata *> Operands;
  Operands.reserve(Payload.getNumOperands());
  for (const llvm::MDOperand &Operand : Payload.operands())
    Operands.push_back(Operand.get());
  Operands.at(Index) = Replacement;
  return llvm::MDNode::get(Context, Operands);
}

llvm::MDNode *functionTableRow(llvm::LLVMContext &Context,
                               llvm::Function &Function,
                               llvm::MDNode &Payload) {
  return llvm::MDNode::get(Context,
                           {llvm::ValueAsMetadata::get(&Function), &Payload});
}

llvm::MDNode *tamperedDiagnosticsPayload(llvm::LLVMContext &Context,
                                         const llvm::MDNode &Payload) {
  llvm::MDNode *Diagnostics = llvm::MDNode::get(
      Context, {llvm::MDString::get(Context, "tampered diagnostic")});
  return replacePayloadOperand(Context, Payload, windows_eh_md::Diagnostics,
                               Diagnostics);
}

llvm::Function *addCompletePatchFunction(CompletePatchInput &Input,
                                         llvm::LLVMContext &Context,
                                         llvm::StringRef Name, va_t Begin) {
  llvm::Function *Function = defineVoidFunction(*Input.Module, Name);
  exception_rewrite::setContract(*Function,
                                 exception_rewrite::SourceState::Complete,
                                 exception_rewrite::LoweringState::NotRequired);

  ExceptionFunction EH;
  EH.CodeRange = {Begin, Begin + 0x20};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  llvm::MDNode *Payload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, EH);
  rewrite_source::setOriginalVA(*Function, Begin);
  Function->setMetadata(windows_eh_md::FunctionAttachment, Payload);

  llvm::NamedMDNode *Table =
      Input.Module->getOrInsertNamedMetadata(windows_eh_md::FunctionTable);
  Table->addOperand(functionTableRow(Context, *Function, *Payload));
  Input.Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Input.Image.ExceptionMetadata.rebuildIndex();
  return Function;
}

void addMixedSourceLayout(CompletePatchInput &Input, va_t PreservedEntry,
                          va_t ReplaceableEntry) {
  const va_t TextBegin = std::min(PreservedEntry, ReplaceableEntry);
  const va_t TextEnd = std::max(PreservedEntry, ReplaceableEntry) + 0x40;
  Segment Text;
  Text.Name = ".text";
  Text.VA = TextBegin;
  Text.Size = TextEnd - TextBegin;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(static_cast<size_t>(Text.Size));
  Input.Image.Segments.push_back(std::move(Text));

  // The authenticated entry inside the first trampoline span makes that source
  // unsafe to overwrite.  The second source retains an uninterrupted owner.
  Input.Image.Symbols.push_back(Symbol::makeFunc(PreservedEntry, 0x20));
  Input.Image.Symbols.push_back(Symbol::makeFunc(PreservedEntry + 1, 1));
  Input.Image.Symbols.push_back(Symbol::makeFunc(ReplaceableEntry, 0x20));
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

TEST(COFFExceptionPatch, RejectsMissingRewriteSourceIdentity) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  Input.Function->setMetadata(rewrite_source::FunctionAttachment, nullptr);

  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: Windows EH function lacks an exact rewrite "
      "source identity: sub_140001000");
}

TEST(COFFExceptionPatch, RejectsRewriteIdentityWithoutExactPrimaryEntry) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  rewrite_source::setOriginalVA(*Input.Function, 0x140002000);

  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: rewrite source identity does not name a primary "
      "Windows EH record for function sub_140001000");
}

TEST(COFFExceptionPatch, RejectsAmbiguousPrimarySourceEntry) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_FALSE(Input.Image.ExceptionMetadata.Functions.empty());
  Input.Image.ExceptionMetadata.Functions.push_back(
      Input.Image.ExceptionMetadata.Functions.front());
  Input.Image.ExceptionMetadata.rebuildIndex();

  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: rewrite source identity names more than one "
      "primary Windows EH record for function sub_140001000");
}

TEST(COFFExceptionPatch, RejectsTwoFunctionsClaimingOneSourceEntry) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *Alias =
      defineVoidFunction(*Input.Module, "duplicate_source_identity");
  exception_rewrite::setContract(*Alias,
                                 exception_rewrite::SourceState::Complete,
                                 exception_rewrite::LoweringState::NotRequired);
  rewrite_source::setOriginalVA(*Alias, 0x140001000);
  llvm::MDNode *Payload =
      Input.Function->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(Payload, nullptr);
  Alias->setMetadata(windows_eh_md::FunctionAttachment, Payload);
  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->addOperand(functionTableRow(Context, *Alias, *Payload));

  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: more than one IR function names Windows EH "
      "source entry 0x140001000");
}

TEST(COFFExceptionPatch, RejectsNonCanonicalDistinctAttachmentNode) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::MDNode *Canonical =
      Input.Function->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(Canonical, nullptr);
  std::vector<llvm::Metadata *> Operands;
  for (const llvm::MDOperand &Operand : Canonical->operands())
    Operands.push_back(Operand.get());
  llvm::MDNode *Distinct = llvm::MDNode::getDistinct(Context, Operands);
  ASSERT_NE(Distinct, Canonical);
  Input.Function->setMetadata(windows_eh_md::FunctionAttachment, Distinct);
  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();
  Table->addOperand(functionTableRow(Context, *Input.Function, *Distinct));

  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: Windows EH metadata does not match the input "
      "image for function sub_140001000");
}

TEST(COFFExceptionPatch, AcceptsTargetAwareCanonicalSourceReencoding) {
  llvm::LLVMContext Context;
  llvm::Module Module("target-aware-eh-identity", Context);
  llvm::Function *Function = defineVoidFunction(Module, "seh_source");
  constexpr va_t Entry = 0x140001000;

  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + 0x20};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = 0x140003000;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Entry, Entry + 8};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Entry + 0x10;
  Scope.ContinuationVA = Scope.HandlerVA;
  EH.SEH->Scopes.push_back(Scope);

  rewrite_source::setOriginalVA(*Function, Entry);
  llvm::MDNode *Payload = windows_eh_md::getCanonicalFunctionMetadata(
      Context, EH, Arch::X64, BinaryFormat::COFF);
  Function->setMetadata(windows_eh_md::FunctionAttachment, Payload);
  Module.getOrInsertNamedMetadata(windows_eh_md::FunctionTable)
      ->addOperand(functionTableRow(Context, *Function, *Payload));

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Image.ExceptionMetadata.rebuildIndex();

  llvm::Error Error =
      validateCOFFExceptionSourceIdentityClosure(Module, Image);
  EXPECT_FALSE(static_cast<bool>(Error)) << llvm::toString(std::move(Error));
}

TEST(COFFExceptionPatch,
     RejectsClassifierRejectedCanonicalSourceAfterNativeLowering) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeNativeSEHPatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);

  auto InitialPlan =
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(InitialPlan))
      << llvm::toString(InitialPlan.takeError());

  ASSERT_EQ(Input.Image.ExceptionMetadata.Functions.size(), 1u);
  ExceptionFunction &EH = Input.Image.ExceptionMetadata.Functions.front();
  GSCookieInfo Cookie;
  Cookie.ParseStatus = ExceptionParseStatus::Complete;
  Cookie.CookieOffset = 0x20;
  Cookie.Payload = {0x20, 0, 0, 0};
  EH.GSCookie = std::move(Cookie);

  const WindowsEHNativeSourceClassification Classification =
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF);
  ASSERT_FALSE(Classification.canRegenerateLanguageMetadata());
  ASSERT_EQ(Classification.Reason,
            WindowsEHNativeSourceReason::UnexpectedGSCookie);

  llvm::MDNode *Payload = windows_eh_md::getCanonicalFunctionMetadata(
      Context, EH, Arch::X64, BinaryFormat::COFF);
  Input.Function->setMetadata(windows_eh_md::FunctionAttachment, Payload);
  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();
  Table->addOperand(functionTableRow(Context, *Input.Function, *Payload));

  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: language metadata failed target-aware "
      "regeneration checks for function native_seh_patch_source: "
      "unexpected-gs-cookie");
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

TEST(COFFExceptionPatch, RejectsNonCriticalNestedMetadataTampering) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::MDNode *Original =
      Input.Function->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(Original, nullptr);
  llvm::MDNode *Tampered = tamperedDiagnosticsPayload(Context, *Original);
  Input.Function->setMetadata(windows_eh_md::FunctionAttachment, Tampered);

  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();
  Table->addOperand(functionTableRow(Context, *Input.Function, *Tampered));

  // Nested operands participate in the source-image canonical identity, even
  // when the top-level version/range/status fields remain unchanged.
  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: Windows EH metadata does not match the input "
      "image for function sub_140001000");
}

TEST(COFFExceptionPatch, RejectsMissingWindowsEHFunctionTable) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->eraseFromParent();

  // Attachments and the module-level table are one closed contract.
  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: module omits the Windows EH function table");
}

TEST(COFFExceptionPatch, RejectsDuplicateWindowsEHFunctionTableRow) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->getNumOperands(), 1u);
  Table->addOperand(Table->getOperand(0));

  // Each attached definition has exactly one row.
  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: duplicate Windows EH function-table row for "
      "function sub_140001000");
}

TEST(COFFExceptionPatch,
     RejectsWindowsEHFunctionTablePayloadDifferentFromAttachment) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::MDNode *Attachment =
      Input.Function->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(Attachment, nullptr);
  llvm::MDNode *DifferentPayload =
      tamperedDiagnosticsPayload(Context, *Attachment);

  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();
  Table->addOperand(
      functionTableRow(Context, *Input.Function, *DifferentPayload));

  // The named row must use the attachment's exact metadata node.
  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: Windows EH function-table payload does not "
      "match the function attachment for function sub_140001000");
}

TEST(COFFExceptionPatch, RejectsOrphanWindowsEHFunctionTableRow) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::MDNode *Payload =
      Input.Function->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(Payload, nullptr);
  llvm::Function *Orphan = llvm::Function::Create(
      Input.Function->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
      "orphan_eh", *Input.Module);

  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->addOperand(functionTableRow(Context, *Orphan, *Payload));

  // A row cannot introduce an unattached function.
  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: orphan Windows EH function-table row for "
      "function orphan_eh");
}

TEST(COFFExceptionPatch, RejectsMalformedWindowsEHFunctionTableRows) {
  for (bool WrongTypes : {false, true}) {
    SCOPED_TRACE(WrongTypes ? "wrong operand types" : "wrong operand count");
    llvm::LLVMContext Context;
    CompletePatchInput Input = makeCompletePatchInput(Context);
    ASSERT_NE(Input.Module, nullptr);
    ASSERT_NE(Input.Function, nullptr);
    llvm::NamedMDNode *Table =
        Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
    ASSERT_NE(Table, nullptr);
    Table->clearOperands();
    if (WrongTypes) {
      Table->addOperand(llvm::MDNode::get(
          Context, {llvm::MDString::get(Context, "not a function"),
                    llvm::MDString::get(Context, "not a payload")}));
    } else {
      Table->addOperand(llvm::MDNode::get(
          Context, {llvm::ValueAsMetadata::get(Input.Function)}));
    }

    // Row arity and operand types are part of the stable table schema.
    expectPatchError(
        planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
        "coff exception patch: malformed Windows EH function-table row");
  }
}

TEST(COFFExceptionPatch, RejectsAttachedDefinedFunctionMissingTableRow) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *Second =
      addCompletePatchFunction(Input, Context, "sub_140002000", 0x140002000);
  ASSERT_NE(Second, nullptr);
  llvm::MDNode *SecondPayload =
      Second->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(SecondPayload, nullptr);

  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();
  Table->addOperand(functionTableRow(Context, *Second, *SecondPayload));

  // A nonempty table still has to cover every attached definition.
  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: Windows EH function table omits attached "
      "function sub_140001000");
}

TEST(COFFExceptionPatch, RejectsDeclarationWithAttachmentAndTableRow) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::MDNode *Payload =
      Input.Function->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(Payload, nullptr);
  llvm::Function *Declaration = llvm::Function::Create(
      Input.Function->getFunctionType(), llvm::GlobalValue::ExternalLinkage,
      "declared_eh", *Input.Module);
  Declaration->setMetadata(windows_eh_md::FunctionAttachment, Payload);

  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->addOperand(functionTableRow(Context, *Declaration, *Payload));

  // Only definitions can claim a generated Windows EH record.
  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: Windows EH function-table row references "
      "declaration declared_eh");
}

TEST(COFFExceptionPatch, RejectsFunctionTableRowFromAnotherModule) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::MDNode *Payload =
      Input.Function->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(Payload, nullptr);

  llvm::Module ExternalModule("external-eh-owner", Context);
  llvm::Function *External = defineVoidFunction(ExternalModule, "external_eh");
  External->setMetadata(windows_eh_md::FunctionAttachment, Payload);
  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->addOperand(functionTableRow(Context, *External, *Payload));

  // ValueAsMetadata cannot import a function identity from another module.
  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: Windows EH function-table row references "
      "external function external_eh");
}

TEST(COFFExceptionPatch, RejectsEmptyFunctionTableWithAttachment) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  ASSERT_NE(Input.Function->getMetadata(windows_eh_md::FunctionAttachment),
            nullptr);
  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();

  // An empty table cannot represent a module with attached definitions.
  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: Windows EH function table is empty while "
      "function attachments are present");
}

TEST(COFFExceptionPatch,
     SourcePreparationRejectsTamperingBeforeExternalizingAnyBody) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *Replaceable = addCompletePatchFunction(
      Input, Context, "sub_140002000", 0x140002000);
  ASSERT_NE(Replaceable, nullptr);
  addMixedSourceLayout(Input, 0x140001000, 0x140002000);

  llvm::MDNode *Attachment =
      Input.Function->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(Attachment, nullptr);
  llvm::MDNode *Tampered = tamperedDiagnosticsPayload(Context, *Attachment);
  Input.Function->setMetadata(windows_eh_md::FunctionAttachment, Tampered);
  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();
  Table->addOperand(functionTableRow(Context, *Input.Function, *Tampered));
  llvm::MDNode *ReplaceablePayload =
      Replaceable->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(ReplaceablePayload, nullptr);
  Table->addOperand(
      functionTableRow(Context, *Replaceable, *ReplaceablePayload));

  const std::string Before = moduleIR(*Input.Module);
  SourceFunctionPreparation Preparation;
  std::string Detail;
  EXPECT_FALSE(SourcePreparationProbe::prepare(
      *Input.Module, &Input.Image, Preparation, Detail));
  EXPECT_EQ(Detail,
            "coff exception patch: Windows EH metadata does not match the "
            "input image for function sub_140001000");
  EXPECT_EQ(moduleIR(*Input.Module), Before);
  EXPECT_FALSE(Input.Function->isDeclaration());
  EXPECT_FALSE(Replaceable->isDeclaration());
}

TEST(COFFExceptionPatch,
     SourcePreparationRemovesPreservedRowFromGeneratedEHClosure) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *Replaceable = addCompletePatchFunction(
      Input, Context, "sub_140002000", 0x140002000);
  ASSERT_NE(Replaceable, nullptr);
  addMixedSourceLayout(Input, 0x140001000, 0x140002000);

  SourceFunctionPreparation Preparation;
  std::string Detail;
  ASSERT_TRUE(SourcePreparationProbe::prepare(
      *Input.Module, &Input.Image, Preparation, Detail))
      << Detail;
  EXPECT_EQ(Preparation.PreservedOriginalVAs.at("sub_140001000"),
            0x140001000u);
  EXPECT_EQ(Preparation.ReplaceableOriginalVAs.at("sub_140002000"),
            0x140002000u);
  EXPECT_TRUE(Input.Function->isDeclaration());
  EXPECT_EQ(Input.Function->getMetadata(windows_eh_md::FunctionAttachment),
            nullptr);
  EXPECT_FALSE(Replaceable->isDeclaration());

  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->getNumOperands(), 1u);
  llvm::MDNode *Row = Table->getOperand(0);
  ASSERT_NE(Row, nullptr);
  const auto *FunctionValue = llvm::dyn_cast<llvm::ValueAsMetadata>(
      Row->getOperand(0).get());
  ASSERT_NE(FunctionValue, nullptr);
  EXPECT_EQ(FunctionValue->getValue(), Replaceable);

  auto Plan = planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
  ASSERT_EQ(Plan->ExceptionFunctionEntries.size(), 1u);
  EXPECT_EQ(Plan->ExceptionFunctionEntries.front(), 0x140002000u);
}

} // namespace
