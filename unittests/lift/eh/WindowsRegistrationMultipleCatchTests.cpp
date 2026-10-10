//===- WindowsRegistrationMultipleCatchTests.cpp - Ordered PE32 EH ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationMultipleCatchTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFPatch.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/high/MedToHigh.h"
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
void emitMultipleCatches(bool Realigned) {
  llvm::LLVMContext Context;
  llvm::Module Module("ordered-catch-parent", Context);
  Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
  Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
  llvm::IRBuilder<> B(Context);
  auto *Parent = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt32Ty(), false),
      llvm::GlobalValue::ExternalLinkage, "callback_parent", Module);
  Parent->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
  Parent->addFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
  Parent->addFnAttr("frame-pointer", "all");
  Parent->addFnAttr(llvm::Attribute::OptimizeNone);
  Parent->addFnAttr(llvm::Attribute::NoInline);
  Parent->setPersonalityFn(llvm::cast<llvm::Constant>(
      Module
          .getOrInsertFunction("__CxxFrameHandler3",
                               llvm::FunctionType::get(B.getInt32Ty(), true))
          .getCallee()));
  auto Block = [&](const char *Name) {
    return llvm::BasicBlock::Create(Context, Name, Parent);
  };
  auto *Entry = Block("entry"), *Choose = Block("choose"),
       *Normal = Block("normal"), *Dispatch = Block("dispatch");
  std::array<llvm::BasicBlock *, 3> Throw, Catch, Resume;
  for (unsigned I = 0; I != 3; ++I) {
    Throw[I] = Block("throw");
    Catch[I] = Block("catch");
    Resume[I] = Block("resume");
  }
  B.SetInsertPoint(Entry);
  auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64));
  Frame->setAlignment(llvm::Align(Realigned ? 64 : 4));
  B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                   &Module, llvm::Intrinsic::localescape),
               {Frame});
  auto Global = [&](const char *Name) {
    return new llvm::GlobalVariable(Module, B.getInt32Ty(), false,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    Name);
  };
  auto *Choice = B.CreateLoad(B.getInt32Ty(), Global("callback_choice"));
  Choice->setVolatile(true);
  B.CreateCondBr(B.CreateICmpEQ(Choice, B.getInt32(0)), Throw[0], Choose);
  B.SetInsertPoint(Choose);
  B.CreateCondBr(B.CreateICmpEQ(Choice, B.getInt32(1)), Throw[1], Throw[2]);
  const char *Names[] = {"callback_throw_int", "callback_throw_unsigned",
                         "callback_throw_float"};
  for (unsigned I = 0; I != 3; ++I) {
    B.SetInsertPoint(Throw[I]);
    auto Callee = Module.getOrInsertFunction(
        Names[I], llvm::FunctionType::get(B.getVoidTy(), false));
    B.CreateInvoke(Callee, Normal, Dispatch);
  }
  B.SetInsertPoint(Normal);
  B.CreateUnreachable();
  B.SetInsertPoint(Dispatch);
  auto *Switch =
      B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context), nullptr, 3);
  auto *Observation = Global("callback_caught");
  const char *Types[] = {"??_R0H@8", "??_R0I@8"};
  for (unsigned I = 0; I != 3; ++I) {
    Switch->addHandler(Catch[I]);
    B.SetInsertPoint(Catch[I]);
    auto *Null = llvm::ConstantPointerNull::get(B.getPtrTy());
    llvm::Constant *Type =
        I == 2 ? static_cast<llvm::Constant *>(Null)
               : new llvm::GlobalVariable(Module, B.getInt8Ty(), false,
                                          llvm::GlobalValue::ExternalLinkage,
                                          nullptr, Types[I]);
    auto *Pad = B.CreateCatchPad(
        Switch, {Type,
                 B.getInt32(I == 2   ? 64
                            : I == 1 ? 8
                                     : 0),
                 I == 2 ? static_cast<llvm::Value *>(Null) : Frame});
    if (I != 2)
      Pad->setMetadata(
          llvm::RewriteWinX86CxxCatchObjectAttachment,
          llvm::MDNode::get(
              Context, {llvm::ConstantAsMetadata::get(B.getInt32(1)),
                        llvm::ConstantAsMetadata::get(B.getInt32(16 + I * 8)),
                        llvm::ConstantAsMetadata::get(B.getInt32(4))}));
    llvm::Value *Value = B.getInt32(39);
    if (I != 2) {
      llvm::Value *Address =
          B.CreateInBoundsGEP(B.getInt8Ty(), Frame, B.getInt32(16 + I * 8));
      if (I == 1)
        Address = B.CreateLoad(B.getPtrTy(), Address);
      Value = B.CreateLoad(B.getInt32Ty(), Address);
      if (I == 1)
        B.CreateStore(Value = B.CreateAdd(Value, B.getInt32(11)), Address);
    }
    B.CreateStore(Value, Observation)->setVolatile(true);
    B.CreateStore(I == 2 ? Value : B.CreateAdd(Value, B.getInt32(10)), Frame)
        ->setVolatile(true);
    B.CreateCatchRet(Pad, Resume[I]);
    B.SetInsertPoint(Resume[I]);
    B.CreateRet(B.CreateLoad(B.getInt32Ty(), Frame));
  }
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  auto Object = Codegen().compile(Module, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(Object.Success);
  if (const auto *Path = std::getenv("NEVERD_REGISTRATION_MULTIPLE_OBJECT")) {
    std::error_code Error;
    llvm::raw_fd_ostream Out(Path, Error);
    ASSERT_FALSE(Error) << Error.message();
    Out.write(reinterpret_cast<const char *>(Object.ObjectData.data()),
              Object.ObjectData.size());
    Out.close();
    ASSERT_FALSE(Out.has_error());
  }
}
TEST(WindowsRegistrationMultipleCatch, EmitsFixedCallbacks) {
  emitMultipleCatches(false);
}
TEST(WindowsRegistrationMultipleCatch, EmitsRealignedCallbacks) {
  emitMultipleCatches(true);
}
TEST(WindowsRegistrationMultipleCatch, InputPE32ReconstructsOrderedCatches) {
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
  ASSERT_EQ(EH.Cxx->TryBlocks[0].Handlers.size(), 3u);
  ASSERT_EQ(States.CxxCatchObjects.size(), 2u);
  ASSERT_EQ(States.CxxContinuations.size(), 3u);
  ASSERT_TRUE(hasCallerCleanupRegistrationABI(Low, *Image));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  inferMedTypes(Med, Arch::X86);
  for (const auto &State : States.Blocks)
    if (State.Reached)
      EXPECT_TRUE(llvm::any_of(Med.Blocks, [&](const auto &Block) {
        return Block.StartAddr == State.Range.Begin &&
               Block.EndAddr == State.Range.End;
      }));
  const auto Throw =
      llvm::find_if(States.CallFrameEffects,
                    [](const auto &Call) { return Call.DoesNotReturn; });
  ASSERT_NE(Throw, States.CallFrameEffects.end());
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Low;
    auto &Proof = *Changed.RegistrationStates;
    auto &Call =
        Proof.CallFrameEffects[Throw - States.CallFrameEffects.begin()];
    if (Mutation == 0)
      ++Call.EndAddress;
    if (Mutation == 1)
      Call.Target += 4;
    if (Mutation == 2)
      Proof.CallFrameEffectsComplete = false;
    auto Unproved = Converter.convert(Changed, Arch::X86, BinaryFormat::COFF);
    const auto Block = llvm::find_if(Unproved.Blocks, [&](const auto &B) {
      return B.StartAddr <= Throw->Address && Throw->Address < B.EndAddr;
    });
    ASSERT_NE(Block, Unproved.Blocks.end());
    EXPECT_FALSE(Block->Succs.empty());
  }
  unsigned TerminalCalls = 0;
  for (const auto &Block : Med.Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      if (Op.Opcode != NdOp::CALL || !Op.DoesNotReturn)
        continue;
      ++TerminalCalls;
      EXPECT_EQ(I + 1, Block.Ops.size());
      EXPECT_TRUE(Block.Succs.empty());
      EXPECT_EQ(Block.ExceptionalSuccs.size(), 3u);
    }
  EXPECT_EQ(TerminalCalls, 3u);
  const auto High = MedToHighConverter().convert(Med, Arch::X86);
  EXPECT_EQ(High.StructuredExceptionRegions, 1u);
  EXPECT_EQ(High.UnstructuredExceptionRegions, 0u);
  const HighStmt *Try = nullptr;
  walkStmts(High.Body, [&](const auto &Stmt) {
    if (Stmt.Kind == StmtKind::CxxTry && Stmt.EHIsReducible)
      Try = &Stmt;
  });
  ASSERT_TRUE(Try);
  ASSERT_EQ(Try->EHClauses.size(), 3u);
  ASSERT_EQ(Try->EHClauseBodies.size(), 3u);
  for (unsigned I = 0; I != 3; ++I) {
    EXPECT_EQ(Try->EHClauses[I].HandlerVA,
              EH.Cxx->TryBlocks[0].Handlers[I].HandlerVA);
    EXPECT_FALSE(Try->EHClauseBodies[I].empty());
    ASSERT_EQ(Try->EHClauses[I].ContinuationVAs.size(), 1u);
    bool Resumes = false;
    walkStmts(Try->EHClauseBodies[I], [&](const auto &Stmt) {
      Resumes |= Stmt.Kind == StmtKind::Goto &&
                 Stmt.GotoTarget == Try->EHClauses[I].ContinuationVAs[0];
      EXPECT_NE(Stmt.Kind, StmtKind::Return);
    });
    EXPECT_TRUE(Resumes);
  }
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Med;
    if (Mutation == 0)
      Changed.RegistrationStates->CallFrameEffectsComplete = false;
    if (Mutation == 1)
      Changed.ExceptionMetadata->Cxx->IsSynchronous = false;
    if (Mutation == 2)
      for (auto &Block : Changed.Blocks)
        for (auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL)
            Op.DoesNotReturn = false;
    if (Mutation == 3)
      for (auto &Block : Changed.RegistrationStates->Blocks)
        if (!Block.CallbackOnly && Block.Levels == std::vector<int32_t>{0})
          Block.Levels = {-1};
    const auto Rejected = MedToHighConverter().convert(Changed, Arch::X86);
    EXPECT_EQ(Rejected.StructuredExceptionRegions, 0u);
    EXPECT_GT(Rejected.UnstructuredExceptionRegions, 0u);
  }
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
  registration_test::checkMultipleCatchEdits(*Parent, EH, *Image);
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
    ASSERT_TRUE(Generated->Cxx);
    ASSERT_EQ(Generated->Cxx->TryBlocks.size(), 1u);
    ASSERT_EQ(Generated->Cxx->TryBlocks[0].Handlers.size(), 3u);
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
          {"incoming_reads",
           llvm::count_if(States.IncomingFrameAccesses,
                          [](const auto &A) { return !A.Write; })},
          {"incoming_writes",
           llvm::count_if(States.IncomingFrameAccesses,
                          [](const auto &A) { return A.Write; })},
          {"schema", 1},
          {"evidence", "checked-realigned-source-reconstruction"},
          {"source_frame", EH.Registration->RealignedFrame ? "realigned"
                           : EH.Registration->hasCxxCallbackStack()
                               ? "fixed-displaced"
                               : "direct"},
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
