//===- WindowsRegistrationRealignedNativeTests.cpp - Aligned PE32 EH ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationCxxCatchTestUtils.h"
#include "RegistrationCxxContinuationTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/COFF/COFFLoader.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/Support/Endian.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdlib>

using namespace neverd;

namespace {
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
enum class CatchForm { Value, Reference, UnnamedValue, UnnamedReference, All };

void emitCallback(CatchForm Form) {
  const bool Bound = Form == CatchForm::Value || Form == CatchForm::Reference;
  const bool Reference =
      Form == CatchForm::Reference || Form == CatchForm::UnnamedReference;
  llvm::LLVMContext Context;
  llvm::Module Module("typed-realigned-parent", Context);
  Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
  Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
  llvm::IRBuilder<> B(Context);
  auto *Parent = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt32Ty(), false),
      llvm::GlobalValue::ExternalLinkage, "callback_parent", Module);
  Parent->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
  Parent->addFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
  Parent->addFnAttr("frame-pointer", "all");
  Parent->setPersonalityFn(llvm::cast<llvm::Constant>(
      Module
          .getOrInsertFunction("__CxxFrameHandler3",
                               llvm::FunctionType::get(B.getInt32Ty(), true))
          .getCallee()));
  auto Block = [&](const char *Name) {
    return llvm::BasicBlock::Create(Context, Name, Parent);
  };
  auto *Entry = Block("entry"), *Normal = Block("normal"),
       *Dispatch = Block("dispatch"), *Catch = Block("catch"),
       *Resume = Block("resume");
  B.SetInsertPoint(Entry);
  auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64));
  Frame->setAlignment(llvm::Align(64));
  auto *Object = B.CreateInBoundsGEP(B.getInt8Ty(), Frame, B.getInt32(16));
  B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                   &Module, llvm::Intrinsic::localescape),
               {Frame});
  auto Throw = Module.getOrInsertFunction(
      "callback_throw", llvm::FunctionType::get(B.getVoidTy(), false));
  B.CreateInvoke(Throw, Normal, Dispatch);
  B.SetInsertPoint(Normal);
  B.CreateUnreachable();
  B.SetInsertPoint(Dispatch);
  auto *Switch =
      B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context), nullptr, 1);
  Switch->addHandler(Catch);
  B.SetInsertPoint(Catch);
  auto *Null = llvm::ConstantPointerNull::get(B.getPtrTy());
  llvm::Constant *Type = Null;
  if (Form != CatchForm::All)
    Type = new llvm::GlobalVariable(Module, B.getInt8Ty(), false,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    "??_R0H@8");
  auto *Pad = B.CreateCatchPad(
      Switch, {Type,
               B.getInt32(Form == CatchForm::All ? 64
                          : Reference            ? 8
                                                 : 0),
               Bound ? static_cast<llvm::Value *>(Frame) : Null});
  if (Bound)
    Pad->setMetadata(
        llvm::RewriteWinX86CxxCatchObjectAttachment,
        llvm::MDNode::get(Context,
                          {llvm::ConstantAsMetadata::get(B.getInt32(1)),
                           llvm::ConstantAsMetadata::get(B.getInt32(16)),
                           llvm::ConstantAsMetadata::get(B.getInt32(4))}));
  llvm::Value *Value = B.getInt32(7), *Caught = Value;
  if (Bound) {
    llvm::Value *Address =
        Reference ? B.CreateLoad(B.getPtrTy(), Object) : Object;
    Value = B.CreateLoad(B.getInt32Ty(), Address);
    Caught = Value;
    if (Reference)
      B.CreateStore(Caught = B.CreateAdd(Value, B.getInt32(11)), Address);
  }
  auto *Observation = new llvm::GlobalVariable(
      Module, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage,
      nullptr, "callback_caught");
  B.CreateStore(Caught, Observation)->setVolatile(true);
  B.CreateStore(Value, Frame)->setVolatile(true);
  B.CreateCatchRet(Pad, Resume);
  B.SetInsertPoint(Resume);
  B.CreateRet(B.CreateLoad(B.getInt32Ty(), Frame));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  auto ObjectFile = Codegen().compile(Module, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(ObjectFile.Success);
  if (const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_OBJECT")) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Out.write(reinterpret_cast<const char *>(ObjectFile.ObjectData.data()),
              ObjectFile.ObjectData.size());
    Out.close();
    ASSERT_FALSE(Out.has_error());
  }
}

TEST(WindowsRegistrationRealignedNative, EmitsValueCallback) {
  emitCallback(CatchForm::Value);
}

TEST(WindowsRegistrationRealignedNative, EmitsReferenceCallback) {
  emitCallback(CatchForm::Reference);
}

TEST(WindowsRegistrationRealignedNative, EmitsUnnamedValueCallback) {
  emitCallback(CatchForm::UnnamedValue);
}

TEST(WindowsRegistrationRealignedNative, EmitsUnnamedReferenceCallback) {
  emitCallback(CatchForm::UnnamedReference);
}

TEST(WindowsRegistrationRealignedNative, EmitsCatchAllCallback) {
  emitCallback(CatchForm::All);
}

void checkFrameEdits(const llvm::Function &Parent,
                     const ExceptionFunction &Source,
                     const BinaryImage &Image) {
  const char *Names[] = {"weakened alignment",
                         "changed source entry offset",
                         "missing callback identity",
                         "callback bound names parent frame",
                         "shifted callback ESP",
                         "missing ESP root",
                         "uninitialized scratch read",
                         "scratch read outside allocation",
                         "scratch read after catch return",
                         "callback pointer in another parent cell",
                         "changed alignment mask",
                         "opaque callback pointer in parent"};
  for (unsigned Mutation = 0; Mutation != std::size(Names); ++Mutation) {
    SCOPED_TRACE(Names[Mutation]);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::AllocaInst *Frame = nullptr, *Stack = nullptr;
    llvm::StoreInst *Seed = nullptr;
    llvm::CatchPadInst *Pad = nullptr;
    llvm::CatchReturnInst *Return = nullptr;
    llvm::BinaryOperator *Mask = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
          Frame = llvm::cast<llvm::AllocaInst>(&I);
        if (I.getMetadata(windows_eh_md::RegistrationCatchStackAttachment))
          Stack = llvm::cast<llvm::AllocaInst>(&I);
        if (I.getMetadata(windows_eh_md::RegistrationRootAttachment))
          Seed = llvm::cast<llvm::StoreInst>(&I);
        if (auto *Candidate = llvm::dyn_cast<llvm::CatchPadInst>(&I))
          Pad = Candidate;
        if (auto *Candidate = llvm::dyn_cast<llvm::CatchReturnInst>(&I))
          Return = Candidate;
        if (!Mask && I.getOpcode() == llvm::Instruction::And)
          Mask = llvm::cast<llvm::BinaryOperator>(&I);
      }
    ASSERT_TRUE(Frame && Stack && Seed && Pad && Return && Mask);
    llvm::IRBuilder<> B(Pad->getNextNode()->getNextNode());
    const auto Size = Stack->getAllocationSize(Module->getDataLayout());
    ASSERT_TRUE(Size);
    switch (Mutation) {
    case 0:
      Frame->setAlignment(llvm::Align(16));
      break;
    case 1: {
      const auto *MD =
          Frame->getMetadata(windows_eh_md::RegistrationFrameAttachment);
      const auto *Offset =
          llvm::mdconst::extract<llvm::ConstantInt>(MD->getOperand(1));
      Frame->setMetadata(
          windows_eh_md::RegistrationFrameAttachment,
          llvm::MDNode::get(B.getContext(),
                            {MD->getOperand(0),
                             llvm::ConstantAsMetadata::get(
                                 B.getInt64(Offset->getZExtValue() + 64))}));
      break;
    }
    case 2:
      Stack->setMetadata(windows_eh_md::RegistrationCatchStackAttachment,
                         nullptr);
      break;
    case 3:
    case 4: {
      auto *Value = llvm::cast<llvm::PtrToIntInst>(Seed->getValueOperand());
      auto *Address =
          llvm::cast<llvm::GetElementPtrInst>(Value->getPointerOperand());
      Address->setOperand(Mutation == 3 ? 0 : 1,
                          Mutation == 3
                              ? static_cast<llvm::Value *>(Frame)
                              : B.getInt32(Size->getFixedValue() - 4));
      break;
    }
    case 5:
      Seed->setMetadata(windows_eh_md::RegistrationRootAttachment, nullptr);
      break;
    case 6:
    case 7:
    case 8: {
      if (Mutation == 8)
        B.SetInsertPoint(&*Return->getSuccessor()->getFirstInsertionPt());
      auto *Address =
          B.CreateGEP(B.getInt8Ty(), Stack,
                      B.getInt32(Mutation == 7 ? Size->getFixedValue() : 0));
      B.CreateLoad(B.getInt32Ty(), Address)->setVolatile(true);
      break;
    }
    case 9:
      B.CreateStore(B.CreatePtrToInt(Stack, B.getInt32Ty()), Frame);
      break;
    case 10:
      Mask->setOperand(1, B.getInt32(-32));
      break;
    case 11:
      B.CreateStore(
          B.CreateXor(B.CreatePtrToInt(Stack, B.getInt32Ty()), B.getInt32(4)),
          Frame);
      break;
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    // These edits retain valid control receipts. Only the post-edit memory
    // proof can reject their initialization, bounds and lifetime changes.
    if (Mutation >= 6 && Mutation <= 9) {
      auto Control =
          validateCOFFRegistrationCxxControlIR(*Function, Source, Image);
      ASSERT_FALSE(bool(Control)) << llvm::toString(std::move(Control));
    }
    auto Changed = validateCOFFRegistrationCxxIR(*Function, Source, Image);
    EXPECT_TRUE(bool(Changed));
    if (Changed)
      llvm::consumeError(std::move(Changed));
  }
}

TEST(WindowsRegistrationRealignedNative, InputPE32ReconstructsTheSourceFrame) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_REALIGNED_NATIVE_PE32";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const auto It =
      llvm::find_if(Image->ExceptionMetadata.Functions,
                    [](const auto &EH) { return EH.Registration && EH.Cxx; });
  ASSERT_NE(It, Image->ExceptionMetadata.Functions.end());
  const auto &EH = *It;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  auto Low = CFGBuilder().build(*Image, Decode, EH.CodeRange.Begin,
                                "realigned_source");
  ASSERT_TRUE(Low.RegistrationStates);
  for (const auto &Diagnostic : Low.RegistrationStates->Diagnostics)
    llvm::errs() << Diagnostic << '\n';
  const auto &States = *Low.RegistrationStates;
  ASSERT_TRUE(States.Complete);
  ASSERT_TRUE(States.CxxContinuationsComplete);
  ASSERT_TRUE(States.CxxCatchObjectsComplete);
  ASSERT_TRUE(States.RuntimeObjectAccessesComplete);
  ASSERT_EQ(EH.Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(EH.Cxx->TryBlocks[0].Handlers.size(), 1u);
  ASSERT_EQ(States.CxxCatchObjects.size(),
            EH.Cxx->TryBlocks[0].Handlers[0].CatchObjectOffset ? 1u : 0u);
  ASSERT_TRUE(hasCallerCleanupRegistrationABI(Low, *Image));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  inferMedTypes(Med, Arch::X86);
  llvm::LLVMContext Context;
  auto Module =
      MedLLVMEmitter().emit({Med}, Context, "realigned-source", Arch::X86, {},
                            &*Image, BinaryFormat::COFF);
  ASSERT_TRUE(Module);
  if (const auto *Output = std::getenv("NEVERD_REGISTRATION_OUTPUT_IR")) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Output, Error);
    ASSERT_FALSE(Error) << Error.message();
    Module->print(Out, nullptr);
  }
  auto *Parent = Module->getFunction(Med.Name);
  ASSERT_TRUE(Parent);
  ASSERT_TRUE(Parent->getMetadata(windows_eh_md::NativeAttachment));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto Proof = validateCOFFRegistrationCxxIR(*Parent, EH, *Image);
  ASSERT_FALSE(bool(Proof)) << llvm::toString(std::move(Proof));
  registration_test::checkUnboundCxxCatchEdits(*Parent, EH, *Image);
  if (EH.Registration->RealignedFrame)
    checkFrameEdits(*Parent, EH, *Image);
  registration_test::checkCxxContinuationEdits(*Parent, EH, *Image);
  if (const auto *Output =
          std::getenv("NEVERD_REGISTRATION_REALIGNED_OUTPUT_PE32")) {
    COFFPatcher Patcher;
    Patcher.setImageContext(&*Image);
    const auto Result = Patcher.patch(Path, Output, *Module, Arch::X86);
    ASSERT_TRUE(Result.Success);
    EXPECT_EQ(Result.TrampolineCount, 1u);
    EXPECT_EQ(Result.PatchedOriginalEntries,
              (std::vector<va_t>{EH.CodeRange.Begin}));
    auto Reloaded = COFFLoader().load(Output);
    ASSERT_TRUE(bool(Reloaded)) << llvm::toString(Reloaded.takeError());
    const auto *Trampoline = Reloaded->readVA(EH.CodeRange.Begin, 5);
    ASSERT_TRUE(Trampoline);
    ASSERT_EQ(Trampoline[0], 0xe9);
    const va_t Target =
        uint32_t(EH.CodeRange.Begin + 5 +
                 llvm::support::endian::read32le(Trampoline + 1));
    const auto Generated = llvm::find_if(
        Reloaded->ExceptionMetadata.Functions, [&](const auto &Candidate) {
          return Candidate.CodeRange.Begin == Target;
        });
    ASSERT_NE(Generated, Reloaded->ExceptionMetadata.Functions.end());
    ASSERT_EQ(Generated->ParseStatus, ExceptionParseStatus::Complete);
    ASSERT_TRUE(Generated->Registration);
    if (EH.Registration->RealignedFrame)
      ASSERT_TRUE(Generated->Registration->RealignedFrame);
    if (const auto *Receipt =
            std::getenv("NEVERD_REGISTRATION_REALIGNED_RECEIPT")) {
      auto Digest = [](const char *File) {
        auto Buffer = llvm::MemoryBuffer::getFile(File);
        EXPECT_TRUE(bool(Buffer));
        if (!Buffer)
          return std::string();
        return llvm::toHex(llvm::SHA256::hash(llvm::arrayRefFromStringRef(
                               (*Buffer)->getBuffer())),
                           true);
      };
      llvm::json::Object Record{
          {"schema", 1},
          {"evidence", "checked-realigned-source-reconstruction"},
          {"source_frame",
           EH.Registration->RealignedFrame ? "realigned" : "direct"},
          {"source_image_sha256", Digest(Path)},
          {"image_sha256", Digest(Output)},
          {"base", Image->Base},
          {"source_begin", EH.CodeRange.Begin - Image->Base},
          {"source_end", EH.CodeRange.End - Image->Base},
          {"generated_begin", Generated->CodeRange.Begin - Image->Base},
          {"generated_end", Generated->CodeRange.End - Image->Base}};
      std::error_code Error;
      llvm::raw_fd_ostream Out(Receipt, Error);
      ASSERT_FALSE(Error) << Error.message();
      Out << llvm::formatv("{0:2}\n", llvm::json::Value(std::move(Record)));
    }
  }
}
#endif
} // namespace
