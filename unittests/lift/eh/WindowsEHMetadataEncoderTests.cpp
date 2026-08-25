//===- WindowsEHMetadataEncoderTests.cpp - Windows EH metadata tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace neverd;

constexpr llvm::StringLiteral RichSchemaV8Fingerprint(
    "34566d14a69607ce502bbd334730a7bca42fd96846ea4c71b6c3efbd8ccc11fa");

ExceptionFunction makeRichExceptionFunction() {
  ExceptionFunction EH;
  EH.CodeRange = {0x140001000, 0x140001234};
  EH.Kind = RuntimeFunctionKind::Chained;
  EH.Encoding = ExceptionEncoding::X64UnwindV2;
  EH.ParseStatus = ExceptionParseStatus::Partial;
  EH.RuntimeFunctionRVA = 0x10203040;
  EH.UnwindInfoRVA = 0x50607080;
  EH.UnwindInfoVA = 0x14000a0b0;
  EH.UnwindVersion = 2;
  EH.UnwindFlags = 0x1b;
  EH.PrologueSize = 0x2a;
  EH.FrameRegister = 13;
  EH.FrameOffset = 0x30;
  EH.PackedUnwindData = 0x89abcdef;
  EH.NativeUnwindBytes = {0x00, 0x7f, 0x80, 0xff};

  UnwindOperation Unwind;
  Unwind.Kind = UnwindOperationKind::SaveNonVolatileFar;
  Unwind.CodeOffset = 0x11223344;
  Unwind.OpInfo = 7;
  Unwind.SlotCount = 3;
  Unwind.Register = 13;
  Unwind.StackOffset = 0x123456789ULL;
  Unwind.RegisterClass = UnwindRegisterClass::GeneralPurpose;
  Unwind.RegisterMask = 0xa5a55a5a;
  Unwind.InstructionSize = 4;
  Unwind.OperandBytes = {0x12, 0x34, 0xab, 0xcd};
  EH.UnwindOperations.push_back(Unwind);

  UnwindEpilog Epilog;
  Epilog.StartOffset = -0x1122334455LL;
  Epilog.Flags = 0xa3;
  Epilog.FirstOperationOffset = 0x55667788;
  Epilog.LastInstructionOffset = 0x99aabbcc;
  UnwindOperation EpilogOp = Unwind;
  EpilogOp.Kind = UnwindOperationKind::LoadReturnAddress;
  EpilogOp.CodeOffset = 9;
  EpilogOp.OperandBytes = {0xde, 0xad};
  Epilog.Operations.push_back(std::move(EpilogOp));
  EH.Epilogs.push_back(std::move(Epilog));

  EH.PersonalityVA = 0x14000b0c0;
  EH.HandlerDataVA = 0x14000c0d0;
  EH.Personality = ExceptionPersonality::GSHandlerCheckEH4;
  EH.PersonalityName = "resolved!__GSHandlerCheck_EH4";

  SEHExceptionInfo SEH;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {0x140001020, 0x140001080};
  Scope.Kind = SEHScopeKind::Finally;
  Scope.FilterOrFinallyVA = 0x140001300;
  Scope.NormalizedFilterVA = 0x140001380;
  Scope.HandlerVA = 0x140001400;
  Scope.ContinuationVA = 0x140001500;
  Scope.ParseStatus = ExceptionParseStatus::Malformed;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);

  CxxExceptionInfo Cxx;
  Cxx.NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  Cxx.NativeFuncInfoVA = 0x14000c0e0;
  Cxx.Magic = 0x19930522;
  Cxx.Version = CxxFuncInfoVersion::WithExceptionSpecs;
  Cxx.Flags = 0xfedcba98;
  Cxx.MaxState = 2;
  Cxx.UnwindHelpOffset = -0x24;
  Cxx.ESTypeListVA = 0x14000d0e0;
  Cxx.ExceptionSpecTypes.push_back({0x76543210, 0x14000e0f0});
  Cxx.BBTFlags = 5;
  Cxx.FrameOffset = 0x38;
  Cxx.IsCatchFunclet = true;
  Cxx.IsSeparated = false;
  Cxx.IsSynchronous = true;
  Cxx.IsNoExcept = false;
  Cxx.HasDynamicStackAlignment = true;

  CxxUnwindAction FirstAction;
  FirstAction.ToState = -1;
  FirstAction.ActionVA = 0x140001600;
  FirstAction.Kind = CxxUnwindAction::ActionKind::DestructorWithObjectPointer;
  FirstAction.ObjectOffset = -0x44;
  Cxx.UnwindMap.push_back(FirstAction);
  CxxUnwindAction SecondAction;
  SecondAction.ToState = 0;
  SecondAction.ActionVA = 0x140001700;
  SecondAction.Kind = CxxUnwindAction::ActionKind::Direct;
  SecondAction.ObjectOffset = 0x48;
  Cxx.UnwindMap.push_back(SecondAction);

  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.Adjectives = 0xcafebabe;
  Catch.TypeDescriptorVA = 0x140001800;
  Catch.CatchObjectOffset = -0x4c;
  Catch.HandlerVA = 0x140001900;
  Catch.ParentFrameOffset = -0x50;
  Catch.ContinuationVAs = {0x140001a00, 0x140001b00};
  Try.Handlers.push_back(std::move(Catch));
  Cxx.TryBlocks.push_back(std::move(Try));
  Cxx.IPMap = {{0x140001010, -1}, {0x140001100, 1}};
  EH.Cxx = std::move(Cxx);

  GSCookieInfo GS;
  GS.ParseStatus = ExceptionParseStatus::Partial;
  GS.CookieOffset = -0x54;
  GS.HasExceptionHandler = true;
  GS.HasUnwindHandler = false;
  GS.HasAlignment = true;
  GS.AlignmentBaseOffset = -0x58;
  GS.Alignment = 0x40;
  GS.Payload = {0xfe, 0xed, 0xfa, 0xce};
  EH.GSCookie = std::move(GS);

  RegistrationChainInfo Registration;
  Registration.HandlerVA = 0x401020;
  Registration.ScopeTableVA = 0x403040;
  Registration.TryLevelOffset = -0x24;
  Registration.TryLevelStores = {{0x401080, 0x401086, 0},
                                 {0x4010a0, 0x4010a6, -2}};
  Registration.SeededTryLevel = -2;
  Registration.RegistrationOffset = -0x18;
  Registration.HasSecurityCookies = true;
  Registration.GSCookieOffset = -0x10;
  Registration.GSCookieXOROffset = 0x0c;
  Registration.EHCookieOffset = -0x0c;
  Registration.EHCookieXOROffset = 0x08;
  Registration.ScopeTableMagic = 0xfffffffe;
  Registration.Scopes = {{-1, 0x401200, 0x401220, false},
                         {0, 0, 0x401240, true}};
  Registration.ChainInstallVA = 0x401010;
  Registration.ChainRemoveVA = 0x4011f0;
  EH.Registration = std::move(Registration);

  EH.PrimaryFunctionIndex = 0x1234;
  EH.ChainedPrimaryRange = ExceptionAddressRange{0x140001000, 0x140001200};
  EH.ChainedUnwindInfoRVA = 0xabcdef01;
  EH.Diagnostics = {"first diagnostic", ""};

  ExceptionFunctionDecodeProvenance Provenance;
  Provenance.Structural.ParseStatus = ExceptionParseStatus::Malformed;
  Provenance.Structural.Diagnostics = {"not part of schema v8"};
  EH.DecodeProvenance = std::move(Provenance);
  return EH;
}

ExceptionFunction makeNativeSEHSource() {
  ExceptionFunction EH;
  EH.CodeRange = {0x140001000, 0x140001040};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PersonalityVA = 0x140002000;
  SEHScopeRecord Scope;
  Scope.GuardedRange = {0x140001000, 0x140001010};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = 0x140001020;
  Scope.ContinuationVA = Scope.HandlerVA;
  SEHExceptionInfo SEH;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);
  return EH;
}

ExceptionFunction makeNativeFH3Source() {
  ExceptionFunction EH;
  EH.CodeRange = {0x140001000, 0x140001040};
  EH.Encoding = ExceptionEncoding::X64UnwindV2;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.PersonalityVA = 0x140002000;

  CxxExceptionInfo Cxx;
  Cxx.Magic = 0x19930522;
  Cxx.Version = CxxFuncInfoVersion::WithEHFlags;
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
  Cxx.IPMap = {{EH.CodeRange.Begin, 0},
               {EH.CodeRange.Begin + 0x10, -1},
               {EH.CodeRange.Begin + 0x20, 1}};
  CxxTryBlock Try;
  Try.TryLow = 0;
  Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.HandlerVA = EH.CodeRange.Begin + 0x20;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(std::move(Try));
  EH.Cxx = std::move(Cxx);
  return EH;
}

ExceptionFunction makeNativeFH4Source() {
  ExceptionFunction EH = makeNativeFH3Source();
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.Personality = ExceptionPersonality::CxxFrameHandler4;

  CxxExceptionInfo &Cxx = *EH.Cxx;
  Cxx.NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  Cxx.Magic = 0;
  Cxx.Flags = 0x38;
  Cxx.UnwindMap[1].ToState = -1;
  Cxx.IPMap = {{EH.CodeRange.Begin, -1},
               {EH.CodeRange.Begin + 4, 0},
               {EH.CodeRange.Begin + 0x10, -1}};
  Cxx.TryBlocks.front().Handlers.front().Adjectives = 0x40;
  Cxx.TryBlocks.front().Handlers.front().TypeDescriptorVA = 0;
  return EH;
}

uint64_t metadataInteger(const llvm::MDNode &Node, unsigned Index,
                         unsigned Width) {
  const auto *Metadata =
      llvm::dyn_cast<llvm::ConstantAsMetadata>(Node.getOperand(Index).get());
  EXPECT_NE(Metadata, nullptr);
  const auto *Integer =
      Metadata ? llvm::dyn_cast<llvm::ConstantInt>(Metadata->getValue())
               : nullptr;
  EXPECT_NE(Integer, nullptr);
  if (!Integer)
    return 0;
  EXPECT_EQ(Integer->getBitWidth(), Width);
  return Integer->getZExtValue();
}

void appendFingerprint(const llvm::Metadata &Metadata, llvm::raw_ostream &OS,
                       llvm::SmallPtrSetImpl<const llvm::Metadata *> &Active) {
  if (const auto *String = llvm::dyn_cast<llvm::MDString>(&Metadata)) {
    OS << 's' << String->getString().size() << ':' << String->getString();
    return;
  }
  if (const auto *Constant =
          llvm::dyn_cast<llvm::ConstantAsMetadata>(&Metadata)) {
    const auto *Integer =
        llvm::dyn_cast<llvm::ConstantInt>(Constant->getValue());
    if (!Integer) {
      OS << "constant<?>";
      return;
    }
    OS << 'i' << Integer->getBitWidth() << ':';
    Integer->getValue().print(OS, /*IsSigned=*/false);
    return;
  }
  const auto *Node = llvm::dyn_cast<llvm::MDNode>(&Metadata);
  if (!Node) {
    OS << "metadata<?>";
    return;
  }
  if (!Active.insert(Node).second) {
    OS << "cycle";
    return;
  }
  OS << 'n' << Node->getNumOperands() << '[';
  for (unsigned I = 0; I != Node->getNumOperands(); ++I) {
    if (I != 0)
      OS << ',';
    const llvm::Metadata *Operand = Node->getOperand(I).get();
    if (!Operand)
      OS << "null";
    else
      appendFingerprint(*Operand, OS, Active);
  }
  OS << ']';
  Active.erase(Node);
}

std::string fingerprint(const llvm::Metadata &Metadata) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  llvm::SmallPtrSet<const llvm::Metadata *, 16> Active;
  appendFingerprint(Metadata, OS, Active);
  return Result;
}

std::string fingerprintDigest(const llvm::Metadata &Metadata) {
  const std::string Fingerprint = fingerprint(Metadata);
  const std::array<uint8_t, 32> Digest =
      llvm::SHA256::hash(llvm::arrayRefFromStringRef(Fingerprint));
  return llvm::toHex(llvm::ArrayRef<uint8_t>(Digest), /*LowerCase=*/true);
}

TEST(WindowsEHMetadataEncoder, PreservesSchemaV8Projection) {
  const ExceptionFunction EH = makeRichExceptionFunction();
  llvm::LLVMContext Context;
  llvm::MDNode *Payload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, EH);
  ASSERT_NE(Payload, nullptr);
  EXPECT_EQ(Payload->getNumOperands(), windows_eh_md::OperandCount);
  EXPECT_EQ(Payload, windows_eh_md::getCanonicalFunctionMetadata(Context, EH));
  const auto *CxxHeader = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::CxxHeader).get());
  ASSERT_NE(CxxHeader, nullptr);
  ASSERT_EQ(CxxHeader->getNumOperands(),
            windows_eh_md::CxxHeaderOperandCount);
  EXPECT_EQ(metadataInteger(*CxxHeader,
                            windows_eh_md::CxxNativeFuncInfoVA, 64),
            EH.Cxx->NativeFuncInfoVA);
  const auto *Scopes = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::SEHScopes).get());
  ASSERT_NE(Scopes, nullptr);
  ASSERT_EQ(Scopes->getNumOperands(), 1u);
  const auto *Scope =
      llvm::dyn_cast<llvm::MDNode>(Scopes->getOperand(0).get());
  ASSERT_NE(Scope, nullptr);
  ASSERT_EQ(Scope->getNumOperands(), windows_eh_md::SEHScopeOperandCount);
  EXPECT_EQ(metadataInteger(*Scope,
                            windows_eh_md::SEHScopeNormalizedFilterVA, 64),
            EH.SEH->Scopes.front().NormalizedFilterVA);
  EXPECT_EQ(fingerprintDigest(*Payload), RichSchemaV8Fingerprint);
}

TEST(WindowsEHMetadataEncoder, PreservesCompleteX86RegistrationChain) {
  const ExceptionFunction EH = makeRichExceptionFunction();
  llvm::LLVMContext Context;
  llvm::MDNode *Payload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, EH);
  ASSERT_NE(Payload, nullptr);

  const auto *Registration = llvm::dyn_cast<llvm::MDNode>(
      Payload->getOperand(windows_eh_md::Registration).get());
  ASSERT_NE(Registration, nullptr);
  ASSERT_EQ(Registration->getNumOperands(),
            windows_eh_md::RegistrationOperandCount);
  EXPECT_EQ(
      metadataInteger(*Registration, windows_eh_md::RegistrationHandlerVA, 64),
      0x401020u);
  EXPECT_EQ(metadataInteger(*Registration,
                            windows_eh_md::RegistrationScopeTableVA, 64),
            0x403040u);
  EXPECT_EQ(metadataInteger(*Registration,
                            windows_eh_md::RegistrationHasSecurityCookies, 1),
            1u);
  EXPECT_EQ(metadataInteger(*Registration,
                            windows_eh_md::RegistrationGSCookieOffset, 32),
            static_cast<uint32_t>(-0x10));
  EXPECT_EQ(metadataInteger(*Registration,
                            windows_eh_md::RegistrationGSCookieXOROffset, 32),
            0x0cu);
  EXPECT_EQ(metadataInteger(*Registration,
                            windows_eh_md::RegistrationEHCookieOffset, 32),
            static_cast<uint32_t>(-0x0c));
  EXPECT_EQ(metadataInteger(*Registration,
                            windows_eh_md::RegistrationEHCookieXOROffset, 32),
            0x08u);
  EXPECT_EQ(metadataInteger(*Registration,
                            windows_eh_md::RegistrationScopeTableMagic, 32),
            0xfffffffeu);
  EXPECT_EQ(metadataInteger(*Registration,
                            windows_eh_md::RegistrationChainInstallVA, 64),
            0x401010u);
  EXPECT_EQ(metadataInteger(*Registration,
                            windows_eh_md::RegistrationChainRemoveVA, 64),
            0x4011f0u);

  const auto *TryLevelOffset = llvm::dyn_cast<llvm::MDNode>(
      Registration->getOperand(windows_eh_md::RegistrationTryLevelOffset)
          .get());
  ASSERT_NE(TryLevelOffset, nullptr);
  ASSERT_EQ(TryLevelOffset->getNumOperands(), 1u);
  EXPECT_EQ(metadataInteger(*TryLevelOffset, 0, 32),
            static_cast<uint32_t>(-0x24));
  const auto *SeededTryLevel = llvm::dyn_cast<llvm::MDNode>(
      Registration->getOperand(windows_eh_md::RegistrationSeededTryLevel)
          .get());
  ASSERT_NE(SeededTryLevel, nullptr);
  ASSERT_EQ(SeededTryLevel->getNumOperands(), 1u);
  EXPECT_EQ(metadataInteger(*SeededTryLevel, 0, 32), static_cast<uint32_t>(-2));
  const auto *RegistrationOffset = llvm::dyn_cast<llvm::MDNode>(
      Registration->getOperand(windows_eh_md::RegistrationRecordOffset).get());
  ASSERT_NE(RegistrationOffset, nullptr);
  ASSERT_EQ(RegistrationOffset->getNumOperands(), 1u);
  EXPECT_EQ(metadataInteger(*RegistrationOffset, 0, 32),
            static_cast<uint32_t>(-0x18));

  const auto *Stores = llvm::dyn_cast<llvm::MDNode>(
      Registration->getOperand(windows_eh_md::RegistrationTryLevelStores)
          .get());
  ASSERT_NE(Stores, nullptr);
  ASSERT_EQ(Stores->getNumOperands(), 2u);
  const auto *FirstStore =
      llvm::dyn_cast<llvm::MDNode>(Stores->getOperand(0).get());
  ASSERT_NE(FirstStore, nullptr);
  ASSERT_EQ(FirstStore->getNumOperands(),
            windows_eh_md::RegistrationTryLevelStoreOperandCount);
  EXPECT_EQ(
      metadataInteger(*FirstStore, windows_eh_md::RegistrationStoreVA, 64),
      0x401080u);
  EXPECT_EQ(
      metadataInteger(*FirstStore, windows_eh_md::RegistrationStoreEndVA, 64),
      0x401086u);
  EXPECT_EQ(
      metadataInteger(*FirstStore, windows_eh_md::RegistrationStoreLevel, 32),
      0u);
  const auto *SecondStore =
      llvm::dyn_cast<llvm::MDNode>(Stores->getOperand(1).get());
  ASSERT_NE(SecondStore, nullptr);
  ASSERT_EQ(SecondStore->getNumOperands(),
            windows_eh_md::RegistrationTryLevelStoreOperandCount);
  EXPECT_EQ(
      metadataInteger(*SecondStore, windows_eh_md::RegistrationStoreVA, 64),
      0x4010a0u);
  EXPECT_EQ(
      metadataInteger(*SecondStore, windows_eh_md::RegistrationStoreEndVA, 64),
      0x4010a6u);
  EXPECT_EQ(
      metadataInteger(*SecondStore, windows_eh_md::RegistrationStoreLevel, 32),
      static_cast<uint32_t>(-2));

  const auto *Scopes = llvm::dyn_cast<llvm::MDNode>(
      Registration->getOperand(windows_eh_md::RegistrationScopes).get());
  ASSERT_NE(Scopes, nullptr);
  ASSERT_EQ(Scopes->getNumOperands(), 2u);
  const auto *ExceptScope =
      llvm::dyn_cast<llvm::MDNode>(Scopes->getOperand(0).get());
  ASSERT_NE(ExceptScope, nullptr);
  ASSERT_EQ(ExceptScope->getNumOperands(),
            windows_eh_md::RegistrationScopeOperandCount);
  EXPECT_EQ(metadataInteger(*ExceptScope,
                            windows_eh_md::RegistrationScopeEnclosingLevel, 32),
            static_cast<uint32_t>(-1));
  EXPECT_EQ(metadataInteger(*ExceptScope,
                            windows_eh_md::RegistrationScopeFilterVA, 64),
            0x401200u);
  EXPECT_EQ(metadataInteger(*ExceptScope,
                            windows_eh_md::RegistrationScopeHandlerVA, 64),
            0x401220u);
  EXPECT_EQ(metadataInteger(*ExceptScope,
                            windows_eh_md::RegistrationScopeIsFinally, 1),
            0u);
  const auto *FinallyScope =
      llvm::dyn_cast<llvm::MDNode>(Scopes->getOperand(1).get());
  ASSERT_NE(FinallyScope, nullptr);
  ASSERT_EQ(FinallyScope->getNumOperands(),
            windows_eh_md::RegistrationScopeOperandCount);
  EXPECT_EQ(metadataInteger(*FinallyScope,
                            windows_eh_md::RegistrationScopeFilterVA, 64),
            0u);
  EXPECT_EQ(metadataInteger(*FinallyScope,
                            windows_eh_md::RegistrationScopeHandlerVA, 64),
            0x401240u);
  EXPECT_EQ(metadataInteger(*FinallyScope,
                            windows_eh_md::RegistrationScopeIsFinally, 1),
            1u);
}

TEST(WindowsEHMetadataEncoder,
     DistinguishesAbsentRegistrationOptionalsFromExplicitZero) {
  llvm::LLVMContext Context;

  ExceptionFunction WithoutRegistration;
  llvm::MDNode *WithoutPayload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, WithoutRegistration);
  const auto *Without = llvm::dyn_cast<llvm::MDNode>(
      WithoutPayload->getOperand(windows_eh_md::Registration).get());
  ASSERT_NE(Without, nullptr);
  EXPECT_EQ(Without->getNumOperands(), 0u);

  ExceptionFunction AbsentOptionals;
  AbsentOptionals.Registration.emplace();
  llvm::MDNode *AbsentPayload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, AbsentOptionals);
  const auto *Absent = llvm::dyn_cast<llvm::MDNode>(
      AbsentPayload->getOperand(windows_eh_md::Registration).get());
  ASSERT_NE(Absent, nullptr);
  ASSERT_EQ(Absent->getNumOperands(), windows_eh_md::RegistrationOperandCount);
  for (unsigned Operand : {
           windows_eh_md::RegistrationTryLevelOffset,
           windows_eh_md::RegistrationSeededTryLevel,
           windows_eh_md::RegistrationRecordOffset,
       }) {
    const auto *Optional =
        llvm::dyn_cast<llvm::MDNode>(Absent->getOperand(Operand).get());
    ASSERT_NE(Optional, nullptr);
    EXPECT_EQ(Optional->getNumOperands(), 0u);
  }

  ExceptionFunction ExplicitZero = AbsentOptionals;
  ExplicitZero.Registration->TryLevelOffset = 0;
  ExplicitZero.Registration->SeededTryLevel = 0;
  ExplicitZero.Registration->RegistrationOffset = 0;
  llvm::MDNode *ZeroPayload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, ExplicitZero);
  const auto *Zero = llvm::dyn_cast<llvm::MDNode>(
      ZeroPayload->getOperand(windows_eh_md::Registration).get());
  ASSERT_NE(Zero, nullptr);
  for (unsigned Operand : {
           windows_eh_md::RegistrationTryLevelOffset,
           windows_eh_md::RegistrationSeededTryLevel,
           windows_eh_md::RegistrationRecordOffset,
       }) {
    const auto *Optional =
        llvm::dyn_cast<llvm::MDNode>(Zero->getOperand(Operand).get());
    ASSERT_NE(Optional, nullptr);
    ASSERT_EQ(Optional->getNumOperands(), 1u);
    EXPECT_EQ(metadataInteger(*Optional, 0, 32), 0u);
  }
  EXPECT_NE(AbsentPayload, ZeroPayload);
}

TEST(WindowsEHMetadataEncoder, DistinguishesRegistrationFieldChanges) {
  ExceptionFunction Original = makeRichExceptionFunction();
  ExceptionFunction Changed = Original;
  ASSERT_TRUE(Changed.Registration.has_value());
  ASSERT_FALSE(Changed.Registration->TryLevelStores.empty());
  ++Changed.Registration->TryLevelStores.back().EndVA;

  llvm::LLVMContext Context;
  llvm::MDNode *OriginalPayload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, Original);
  llvm::MDNode *ChangedPayload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, Changed);
  EXPECT_NE(OriginalPayload, ChangedPayload);
  EXPECT_NE(fingerprintDigest(*OriginalPayload),
            fingerprintDigest(*ChangedPayload));
}

TEST(WindowsEHMetadataEncoder, NestedFieldChangeProducesDifferentNode) {
  ExceptionFunction Original = makeRichExceptionFunction();
  ExceptionFunction Changed = Original;
  ASSERT_TRUE(Changed.Cxx.has_value());
  ASSERT_FALSE(Changed.Cxx->TryBlocks.empty());
  ASSERT_FALSE(Changed.Cxx->TryBlocks.front().Handlers.empty());
  ASSERT_EQ(
      Changed.Cxx->TryBlocks.front().Handlers.front().ContinuationVAs.size(),
      2u);
  ++Changed.Cxx->TryBlocks.front().Handlers.front().ContinuationVAs.back();

  llvm::LLVMContext Context;
  llvm::MDNode *OriginalPayload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, Original);
  llvm::MDNode *ChangedPayload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, Changed);
  EXPECT_NE(OriginalPayload, ChangedPayload);
  EXPECT_NE(fingerprintDigest(*OriginalPayload),
            fingerprintDigest(*ChangedPayload));
}

TEST(WindowsEHMetadataEncoder, DistinguishesNativeFunctionInfoGroupIdentity) {
  ExceptionFunction Original = makeRichExceptionFunction();
  ExceptionFunction Changed = Original;
  ASSERT_TRUE(Changed.Cxx.has_value());
  ASSERT_NE(Changed.Cxx->NativeFuncInfoVA, 0u);
  ++Changed.Cxx->NativeFuncInfoVA;

  llvm::LLVMContext Context;
  llvm::MDNode *OriginalPayload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, Original);
  llvm::MDNode *ChangedPayload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, Changed);
  EXPECT_NE(OriginalPayload, ChangedPayload);
  EXPECT_NE(fingerprintDigest(*OriginalPayload),
            fingerprintDigest(*ChangedPayload));
}

TEST(WindowsEHMetadataEncoder, BitcodeRoundTripRemainsCanonical) {
  const ExceptionFunction EH = makeRichExceptionFunction();
  llvm::LLVMContext SourceContext;
  llvm::Module SourceModule("windows-eh-metadata", SourceContext);
  llvm::FunctionType *Type =
      llvm::FunctionType::get(llvm::Type::getVoidTy(SourceContext), false);
  llvm::Function *Function =
      llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                             "rich_windows_eh", SourceModule);
  llvm::MDNode *Payload =
      windows_eh_md::getCanonicalFunctionMetadata(SourceContext, EH);
  Function->setMetadata(windows_eh_md::FunctionAttachment, Payload);

  std::string Bitcode;
  llvm::raw_string_ostream BitcodeStream(Bitcode);
  llvm::WriteBitcodeToFile(SourceModule, BitcodeStream);
  BitcodeStream.flush();

  llvm::LLVMContext RoundTripContext;
  auto ModuleOrError = llvm::parseBitcodeFile(
      llvm::MemoryBufferRef(Bitcode, "windows-eh-metadata.bc"),
      RoundTripContext);
  if (!ModuleOrError) {
    ADD_FAILURE() << llvm::toString(ModuleOrError.takeError());
    return;
  }
  std::unique_ptr<llvm::Module> RoundTripModule = std::move(*ModuleOrError);
  llvm::Function *RoundTripFunction =
      RoundTripModule->getFunction("rich_windows_eh");
  ASSERT_NE(RoundTripFunction, nullptr);
  llvm::MDNode *RoundTripPayload =
      RoundTripFunction->getMetadata(windows_eh_md::FunctionAttachment);
  ASSERT_NE(RoundTripPayload, nullptr);

  EXPECT_EQ(RoundTripPayload,
            windows_eh_md::getCanonicalFunctionMetadata(RoundTripContext, EH));
  EXPECT_EQ(fingerprintDigest(*RoundTripPayload), RichSchemaV8Fingerprint);
}

TEST(WindowsEHNativeSource, AcceptsOnlyTheExactSupportedCOFFSourceModels) {
  ExceptionFunction SEH = makeNativeSEHSource();
  WindowsEHNativeSourceClassification SEHResult =
      classifyWindowsEHNativeSource(SEH, Arch::X64, BinaryFormat::COFF);
  EXPECT_TRUE(SEHResult.canRegenerateLanguageMetadata());
  EXPECT_EQ(SEHResult.Model, WindowsEHNativeSourceModel::SEH);
  EXPECT_EQ(SEHResult.Reason, WindowsEHNativeSourceReason::Eligible);

  ExceptionFunction Cxx = makeNativeFH3Source();
  WindowsEHNativeSourceClassification CxxResult =
      classifyWindowsEHNativeSource(Cxx, Arch::X64, BinaryFormat::COFF);
  EXPECT_TRUE(CxxResult.canRegenerateLanguageMetadata());
  EXPECT_EQ(CxxResult.Model, WindowsEHNativeSourceModel::CxxFH3);
  EXPECT_EQ(CxxResult.Reason, WindowsEHNativeSourceReason::Eligible);

  ExceptionFunction ARM32 = SEH;
  ARM32.CodeRange = {0x10001000, 0x10001040};
  ARM32.Encoding = ExceptionEncoding::ARM32Unpacked;
  ARM32.PersonalityVA = 0x10002001;
  ASSERT_TRUE(ARM32.SEH.has_value());
  ASSERT_EQ(ARM32.SEH->Scopes.size(), 1u);
  ARM32.SEH->Scopes.front().GuardedRange = {0x10001000, 0x10001010};
  ARM32.SEH->Scopes.front().HandlerVA = 0x10001020;
  ARM32.SEH->Scopes.front().ContinuationVA = 0x10001020;
  const WindowsEHNativeSourceClassification ARM32Result =
      classifyWindowsEHNativeSource(ARM32, Arch::ARM, BinaryFormat::COFF);
  EXPECT_TRUE(ARM32Result.canRegenerateLanguageMetadata());
  EXPECT_EQ(ARM32Result.Model, WindowsEHNativeSourceModel::SEH);
  EXPECT_EQ(ARM32Result.Reason, WindowsEHNativeSourceReason::Eligible);

  EXPECT_EQ(
      classifyWindowsEHNativeSource(SEH, Arch::ARM, BinaryFormat::COFF).Reason,
      WindowsEHNativeSourceReason::UnsupportedUnwindEncoding);
  EXPECT_EQ(
      classifyWindowsEHNativeSource(SEH, Arch::X64, BinaryFormat::ELF).Reason,
      WindowsEHNativeSourceReason::UnsupportedObjectFormat);
}

TEST(WindowsEHNativeSource, AcceptsOnlyTheBoundedAArch64FH3Singleton) {
  ExceptionFunction EH = makeNativeFH3Source();
  EH.Encoding = ExceptionEncoding::ARM64Unpacked;

  const WindowsEHNativeSourceClassification IR =
      classifyWindowsEHNativeSource(EH, Arch::AArch64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::IRLowering);
  EXPECT_TRUE(IR.canLowerNativeIR());
  EXPECT_EQ(IR.Model, WindowsEHNativeSourceModel::CxxFH3);
  EXPECT_EQ(IR.Reason, WindowsEHNativeSourceReason::Eligible);

  const WindowsEHNativeSourceClassification Output =
      classifyWindowsEHNativeSource(EH, Arch::AArch64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch);
  EXPECT_TRUE(Output.canPatchOutput());
  EXPECT_EQ(Output.Model, WindowsEHNativeSourceModel::CxxFH3);
  EXPECT_EQ(Output.Reason, WindowsEHNativeSourceReason::Eligible);

  ExceptionFunction Typed = EH;
  Typed.Cxx->TryBlocks.front().Handlers.front().TypeDescriptorVA =
      EH.CodeRange.End + 0x100;
  EXPECT_TRUE(
      classifyWindowsEHNativeSource(Typed, Arch::AArch64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch)
          .canPatchOutput());

  ExceptionFunction Rejected = EH;
  Rejected.Encoding = ExceptionEncoding::ARM64Packed;
  EXPECT_EQ(
      classifyWindowsEHNativeSource(Rejected, Arch::AArch64, BinaryFormat::COFF)
          .Reason,
      WindowsEHNativeSourceReason::UnsupportedUnwindEncoding);

  Rejected = EH;
  Rejected.Cxx->IsSeparated = true;
  EXPECT_EQ(
      classifyWindowsEHNativeSource(Rejected, Arch::AArch64, BinaryFormat::COFF)
          .Reason,
      WindowsEHNativeSourceReason::UnsupportedCxxSeparated);

  Rejected = EH;
  Rejected.Cxx->IsCatchFunclet = true;
  EXPECT_EQ(
      classifyWindowsEHNativeSource(Rejected, Arch::AArch64, BinaryFormat::COFF)
          .Reason,
      WindowsEHNativeSourceReason::UnsupportedCxxCatchFunclet);

  Rejected = Typed;
  Rejected.Cxx->TryBlocks.front().Handlers.front().CatchObjectOffset = 8;
  EXPECT_EQ(
      classifyWindowsEHNativeSource(Rejected, Arch::AArch64, BinaryFormat::COFF)
          .Reason,
      WindowsEHNativeSourceReason::UnsupportedCxxHandlerFrameState);

  Rejected = makeNativeFH4Source();
  Rejected.Encoding = ExceptionEncoding::ARM64Unpacked;
  EXPECT_EQ(
      classifyWindowsEHNativeSource(Rejected, Arch::AArch64, BinaryFormat::COFF)
          .Reason,
      WindowsEHNativeSourceReason::UnsupportedArchitecture);
}

TEST(WindowsEHNativeSource, AcceptsOnlyTheBoundedLosslessFH4WriterShape) {
  ExceptionFunction EH = makeNativeFH4Source();

  const WindowsEHNativeSourceClassification IR = classifyWindowsEHNativeSource(
      EH, Arch::X64, BinaryFormat::COFF, WindowsEHNativeCapability::IRLowering);
  EXPECT_TRUE(IR.canLowerNativeIR());
  EXPECT_EQ(IR.Model, WindowsEHNativeSourceModel::CxxFH4);
  EXPECT_EQ(IR.Reason, WindowsEHNativeSourceReason::Eligible);

  const WindowsEHNativeSourceClassification Output =
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch);
  EXPECT_TRUE(Output.canPatchOutput());
  EXPECT_EQ(Output.Model, WindowsEHNativeSourceModel::CxxFH4);

  ExceptionFunction Typed = EH;
  CxxCatchHandler &TypedCatch = Typed.Cxx->TryBlocks.front().Handlers.front();
  TypedCatch.Adjectives = 9;
  TypedCatch.TypeDescriptorVA = EH.CodeRange.End + 0x100;
  const WindowsEHNativeSourceClassification TypedOutput =
      classifyWindowsEHNativeSource(Typed, Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch);
  EXPECT_TRUE(TypedOutput.canPatchOutput());
  EXPECT_EQ(TypedOutput.Model, WindowsEHNativeSourceModel::CxxFH4);

  ExceptionFunction GS = EH;
  GS.Personality = ExceptionPersonality::GSHandlerCheckEH4;
  GSCookieInfo Cookie;
  Cookie.ParseStatus = ExceptionParseStatus::Complete;
  Cookie.CookieOffset = 0x20;
  Cookie.HasExceptionHandler = true;
  Cookie.HasUnwindHandler = true;
  Cookie.Payload = {0x23, 0, 0, 0};
  GS.GSCookie = std::move(Cookie);
  const WindowsEHNativeSourceClassification GSOutput =
      classifyWindowsEHNativeSource(GS, Arch::X64, BinaryFormat::COFF,
                                    WindowsEHNativeCapability::OutputPatch);
  EXPECT_TRUE(GSOutput.canPatchOutput());
  EXPECT_EQ(GSOutput.Model, WindowsEHNativeSourceModel::CxxFH4);

  std::vector<ExceptionFunction> Rejected;
  Rejected.push_back(EH);
  Rejected.back().Cxx->Flags |= 0x40;
  Rejected.push_back(EH);
  Rejected.back().Cxx->UnwindMap[1].ToState = 0;
  Rejected.push_back(Typed);
  Rejected.back().Cxx->TryBlocks.front().Handlers.front().CatchObjectOffset = 8;
  Rejected.push_back(EH);
  Rejected.back().Cxx->TryBlocks.front().Handlers.front().ContinuationVAs = {
      EH.CodeRange.Begin + 0x18};
  Rejected.push_back(EH);
  Rejected.back().GSCookie.emplace();
  Rejected.push_back(GS);
  Rejected.back().GSCookie->Payload.front() ^= 1u;
  Rejected.push_back(GS);
  Rejected.back().GSCookie->HasAlignment = true;
  for (const ExceptionFunction &Candidate : Rejected) {
    const WindowsEHNativeSourceClassification Classification =
        classifyWindowsEHNativeSource(Candidate, Arch::X64, BinaryFormat::COFF,
                                      WindowsEHNativeCapability::OutputPatch);
    EXPECT_FALSE(Classification.canPatchOutput());
  }

  EXPECT_EQ(classifyWindowsEHNativeSource(EH, Arch::AArch64, BinaryFormat::COFF,
                                          WindowsEHNativeCapability::IRLowering)
                .Reason,
            WindowsEHNativeSourceReason::UnsupportedArchitecture);
}

TEST(WindowsEHNativeSource,
     RequiresInnerFirstNestedSEHAndKeepsAArch64OutputSingleScope) {
  ExceptionFunction EH = makeNativeSEHSource();
  ASSERT_TRUE(EH.SEH.has_value());
  ASSERT_EQ(EH.SEH->Scopes.size(), 1u);
  const SEHScopeRecord Inner = EH.SEH->Scopes.front();
  SEHScopeRecord Outer = Inner;
  Outer.GuardedRange.End = EH.CodeRange.Begin + 0x18;
  EH.SEH->Scopes = {Outer, Inner};

  EXPECT_EQ(
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF).Reason,
      WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph);

  EH.SEH->Scopes = {Inner, Outer};
  EXPECT_TRUE(
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF)
          .canPatchOutput());

  EH.Encoding = ExceptionEncoding::ARM64Unpacked;
  const WindowsEHNativeSourceClassification IRSource =
      classifyWindowsEHNativeSource(
          EH, Arch::AArch64, BinaryFormat::COFF,
          WindowsEHNativeCapability::IRLowering);
  EXPECT_TRUE(IRSource.canLowerNativeIR());
  const WindowsEHNativeSourceClassification PatchSource =
      classifyWindowsEHNativeSource(
          EH, Arch::AArch64, BinaryFormat::COFF,
          WindowsEHNativeCapability::OutputPatch);
  EXPECT_FALSE(PatchSource.canPatchOutput());
  EXPECT_EQ(PatchSource.Reason,
            WindowsEHNativeSourceReason::UnsupportedSEHScopeGraph);

  EH.SEH->Scopes = {Inner};
  ++EH.SEH->Scopes.front().GuardedRange.End;
  const ExceptionAddressRange RawLegacyRange =
      EH.SEH->Scopes.front().GuardedRange;
  const std::optional<ExceptionAddressRange> SemanticLegacyRange =
      getSemanticSEHGuardedRange(EH.SEH->Scopes.front(), Arch::AArch64,
                                 EH.CodeRange);
  ASSERT_TRUE(SemanticLegacyRange.has_value());
  EXPECT_EQ(SemanticLegacyRange->Begin, RawLegacyRange.Begin);
  EXPECT_EQ(SemanticLegacyRange->End, RawLegacyRange.End + 3);
  EXPECT_EQ(EH.SEH->Scopes.front().GuardedRange.End, RawLegacyRange.End);
  const WindowsEHNativeSourceClassification LegacyIRSource =
      classifyWindowsEHNativeSource(
          EH, Arch::AArch64, BinaryFormat::COFF,
          WindowsEHNativeCapability::IRLowering);
  EXPECT_TRUE(LegacyIRSource.canLowerNativeIR());
  const WindowsEHNativeSourceClassification LegacyPatchSource =
      classifyWindowsEHNativeSource(
          EH, Arch::AArch64, BinaryFormat::COFF,
          WindowsEHNativeCapability::OutputPatch);
  EXPECT_TRUE(LegacyPatchSource.canPatchOutput());

  ++EH.SEH->Scopes.front().GuardedRange.End;
  EXPECT_EQ(classifyWindowsEHNativeSource(
                EH, Arch::AArch64, BinaryFormat::COFF,
                WindowsEHNativeCapability::IRLowering)
                .Reason,
            WindowsEHNativeSourceReason::InvalidSEHScope);
}

TEST(WindowsEHNativeSource,
     RejectsAArch64LegacyScopeEndOverflowAndOwnerEscape) {
  constexpr va_t Max = std::numeric_limits<va_t>::max();
  SEHScopeRecord Overflow;
  Overflow.GuardedRange = {Max - 7, Max - 2};
  EXPECT_FALSE(getSemanticSEHGuardedRange(
      Overflow, Arch::AArch64, ExceptionAddressRange{Max - 7, Max}));

  SEHScopeRecord EscapesOwner;
  EscapesOwner.GuardedRange = {0x140001000, 0x140001011};
  EXPECT_FALSE(getSemanticSEHGuardedRange(
      EscapesOwner, Arch::AArch64,
      ExceptionAddressRange{0x140001000, 0x140001012}));

  SEHScopeRecord InvalidLowBits = EscapesOwner;
  InvalidLowBits.GuardedRange.End = 0x140001012;
  EXPECT_FALSE(getSemanticSEHGuardedRange(
      InvalidLowBits, Arch::AArch64,
      ExceptionAddressRange{0x140001000, 0x140001020}));
  InvalidLowBits.GuardedRange.End = 0x140001013;
  EXPECT_FALSE(getSemanticSEHGuardedRange(
      InvalidLowBits, Arch::AArch64,
      ExceptionAddressRange{0x140001000, 0x140001020}));
}

TEST(WindowsEHNativeSource, RejectsFH3HandlerStateBeforeCatchInterval) {
  ExceptionFunction EH = makeNativeFH3Source();
  ASSERT_TRUE(EH.Cxx.has_value());
  ASSERT_EQ(EH.Cxx->IPMap.size(), 3u);
  EH.Cxx->IPMap.back().State = -1;

  const WindowsEHNativeSourceClassification Result =
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF);
  EXPECT_FALSE(Result.canRegenerateLanguageMetadata());
  EXPECT_EQ(Result.Model, WindowsEHNativeSourceModel::CxxFH3);
  EXPECT_EQ(Result.Reason, WindowsEHNativeSourceReason::InvalidCxxHandler);
}

TEST(WindowsEHNativeSource, RejectsFH3HandlerStateAfterCatchInterval) {
  ExceptionFunction EH = makeNativeFH3Source();
  ASSERT_TRUE(EH.Cxx.has_value());
  CxxExceptionInfo &Cxx = *EH.Cxx;
  ASSERT_EQ(Cxx.IPMap.size(), 3u);
  Cxx.MaxState = 3;
  CxxUnwindAction State2;
  State2.ToState = 1;
  State2.Kind = CxxUnwindAction::ActionKind::None;
  Cxx.UnwindMap.push_back(State2);
  Cxx.IPMap.back().State = 2;
  ASSERT_TRUE(Cxx.hasValidStateGraph());

  const WindowsEHNativeSourceClassification Result =
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF);
  EXPECT_FALSE(Result.canRegenerateLanguageMetadata());
  EXPECT_EQ(Result.Model, WindowsEHNativeSourceModel::CxxFH3);
  EXPECT_EQ(Result.Reason, WindowsEHNativeSourceReason::InvalidCxxHandler);
}

TEST(WindowsEHNativeSource, RejectsStaleDecodeSummariesAndLanguageOverlays) {
  ExceptionFunction EH = makeNativeFH3Source();
  ExceptionFunctionDecodeProvenance Provenance;
  Provenance.Structural.Diagnostics = {"structural"};
  Provenance.Language.Diagnostics = {"language"};
  EH.DecodeProvenance = std::move(Provenance);
  EH.rebuildParseSummary();
  EXPECT_TRUE(classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF)
                  .canRegenerateLanguageMetadata());

  EH.Diagnostics = {"language", "structural"};
  EXPECT_EQ(
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF).Reason,
      WindowsEHNativeSourceReason::InconsistentDecodeProvenance);
  EH.rebuildParseSummary();

  EH.Rust.emplace();
  EXPECT_EQ(
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF).Reason,
      WindowsEHNativeSourceReason::LanguageOverlay);
  EH.Rust.reset();
  EH.ObjC.emplace();
  EXPECT_EQ(
      classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF).Reason,
      WindowsEHNativeSourceReason::LanguageOverlay);
}

TEST(WindowsEHMetadataEncoder,
     EncodesTargetAwareLanguageRegenerationIndependentlyOfUnwindOnlyFrames) {
  llvm::LLVMContext Context;
  ExceptionFunction SEH = makeNativeSEHSource();
  llvm::MDNode *X64 = windows_eh_md::getCanonicalFunctionMetadata(
      Context, SEH, Arch::X64, BinaryFormat::COFF);
  llvm::MDNode *ARM = windows_eh_md::getCanonicalFunctionMetadata(
      Context, SEH, Arch::ARM, BinaryFormat::COFF);
  llvm::MDNode *UnknownTarget =
      windows_eh_md::getCanonicalFunctionMetadata(Context, SEH);
  EXPECT_EQ(metadataInteger(*X64, windows_eh_md::CanRegenerate, 1), 1u);
  EXPECT_EQ(metadataInteger(*ARM, windows_eh_md::CanRegenerate, 1), 0u);
  EXPECT_EQ(metadataInteger(*UnknownTarget, windows_eh_md::CanRegenerate, 1),
            0u);

  ExceptionFunction UnwindOnly;
  UnwindOnly.CodeRange = SEH.CodeRange;
  UnwindOnly.Encoding = ExceptionEncoding::X64UnwindV1;
  WindowsEHNativeSourceClassification Result =
      classifyWindowsEHNativeSource(UnwindOnly, Arch::X64, BinaryFormat::COFF);
  EXPECT_FALSE(Result.canRegenerateLanguageMetadata());
  EXPECT_EQ(Result.Reason, WindowsEHNativeSourceReason::NoLanguagePersonality);
  llvm::MDNode *UnwindOnlyMetadata =
      windows_eh_md::getCanonicalFunctionMetadata(
          Context, UnwindOnly, Arch::X64, BinaryFormat::COFF);
  EXPECT_EQ(
      metadataInteger(*UnwindOnlyMetadata, windows_eh_md::CanRegenerate, 1),
      0u);
}

TEST(WindowsEHNativeSource, ExposesStableFailureReasonNames) {
  EXPECT_STREQ(
      getWindowsEHNativeSourceReasonName(WindowsEHNativeSourceReason::Eligible),
      "eligible");
  EXPECT_STREQ(getWindowsEHNativeSourceReasonName(
                   WindowsEHNativeSourceReason::InconsistentDecodeProvenance),
               "inconsistent-decode-provenance");
  EXPECT_STREQ(getWindowsEHNativeSourceReasonName(
                   WindowsEHNativeSourceReason::LanguageOverlay),
               "language-overlay");
  EXPECT_STREQ(getWindowsEHNativeSourceReasonName(
                   WindowsEHNativeSourceReason::UnsupportedArchitecture),
               "unsupported-architecture");
}

TEST(WindowsEHSemanticDigest, UsesVersionedLittleEndianSHA256Words) {
  const ExceptionFunction EH = makeNativeSEHSource();
  const auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
      EH, Arch::X64, /*ScopeIndex=*/0);
  ASSERT_TRUE(Token.has_value());
  const std::array<uint64_t, 4> Expected{
      0x676768c26eeaeb60ULL, 0xd96201b404074e2cULL, 0x4eda5ea04edcb1ffULL,
      0x973ed61af6d399a1ULL};
  EXPECT_EQ(Token->Digest, Expected);
  EXPECT_EQ(windows_eh_semantics::SemanticDigestSchemaVersion, 1u);
}

TEST(WindowsEHSemanticDigest,
     BindsEverySEHTokenToTheCompleteOrderedScopeGraph) {
  ExceptionFunction EH = makeNativeSEHSource();
  ASSERT_TRUE(EH.SEH.has_value());
  SEHScopeRecord Middle = EH.SEH->Scopes.front();
  Middle.GuardedRange = {EH.CodeRange.Begin + 0x10, EH.CodeRange.Begin + 0x20};
  Middle.HandlerVA = EH.CodeRange.Begin + 0x38;
  Middle.ContinuationVA = Middle.HandlerVA;
  SEHScopeRecord Last = Middle;
  Last.GuardedRange = {EH.CodeRange.Begin + 0x20, EH.CodeRange.Begin + 0x30};
  EH.SEH->Scopes.push_back(Middle);
  EH.SEH->Scopes.push_back(Last);

  const auto Baseline = windows_eh_semantics::getSEHScopeSemanticToken(
      EH, Arch::X64, /*ScopeIndex=*/0);
  const auto Repeated = windows_eh_semantics::getSEHScopeSemanticToken(
      EH, Arch::X64, /*ScopeIndex=*/0);
  ASSERT_TRUE(Baseline.has_value());
  ASSERT_TRUE(Repeated.has_value());
  EXPECT_EQ(*Repeated, *Baseline);
  EXPECT_EQ(Baseline->Kind,
            llvm::mc_rewrite::RewriteWinEHSemanticKind::SEHScope);
  EXPECT_EQ(Baseline->Region, 0u);
  EXPECT_EQ(Baseline->Clause, 0u);

  ExceptionFunction ChangedField = EH;
  ++ChangedField.SEH->Scopes[1].HandlerVA;
  const auto ChangedFieldToken = windows_eh_semantics::getSEHScopeSemanticToken(
      ChangedField, Arch::X64, /*ScopeIndex=*/0);
  ASSERT_TRUE(ChangedFieldToken.has_value());
  EXPECT_NE(*ChangedFieldToken, *Baseline);

  ExceptionFunction ChangedOrder = EH;
  std::swap(ChangedOrder.SEH->Scopes[1], ChangedOrder.SEH->Scopes[2]);
  const auto ChangedOrderToken = windows_eh_semantics::getSEHScopeSemanticToken(
      ChangedOrder, Arch::X64, /*ScopeIndex=*/0);
  ASSERT_TRUE(ChangedOrderToken.has_value());
  EXPECT_NE(*ChangedOrderToken, *Baseline);

  ExceptionFunction AArch64EH = EH;
  AArch64EH.Encoding = ExceptionEncoding::ARM64Unpacked;
  const auto AArch64Token = windows_eh_semantics::getSEHScopeSemanticToken(
      AArch64EH, Arch::AArch64, /*ScopeIndex=*/0);
  ASSERT_TRUE(AArch64Token.has_value());
  EXPECT_NE(*AArch64Token, *Baseline);
  EXPECT_FALSE(windows_eh_semantics::getSEHScopeSemanticToken(
      EH, Arch::Unknown, /*ScopeIndex=*/0));
}

TEST(WindowsEHSemanticDigest,
     RejectsAmbiguousSEHContainmentAndMissingScopeIdentity) {
  ExceptionFunction EH = makeNativeSEHSource();
  ASSERT_TRUE(EH.SEH.has_value());
  SEHScopeRecord Crossing = EH.SEH->Scopes.front();
  Crossing.GuardedRange = {EH.CodeRange.Begin + 8, EH.CodeRange.Begin + 0x18};
  EH.SEH->Scopes.push_back(Crossing);

  EXPECT_FALSE(windows_eh_semantics::getSEHScopeSemanticToken(
      EH, Arch::X64, /*ScopeIndex=*/0));
  EXPECT_FALSE(windows_eh_semantics::getSEHScopeSemanticToken(
      EH, Arch::X64, /*ScopeIndex=*/2));
}

TEST(WindowsEHSemanticDigest, BindsEveryCxxTokenToTheCompleteOrderedFH3Graph) {
  ExceptionFunction EH = makeNativeFH3Source();
  ASSERT_TRUE(EH.Cxx.has_value());
  ASSERT_EQ(EH.Cxx->TryBlocks.size(), 1u);
  CxxCatchHandler SecondCatch = EH.Cxx->TryBlocks.front().Handlers.front();
  SecondCatch.Adjectives = 0x40;
  SecondCatch.TypeDescriptorVA = 0x140003000;
  SecondCatch.CatchObjectOffset = -8;
  SecondCatch.HandlerVA = EH.CodeRange.Begin + 0x30;
  SecondCatch.ParentFrameOffset = 0x20;
  SecondCatch.ContinuationVAs = {EH.CodeRange.Begin + 0x10,
                                 EH.CodeRange.Begin + 0x20};
  EH.Cxx->TryBlocks.front().Handlers.push_back(SecondCatch);

  const auto Baseline = windows_eh_semantics::getCxxCatchSemanticToken(
      EH, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0);
  const auto Repeated = windows_eh_semantics::getCxxCatchSemanticToken(
      EH, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0);
  const auto Second = windows_eh_semantics::getCxxCatchSemanticToken(
      EH, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/1);
  ASSERT_TRUE(Baseline.has_value());
  ASSERT_TRUE(Repeated.has_value());
  ASSERT_TRUE(Second.has_value());
  EXPECT_EQ(*Repeated, *Baseline);
  EXPECT_NE(*Second, *Baseline);
  EXPECT_EQ(Second->Kind, llvm::mc_rewrite::RewriteWinEHSemanticKind::CxxCatch);
  EXPECT_EQ(Second->Region, 0u);
  EXPECT_EQ(Second->Clause, 1u);

  std::vector<ExceptionFunction> ChangedGraphs;
  ChangedGraphs.push_back(EH);
  ChangedGraphs.back().Cxx->NativeFuncInfoVA = 0x140004000;
  ChangedGraphs.push_back(EH);
  ChangedGraphs.back().Cxx->UnwindMap[1].ObjectOffset = -4;
  ChangedGraphs.push_back(EH);
  ++ChangedGraphs.back().Cxx->TryBlocks[0].Handlers[1].Adjectives;
  ChangedGraphs.push_back(EH);
  ++ChangedGraphs.back().Cxx->IPMap[1].IP;
  ChangedGraphs.push_back(EH);
  std::swap(
      ChangedGraphs.back().Cxx->TryBlocks[0].Handlers[1].ContinuationVAs[0],
      ChangedGraphs.back().Cxx->TryBlocks[0].Handlers[1].ContinuationVAs[1]);
  for (const ExceptionFunction &Changed : ChangedGraphs) {
    const auto ChangedToken = windows_eh_semantics::getCxxCatchSemanticToken(
        Changed, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0);
    ASSERT_TRUE(ChangedToken.has_value());
    EXPECT_NE(*ChangedToken, *Baseline);
  }

  const auto AArch64Token = windows_eh_semantics::getCxxCatchSemanticToken(
      EH, Arch::AArch64, /*TryBlockIndex=*/0, /*CatchIndex=*/0);
  ASSERT_TRUE(AArch64Token.has_value());
  EXPECT_NE(*AArch64Token, *Baseline);
}

TEST(WindowsEHSemanticDigest,
     DistinguishesFH4WireIdentityAndBindsItsCompleteGraph) {
  ExceptionFunction FH4 = makeNativeFH4Source();
  const auto Baseline = windows_eh_semantics::getCxxCatchSemanticToken(
      FH4, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0);
  const auto Repeated = windows_eh_semantics::getCxxCatchSemanticToken(
      FH4, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0);
  ASSERT_TRUE(Baseline.has_value());
  ASSERT_TRUE(Repeated.has_value());
  EXPECT_EQ(*Repeated, *Baseline);

  const auto FH3 = windows_eh_semantics::getCxxCatchSemanticToken(
      makeNativeFH3Source(), Arch::X64, /*TryBlockIndex=*/0,
      /*CatchIndex=*/0);
  ASSERT_TRUE(FH3.has_value());
  EXPECT_NE(*FH3, *Baseline);

  ExceptionFunction Changed = FH4;
  ++Changed.Cxx->TryBlocks.front().Handlers.front().Adjectives;
  const auto ChangedToken = windows_eh_semantics::getCxxCatchSemanticToken(
      Changed, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0);
  ASSERT_TRUE(ChangedToken.has_value());
  EXPECT_NE(*ChangedToken, *Baseline);

  ExceptionFunction GS = FH4;
  GS.Personality = ExceptionPersonality::GSHandlerCheckEH4;
  GSCookieInfo Cookie;
  Cookie.ParseStatus = ExceptionParseStatus::Complete;
  Cookie.CookieOffset = 0x20;
  Cookie.HasExceptionHandler = true;
  Cookie.HasUnwindHandler = true;
  Cookie.Payload = {0x23, 0, 0, 0};
  GS.GSCookie = std::move(Cookie);
  const auto GSToken = windows_eh_semantics::getCxxCatchSemanticToken(
      GS, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0);
  ASSERT_TRUE(GSToken.has_value());
  EXPECT_NE(*GSToken, *Baseline);

  GS.GSCookie->Payload.front() ^= 1u;
  const auto ChangedGSToken = windows_eh_semantics::getCxxCatchSemanticToken(
      GS, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0);
  ASSERT_TRUE(ChangedGSToken.has_value());
  EXPECT_NE(*ChangedGSToken, *GSToken);

  FH4.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH3;
  EXPECT_FALSE(windows_eh_semantics::getCxxCatchSemanticToken(
      FH4, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0));
}

TEST(WindowsEHSemanticDigest, RejectsMalformedOrUnsupportedCxxGraphs) {
  ExceptionFunction EH = makeNativeFH3Source();
  ASSERT_TRUE(EH.Cxx.has_value());

  EH.Cxx->IPMap[1].IP = EH.Cxx->IPMap[0].IP;
  EXPECT_FALSE(windows_eh_semantics::getCxxCatchSemanticToken(
      EH, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0));

  EH = makeNativeFH3Source();
  EH.Cxx->NativeEncoding = CxxExceptionInfo::Encoding::FH4;
  EXPECT_FALSE(windows_eh_semantics::getCxxCatchSemanticToken(
      EH, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/0));
  EXPECT_FALSE(windows_eh_semantics::getCxxCatchSemanticToken(
      EH, Arch::X64, /*TryBlockIndex=*/0, /*CatchIndex=*/1));
}

} // namespace
