//===- SafetyCallsiteMetadataTests.cpp - Persistent callsite identity -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/LanguageEHMetadata.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/SafetyCallsiteMetadata.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

namespace {

using namespace neverd;

constexpr va_t FunctionVA = 0x1000;
constexpr va_t CallVA = 0x1010;
constexpr va_t CalleeVA = 0x2000;
constexpr int BlockId = 7;
constexpr int OpIdx = 0;
constexpr int OriginSeq = 13;
constexpr uint32_t CallSiteId = 17;

MedFunc makeDirectCallFunction(llvm::StringRef FunctionName,
                               llvm::StringRef CalleeName,
                               unsigned ArgumentCount = 3) {
  MedFunc Func;
  Func.Entry = FunctionVA;
  Func.Name = FunctionName.str();
  Func.ReturnType = NdType::makeVoid();

  MedBlock Block;
  Block.Id = BlockId;
  Block.StartAddr = FunctionVA;
  Block.EndAddr = FunctionVA + 0x20;

  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = CallVA;
  Call.OriginSeq = OriginSeq;
  Call.CallSiteId = CallSiteId;
  Call.addInput(MedVar::makeConst(CalleeVA, 8));
  Block.Ops.push_back(Call);

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = CallVA + 4;
  Return.OriginSeq = OriginSeq + 1;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  MedCallInfo Info;
  Info.BlockId = BlockId;
  Info.OpIdx = OpIdx;
  Info.TargetAddr = CalleeVA;
  Info.TargetName = CalleeName.str();
  Info.Args = {MedVar::makeConst(0x3000, 8), MedVar::makeConst(0x4000, 8),
               MedVar::makeConst(5, 8), MedVar::makeConst(0x100, 8)};
  Info.Args.resize(ArgumentCount);
  Func.CallInfos.push_back(std::move(Info));
  return Func;
}

llvm::CallBase *findCallTo(llvm::Module &Module, llvm::StringRef FunctionName,
                           llvm::StringRef CalleeName) {
  llvm::Function *Function = Module.getFunction(FunctionName);
  if (!Function)
    return nullptr;
  for (llvm::BasicBlock &Block : *Function)
    for (llvm::Instruction &Instruction : Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
        if (const llvm::Function *Callee = Call->getCalledFunction())
          if (Callee->getName() == CalleeName)
            return Call;
  return nullptr;
}

llvm::CallBase *findIntrinsicCall(llvm::Module &Module,
                                  llvm::StringRef FunctionName,
                                  llvm::Intrinsic::ID IntrinsicID) {
  llvm::Function *Function = Module.getFunction(FunctionName);
  if (!Function)
    return nullptr;
  for (llvm::BasicBlock &Block : *Function)
    for (llvm::Instruction &Instruction : Block)
      if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
        if (const llvm::Function *Callee = Call->getCalledFunction())
          if (Callee->getIntrinsicID() == IntrinsicID)
            return Call;
  return nullptr;
}

safety_callsite_md::SafetyCallsiteRecord
expectedRecord(safety_callsite_md::SemanticKind Kind,
               uint32_t DestinationOperandIndex, uint32_t LengthOperandIndex,
               uint32_t ElementBytes = 1) {
  safety_callsite_md::SafetyCallsiteRecord Record;
  Record.Occurrence = {FunctionVA, CallVA,    BlockId,
                       OpIdx,      OriginSeq, CallSiteId};
  Record.Kind = Kind;
  Record.DestinationOperandIndex = DestinationOperandIndex;
  Record.LengthOperandIndex = LengthOperandIndex;
  Record.ElementBytes = ElementBytes;
  return Record;
}

void expectRecordEquals(
    const llvm::CallBase &Call,
    const safety_callsite_md::SafetyCallsiteRecord &Expected) {
  auto Parsed = safety_callsite_md::parse(Call);
  if (!Parsed)
    FAIL() << llvm::toString(Parsed.takeError());
  ASSERT_TRUE(*Parsed);
  EXPECT_EQ(**Parsed, Expected);
}

llvm::CallInst *makeBareCall(llvm::Module &Module, unsigned ArgumentCount = 3) {
  llvm::LLVMContext &Context = Module.getContext();
  std::vector<llvm::Type *> Params(ArgumentCount,
                                   llvm::Type::getInt64Ty(Context));
  auto *CalleeType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), Params, false);
  auto *Callee = llvm::Function::Create(
      CalleeType, llvm::GlobalValue::ExternalLinkage, "counted_write", Module);
  auto *CallerType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Caller = llvm::Function::Create(
      CallerType, llvm::GlobalValue::ExternalLinkage, "caller", Module);
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, "entry", Caller));
  std::vector<llvm::Value *> Args(
      ArgumentCount,
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context), 0));
  llvm::CallInst *Call = Builder.CreateCall(Callee, Args);
  Builder.CreateRetVoid();
  return Call;
}

llvm::Metadata *metadataUInt(llvm::LLVMContext &Context, uint64_t Value,
                             unsigned Width) {
  return llvm::ConstantAsMetadata::get(
      llvm::ConstantInt::get(llvm::IntegerType::get(Context, Width), Value));
}

llvm::SmallVector<llvm::Metadata *, safety_callsite_md::OperandCount>
validRawTuple(llvm::LLVMContext &Context) {
  const auto Record =
      expectedRecord(safety_callsite_md::SemanticKind::Memcpy, 0, 2);
  return {
      metadataUInt(Context, safety_callsite_md::SchemaVersion, 32),
      metadataUInt(Context, Record.Occurrence.FuncEntry, 64),
      metadataUInt(Context, Record.Occurrence.CallVA, 64),
      metadataUInt(Context, Record.Occurrence.BlockId, 32),
      metadataUInt(Context, Record.Occurrence.OpIdx, 32),
      metadataUInt(Context, Record.Occurrence.OriginSeq, 32),
      metadataUInt(Context, Record.Occurrence.CallSiteId, 32),
      metadataUInt(Context, static_cast<uint32_t>(Record.Kind), 32),
      metadataUInt(Context, Record.DestinationOperandIndex, 32),
      metadataUInt(Context, Record.LengthOperandIndex, 32),
      metadataUInt(Context, Record.ElementBytes, 32),
  };
}

TEST(SafetyCallsiteMetadata, OrdinaryMemcpyCarriesPersistentIdentity) {
  MedFunc Func = makeDirectCallFunction("metadata_memcpy", "memcpy");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, "metadata-memcpy",
                                      Arch::X64, {{CalleeVA, "memcpy"}});
  ASSERT_NE(Module, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  ASSERT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;

  llvm::CallBase *Call = findCallTo(*Module, Func.Name, "memcpy");
  ASSERT_NE(Call, nullptr);
  expectRecordEquals(*Call,
                     expectedRecord(safety_callsite_md::SemanticKind::Memcpy,
                                    /*DestinationOperandIndex=*/0,
                                    /*LengthOperandIndex=*/2));
}

TEST(SafetyCallsiteMetadata, OrdinaryMemmoveCarriesPersistentIdentity) {
  MedFunc Func = makeDirectCallFunction("metadata_memmove", "memmove");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, "metadata-memmove",
                                      Arch::X64, {{CalleeVA, "memmove"}});
  ASSERT_NE(Module, nullptr);

  llvm::CallBase *Call = findCallTo(*Module, Func.Name, "memmove");
  ASSERT_NE(Call, nullptr);
  expectRecordEquals(*Call,
                     expectedRecord(safety_callsite_md::SemanticKind::Memmove,
                                    /*DestinationOperandIndex=*/0,
                                    /*LengthOperandIndex=*/2));
}

TEST(SafetyCallsiteMetadata, BzeroUsesItsEmittedLengthOperand) {
  MedFunc Func = makeDirectCallFunction("metadata_bzero", "bzero", 2);
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, "metadata-bzero",
                                      Arch::X64, {{CalleeVA, "bzero"}});
  ASSERT_NE(Module, nullptr);

  llvm::CallBase *Call = findCallTo(*Module, Func.Name, "bzero");
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->arg_size(), 2u);
  expectRecordEquals(*Call,
                     expectedRecord(safety_callsite_md::SemanticKind::Bzero,
                                    /*DestinationOperandIndex=*/0,
                                    /*LengthOperandIndex=*/1));
}

TEST(SafetyCallsiteMetadata, BcopyRecordsItsReversedDestinationOperand) {
  MedFunc Func = makeDirectCallFunction("metadata_bcopy", "bcopy");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, "metadata-bcopy",
                                      Arch::X64, {{CalleeVA, "bcopy"}});
  ASSERT_NE(Module, nullptr);

  llvm::CallBase *Call = findCallTo(*Module, Func.Name, "bcopy");
  ASSERT_NE(Call, nullptr);
  expectRecordEquals(*Call,
                     expectedRecord(safety_callsite_md::SemanticKind::Bcopy,
                                    /*DestinationOperandIndex=*/1,
                                    /*LengthOperandIndex=*/2));
}

TEST(SafetyCallsiteMetadata, WideMemcpyRecordsFormatElementBytes) {
  struct FormatCase {
    BinaryFormat Format;
    uint32_t ElementBytes;
    const char *ModuleName;
  };
  constexpr FormatCase Cases[] = {
      {BinaryFormat::COFF, 2, "metadata-wmemcpy-coff"},
      {BinaryFormat::ELF, 4, "metadata-wmemcpy-elf"},
      {BinaryFormat::MachO, 4, "metadata-wmemcpy-macho"},
  };
  for (const FormatCase &Case : Cases) {
    SCOPED_TRACE(Case.ModuleName);
    MedFunc Func = makeDirectCallFunction(Case.ModuleName, "wmemcpy");
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Func}, Context, Case.ModuleName, Arch::X64,
                              {{CalleeVA, "wmemcpy"}}, nullptr, Case.Format);
    ASSERT_NE(Module, nullptr);

    llvm::CallBase *Call = findCallTo(*Module, Func.Name, "wmemcpy");
    ASSERT_NE(Call, nullptr);
    expectRecordEquals(
        *Call, expectedRecord(safety_callsite_md::SemanticKind::Memcpy,
                              /*DestinationOperandIndex=*/0,
                              /*LengthOperandIndex=*/2, Case.ElementBytes));
  }
}

TEST(SafetyCallsiteMetadata,
     AuthoritativeAliasesUseTheirActualEmittedOperandLayout) {
  struct AliasCase {
    const char *Name;
    unsigned ArgumentCount;
    safety_callsite_md::SemanticKind Kind;
    uint32_t DestinationOperandIndex;
    uint32_t LengthOperandIndex;
    uint32_t ElementBytes;
  };
  constexpr AliasCase Cases[] = {
      {"__aeabi_memcpy4", 3, safety_callsite_md::SemanticKind::Memcpy, 0, 2, 1},
      {"__aeabi_memmove8", 3, safety_callsite_md::SemanticKind::Memmove, 0, 2,
       1},
      {"__aeabi_memset4", 3, safety_callsite_md::SemanticKind::Memset, 0, 1, 1},
      {"__aeabi_memclr8", 2, safety_callsite_md::SemanticKind::Bzero, 0, 1, 1},
      {"__memcpy_chk", 4, safety_callsite_md::SemanticKind::Memcpy, 0, 2, 1},
      {"__memmove_chk", 4, safety_callsite_md::SemanticKind::Memmove, 0, 2, 1},
      {"wmemmove", 3, safety_callsite_md::SemanticKind::Memmove, 0, 2, 4},
  };
  for (const AliasCase &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    const std::string FunctionName = std::string("metadata_alias_") + Case.Name;
    MedFunc Func =
        makeDirectCallFunction(FunctionName, Case.Name, Case.ArgumentCount);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Func}, Context, FunctionName,
                                        Arch::X64, {{CalleeVA, Case.Name}},
                                        nullptr, BinaryFormat::ELF);
    ASSERT_NE(Module, nullptr);

    llvm::CallBase *Call = findCallTo(*Module, Func.Name, Case.Name);
    ASSERT_NE(Call, nullptr);
    ASSERT_EQ(Call->arg_size(), Case.ArgumentCount);
    expectRecordEquals(
        *Call, expectedRecord(Case.Kind, Case.DestinationOperandIndex,
                              Case.LengthOperandIndex, Case.ElementBytes));
  }
}

TEST(SafetyCallsiteMetadata,
     FortifiedMemsetKeepsLengthIndexAfterIntrinsicLowering) {
  MedFunc Func =
      makeDirectCallFunction("metadata_memset_chk", "__memset_chk", 4);
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, "metadata-memset-chk",
                                      Arch::X64, {{CalleeVA, "__memset_chk"}},
                                      nullptr, BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);

  llvm::CallBase *Call =
      findIntrinsicCall(*Module, Func.Name, llvm::Intrinsic::memset);
  ASSERT_NE(Call, nullptr);
  expectRecordEquals(*Call,
                     expectedRecord(safety_callsite_md::SemanticKind::Memset,
                                    /*DestinationOperandIndex=*/0,
                                    /*LengthOperandIndex=*/2));
}

TEST(SafetyCallsiteMetadata, EarlyLoweredMemsetCarriesPersistentIdentity) {
  MedFunc Func = makeDirectCallFunction("metadata_memset", "memset");
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, "metadata-memset",
                                      Arch::X64, {{CalleeVA, "memset"}});
  ASSERT_NE(Module, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  ASSERT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;

  llvm::CallBase *Call =
      findIntrinsicCall(*Module, Func.Name, llvm::Intrinsic::memset);
  ASSERT_NE(Call, nullptr);
  expectRecordEquals(*Call,
                     expectedRecord(safety_callsite_md::SemanticKind::Memset,
                                    /*DestinationOperandIndex=*/0,
                                    /*LengthOperandIndex=*/2));
}

TEST(SafetyCallsiteMetadata,
     PersistentIdentitySurvivesProtectedCallToInvokeLowering) {
  MedFunc Func = makeDirectCallFunction("metadata_invoke", "memcpy");
  constexpr va_t HandlerVA = FunctionVA + 0x40;
  MedBlock Handler;
  Handler.Id = BlockId + 1;
  Handler.StartAddr = HandlerVA;
  Handler.EndAddr = HandlerVA + 0x10;
  MedOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = HandlerVA;
  HandlerReturn.OriginSeq = OriginSeq + 2;
  Handler.Ops.push_back(HandlerReturn);
  Func.Blocks.push_back(std::move(Handler));

  ExceptionFunction EH;
  EH.CodeRange = {FunctionVA, HandlerVA + 0x10};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;
  ItaniumCallSite Site;
  Site.GuardedRange = {FunctionVA, FunctionVA + 0x20};
  Site.LandingPadVA = HandlerVA;
  EH.Itanium->CallSites.push_back(Site);
  Func.ExceptionMetadata = std::move(EH);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, "metadata-invoke",
                                      Arch::X64, {{CalleeVA, "memcpy"}});
  ASSERT_NE(Module, nullptr);
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  ASSERT_FALSE(llvm::verifyModule(*Module, &VerificationOS)) << Verification;

  llvm::CallBase *Call = findCallTo(*Module, Func.Name, "memcpy");
  ASSERT_NE(Call, nullptr);
  EXPECT_TRUE(llvm::isa<llvm::InvokeInst>(Call));
  expectRecordEquals(*Call,
                     expectedRecord(safety_callsite_md::SemanticKind::Memcpy,
                                    /*DestinationOperandIndex=*/0,
                                    /*LengthOperandIndex=*/2));
  EXPECT_EQ(Call->getMetadata(language_eh_md::InternalSourceCallAttachment),
            nullptr)
      << "transient EH identity must be cleaned without deleting persistent "
         "safety metadata";
}

TEST(SafetyCallsiteMetadata, ParseDistinguishesAbsentFromMalformedTuples) {
  llvm::LLVMContext Context;
  llvm::Module Module("metadata-parse", Context);
  llvm::CallInst *Call = makeBareCall(Module);

  auto Absent = safety_callsite_md::parse(*Call);
  ASSERT_TRUE(static_cast<bool>(Absent));
  EXPECT_FALSE(*Absent);

  auto ExpectMalformed = [&](auto Mutate) {
    auto Operands = validRawTuple(Context);
    Mutate(Operands);
    Call->setMetadata(safety_callsite_md::Attachment,
                      llvm::MDNode::get(Context, Operands));
    auto Parsed = safety_callsite_md::parse(*Call);
    EXPECT_FALSE(static_cast<bool>(Parsed));
    if (!Parsed)
      EXPECT_FALSE(llvm::toString(Parsed.takeError()).empty());
    Call->setMetadata(safety_callsite_md::Attachment, nullptr);
  };

  ExpectMalformed([](auto &Operands) { Operands.pop_back(); });
  ExpectMalformed([&](auto &Operands) {
    Operands[safety_callsite_md::Version] = metadataUInt(Context, 2, 32);
  });
  ExpectMalformed([&](auto &Operands) {
    Operands[safety_callsite_md::CallVA] = metadataUInt(Context, CallVA, 32);
  });
  ExpectMalformed([&](auto &Operands) {
    Operands[safety_callsite_md::Kind] = metadataUInt(Context, 99, 32);
  });
}

TEST(SafetyCallsiteMetadata,
     AttachIsTransactionalForIncompleteOrOverlappingRecords) {
  llvm::LLVMContext Context;
  llvm::Module Module("metadata-attach", Context);
  llvm::CallInst *Call = makeBareCall(Module);
  auto Record = expectedRecord(safety_callsite_md::SemanticKind::Memcpy, 0, 2);

  Record.Occurrence.CallSiteId = 0;
  llvm::Error Incomplete = safety_callsite_md::attach(*Call, Record);
  EXPECT_TRUE(static_cast<bool>(Incomplete));
  llvm::consumeError(std::move(Incomplete));
  EXPECT_EQ(Call->getMetadata(safety_callsite_md::Attachment), nullptr);

  Record = expectedRecord(safety_callsite_md::SemanticKind::Memcpy, 0, 2);
  Record.LengthOperandIndex = Record.DestinationOperandIndex;
  llvm::Error Overlap = safety_callsite_md::attach(*Call, Record);
  EXPECT_TRUE(static_cast<bool>(Overlap));
  llvm::consumeError(std::move(Overlap));
  EXPECT_EQ(Call->getMetadata(safety_callsite_md::Attachment), nullptr);
}

TEST(SafetyCallsiteMetadata,
     SyntheticOrIncompleteMedIROccurrencesAreNotPublished) {
  for (const bool Synthetic : {false, true}) {
    SCOPED_TRACE(Synthetic ? "synthetic-origin" : "missing-callsite-id");
    MedFunc Func = makeDirectCallFunction(
        Synthetic ? "metadata_synthetic" : "metadata_incomplete", "memcpy");
    MedOp &CallOp = Func.Blocks.front().Ops.front();
    if (Synthetic)
      CallOp.OriginSeq = -1;
    else
      CallOp.CallSiteId = 0;

    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::X64,
                                        {{CalleeVA, "memcpy"}});
    ASSERT_NE(Module, nullptr);
    llvm::CallBase *Call = findCallTo(*Module, Func.Name, "memcpy");
    ASSERT_NE(Call, nullptr);
    auto Parsed = safety_callsite_md::parse(*Call);
    ASSERT_TRUE(static_cast<bool>(Parsed));
    EXPECT_FALSE(*Parsed);
  }
}

} // namespace
