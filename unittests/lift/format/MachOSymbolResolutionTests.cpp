//===- MachOSymbolResolutionTests.cpp - Mach-O patch resolver tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachOSymbolResolution.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

using namespace neverd;
using namespace neverd::macho_patch_detail;

namespace {

constexpr uint64_t StubVA = 0x1010;
constexpr uint64_t SlotVA = 0x2010;
constexpr uint64_t SlotContents = 0xfeedfacecafebeefULL;

llvm::mc_rewrite::RewriteSymbolResolveRequest
makeRequest(uint32_t Specifier = 0, llvm::StringRef SpecifierName = {}) {
  llvm::mc_rewrite::RewriteSymbolResolveRequest Request;
  Request.Symbol = "_callee";
  Request.Specifier = Specifier;
  Request.SpecifierName = SpecifierName;
  Request.FixupKindName = "FK_Data_8";
  Request.SectionName = "__text";
  Request.BitWidth = 64;
  return Request;
}

BinaryImage makeStubAndSlotImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = 0x1000;
  Text.Size = 0x100;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));

  Segment Data;
  Data.Name = "__DATA_CONST";
  Data.VA = 0x2000;
  Data.Size = 0x100;
  Data.FileSz = Data.Size;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Data.Size);
  llvm::support::endian::write64le(Data.Data.data() + SlotVA - Data.VA,
                                   SlotContents);
  Image.Segments.push_back(std::move(Data));

  Section Stubs;
  Stubs.Name = "__stubs";
  Stubs.SegmentName = "__TEXT";
  Stubs.VA = 0x1000;
  Stubs.Size = 0x40;
  Stubs.FileSz = Stubs.Size;
  Stubs.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Stubs.Type = static_cast<uint32_t>(llvm::MachO::S_SYMBOL_STUBS) |
               static_cast<uint32_t>(llvm::MachO::S_ATTR_PURE_INSTRUCTIONS);
  Image.Sections.push_back(std::move(Stubs));

  Section Got;
  Got.Name = "__got";
  Got.SegmentName = "__DATA_CONST";
  Got.VA = 0x2000;
  Got.Size = 0x40;
  Got.FileSz = Got.Size;
  Got.Flags = SegmentFlags::Readable;
  Got.Type = llvm::MachO::S_NON_LAZY_SYMBOL_POINTERS;
  Image.Sections.push_back(std::move(Got));

  Import Imported;
  Imported.Name = "_callee";
  Imported.IATAddr = StubVA;
  Image.Imports.push_back(std::move(Imported));
  Image.ImportStubIndices.emplace(StubVA, 0);
  Image.ImportPtrSlots.emplace(SlotVA, "_callee");
  return Image;
}

BinaryImage makePageZeroImage() {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = BinaryFormat::MachO;
  Image.Bits = Bitness::Bits64;

  Segment PageZero;
  PageZero.Name = "__PAGEZERO";
  PageZero.VA = 0;
  PageZero.Size = 0x100000000ULL;
  PageZero.Flags = SegmentFlags::None;
  Image.Segments.push_back(std::move(PageZero));

  Segment Text;
  Text.Name = "__TEXT";
  Text.VA = 0x100000000ULL;
  Text.Size = 0x1000;
  Text.FileSz = Text.Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  Image.Segments.push_back(std::move(Text));
  return Image;
}

TEST(MachOSymbolResolution, ClassifiesArchitectureSpecificSpecifiers) {
  auto X64Direct = classifyMachOSymbolUse(Arch::X64, makeRequest());
  ASSERT_TRUE(static_cast<bool>(X64Direct));
  EXPECT_EQ(*X64Direct, MachOSymbolUse::Direct);

  auto X64GOT = classifyMachOSymbolUse(Arch::X64, makeRequest(11, "GOTPCREL"));
  ASSERT_TRUE(static_cast<bool>(X64GOT));
  EXPECT_EQ(*X64GOT, MachOSymbolUse::ImportSlot);

  auto X64GOTNoRelax =
      classifyMachOSymbolUse(Arch::X64, makeRequest(12, "GOTPCREL_NORELAX"));
  ASSERT_TRUE(static_cast<bool>(X64GOTNoRelax));
  EXPECT_EQ(*X64GOTNoRelax, MachOSymbolUse::ImportSlot);

  auto A64Page =
      classifyMachOSymbolUse(Arch::AArch64, makeRequest(0x406, "PAGE"));
  ASSERT_TRUE(static_cast<bool>(A64Page));
  EXPECT_EQ(*A64Page, MachOSymbolUse::Direct);

  auto A64GOTPage =
      classifyMachOSymbolUse(Arch::AArch64, makeRequest(0x404, "GOTPAGE"));
  ASSERT_TRUE(static_cast<bool>(A64GOTPage));
  EXPECT_EQ(*A64GOTPage, MachOSymbolUse::ImportSlot);

  auto ARMUpper = classifyMachOSymbolUse(Arch::ARM, makeRequest(4));
  ASSERT_TRUE(static_cast<bool>(ARMUpper));
  EXPECT_EQ(*ARMUpper, MachOSymbolUse::Direct);

  auto Branch = makeRequest();
  Branch.FixupKindName = "fixup_aarch64_pcrel_call26";
  Branch.IsPCRel = true;
  auto A64Call = classifyMachOSymbolUse(Arch::AArch64, Branch);
  ASSERT_TRUE(static_cast<bool>(A64Call));
  EXPECT_EQ(*A64Call, MachOSymbolUse::Callable);

  auto X64PLT = classifyMachOSymbolUse(Arch::X64, makeRequest(18, "PLT"));
  ASSERT_TRUE(static_cast<bool>(X64PLT));
  EXPECT_EQ(*X64PLT, MachOSymbolUse::Callable);

  auto ARMGOT = classifyMachOSymbolUse(Arch::ARM, makeRequest(18, "GOT_PREL"));
  ASSERT_TRUE(static_cast<bool>(ARMGOT));
  EXPECT_EQ(*ARMGOT, MachOSymbolUse::ImportSlot);
}

TEST(MachOSymbolResolution, RejectsTLSAuthenticatedAndUnknownSpecifiers) {
  auto X64TLS = classifyMachOSymbolUse(Arch::X64, makeRequest(26, "TLVP"));
  ASSERT_FALSE(static_cast<bool>(X64TLS));
  llvm::consumeError(X64TLS.takeError());

  auto A64TLS =
      classifyMachOSymbolUse(Arch::AArch64, makeRequest(0x409, "TLVPPAGE"));
  ASSERT_FALSE(static_cast<bool>(A64TLS));
  llvm::consumeError(A64TLS.takeError());

  auto A64Authenticated =
      classifyMachOSymbolUse(Arch::AArch64, makeRequest(0x00a));
  ASSERT_FALSE(static_cast<bool>(A64Authenticated));
  llvm::consumeError(A64Authenticated.takeError());

  auto A64AuthenticatedPage =
      classifyMachOSymbolUse(Arch::AArch64, makeRequest(0x01c));
  ASSERT_FALSE(static_cast<bool>(A64AuthenticatedPage));
  llvm::consumeError(A64AuthenticatedPage.takeError());

  auto A64CombinedTLS =
      classifyMachOSymbolUse(Arch::AArch64, makeRequest(0x127));
  ASSERT_FALSE(static_cast<bool>(A64CombinedTLS));
  llvm::consumeError(A64CombinedTLS.takeError());

  auto X64Unknown = classifyMachOSymbolUse(Arch::X64, makeRequest(0xffff));
  ASSERT_FALSE(static_cast<bool>(X64Unknown));
  llvm::consumeError(X64Unknown.takeError());

  auto Subtractive = makeRequest();
  Subtractive.IsSubtrahend = true;
  auto X64Subtractive = classifyMachOSymbolUse(Arch::X64, Subtractive);
  ASSERT_FALSE(static_cast<bool>(X64Subtractive));
  llvm::consumeError(X64Subtractive.takeError());
}

TEST(MachOSymbolResolution, CompactUnwindPersonalityUsesImportSlotContext) {
  auto A64Personality = makeRequest();
  A64Personality.SectionName = "__compact_unwind";
  A64Personality.SectionOffset = 16;
  auto A64Use = classifyMachOSymbolUse(Arch::AArch64, A64Personality);
  ASSERT_TRUE(static_cast<bool>(A64Use));
  EXPECT_EQ(*A64Use, MachOSymbolUse::ImportSlot);

  auto ARM32Personality = makeRequest();
  ARM32Personality.FixupKindName = "FK_Data_4";
  ARM32Personality.SectionName = "__compact_unwind";
  ARM32Personality.SectionOffset = 12;
  ARM32Personality.BitWidth = 32;
  auto ARM32Use = classifyMachOSymbolUse(Arch::ARM, ARM32Personality);
  ASSERT_TRUE(static_cast<bool>(ARM32Use));
  EXPECT_EQ(*ARM32Use, MachOSymbolUse::ImportSlot);

  A64Personality.SectionOffset = 24;
  auto InvalidField = classifyMachOSymbolUse(Arch::AArch64, A64Personality);
  ASSERT_FALSE(static_cast<bool>(InvalidField));
  llvm::consumeError(InvalidField.takeError());

  auto DisguisedInvalidField = A64Personality;
  DisguisedInvalidField.Specifier = 0x404;
  DisguisedInvalidField.SpecifierName = "GOTPAGE";
  auto DisguisedUse =
      classifyMachOSymbolUse(Arch::AArch64, DisguisedInvalidField);
  ASSERT_FALSE(static_cast<bool>(DisguisedUse));
  llvm::consumeError(DisguisedUse.takeError());
}

TEST(MachOSymbolResolution, DirectReferenceSelectsStubNotPointerSlot) {
  BinaryImage Image = makeStubAndSlotImage();
  auto Target =
      resolveUniqueMachOSymbol(Image, "_callee", MachOSymbolUse::Direct);
  ASSERT_TRUE(static_cast<bool>(Target));
  ASSERT_TRUE(Target->has_value());
  EXPECT_EQ((*Target)->Address, StubVA);
  EXPECT_EQ((*Target)->Kind, MachOSymbolTargetKind::Callable);
}

TEST(MachOSymbolResolution, GOTReferenceReturnsSlotAddressWithoutDereference) {
  BinaryImage Image = makeStubAndSlotImage();
  auto Target =
      resolveUniqueMachOSymbol(Image, "_callee", MachOSymbolUse::ImportSlot);
  ASSERT_TRUE(static_cast<bool>(Target));
  ASSERT_TRUE(Target->has_value());
  EXPECT_EQ((*Target)->Address, SlotVA);
  EXPECT_NE((*Target)->Address, SlotContents);
  EXPECT_EQ((*Target)->Kind, MachOSymbolTargetKind::ImportSlot);
}

TEST(MachOSymbolResolution, SlotOnlyPersonalityResolvesWithoutCallableTarget) {
  BinaryImage Image = makeStubAndSlotImage();
  Image.Imports.clear();
  Image.ImportStubIndices.clear();

  auto Target =
      resolveUniqueMachOSymbol(Image, "_callee", MachOSymbolUse::ImportSlot);
  ASSERT_TRUE(static_cast<bool>(Target));
  ASSERT_TRUE(Target->has_value());
  EXPECT_EQ((*Target)->Address, SlotVA);
  EXPECT_EQ((*Target)->Kind, MachOSymbolTargetKind::ImportSlot);
}

TEST(MachOSymbolResolution, RejectsSlotOutsideNonLazyPointerSection) {
  BinaryImage Image = makeStubAndSlotImage();
  ASSERT_EQ(Image.Sections.back().Name, "__got");
  Image.Sections.back().Type = llvm::MachO::S_LAZY_SYMBOL_POINTERS;

  auto Target =
      resolveUniqueMachOSymbol(Image, "_callee", MachOSymbolUse::ImportSlot);
  ASSERT_FALSE(static_cast<bool>(Target));
  llvm::consumeError(Target.takeError());
}

TEST(MachOSymbolResolution, DirectReferenceSelectsMappedData) {
  BinaryImage Image = makeStubAndSlotImage();
  Symbol Data;
  Data.Name = "_global";
  Data.Addr = 0x2020;
  Image.Symbols.push_back(std::move(Data));

  auto Target =
      resolveUniqueMachOSymbol(Image, "_global", MachOSymbolUse::Direct);
  ASSERT_TRUE(static_cast<bool>(Target));
  ASSERT_TRUE(Target->has_value());
  EXPECT_EQ((*Target)->Address, 0x2020u);
  EXPECT_EQ((*Target)->Kind, MachOSymbolTargetKind::Data);
}

TEST(MachOSymbolResolution, CallableReferenceRejectsMappedData) {
  BinaryImage Image = makeStubAndSlotImage();
  Symbol Data;
  Data.Name = "_global";
  Data.Addr = 0x2020;
  Image.Symbols.push_back(std::move(Data));

  auto Target =
      resolveUniqueMachOSymbol(Image, "_global", MachOSymbolUse::Callable);
  ASSERT_FALSE(static_cast<bool>(Target));
  llvm::consumeError(Target.takeError());
}

TEST(MachOSymbolResolution, AmbiguousSelectedCandidateSetFailsClosed) {
  BinaryImage Image = makeStubAndSlotImage();
  Image.ImportPtrSlots.emplace(0x2020, "_callee");

  auto Target =
      resolveUniqueMachOSymbol(Image, "_callee", MachOSymbolUse::ImportSlot);
  ASSERT_FALSE(static_cast<bool>(Target));
  llvm::consumeError(Target.takeError());
}

TEST(MachOSymbolResolution,
     PatchSymbolizationRejectsIntToPtrInsideUnreadablePageZero) {
  llvm::LLVMContext Context;
  llvm::Module Module("pagezero-symbolization", Context);
  llvm::IRBuilder<> Builder(Context);

  auto *CalleeTy = llvm::FunctionType::get(
      Builder.getVoidTy(), {Builder.getPtrTy()}, /*isVarArg=*/false);
  auto *Callee = llvm::Function::Create(
      CalleeTy, llvm::GlobalValue::ExternalLinkage, "consume", Module);
  auto *CallerTy =
      llvm::FunctionType::get(Builder.getVoidTy(), /*isVarArg=*/false);
  auto *Caller = llvm::Function::Create(
      CallerTy, llvm::GlobalValue::ExternalLinkage, "caller", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Caller);
  Builder.SetInsertPoint(Entry);
  llvm::Constant *SmallPointer = llvm::ConstantExpr::getIntToPtr(
      llvm::ConstantInt::get(Builder.getInt64Ty(), 3), Builder.getPtrTy());
  llvm::CallInst *Call = Builder.CreateCall(CalleeTy, Callee, {SmallPointer});
  Builder.CreateRetVoid();

  BinaryImage Image = makePageZeroImage();
  ASSERT_TRUE(Image.containsVA(3));
  ASSERT_FALSE(Image.getSegmentFor(3)->isReadable());

  symbolizeImageAbsolutePointers(Module, Image);

  EXPECT_EQ(Module.getNamedGlobal(makeNdDataSymbol(3)), nullptr);
  auto *Preserved = llvm::dyn_cast<llvm::ConstantExpr>(Call->getArgOperand(0));
  ASSERT_NE(Preserved, nullptr);
  EXPECT_EQ(Preserved->getOpcode(), llvm::Instruction::IntToPtr);
}

TEST(MachOSymbolResolution, PatchSymbolizationCancelsSelectedFrameReloadBase) {
  constexpr uint64_t ImageBase = 0x100000000ULL;
  constexpr uint64_t FirstString = ImageBase + 0x100;
  constexpr uint64_t SecondString = ImageBase + 0x200;

  llvm::LLVMContext Context;
  llvm::Module Module("selected-frame-reload-symbolization", Context);
  llvm::IRBuilder<> Builder(Context);
  auto *CallerTy = llvm::FunctionType::get(
      Builder.getInt64Ty(), {Builder.getInt1Ty()}, /*isVarArg=*/false);
  auto *Caller = llvm::Function::Create(
      CallerTy, llvm::GlobalValue::ExternalLinkage, "caller", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Caller);
  Builder.SetInsertPoint(Entry);

  llvm::Value *Slot = Builder.CreateAlloca(Builder.getInt64Ty());
  llvm::Value *Selected = Builder.CreateSelect(
      Caller->getArg(0),
      llvm::ConstantInt::get(Builder.getInt64Ty(), FirstString),
      llvm::ConstantInt::get(Builder.getInt64Ty(), SecondString));
  Builder.CreateStore(Selected, Slot);
  llvm::Value *Reloaded = Builder.CreateLoad(Builder.getInt64Ty(), Slot);
  auto *Offset = llvm::cast<llvm::BinaryOperator>(Builder.CreateSub(
      Reloaded, llvm::ConstantInt::get(Builder.getInt64Ty(), ImageBase)));
  Builder.CreateRet(Offset);

  auto *MixedCaller = llvm::Function::Create(
      CallerTy, llvm::GlobalValue::ExternalLinkage, "mixed_caller", Module);
  auto *MixedEntry = llvm::BasicBlock::Create(Context, "entry", MixedCaller);
  Builder.SetInsertPoint(MixedEntry);
  llvm::Value *MixedSlot = Builder.CreateAlloca(Builder.getInt64Ty());
  llvm::Value *MixedSelected = Builder.CreateSelect(
      MixedCaller->getArg(0),
      llvm::ConstantInt::get(Builder.getInt64Ty(), FirstString),
      llvm::ConstantInt::get(Builder.getInt64Ty(), 7));
  Builder.CreateStore(MixedSelected, MixedSlot);
  llvm::Value *MixedReloaded =
      Builder.CreateLoad(Builder.getInt64Ty(), MixedSlot);
  auto *MixedOffset = llvm::cast<llvm::BinaryOperator>(Builder.CreateSub(
      MixedReloaded, llvm::ConstantInt::get(Builder.getInt64Ty(), ImageBase)));
  Builder.CreateRet(MixedOffset);

  BinaryImage Image = makePageZeroImage();
  symbolizeImageAbsolutePointers(Module, Image);

  auto *SelectedInst = llvm::cast<llvm::SelectInst>(Selected);
  for (unsigned Arm : {1u, 2u}) {
    auto *RelocatedArm =
        llvm::dyn_cast<llvm::ConstantExpr>(SelectedInst->getOperand(Arm));
    ASSERT_NE(RelocatedArm, nullptr);
    EXPECT_EQ(RelocatedArm->getOpcode(), llvm::Instruction::PtrToInt);
  }

  auto *RelocatedBase =
      llvm::dyn_cast<llvm::ConstantExpr>(Offset->getOperand(1));
  ASSERT_NE(RelocatedBase, nullptr)
      << "a selected pointer reloaded from a frame slot must cancel a "
         "relocatable base, not the original image VA";
  ASSERT_EQ(RelocatedBase->getOpcode(), llvm::Instruction::PtrToInt);
  auto *BaseGlobal =
      llvm::dyn_cast<llvm::GlobalVariable>(RelocatedBase->getOperand(0));
  ASSERT_NE(BaseGlobal, nullptr);
  EXPECT_EQ(BaseGlobal->getName(), makeNdDataSymbol(ImageBase));

  EXPECT_TRUE(llvm::isa<llvm::ConstantInt>(MixedOffset->getOperand(1)))
      << "a pointer/scalar SELECT must not prove a uniformly relocatable "
         "minuend";
}

TEST(MachOSymbolResolution, PatchSymbolizationCancelsPhiFrameReloadBase) {
  constexpr uint64_t ImageBase = 0x100000000ULL;
  constexpr uint64_t FirstString = ImageBase + 0x100;
  constexpr uint64_t SecondString = ImageBase + 0x200;

  llvm::LLVMContext Context;
  llvm::Module Module("phi-frame-reload-symbolization", Context);
  llvm::IRBuilder<> Builder(Context);
  auto *CallerTy = llvm::FunctionType::get(
      Builder.getInt64Ty(), {Builder.getInt1Ty()}, /*isVarArg=*/false);
  auto *Caller = llvm::Function::Create(
      CallerTy, llvm::GlobalValue::ExternalLinkage, "caller", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Caller);
  auto *Left = llvm::BasicBlock::Create(Context, "left", Caller);
  auto *Right = llvm::BasicBlock::Create(Context, "right", Caller);
  auto *Merge = llvm::BasicBlock::Create(Context, "merge", Caller);

  Builder.SetInsertPoint(Entry);
  llvm::Value *Slot = Builder.CreateAlloca(Builder.getInt64Ty());
  Builder.CreateCondBr(Caller->getArg(0), Left, Right);
  Builder.SetInsertPoint(Left);
  Builder.CreateBr(Merge);
  Builder.SetInsertPoint(Right);
  Builder.CreateBr(Merge);
  Builder.SetInsertPoint(Merge);
  auto *Phi = Builder.CreatePHI(Builder.getInt64Ty(), 2);
  Phi->addIncoming(llvm::ConstantInt::get(Builder.getInt64Ty(), FirstString),
                   Left);
  Phi->addIncoming(llvm::ConstantInt::get(Builder.getInt64Ty(), SecondString),
                   Right);
  Builder.CreateStore(Phi, Slot);
  llvm::Value *Reloaded = Builder.CreateLoad(Builder.getInt64Ty(), Slot);
  auto *Offset = llvm::cast<llvm::BinaryOperator>(Builder.CreateSub(
      Reloaded, llvm::ConstantInt::get(Builder.getInt64Ty(), ImageBase)));
  Builder.CreateRet(Offset);

  BinaryImage Image = makePageZeroImage();
  symbolizeImageAbsolutePointers(Module, Image);

  for (unsigned Incoming = 0; Incoming < Phi->getNumIncomingValues();
       ++Incoming) {
    auto *RelocatedIncoming =
        llvm::dyn_cast<llvm::ConstantExpr>(Phi->getIncomingValue(Incoming));
    ASSERT_NE(RelocatedIncoming, nullptr);
    EXPECT_EQ(RelocatedIncoming->getOpcode(), llvm::Instruction::PtrToInt);
  }
  auto *RelocatedBase =
      llvm::dyn_cast<llvm::ConstantExpr>(Offset->getOperand(1));
  ASSERT_NE(RelocatedBase, nullptr);
  ASSERT_EQ(RelocatedBase->getOpcode(), llvm::Instruction::PtrToInt);
  auto *BaseGlobal =
      llvm::dyn_cast<llvm::GlobalVariable>(RelocatedBase->getOperand(0));
  ASSERT_NE(BaseGlobal, nullptr);
  EXPECT_EQ(BaseGlobal->getName(), makeNdDataSymbol(ImageBase));
}

} // namespace
