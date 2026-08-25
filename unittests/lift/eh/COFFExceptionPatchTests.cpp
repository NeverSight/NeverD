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
#include "neverd/backend/codegen/COFF/COFFPatch.h"
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
#include <array>
#include <initializer_list>
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

template <typename T>
void expectCxxGroupContractError(
    llvm::Expected<T> Result,
    exception_rewrite::CxxGroupContractErrorReason ExpectedReason,
    va_t ExpectedGroup) {
  ASSERT_FALSE(static_cast<bool>(Result));
  bool SawContractError = false;
  llvm::handleAllErrors(
      Result.takeError(),
      [&](const exception_rewrite::CxxGroupRewriteContractError &Error) {
        SawContractError = true;
        EXPECT_EQ(Error.reason(), ExpectedReason);
        EXPECT_EQ(Error.groupIdentity(), ExpectedGroup);
      },
      [&](const llvm::ErrorInfoBase &Error) {
        std::string Message;
        llvm::raw_string_ostream Stream(Message);
        Error.log(Stream);
        ADD_FAILURE() << "unexpected wrapped error: " << Stream.str();
      });
  EXPECT_TRUE(SawContractError);
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

class COFFPatcherProbe : public COFFPatcher {
public:
  static bool normalizeCompilerOwnedGS(
      llvm::Module &Module, const COFFExceptionPatchPlan &ExceptionPlan,
      const SourceFunctionPreparation &SourcePreparation, std::string &Detail) {
    return normalizeCompilerOwnedGSSecurityCheck(
        Module, Arch::X64, ExceptionPlan, SourcePreparation, Detail);
  }
};

llvm::Function *makeCompilerOwnedGSFunction(llvm::Module &Module,
                                            llvm::StringRef Name,
                                            va_t OriginalVA) {
  llvm::Function *Function = defineVoidFunction(Module, Name);
  rewrite_source::setOriginalVA(*Function, OriginalVA);
  Function->addFnAttr(llvm::mc_rewrite::RewriteWinCxxFH4Attribute);
  Function->addFnAttr(llvm::mc_rewrite::RewriteWinGSHandlerAttribute,
                      llvm::mc_rewrite::RewriteWinGSHandlerCxxFH4);
  Function->addFnAttr(llvm::Attribute::StackProtectReq);
  return Function;
}

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

void markSeparatedCxxMember(ExceptionFunction &EH, va_t GroupIdentity,
                            bool IsCatchFunclet) {
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.PersonalityVA = GroupIdentity - 0x40;
  EH.Cxx.emplace();
  EH.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH3;
  EH.Cxx->NativeFuncInfoVA = GroupIdentity;
  EH.Cxx->IsCatchFunclet = IsCatchFunclet;
  EH.Cxx->IsSeparated = true;
}

void rebuildPatchFunctionTable(
    CompletePatchInput &Input, llvm::LLVMContext &Context,
    llvm::ArrayRef<std::pair<llvm::Function *, size_t>> Contributions) {
  Input.Image.ExceptionMetadata.rebuildIndex();
  llvm::NamedMDNode *Table =
      Input.Module->getOrInsertNamedMetadata(windows_eh_md::FunctionTable);
  Table->clearOperands();
  for (const auto &[Function, Index] : Contributions) {
    llvm::MDNode *Payload = windows_eh_md::getCanonicalFunctionMetadata(
        Context, Input.Image.ExceptionMetadata.Functions[Index], Arch::X64,
        BinaryFormat::COFF);
    Function->setMetadata(windows_eh_md::FunctionAttachment, Payload);
    Table->addOperand(functionTableRow(Context, *Function, *Payload));
  }
}

exception_rewrite::CxxGroupRewriteContract makeCompleteCxxGroupContract(
    va_t GroupIdentity, va_t CanonicalOwner,
    std::initializer_list<exception_rewrite::CxxGroupMemberBinding> Members) {
  exception_rewrite::CxxGroupRewriteContract Contract;
  Contract.GroupIdentity = GroupIdentity;
  Contract.CanonicalSourceOwnerVA = CanonicalOwner;
  Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
  Contract.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
  Contract.Members.assign(Members.begin(), Members.end());
  return Contract;
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
     RejectsSharedCxxFuncInfoGroupWithoutAtomicRewriteContract) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *CatchContribution =
      addCompletePatchFunction(Input, Context, "sub_140002000", 0x140002000);
  ASSERT_NE(CatchContribution, nullptr);
  ASSERT_EQ(Input.Image.ExceptionMetadata.Functions.size(), 2u);

  constexpr va_t SharedFuncInfoVA = 0x140003040;
  for (size_t I = 0; I < Input.Image.ExceptionMetadata.Functions.size(); ++I) {
    ExceptionFunction &EH = Input.Image.ExceptionMetadata.Functions[I];
    EH.Personality = ExceptionPersonality::CxxFrameHandler3;
    EH.PersonalityVA = 0x140003000;
    EH.Cxx.emplace();
    EH.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH3;
    EH.Cxx->NativeFuncInfoVA = SharedFuncInfoVA;
    EH.Cxx->IsCatchFunclet = I != 0;
    EH.Cxx->IsSeparated = true;
  }
  Input.Image.ExceptionMetadata.rebuildIndex();

  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();
  const std::array<std::pair<llvm::Function *, size_t>, 2> Contributions{{
      {Input.Function, 0},
      {CatchContribution, 1},
  }};
  for (const auto &[Function, Index] : Contributions) {
    llvm::MDNode *Payload = windows_eh_md::getCanonicalFunctionMetadata(
        Context, Input.Image.ExceptionMetadata.Functions[Index], Arch::X64,
        BinaryFormat::COFF);
    Function->setMetadata(windows_eh_md::FunctionAttachment, Payload);
    Table->addOperand(functionTableRow(Context, *Function, *Payload));
  }

  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "C++ exception group rewrite contract 0x140003040: source group "
      "membership does not match (replaced source group has no atomic "
      "rewrite contract)");
}

TEST(COFFExceptionPatch, RejectsCxxGroupContractWithMissingSourceMember) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *CatchContribution =
      addCompletePatchFunction(Input, Context, "sub_140002000", 0x140002000);
  ASSERT_NE(CatchContribution, nullptr);

  constexpr va_t GroupIdentity = 0x140003040;
  markSeparatedCxxMember(Input.Image.ExceptionMetadata.Functions[0],
                         GroupIdentity, false);
  markSeparatedCxxMember(Input.Image.ExceptionMetadata.Functions[1],
                         GroupIdentity, true);
  const std::array<std::pair<llvm::Function *, size_t>, 2> Contributions{{
      {Input.Function, 0},
      {CatchContribution, 1},
  }};
  rebuildPatchFunctionTable(Input, Context, Contributions);

  const auto Contract = makeCompleteCxxGroupContract(
      GroupIdentity, 0x140001000, {{0x140001000, Input.Function}});
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      *Input.Module, llvm::ArrayRef(Contract)));

  expectCxxGroupContractError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      exception_rewrite::CxxGroupContractErrorReason::MembershipMismatch,
      GroupIdentity);
}

TEST(COFFExceptionPatch, RejectsCxxGroupContractForUnknownSourceGroup) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *CatchContribution =
      addCompletePatchFunction(Input, Context, "sub_140002000", 0x140002000);
  ASSERT_NE(CatchContribution, nullptr);

  constexpr va_t GroupIdentity = 0x140003040;
  constexpr va_t UnknownGroupIdentity = 0x140003080;
  markSeparatedCxxMember(Input.Image.ExceptionMetadata.Functions[0],
                         GroupIdentity, false);
  markSeparatedCxxMember(Input.Image.ExceptionMetadata.Functions[1],
                         GroupIdentity, true);
  const std::array<std::pair<llvm::Function *, size_t>, 2> Contributions{{
      {Input.Function, 0},
      {CatchContribution, 1},
  }};
  rebuildPatchFunctionTable(Input, Context, Contributions);

  const std::array<exception_rewrite::CxxGroupRewriteContract, 2> Contracts{{
      makeCompleteCxxGroupContract(
          GroupIdentity, 0x140001000,
          {{0x140001000, Input.Function}, {0x140002000, CatchContribution}}),
      makeCompleteCxxGroupContract(UnknownGroupIdentity, 0x140001000,
                                   {{0x140001000, Input.Function}}),
  }};
  ASSERT_FALSE(
      exception_rewrite::setCxxGroupRewriteContracts(*Input.Module, Contracts));

  expectPatchError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      "coff exception patch: C++ group contract 0x140003080 has no "
      "loader-authenticated source group");
}

TEST(COFFExceptionPatch, RejectsCxxContractMemberBoundAcrossSourceGroups) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *CatchA =
      addCompletePatchFunction(Input, Context, "sub_140002000", 0x140002000);
  llvm::Function *ParentB =
      addCompletePatchFunction(Input, Context, "sub_140003000", 0x140003000);
  llvm::Function *CatchB =
      addCompletePatchFunction(Input, Context, "sub_140004000", 0x140004000);
  ASSERT_NE(CatchA, nullptr);
  ASSERT_NE(ParentB, nullptr);
  ASSERT_NE(CatchB, nullptr);

  constexpr va_t GroupA = 0x140005040;
  constexpr va_t GroupB = 0x140005080;
  markSeparatedCxxMember(Input.Image.ExceptionMetadata.Functions[0], GroupA,
                         false);
  markSeparatedCxxMember(Input.Image.ExceptionMetadata.Functions[1], GroupA,
                         true);
  markSeparatedCxxMember(Input.Image.ExceptionMetadata.Functions[2], GroupB,
                         false);
  markSeparatedCxxMember(Input.Image.ExceptionMetadata.Functions[3], GroupB,
                         true);
  const std::array<std::pair<llvm::Function *, size_t>, 4> Contributions{{
      {Input.Function, 0},
      {CatchA, 1},
      {ParentB, 2},
      {CatchB, 3},
  }};
  rebuildPatchFunctionTable(Input, Context, Contributions);

  const std::array<exception_rewrite::CxxGroupRewriteContract, 2> Contracts{{
      makeCompleteCxxGroupContract(
          GroupA, 0x140001000,
          {{0x140001000, Input.Function}, {0x140004000, CatchB}}),
      makeCompleteCxxGroupContract(
          GroupB, 0x140003000, {{0x140002000, CatchA}, {0x140003000, ParentB}}),
  }};
  ASSERT_FALSE(
      exception_rewrite::setCxxGroupRewriteContracts(*Input.Module, Contracts));

  expectCxxGroupContractError(
      planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64),
      exception_rewrite::CxxGroupContractErrorReason::MembershipMismatch,
      GroupA);
}

TEST(COFFExceptionPatch,
     CompleteCxxGroupContractDoesNotBypassNativeSemanticGate) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *CatchContribution =
      addCompletePatchFunction(Input, Context, "sub_140002000", 0x140002000);
  ASSERT_NE(CatchContribution, nullptr);

  constexpr va_t GroupIdentity = 0x140003040;
  markSeparatedCxxMember(Input.Image.ExceptionMetadata.Functions[0],
                         GroupIdentity, false);
  markSeparatedCxxMember(Input.Image.ExceptionMetadata.Functions[1],
                         GroupIdentity, true);
  const std::array<std::pair<llvm::Function *, size_t>, 2> Contributions{{
      {Input.Function, 0},
      {CatchContribution, 1},
  }};
  rebuildPatchFunctionTable(Input, Context, Contributions);
  const auto Contract = makeCompleteCxxGroupContract(
      GroupIdentity, 0x140001000,
      {{0x140001000, Input.Function}, {0x140002000, CatchContribution}});
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      *Input.Module, llvm::ArrayRef(Contract)));

  auto Plan = planCOFFExceptionPatch(*Input.Module, Input.Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(Plan));
  const std::string Message = llvm::toString(Plan.takeError());
  EXPECT_NE(Message.find("target-aware regeneration checks"),
            std::string::npos);
  EXPECT_NE(Message.find("unsupported-cxx-version"), std::string::npos);
  EXPECT_EQ(Message.find("atomic group rewrite contract"), std::string::npos);
}

TEST(COFFExceptionPatch,
     DirectoryPreparationRejectsPartialSharedCxxFuncInfoIdentityConflict) {
  constexpr va_t ImageBase = 0x140000000;
  constexpr va_t SharedFuncInfoVA = ImageBase + 0x3040;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = ImageBase;

  ExceptionFunction Parent;
  Parent.CodeRange = {ImageBase + 0x1000, ImageBase + 0x1020};
  Parent.Kind = RuntimeFunctionKind::Primary;
  Parent.Cxx.emplace();
  Parent.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH3;
  Parent.Cxx->NativeFuncInfoVA = SharedFuncInfoVA;
  // Two records sharing the identity contradict a non-separated claim.  The
  // conflict cannot downgrade the group into independently replaceable rows.
  Parent.Cxx->IsSeparated = false;
  ExceptionFunction CatchContribution = Parent;
  CatchContribution.CodeRange = {ImageBase + 0x2000, ImageBase + 0x2020};
  Image.ExceptionMetadata.Functions.push_back(std::move(Parent));
  Image.ExceptionMetadata.Functions.push_back(std::move(CatchContribution));
  Image.ExceptionMetadata.rebuildIndex();

  CompiledImage Compiled;
  const std::vector<uint8_t> InvalidPE{0};
  const std::array<va_t, 1> PatchedEntries{{ImageBase + 0x1000}};
  auto Update = prepareCOFFExceptionDirectory(
      InvalidPE, Image, Compiled, PatchedEntries,
      llvm::ArrayRef<std::pair<va_t, va_t>>(), ImageBase + 0x10000, Arch::X64);

  ASSERT_FALSE(static_cast<bool>(Update));
  EXPECT_EQ(llvm::toString(Update.takeError()),
            "coff exception patch: shared C++ FuncInfo group 0x140003040 "
            "has inconsistent separated-member flags");
  EXPECT_TRUE(Compiled.Bytes.empty());
  EXPECT_TRUE(Compiled.Sections.empty());
}

TEST(COFFExceptionPatch,
     DirectoryPreparationRequiresModuleForSingletonSeparatedCxxGroup) {
  constexpr va_t ImageBase = 0x140000000;
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = ImageBase;

  ExceptionFunction LoneContribution;
  LoneContribution.CodeRange = {ImageBase + 0x1000, ImageBase + 0x1020};
  LoneContribution.Kind = RuntimeFunctionKind::Primary;
  LoneContribution.Cxx.emplace();
  LoneContribution.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH3;
  LoneContribution.Cxx->NativeFuncInfoVA = ImageBase + 0x3040;
  LoneContribution.Cxx->IsSeparated = true;
  Image.ExceptionMetadata.Functions.push_back(std::move(LoneContribution));
  Image.ExceptionMetadata.rebuildIndex();

  CompiledImage Compiled;
  const std::vector<uint8_t> InvalidPE{0};
  const std::array<va_t, 1> PatchedEntries{{ImageBase + 0x1000}};
  auto Update = prepareCOFFExceptionDirectory(
      InvalidPE, Image, Compiled, PatchedEntries,
      llvm::ArrayRef<std::pair<va_t, va_t>>(), ImageBase + 0x10000, Arch::X64);

  ASSERT_FALSE(static_cast<bool>(Update));
  EXPECT_EQ(llvm::toString(Update.takeError()),
            "coff exception patch: C++ FuncInfo group installation requires "
            "the rewrite module");
  EXPECT_TRUE(Compiled.Bytes.empty());
  EXPECT_TRUE(Compiled.Sections.empty());
}

TEST(COFFExceptionPatch,
     DirectoryPreparationRejectsMalformedCxxGroupInventoriesBeforePEParsing) {
  constexpr va_t ImageBase = 0x140000000;
  const std::vector<uint8_t> InvalidPE{0};
  auto MakeImage = [&] {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Format = BinaryFormat::COFF;
    Image.Base = ImageBase;
    return Image;
  };
  auto MakeMember = [&](va_t Begin, va_t End, va_t GroupIdentity, bool IsCatch,
                        RuntimeFunctionKind Kind =
                            RuntimeFunctionKind::Primary) {
    ExceptionFunction EH;
    EH.CodeRange = {Begin, End};
    EH.Kind = Kind;
    EH.Cxx.emplace();
    EH.Cxx->NativeFuncInfoVA = GroupIdentity;
    EH.Cxx->IsCatchFunclet = IsCatch;
    EH.Cxx->IsSeparated = true;
    return EH;
  };
  auto ErrorFor = [&](BinaryImage Image, llvm::ArrayRef<va_t> Entries) {
    CompiledImage Compiled;
    auto Update =
        prepareCOFFExceptionDirectory(InvalidPE, Image, Compiled, Entries,
                                      llvm::ArrayRef<std::pair<va_t, va_t>>(),
                                      ImageBase + 0x10000, Arch::X64);
    EXPECT_FALSE(static_cast<bool>(Update));
    return Update ? std::string() : llvm::toString(Update.takeError());
  };

  {
    BinaryImage Image = MakeImage();
    Image.ExceptionMetadata.Functions.push_back(
        MakeMember(ImageBase + 0x1000, ImageBase + 0x1020, 0, false));
    const std::array<va_t, 1> Entries{{ImageBase + 0x1000}};
    EXPECT_EQ(ErrorFor(std::move(Image), Entries),
              "coff exception patch: separated C++ EH contribution lacks a "
              "valid native FuncInfo group identity");
  }
  {
    BinaryImage Image = MakeImage();
    constexpr va_t GroupIdentity = ImageBase + 0x3040;
    Image.ExceptionMetadata.Functions.push_back(MakeMember(
        ImageBase + 0x1000, ImageBase + 0x1020, GroupIdentity, false));
    Image.ExceptionMetadata.Functions.push_back(MakeMember(
        ImageBase + 0x1000, ImageBase + 0x1020, GroupIdentity, true));
    const std::array<va_t, 1> Entries{{ImageBase + 0x1000}};
    EXPECT_EQ(ErrorFor(std::move(Image), Entries),
              "coff exception patch: C++ FuncInfo group 0x140003040 contains "
              "a duplicate primary member entry");
  }
  {
    BinaryImage Image = MakeImage();
    constexpr va_t GroupIdentity = ImageBase + 0x3040;
    Image.ExceptionMetadata.Functions.push_back(MakeMember(
        ImageBase + 0x1000, ImageBase + 0x1020, GroupIdentity, false));
    Image.ExceptionMetadata.Functions.push_back(MakeMember(
        ImageBase + 0x2000, ImageBase + 0x2020, GroupIdentity, false));
    const std::array<va_t, 2> Entries{{ImageBase + 0x1000, ImageBase + 0x2000}};
    EXPECT_EQ(ErrorFor(std::move(Image), Entries),
              "coff exception patch: C++ FuncInfo group 0x140003040 does "
              "not have exactly one canonical parent");
  }
  {
    BinaryImage Image = MakeImage();
    constexpr va_t GroupIdentity = ImageBase + 0x3040;
    Image.ExceptionMetadata.Functions.push_back(MakeMember(
        ImageBase + 0x1000, ImageBase + 0x1020, GroupIdentity, false));
    Image.ExceptionMetadata.Functions.push_back(
        MakeMember(ImageBase + 0x2000, ImageBase + 0x2020, GroupIdentity, true,
                   RuntimeFunctionKind::Fragment));
    const std::array<va_t, 1> Entries{{ImageBase + 0x1000}};
    EXPECT_EQ(ErrorFor(std::move(Image), Entries),
              "coff exception patch: C++ FuncInfo group 0x140003040 contains "
              "a non-primary runtime-function record");
  }
  {
    BinaryImage Image = MakeImage();
    constexpr va_t GroupA = ImageBase + 0x3040;
    constexpr va_t GroupB = ImageBase + 0x3080;
    Image.ExceptionMetadata.Functions.push_back(
        MakeMember(ImageBase + 0x1000, ImageBase + 0x1030, GroupA, false));
    Image.ExceptionMetadata.Functions.push_back(
        MakeMember(ImageBase + 0x2000, ImageBase + 0x2020, GroupA, true));
    Image.ExceptionMetadata.Functions.push_back(
        MakeMember(ImageBase + 0x1020, ImageBase + 0x1040, GroupB, false));
    Image.ExceptionMetadata.Functions.push_back(
        MakeMember(ImageBase + 0x3000, ImageBase + 0x3020, GroupB, true));
    const std::array<va_t, 4> Entries{{
        ImageBase + 0x1000,
        ImageBase + 0x1020,
        ImageBase + 0x2000,
        ImageBase + 0x3000,
    }};
    EXPECT_EQ(ErrorFor(std::move(Image), Entries),
              "coff exception patch: C++ FuncInfo group inventories overlap "
              "at primary member entry 0x140001020");
  }
}

TEST(COFFExceptionPatch,
     DirectoryPreparationAcceptsCompleteResolvedCxxGroupPreflight) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *CatchContribution =
      addCompletePatchFunction(Input, Context, "sub_140002000", 0x140002000);
  ASSERT_NE(CatchContribution, nullptr);

  constexpr va_t SharedFuncInfoVA = 0x140003040;
  ASSERT_EQ(Input.Image.ExceptionMetadata.Functions.size(), 2u);
  for (size_t I = 0; I < Input.Image.ExceptionMetadata.Functions.size(); ++I) {
    ExceptionFunction &EH = Input.Image.ExceptionMetadata.Functions[I];
    EH.Personality = ExceptionPersonality::CxxFrameHandler3;
    EH.PersonalityVA = 0x140003000;
    EH.Cxx.emplace();
    EH.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH3;
    EH.Cxx->NativeFuncInfoVA = SharedFuncInfoVA;
    EH.Cxx->IsCatchFunclet = I != 0;
    EH.Cxx->IsSeparated = true;
  }
  Input.Image.ExceptionMetadata.rebuildIndex();

  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();
  const std::array<std::pair<llvm::Function *, size_t>, 2> Contributions{{
      {Input.Function, 0},
      {CatchContribution, 1},
  }};
  for (const auto &[Function, Index] : Contributions) {
    llvm::MDNode *Payload = windows_eh_md::getCanonicalFunctionMetadata(
        Context, Input.Image.ExceptionMetadata.Functions[Index], Arch::X64,
        BinaryFormat::COFF);
    Function->setMetadata(windows_eh_md::FunctionAttachment, Payload);
    Table->addOperand(functionTableRow(Context, *Function, *Payload));
  }

  exception_rewrite::CxxGroupRewriteContract Contract;
  Contract.GroupIdentity = SharedFuncInfoVA;
  Contract.CanonicalSourceOwnerVA = 0x140001000;
  Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
  Contract.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
  Contract.Members = {{0x140001000, Input.Function},
                      {0x140002000, CatchContribution}};
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      *Input.Module, llvm::ArrayRef(Contract)));

  constexpr va_t GeneratedBase = 0x140010000;
  CompiledImage Compiled;
  Compiled.SourceFunctionOriginalVAs = {
      {Input.Function->getName().str(), 0x140001000},
      {CatchContribution->getName().str(), 0x140002000},
  };
  using OwnerKind = llvm::mc_rewrite::RewriteSourceFunctionOwnerKind;
  Compiled.SourceFunctionOwners = {
      {Input.Function->getName().str(),
       "parent.owner",
       GeneratedBase,
       false,
       OwnerKind::FunctionEntry,
       {}},
      {CatchContribution->getName().str(), "catch.owner", GeneratedBase + 0x20,
       true, OwnerKind::WinCxxCatchFunclet, Input.Function->getName().str()},
  };
  Compiled.FunctionOwnerAddrs = {{"parent.owner", GeneratedBase},
                                 {"catch.owner", GeneratedBase + 0x20}};
  Compiled.Sections.push_back({".text", 0, GeneratedBase, 0x40, 16,
                               llvm::mc_rewrite::RewriteSectionKind::Code,
                               true});

  const std::vector<uint8_t> InvalidPE{0};
  const std::array<va_t, 2> PatchedEntries{{0x140001000, 0x140002000}};
  const std::array<std::pair<va_t, va_t>, 2> PatchedMappings{{
      {0x140001000, GeneratedBase},
      {0x140002000, GeneratedBase + 0x20},
  }};
  const std::array<std::pair<va_t, va_t>, 2> WrongMappings{{
      {0x140001000, GeneratedBase + 0x20},
      {0x140002000, GeneratedBase},
  }};
  auto Rejected = prepareCOFFExceptionDirectory(
      InvalidPE, Input.Image, Compiled, PatchedEntries, WrongMappings,
      GeneratedBase, Arch::X64, Input.Module.get());
  ASSERT_FALSE(static_cast<bool>(Rejected));
  EXPECT_EQ(llvm::toString(Rejected.takeError()),
            "coff exception patch: C++ FuncInfo group 0x140003040 member "
            "0x140001000 lacks its exact generated-owner mapping");
  EXPECT_TRUE(Compiled.Bytes.empty());
  ASSERT_EQ(Compiled.Sections.size(), 1u);

  auto Update = prepareCOFFExceptionDirectory(
      InvalidPE, Input.Image, Compiled, PatchedEntries, PatchedMappings,
      GeneratedBase, Arch::X64, Input.Module.get());

  ASSERT_FALSE(static_cast<bool>(Update));
  EXPECT_EQ(llvm::toString(Update.takeError()),
            "coff exception patch: invalid PE headers");
  EXPECT_TRUE(Compiled.Bytes.empty());
  ASSERT_EQ(Compiled.Sections.size(), 1u);
  EXPECT_EQ(Compiled.Sections.front().Size, 0x40u);
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
     SourcePreparationRejectsMixedCxxGroupBeforeMutatingModule) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *Replaceable =
      addCompletePatchFunction(Input, Context, "sub_140002000", 0x140002000);
  ASSERT_NE(Replaceable, nullptr);
  addMixedSourceLayout(Input, 0x140001000, 0x140002000);

  constexpr va_t SharedFuncInfoVA = 0x140003040;
  ASSERT_EQ(Input.Image.ExceptionMetadata.Functions.size(), 2u);
  for (size_t I = 0; I < Input.Image.ExceptionMetadata.Functions.size(); ++I) {
    ExceptionFunction &EH = Input.Image.ExceptionMetadata.Functions[I];
    EH.Personality = ExceptionPersonality::CxxFrameHandler3;
    EH.PersonalityVA = 0x140003000;
    EH.Cxx.emplace();
    EH.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH3;
    EH.Cxx->NativeFuncInfoVA = SharedFuncInfoVA;
    EH.Cxx->IsCatchFunclet = I != 0;
    EH.Cxx->IsSeparated = true;
  }
  Input.Image.ExceptionMetadata.rebuildIndex();

  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();
  const std::array<std::pair<llvm::Function *, size_t>, 2> Contributions{{
      {Input.Function, 0},
      {Replaceable, 1},
  }};
  for (const auto &[Function, Index] : Contributions) {
    llvm::MDNode *Payload = windows_eh_md::getCanonicalFunctionMetadata(
        Context, Input.Image.ExceptionMetadata.Functions[Index], Arch::X64,
        BinaryFormat::COFF);
    Function->setMetadata(windows_eh_md::FunctionAttachment, Payload);
    Table->addOperand(functionTableRow(Context, *Function, *Payload));
  }

  const std::string Before = moduleIR(*Input.Module);
  SourceFunctionPreparation Preparation;
  std::string Detail;
  EXPECT_FALSE(SourcePreparationProbe::prepare(*Input.Module, &Input.Image,
                                               Preparation, Detail));
  EXPECT_EQ(Detail,
            "coff exception patch: C++ FuncInfo group 0x140003040 cannot "
            "mix replaced and preserved source members");
  EXPECT_EQ(moduleIR(*Input.Module), Before);
  EXPECT_FALSE(Input.Function->isDeclaration());
  EXPECT_FALSE(Replaceable->isDeclaration());
}

TEST(COFFExceptionPatch,
     SourcePreparationRemovesWhollyPreservedCxxGroupContract) {
  llvm::LLVMContext Context;
  CompletePatchInput Input = makeCompletePatchInput(Context);
  ASSERT_NE(Input.Module, nullptr);
  ASSERT_NE(Input.Function, nullptr);
  llvm::Function *CatchContribution =
      addCompletePatchFunction(Input, Context, "sub_140002000", 0x140002000);
  llvm::Function *Replaceable =
      addCompletePatchFunction(Input, Context, "sub_140003000", 0x140003000);
  ASSERT_NE(CatchContribution, nullptr);
  ASSERT_NE(Replaceable, nullptr);

  Segment Text;
  Text.Name = ".text";
  Text.VA = 0x140001000;
  Text.Size = 0x2040;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(static_cast<size_t>(Text.Size));
  Input.Image.Segments.push_back(std::move(Text));
  Input.Image.Symbols.push_back(Symbol::makeFunc(0x140001000, 0x20));
  Input.Image.Symbols.push_back(Symbol::makeFunc(0x140001001, 1));
  Input.Image.Symbols.push_back(Symbol::makeFunc(0x140002000, 0x20));
  Input.Image.Symbols.push_back(Symbol::makeFunc(0x140002001, 1));
  Input.Image.Symbols.push_back(Symbol::makeFunc(0x140003000, 0x20));

  constexpr va_t SharedFuncInfoVA = 0x140004040;
  for (size_t I = 0; I < 2; ++I) {
    ExceptionFunction &EH = Input.Image.ExceptionMetadata.Functions[I];
    EH.Personality = ExceptionPersonality::CxxFrameHandler3;
    EH.PersonalityVA = 0x140004000;
    EH.Cxx.emplace();
    EH.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH3;
    EH.Cxx->NativeFuncInfoVA = SharedFuncInfoVA;
    EH.Cxx->IsCatchFunclet = I != 0;
    EH.Cxx->IsSeparated = true;
  }
  Input.Image.ExceptionMetadata.rebuildIndex();

  llvm::NamedMDNode *Table =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(Table, nullptr);
  Table->clearOperands();
  const std::array<std::pair<llvm::Function *, size_t>, 3> Contributions{{
      {Input.Function, 0},
      {CatchContribution, 1},
      {Replaceable, 2},
  }};
  for (const auto &[Function, Index] : Contributions) {
    llvm::MDNode *Payload = windows_eh_md::getCanonicalFunctionMetadata(
        Context, Input.Image.ExceptionMetadata.Functions[Index], Arch::X64,
        BinaryFormat::COFF);
    Function->setMetadata(windows_eh_md::FunctionAttachment, Payload);
    Table->addOperand(functionTableRow(Context, *Function, *Payload));
  }

  exception_rewrite::CxxGroupRewriteContract Contract;
  Contract.GroupIdentity = SharedFuncInfoVA;
  Contract.CanonicalSourceOwnerVA = 0x140001000;
  Contract.Lowering = exception_rewrite::CxxGroupLoweringState::Complete;
  Contract.Installation = exception_rewrite::CxxGroupInstallState::AllOrNone;
  Contract.Members = {{0x140001000, Input.Function},
                      {0x140002000, CatchContribution}};
  ASSERT_FALSE(exception_rewrite::setCxxGroupRewriteContracts(
      *Input.Module, llvm::ArrayRef(Contract)));

  SourceFunctionPreparation Preparation;
  std::string Detail;
  ASSERT_TRUE(SourcePreparationProbe::prepare(*Input.Module, &Input.Image,
                                              Preparation, Detail))
      << Detail;
  EXPECT_TRUE(Input.Function->isDeclaration());
  EXPECT_TRUE(CatchContribution->isDeclaration());
  EXPECT_FALSE(Replaceable->isDeclaration());
  EXPECT_EQ(
      Input.Module->getNamedMetadata(exception_rewrite::CxxGroupTableMetadata),
      nullptr);
  llvm::NamedMDNode *RetainedTable =
      Input.Module->getNamedMetadata(windows_eh_md::FunctionTable);
  ASSERT_NE(RetainedTable, nullptr);
  ASSERT_EQ(RetainedTable->getNumOperands(), 1u);
  const auto *FunctionValue = llvm::dyn_cast<llvm::ValueAsMetadata>(
      RetainedTable->getOperand(0)->getOperand(0).get());
  ASSERT_NE(FunctionValue, nullptr);
  EXPECT_EQ(FunctionValue->getValue(), Replaceable);
}

TEST(COFFExceptionPatch, NormalizesLiftedSecurityCheckForCompilerOwnedGSFrame) {
  llvm::LLVMContext Context;
  llvm::Module Module("coff-gs-runtime-normalization", Context);
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t SecurityCheckVA = 0x140001100;
  llvm::Function *GSFunction =
      makeCompilerOwnedGSFunction(Module, "compiler_owned_gs", FunctionVA);

  auto *WrongType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(Context), false);
  llvm::Function *WrongCheck =
      llvm::Function::Create(WrongType, llvm::GlobalValue::ExternalLinkage,
                             "__security_check_cookie", Module);
  rewrite_source::setOriginalVA(*WrongCheck, SecurityCheckVA);
  llvm::IRBuilder<> Builder(GSFunction->getEntryBlock().getTerminator());
  Builder.CreateCall(WrongCheck);

  COFFExceptionPatchPlan Plan;
  Plan.LanguageExceptionFunctionEntries.push_back(FunctionVA);
  SourceFunctionPreparation Preparation;
  Preparation.PreservedOriginalVAs.emplace("__security_check_cookie",
                                           SecurityCheckVA);
  std::string Detail;
  ASSERT_TRUE(COFFPatcherProbe::normalizeCompilerOwnedGS(Module, Plan,
                                                         Preparation, Detail))
      << Detail;

  llvm::Function *Normalized = Module.getFunction("__security_check_cookie");
  ASSERT_NE(Normalized, nullptr);
  EXPECT_TRUE(Normalized->isDeclaration());
  EXPECT_TRUE(Normalized->getReturnType()->isVoidTy());
  ASSERT_EQ(Normalized->arg_size(), 1u);
  EXPECT_TRUE(Normalized->getFunctionType()->getParamType(0)->isPointerTy());
  EXPECT_EQ(Normalized->getCallingConv(), llvm::CallingConv::X86_FastCall);
  EXPECT_TRUE(Normalized->hasParamAttribute(0, llvm::Attribute::InReg));
  EXPECT_TRUE(Normalized->isDSOLocal());
  for (const llvm::Instruction &Instruction : GSFunction->getEntryBlock())
    EXPECT_FALSE(llvm::isa<llvm::CallBase>(Instruction));
}

TEST(COFFExceptionPatch,
     RejectsIncompatibleSecurityCheckUsedOutsideCompilerOwnedGSFrame) {
  llvm::LLVMContext Context;
  llvm::Module Module("coff-gs-runtime-non-gs-use", Context);
  constexpr va_t FunctionVA = 0x140001000;
  constexpr va_t SecurityCheckVA = 0x140001100;
  llvm::Function *GSFunction =
      makeCompilerOwnedGSFunction(Module, "compiler_owned_gs", FunctionVA);
  llvm::Function *Ordinary = defineVoidFunction(Module, "ordinary");

  auto *WrongType =
      llvm::FunctionType::get(llvm::Type::getInt64Ty(Context), false);
  llvm::Function *WrongCheck =
      llvm::Function::Create(WrongType, llvm::GlobalValue::ExternalLinkage,
                             "__security_check_cookie", Module);
  rewrite_source::setOriginalVA(*WrongCheck, SecurityCheckVA);
  llvm::IRBuilder<>(GSFunction->getEntryBlock().getTerminator())
      .CreateCall(WrongCheck);
  llvm::IRBuilder<>(Ordinary->getEntryBlock().getTerminator())
      .CreateCall(WrongCheck);

  COFFExceptionPatchPlan Plan;
  Plan.LanguageExceptionFunctionEntries.push_back(FunctionVA);
  SourceFunctionPreparation Preparation;
  Preparation.PreservedOriginalVAs.emplace("__security_check_cookie",
                                           SecurityCheckVA);
  std::string Detail;
  EXPECT_FALSE(COFFPatcherProbe::normalizeCompilerOwnedGS(Module, Plan,
                                                          Preparation, Detail));
  EXPECT_NE(Detail.find("non-GS use"), std::string::npos) << Detail;
  EXPECT_EQ(Module.getFunction("__security_check_cookie"), WrongCheck);
  EXPECT_EQ(WrongCheck->getReturnType(), llvm::Type::getInt64Ty(Context));
  EXPECT_EQ(WrongCheck->getNumUses(), 2u);
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
