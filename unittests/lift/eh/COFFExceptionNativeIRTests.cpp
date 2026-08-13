//===- COFFExceptionNativeIRTests.cpp - Native WinEH lowering tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/COFF/COFFExceptionPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <mutex>

namespace {

using namespace neverd;

void ensureCOFFCodegenTargets() {
  static std::once_flag Once;
  std::call_once(Once, [] {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
  });
}
TEST(COFFExceptionIR, EmitsVerifierCleanNativeCatchAllSEH) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "native_seh_test";

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x10;
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = Func.Entry + 8;
  Protected.Ops.push_back(ProtectedReturn);

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = Func.Entry + 0x20;
  Handler.EndAddr = Func.Entry + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = Func.Entry + 0x28;
  Handler.Ops.push_back(HandlerReturn);

  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = Func.Entry + 0x100;
  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Func.Entry, Func.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Func.Entry + 0x20;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Ctx;
  MedLLVMEmitter Emitter;
  auto Mod = Emitter.emit({Func}, Ctx, "native_seh", Arch::X64, {}, nullptr,
                          BinaryFormat::COFF);
  ASSERT_NE(Mod, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Mod, &VerificationOS)) << Verification;

  llvm::Function *F = Mod->getFunction(Func.Name);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->hasPersonalityFn());
  EXPECT_NE(F->getMetadata(windows_eh_md::NativeAttachment), nullptr);
  EXPECT_NE(Mod->getModuleFlag("eh-asynch"), nullptr);

  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Mod->print(OS, nullptr);
  EXPECT_NE(IR.find("invoke void @llvm.seh.try.begin"), std::string::npos);
  EXPECT_NE(IR.find("invoke void @llvm.seh.try.end"), std::string::npos);
  EXPECT_NE(IR.find("catchswitch within none"), std::string::npos);
  EXPECT_NE(IR.find("catchpad within"), std::string::npos);
  EXPECT_NE(IR.find("catchret from"), std::string::npos);
}

TEST(COFFExceptionIR, EmitsVerifierCleanNativeSimpleFH3Catch) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "native_cxx_test";
  Func.ReturnType = NdType::makeVoid();
  constexpr va_t MayThrowVA = 0x140001100;

  MedBlock Protected;
  Protected.Id = 0;
  Protected.StartAddr = Func.Entry;
  Protected.EndAddr = Func.Entry + 0x10;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Func.Entry + 4;
  Call.addInput(MedVar::makeConst(MayThrowVA, 8));
  Protected.Ops.push_back(Call);
  MedOp ProtectedReturn;
  ProtectedReturn.Opcode = NdOp::RETURN;
  ProtectedReturn.Addr = Func.Entry + 8;
  Protected.Ops.push_back(ProtectedReturn);

  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = Func.Entry + 0x20;
  Handler.EndAddr = Func.Entry + 0x30;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = Func.Entry + 0x28;
  Handler.Ops.push_back(HandlerReturn);

  Func.Blocks.push_back(std::move(Protected));
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x40};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.Flags = 1;
  Cxx.IsSynchronous = true;
  Cxx.MaxState = 2;
  CxxUnwindAction State0;
  State0.ToState = -1;
  State0.Kind = CxxUnwindAction::ActionKind::None;
  CxxUnwindAction State1;
  State1.ToState = 0;
  State1.Kind = CxxUnwindAction::ActionKind::None;
  Cxx.UnwindMap = {State0, State1};
  Cxx.IPMap = {{Func.Entry, 0}, {Func.Entry + 0x10, -1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.Adjectives = 0x40;
  Catch.HandlerVA = Func.Entry + 0x20;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = EH;

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.DynInfo.GuardFlags =
      uint32_t(llvm::COFF::GuardFlags::EH_CONTINUATION_TABLE_PRESENT) |
      uint32_t(llvm::COFF::GuardFlags::CF_INSTRUMENTED) |
      uint32_t(llvm::COFF::GuardFlags::CF_FUNCTION_TABLE_PRESENT);
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  MedLLVMEmitter Emitter;
  auto Mod =
      Emitter.emit({Func}, Ctx, "native_cxx", Arch::X64,
                   {{MayThrowVA, "may_throw"}}, &Image, BinaryFormat::COFF);
  ASSERT_NE(Mod, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Mod, &VerificationOS)) << Verification;

  llvm::Function *F = Mod->getFunction(Func.Name);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->hasPersonalityFn());
  EXPECT_NE(F->getMetadata(windows_eh_md::NativeAttachment), nullptr);
  EXPECT_EQ(Mod->getModuleFlag("eh-asynch"), nullptr);
  EXPECT_NE(Mod->getModuleFlag("cfguard"), nullptr);
  EXPECT_NE(Mod->getModuleFlag("ehcontguard"), nullptr);

  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Mod->print(OS, nullptr);
  EXPECT_NE(IR.find("personality ptr @__CxxFrameHandler3"), std::string::npos);
  EXPECT_NE(IR.find("invoke"), std::string::npos);
  EXPECT_NE(IR.find("catchswitch within none"), std::string::npos);
  EXPECT_NE(IR.find("catchpad within"), std::string::npos);
  EXPECT_NE(IR.find("i32 64"), std::string::npos);

  auto Plan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());

  ensureCOFFCodegenTargets();
  constexpr uint64_t GeneratedVA = 0x140004000;
  CompiledImage Compiled = compileImageForPatch(
      *Mod, Arch::X64, BinaryFormat::COFF, GeneratedVA,
      [&](llvm::StringRef Symbol, uint32_t) -> std::optional<uint64_t> {
        if (Symbol == "may_throw")
          return MayThrowVA;
        if (Symbol == "__CxxFrameHandler3")
          return Func.Entry + 0x180;
        return std::nullopt;
      },
      Image.Base);
  ASSERT_TRUE(Compiled.Success);
  EXPECT_TRUE(Compiled.Unresolved.empty());
  EXPECT_TRUE(Compiled.SymbolAddrs.count(Func.Name));

  const CompiledSection *EHCont = nullptr;
  const CompiledSection *Text = nullptr;
  for (const CompiledSection &Section : Compiled.Sections) {
    if (llvm::StringRef(Section.Name).starts_with(".gehcont"))
      EHCont = &Section;
    if (Section.Kind == llvm::mc_rewrite::RewriteSectionKind::Code)
      Text = &Section;
  }
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(EHCont, nullptr);
  EXPECT_FALSE(EHCont->IsAllocated);
  EXPECT_EQ(EHCont->VA, 0u);
  ASSERT_FALSE(EHCont->SymbolIndexReferences.empty());
  for (const auto &Reference : EHCont->SymbolIndexReferences) {
    EXPECT_GE(Reference.TargetVA, Text->VA);
    EXPECT_LT(Reference.TargetVA, Text->VA + Text->Size);
  }

  auto *WrongPersonalityTy =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Ctx), {}, true);
  llvm::FunctionCallee WrongPersonality =
      Mod->getOrInsertFunction("__C_specific_handler", WrongPersonalityTy);
  F->setPersonalityFn(llvm::cast<llvm::Constant>(WrongPersonality.getCallee()));
  auto TamperedPlan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_FALSE(static_cast<bool>(TamperedPlan));
  EXPECT_NE(llvm::toString(TamperedPlan.takeError())
                .find("native WinEH IR contract was altered"),
            std::string::npos);
}

TEST(COFFExceptionIR, EmitsVerifierCleanNestedFH3CatchRegions) {
  MedFunc Func;
  Func.Entry = 0x140001000;
  Func.Name = "native_nested_cxx_test";
  Func.ReturnType = NdType::makeVoid();
  constexpr va_t MayThrowVA = 0x140001200;

  auto AddCallBlock = [&](int Id, va_t Begin) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Begin;
    Block.EndAddr = Begin + 0x10;
    MedOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Begin + 4;
    Call.addInput(MedVar::makeConst(MayThrowVA, 8));
    Block.Ops.push_back(Call);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Begin + 8;
    Block.Ops.push_back(Return);
    Func.Blocks.push_back(std::move(Block));
  };
  auto AddHandlerBlock = [&](int Id, va_t Begin) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Begin;
    Block.EndAddr = Begin + 0x10;
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Begin + 8;
    Block.Ops.push_back(Return);
    Func.Blocks.push_back(std::move(Block));
  };
  AddCallBlock(0, Func.Entry);
  AddCallBlock(1, Func.Entry + 0x10);
  AddHandlerBlock(2, Func.Entry + 0x40);
  AddHandlerBlock(3, Func.Entry + 0x50);

  ExceptionFunction EH;
  EH.CodeRange = {Func.Entry, Func.Entry + 0x60};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  CxxExceptionInfo Cxx;
  Cxx.Flags = 1;
  Cxx.IsSynchronous = true;
  Cxx.MaxState = 4;
  for (int32_t State = 0; State < 4; ++State) {
    CxxUnwindAction Action;
    Action.ToState = State - 1;
    Action.Kind = CxxUnwindAction::ActionKind::None;
    Cxx.UnwindMap.push_back(Action);
  }
  Cxx.IPMap = {
      {Func.Entry, 0}, {Func.Entry + 0x10, 1}, {Func.Entry + 0x20, -1}};

  CxxTryBlock Outer;
  Outer.TryLow = 0;
  Outer.TryHigh = 1;
  Outer.CatchHigh = 3;
  CxxCatchHandler OuterCatch;
  OuterCatch.HandlerVA = Func.Entry + 0x40;
  Outer.Handlers.push_back(OuterCatch);
  Cxx.TryBlocks.push_back(std::move(Outer));

  CxxTryBlock Inner;
  Inner.TryLow = 1;
  Inner.TryHigh = 1;
  Inner.CatchHigh = 2;
  CxxCatchHandler InnerCatch;
  InnerCatch.HandlerVA = Func.Entry + 0x50;
  Inner.Handlers.push_back(InnerCatch);
  Cxx.TryBlocks.push_back(std::move(Inner));
  ASSERT_TRUE(Cxx.hasValidStateGraph());
  EH.Cxx = std::move(Cxx);
  Func.ExceptionMetadata = EH;

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x140000000;
  Image.ExceptionMetadata.Functions.push_back(EH);
  Image.ExceptionMetadata.rebuildIndex();

  llvm::LLVMContext Ctx;
  MedLLVMEmitter Emitter;
  auto Mod =
      Emitter.emit({Func}, Ctx, "native_nested_cxx", Arch::X64,
                   {{MayThrowVA, "may_throw"}}, &Image, BinaryFormat::COFF);
  ASSERT_NE(Mod, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(*Mod, &VerificationOS)) << Verification;

  llvm::Function *F = Mod->getFunction(Func.Name);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->hasPersonalityFn());
  EXPECT_NE(F->getMetadata(windows_eh_md::NativeAttachment), nullptr);

  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Mod->print(OS, nullptr);
  EXPECT_NE(IR.find("cxx.catch.dispatch.0"), std::string::npos);
  EXPECT_NE(IR.find("cxx.catch.dispatch.1"), std::string::npos);
  EXPECT_NE(IR.find("unwind label %cxx.catch.dispatch.0"), std::string::npos);

  auto Plan = planCOFFExceptionPatch(*Mod, Image, Arch::X64);
  ASSERT_TRUE(static_cast<bool>(Plan)) << llvm::toString(Plan.takeError());
}

} // namespace
