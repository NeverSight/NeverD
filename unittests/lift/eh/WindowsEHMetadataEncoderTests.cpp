//===- WindowsEHMetadataEncoderTests.cpp - Windows EH metadata tests ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
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
#include <memory>
#include <string>
#include <utility>

namespace {

using namespace neverd;

constexpr llvm::StringLiteral RichSchemaV5Fingerprint(
    "933d6d68379829fb4310190abb6f4378512bd346f85d3717181f47527bb08c45");

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
  Scope.HandlerVA = 0x140001400;
  Scope.ContinuationVA = 0x140001500;
  Scope.ParseStatus = ExceptionParseStatus::Malformed;
  SEH.Scopes.push_back(Scope);
  EH.SEH = std::move(SEH);

  CxxExceptionInfo Cxx;
  Cxx.NativeEncoding = CxxExceptionInfo::Encoding::FH4;
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

  EH.PrimaryFunctionIndex = 0x1234;
  EH.ChainedPrimaryRange = ExceptionAddressRange{0x140001000, 0x140001200};
  EH.ChainedUnwindInfoRVA = 0xabcdef01;
  EH.Diagnostics = {"first diagnostic", ""};

  ExceptionFunctionDecodeProvenance Provenance;
  Provenance.Structural.ParseStatus = ExceptionParseStatus::Malformed;
  Provenance.Structural.Diagnostics = {"not part of schema v5"};
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

TEST(WindowsEHMetadataEncoder, PreservesSchemaV5Projection) {
  const ExceptionFunction EH = makeRichExceptionFunction();
  llvm::LLVMContext Context;
  llvm::MDNode *Payload =
      windows_eh_md::getCanonicalFunctionMetadata(Context, EH);
  ASSERT_NE(Payload, nullptr);
  EXPECT_EQ(Payload->getNumOperands(), windows_eh_md::OperandCount);
  EXPECT_EQ(Payload, windows_eh_md::getCanonicalFunctionMetadata(Context, EH));
  EXPECT_EQ(fingerprintDigest(*Payload), RichSchemaV5Fingerprint);
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
  EXPECT_EQ(fingerprintDigest(*RoundTripPayload), RichSchemaV5Fingerprint);
}

TEST(WindowsEHNativeSource, AcceptsOnlyTheExactX64COFFSourceModels) {
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

  EXPECT_EQ(classifyWindowsEHNativeSource(SEH, Arch::ARM, BinaryFormat::COFF)
                .Reason,
            WindowsEHNativeSourceReason::UnsupportedArchitecture);
  EXPECT_EQ(classifyWindowsEHNativeSource(SEH, Arch::X64, BinaryFormat::ELF)
                .Reason,
            WindowsEHNativeSourceReason::UnsupportedObjectFormat);
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
  EXPECT_TRUE(classifyWindowsEHNativeSource(EH, Arch::X64,
                                            BinaryFormat::COFF)
                  .canRegenerateLanguageMetadata());

  EH.Diagnostics = {"language", "structural"};
  EXPECT_EQ(classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF)
                .Reason,
            WindowsEHNativeSourceReason::InconsistentDecodeProvenance);
  EH.rebuildParseSummary();

  EH.Rust.emplace();
  EXPECT_EQ(classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF)
                .Reason,
            WindowsEHNativeSourceReason::LanguageOverlay);
  EH.Rust.reset();
  EH.ObjC.emplace();
  EXPECT_EQ(classifyWindowsEHNativeSource(EH, Arch::X64, BinaryFormat::COFF)
                .Reason,
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
  WindowsEHNativeSourceClassification Result = classifyWindowsEHNativeSource(
      UnwindOnly, Arch::X64, BinaryFormat::COFF);
  EXPECT_FALSE(Result.canRegenerateLanguageMetadata());
  EXPECT_EQ(Result.Reason,
            WindowsEHNativeSourceReason::NoLanguagePersonality);
  llvm::MDNode *UnwindOnlyMetadata =
      windows_eh_md::getCanonicalFunctionMetadata(
          Context, UnwindOnly, Arch::X64, BinaryFormat::COFF);
  EXPECT_EQ(metadataInteger(*UnwindOnlyMetadata,
                            windows_eh_md::CanRegenerate, 1),
            0u);
}

TEST(WindowsEHNativeSource, ExposesStableFailureReasonNames) {
  EXPECT_STREQ(getWindowsEHNativeSourceReasonName(
                   WindowsEHNativeSourceReason::Eligible),
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

} // namespace
